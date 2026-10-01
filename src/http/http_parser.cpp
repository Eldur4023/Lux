#include "http_parser.hpp"
#include <llhttp.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace lux::http {

// ── Per-connection parse state ─────────────────────────────────────────────

struct HttpParser::ParseContext {
    ParsedRequest current;

    // Header accumulation
    std::string last_field;
    std::string last_value;
    bool        value_pending = false;  // last_value not yet committed to map
    size_t      header_count  = 0;

    // Flag set by any callback that detects a limit violation
    bool error = false;

    // Set specifically when the body exceeds kMaxBodySize, so the connection
    // layer can answer 413 instead of a generic 400 — the client sent a
    // well-formed request that is simply too big, not a malformed one.
    bool body_too_large = false;

    bool in_message = false;   // see HttpParser::in_message()

    // A multipart body that outgrew kMemBodyMax is written to an unnamed temp
    // file as it arrives, so a big upload never sits in memory.
    bool   multipart  = false;
    int    spool_fd   = -1;
    size_t spool_size = 0;

    ~ParseContext() { if (spool_fd >= 0) ::close(spool_fd); }
    size_t body_limit() const {
        size_t cap = g_max_body_size.load(std::memory_order_relaxed);
        return multipart ? cap : std::min(cap, kMemBodyMax);
    }

    // Back-pointer to the owning parser's callbacks (stable address)
    OnComplete*        on_complete         = nullptr;
    OnHeadersComplete* on_headers_complete = nullptr;
};

// ── llhttp callbacks ───────────────────────────────────────────────────────

static HttpParser::ParseContext* ctx(llhttp_t* p) {
    return static_cast<HttpParser::ParseContext*>(p->data);
}

static int cb_on_message_begin(llhttp_t* p) {
    ctx(p)->in_message = true;
    return HPE_OK;
}

static int cb_on_url(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    if (c->current.path.size() + len > kMaxUrlSize) { c->error = true; return HPE_USER; }
    c->current.path.append(at, len);
    return HPE_OK;
}

static void commit_header(HttpParser::ParseContext* c) {
    if (!c->value_pending) return;
    std::string& key = c->last_field;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });

    auto& hs = c->current.headers;
    auto it = std::find_if(hs.begin(), hs.end(), [&](const auto& h) { return h.first == key; });
    if (it != hs.end()) {
        // RFC 7230 §3.2.2: duplicate headers may be combined with ", ".
        // Two exceptions:
        //  - set-cookie: each value must stay on its own line (in practice
        //    it appears in responses, not requests, but guard anyway).
        //  - cookie: RFC 6265bis §5.4 and RFC 7540 §8.1.2.5 both allow a
        //    client to send it as SEVERAL header fields (HTTP/2 encourages
        //    exactly this, splitting on ';' for better HPACK compression),
        //    and both specify joining them back with "; " -- Cookie's OWN
        //    value syntax already uses "; " to separate cookie-pairs, so
        //    joining with ", " (correct for headers where comma has no
        //    special meaning) instead glues the last pair of one header
        //    field to the first pair of the next with a comma in between:
        //    `Cookie: a=1` + `Cookie: b=2` became "a=1, b=2", which
        //    parse_cookie_header() (cookies.hpp) — splitting on ';', not
        //    ',' — then reads as ONE cookie named "a" with the value
        //    "1, b=2", silently losing "b" entirely.
        const char* sep = (key == "set-cookie") ? "\n"
                         : (key == "cookie")     ? "; "
                                                  : ", ";
        it->second += sep;
        it->second += c->last_value;
    } else {
        if (hs.empty()) hs.reserve(16);
        hs.emplace_back(std::move(key), std::move(c->last_value));
    }

    c->last_field.clear();
    c->last_value.clear();
    c->value_pending = false;
    ++c->header_count;
}

static int cb_on_header_field(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    // A new field name means the previous field+value pair is complete
    if (c->value_pending) {
        if (c->header_count >= kMaxHeaderCount) { c->error = true; return HPE_USER; }
        commit_header(c);
    }
    if (c->last_field.size() + len > kMaxHeaderSize) { c->error = true; return HPE_USER; }
    c->last_field.append(at, len);
    return HPE_OK;
}

static int cb_on_header_value(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    if (c->last_value.size() + len > kMaxHeaderSize) { c->error = true; return HPE_USER; }
    c->last_value.append(at, len);
    c->value_pending = true;
    return HPE_OK;
}

static int cb_on_headers_complete(llhttp_t* p) {
    auto* c = ctx(p);
    // Commit the final header
    if (c->value_pending) {
        if (c->header_count >= kMaxHeaderCount) { c->error = true; return HPE_USER; }
        commit_header(c);
    }
    // HTTP version
    int major = llhttp_get_http_major(p);
    int minor = llhttp_get_http_minor(p);
    c->current.version = (major == 1 && minor == 0) ? "HTTP/1.0" : "HTTP/1.1";

    // A declared Content-Length over the cap is refused now, before a byte of
    // the body is read (chunked bodies are still caught in cb_on_body).
    for (const auto& h : c->current.headers)
        if (h.first == "content-type") c->multipart = h.second.rfind("multipart/form-data", 0) == 0;
    if (!(p->flags & F_CHUNKED) && p->content_length > c->body_limit()) {
        c->error = true;
        c->body_too_large = true;
        return HPE_USER;
    }

    // Headers are done; the body (if any) starts next. Let the connection
    // layer swap the Slowloris header timer for the request timer HERE,
    // not once the body has also fully arrived (see OnHeadersComplete's
    // comment in http_parser.hpp).
    if (c->on_headers_complete && *c->on_headers_complete) (*c->on_headers_complete)();
    return HPE_OK;
}

static bool spool_write(HttpParser::ParseContext* c, const char* at, size_t len) {
    while (len > 0) {
        ssize_t n = ::write(c->spool_fd, at, len);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        at += n; len -= static_cast<size_t>(n);
        c->spool_size += static_cast<size_t>(n);
    }
    return true;
}

static int cb_on_body(llhttp_t* p, const char* at, size_t len) {
    auto* c = ctx(p);
    size_t have = c->spool_fd >= 0 ? c->spool_size : c->current.body.size();
    if (have + len > c->body_limit()) {
        c->error = true;
        c->body_too_large = true;
        return HPE_USER;
    }
    if (c->spool_fd < 0 && c->multipart && have + len > kMemBodyMax) {
        std::error_code ec;
        std::string dir = std::filesystem::temp_directory_path(ec).string();
        c->spool_fd = ::open(dir.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
        if (c->spool_fd < 0 || !spool_write(c, c->current.body.data(), c->current.body.size())) {
            c->error = true;
            return HPE_USER;
        }
        std::string().swap(c->current.body);
    }
    if (c->spool_fd >= 0) {
        if (!spool_write(c, at, len)) { c->error = true; return HPE_USER; }
        return HPE_OK;
    }
    c->current.body.append(at, len);
    return HPE_OK;
}

static int cb_on_message_complete(llhttp_t* p) {
    auto* c = ctx(p);
    c->in_message = false;

    // Split path from query string
    auto q = c->current.path.find('?');
    if (q != std::string::npos) {
        c->current.query = c->current.path.substr(q + 1);
        c->current.path  = c->current.path.substr(0, q);
    }

    if (c->spool_fd >= 0) {
        void* m = ::mmap(nullptr, c->spool_size, PROT_READ, MAP_PRIVATE, c->spool_fd, 0);
        if (m == MAP_FAILED) { c->error = true; return HPE_USER; }
        c->current.body_map = std::make_shared<const MappedBody>(m, c->spool_size);
        ::close(c->spool_fd);
        c->spool_fd = -1;
        c->spool_size = 0;
    }
    c->multipart = false;

    // Method name
    c->current.method = llhttp_method_name(
        static_cast<llhttp_method_t>(llhttp_get_method(p)));

    (*c->on_complete)(std::move(c->current));

    // Reset per-message state (keep parser alive for keep-alive).
    //
    // Chunked trailers (RFC 7230 §4.1.2) run through the SAME
    // on_header_field/on_header_value callbacks as the real headers, but
    // arrive AFTER on_headers_complete already fired for this message —
    // there is no later "new field" callback in this message to commit the
    // last trailer via commit_header()'s value_pending check, so a
    // single-trailer message left last_field/last_value/value_pending set
    // here. Without clearing them, the NEXT request parsed on this
    // keep-alive connection would see cb_on_header_field's "previous
    // pair is complete" branch fire on ITS first header and commit the
    // stale trailer — from a request that already finished — into the new
    // request's header map. Trailers are not exposed to handlers at all,
    // so discarding rather than committing them is correct either way.
    c->last_field.clear();
    c->last_value.clear();
    c->value_pending = false;
    c->current      = {};
    c->header_count = 0;

    // Pause so the connection layer can serialise pipelined requests: the
    // bytes that follow this message stay in the caller's buffer until the
    // current response is on the wire.
    return HPE_PAUSED;
}

// ── HttpParser ─────────────────────────────────────────────────────────────

HttpParser::HttpParser(OnComplete on_complete, OnHeadersComplete on_headers_complete)
    : on_complete_(std::move(on_complete))
    , on_headers_complete_(std::move(on_headers_complete))
    , ctx_(std::make_unique<ParseContext>())
    , parser_(std::make_unique<llhttp_t>())
    , settings_(std::make_unique<llhttp_settings_t>())
{
    ctx_->on_complete         = &on_complete_;
    ctx_->on_headers_complete = &on_headers_complete_;

    llhttp_settings_init(settings_.get());
    settings_->on_message_begin    = cb_on_message_begin;
    settings_->on_url              = cb_on_url;
    settings_->on_header_field     = cb_on_header_field;
    settings_->on_header_value     = cb_on_header_value;
    settings_->on_headers_complete = cb_on_headers_complete;
    settings_->on_body             = cb_on_body;
    settings_->on_message_complete = cb_on_message_complete;

    llhttp_init(parser_.get(), HTTP_REQUEST, settings_.get());
    parser_->data = ctx_.get();
}

HttpParser::~HttpParser() = default;

bool HttpParser::in_message() const { return ctx_->in_message; }

bool HttpParser::feed(const char* data, size_t len) {
    if (ctx_->error) return false;
    last_data_ = data;
    last_len_  = len;
    llhttp_errno_t err = llhttp_execute(parser_.get(), data, len);
    if (err == HPE_OK)     return !ctx_->error;
    if (err == HPE_PAUSED) return true;   // request completed → paused for serialisation
    return false;
}

bool HttpParser::is_paused() const {
    return llhttp_get_errno(parser_.get()) == HPE_PAUSED;
}

bool HttpParser::body_too_large() const {
    return ctx_->body_too_large;
}

size_t HttpParser::unconsumed() const {
    if (!is_paused() || !last_data_) return 0;
    const char* pos = llhttp_get_error_pos(parser_.get());
    if (pos == nullptr || pos < last_data_ || pos > last_data_ + last_len_) return 0;
    return last_len_ - static_cast<size_t>(pos - last_data_);
}

void HttpParser::resume() {
    if (is_paused()) llhttp_resume(parser_.get());
    last_data_ = nullptr;
    last_len_  = 0;
}

} // namespace lux::http
