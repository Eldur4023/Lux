// Outbound HTTP(S) through libcurl -- the one narrow exception to "Lux
// links no TLS": that rule is about terminating inbound TLS (the reverse
// proxy's job), and an outbound call has no proxy to delegate to. libcurl's
// defaults stay on: certificates and host names are always verified.
//
// Every call is is_async (runs on the I/O pool, `await` required) and
// bounded by a timeout. Each pool thread keeps one curl handle, so its
// connection cache carries over: a second call to the same API reuses the
// TCP+TLS connection instead of paying a new handshake.
#include <lux_script/builtin_module.hpp>
#include <lux/percent_encoding.hpp>

#include "netaddr.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace lux_script {

namespace {

constexpr long kDefaultTimeoutMs = 15'000, kMaxTimeoutMs = 120'000;
constexpr long kMaxRedirects = 5;

// The remote end is as untrusted as any client: without a cap, a server
// could stream until this worker ran out of memory. A short write from a
// callback aborts the transfer (CURLE_WRITE_ERROR).
constexpr size_t kMaxResponseBodyBytes   = 16 * 1024 * 1024;
constexpr size_t kMaxResponseHeaderBytes = 64 * 1024;
// curl_global_init() is NOT thread-safe against concurrent calls, but IS
// safe to call once, early, before any other thread touches curl -- exactly
// what a call from BuiltinModuleRegistry's constructor gives (single-
// threaded startup, before the event loop threads exist).
// The share handle is what makes connections outlive a call: every pool
// thread's handle draws on one connection cache (plus DNS and TLS
// sessions), so a call reuses a connection another thread opened.
struct CurlGlobal {
    CURLSH*    share = nullptr;
    std::mutex locks[CURL_LOCK_DATA_LAST];

    CurlGlobal() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        share = curl_share_init();
        curl_share_setopt(share, CURLSHOPT_LOCKFUNC, +[](CURL*, curl_lock_data d, curl_lock_access, void* g) {
            static_cast<CurlGlobal*>(g)->locks[d].lock();
        });
        curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, +[](CURL*, curl_lock_data d, void* g) {
            static_cast<CurlGlobal*>(g)->locks[d].unlock();
        });
        curl_share_setopt(share, CURLSHOPT_USERDATA, this);
        for (auto d : {CURL_LOCK_DATA_CONNECT, CURL_LOCK_DATA_DNS, CURL_LOCK_DATA_SSL_SESSION})
            curl_share_setopt(share, CURLSHOPT_SHARE, d);
    }
    ~CurlGlobal() { curl_share_cleanup(share); curl_global_cleanup(); }
};

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto*  out = static_cast<std::string*>(userdata);
    size_t n   = size * nmemb;
    if (out->size() + n > kMaxResponseBodyBytes) return 0; // short write -> curl aborts
    out->append(ptr, n);
    return n;
}

// Bundles the Dict together with a running byte total: the total has to be
// tracked incrementally here since summing the Dict's own strings on every
// call would make the cap itself O(n^2) over a header block with many lines.
struct HeaderSink {
    Value::Dict headers;
    size_t      bytes = 0;
};

// Collects response headers into a Dict -- a repeated header keeps its LAST
// occurrence, documented (GUIDE.md), not an oversight.
size_t write_header(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;
    auto*  sink  = static_cast<HeaderSink*>(userdata);
    if (sink->bytes + total > kMaxResponseHeaderBytes) return 0; // short write -> curl aborts
    sink->bytes += total;

    std::string_view line(buffer, total);
    // The status line ("HTTP/1.1 200 OK") and the blank line that ends the
    // header block both arrive through this callback too -- neither has a
    // ':', so both are skipped by the same check.
    size_t colon = line.find(':');
    if (colon == std::string_view::npos) return total;
    std::string name(line.substr(0, colon));
    size_t vstart = colon + 1;
    while (vstart < line.size() && (line[vstart] == ' ' || line[vstart] == '\t')) ++vstart;
    size_t vend = line.size();
    while (vend > vstart && (line[vend - 1] == '\r' || line[vend - 1] == '\n')) --vend;
    std::string value(line.substr(vstart, vend - vstart));
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    sink->headers[name] = Value::str(value);
    return total;
}

// public_only: refuse any address that is not public -- checked on the
// address curl actually connected to, before every request (a reused
// connection and each redirect included), so neither a hostname that
// resolves to 10.0.0.5 nor DNS rebinding reaches the internal network or
// the cloud metadata endpoint. For URLs that come from users (webhooks,
// "import from URL").
int only_public(void*, char* ip, char*, int, int) {
    netaddr::Addr a;
    return netaddr::parse(ip, a) && !netaddr::is_private(a) ? CURL_PREREQFUNC_OK : CURL_PREREQFUNC_ABORT;
}

struct Options { long timeout_ms = kDefaultTimeoutMs; bool public_only = false; };

bool read_options(const Value* v, Options& o, const std::string& method, std::string& error) {
    if (!v) return true;
    const auto& d = v->as_dict();
    if (auto it = d.find("timeout_ms"); it != d.end()) {
        if (!it->second.is_int() || it->second.as_int() < 1 || it->second.as_int() > kMaxTimeoutMs) {
            error = "http." + method + "(): timeout_ms must be between 1 and " + std::to_string(kMaxTimeoutMs);
            return false;
        }
        o.timeout_ms = static_cast<long>(it->second.as_int());
    }
    if (auto it = d.find("public_only"); it != d.end()) o.public_only = it->second.truthy();
    return true;
}

Value do_request(const std::string& method, const std::string& url,
                 const std::string* body, bool body_is_json,
                 const Value* headers_arg, const Value* options_arg, std::string& error) {
    static CurlGlobal global;

    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        error = "http." + method + "(): url must start with http:// or https://";
        return Value::null();
    }
    Options opts;
    if (!read_options(options_arg, opts, method, error)) return Value::null();

    // One handle per pool thread, reset between calls; connections live in
    // the shared cache (CurlGlobal).
    thread_local struct Handle {
        CURL* h = curl_easy_init();
        ~Handle() { if (h) curl_easy_cleanup(h); }
    } handle;
    CURL* curl = handle.h;
    if (!curl) { error = "http: could not initialize the request"; return Value::null(); }
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_SHARE, global.share);

    std::string response_body;
    HeaderSink  response_headers;
    struct curl_slist* header_list = nullptr;
    bool content_type_set = false;

    if (headers_arg && headers_arg->is_dict()) {
        for (const auto& [key, val] : headers_arg->as_dict()) {
            if (!val.is_str()) continue; // a non-string header value is silently skipped, not an error
            // Strip CR/LF and NUL before handing the line to curl_slist_append:
            // Response::header() (response.hpp) and build_set_cookie()
            // (cookies.hpp) both strip CR/LF on the INBOUND side of this
            // framework for exactly this reason -- without it here, a
            // key/value built from request data lets a handler inject
            // arbitrary extra header lines (or truncate this one at an
            // embedded NUL) into the OUTBOUND request this module sends.
            auto strip = [](std::string s) {
                s.erase(std::remove_if(s.begin(), s.end(),
                    [](char c) { return c == '\r' || c == '\n' || c == '\0'; }), s.end());
                return s;
            };
            std::string line = strip(key) + ": " + strip(val.as_str());
            header_list = curl_slist_append(header_list, line.c_str());
            std::string lower = key;
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "content-type") content_type_set = true;
        }
    }
    if (body && body_is_json && !content_type_set) {
        // Only for a body the CALLER passed as a Dict/List/Json (auto-
        // serialized in do_body_verb below) -- a raw string body is sent
        // exactly as given, with no assumption about its content type.
        header_list = curl_slist_append(header_list, "Content-Type: application/json");
    }
    if (header_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (method == "post") curl_easy_setopt(curl, CURLOPT_POST, 1L);
    else if (method == "put") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    else if (method == "patch") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    else if (method == "delete") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    // "get" needs neither option: CURLOPT_HTTPGET is curl's default state.

    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, opts.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, opts.timeout_ms);
    if (opts.public_only) curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, only_public);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
    // A redirect is server-controlled: only ever to http/https, never
    // file:// or whatever else curl speaks. (The bitmask form keeps older
    // libcurl-dev building.)
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lux/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode rc = curl_easy_perform(curl);
    if (header_list) curl_slist_free_all(header_list);
    if (rc != CURLE_OK) {
        error = std::string("http.") + method + "(): " +
                (rc == CURLE_ABORTED_BY_CALLBACK ? "the address is not public (public_only)" : curl_easy_strerror(rc));
        return Value::null();
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    Value::Dict out;
    out["status"] = Value::integer(status);
    out["headers"] = Value::dict(std::move(response_headers.headers));
    // Parsed when it is JSON, the raw text otherwise.
    Value parsed;
    if (Value::parse_json(response_body, parsed)) out["body"] = parsed;
    else out["body"] = Value::str(response_body);
    return Value::dict(std::move(out));
}

const Value* opt(std::vector<Value>& args, size_t i) {
    return args.size() > i && args[i].is_dict() ? &args[i] : nullptr;
}

Value fn_http_get(NativeCtx&, std::vector<Value>& a, std::string& e) {
    return do_request("get", a[0].as_str(), nullptr, false, opt(a, 1), opt(a, 2), e);
}

Value fn_http_delete(NativeCtx&, std::vector<Value>& a, std::string& e) {
    return do_request("delete", a[0].as_str(), nullptr, false, opt(a, 1), opt(a, 2), e);
}

// post/put/patch: a string body goes as is, anything else as JSON.
Value do_body_verb(const char* method, std::vector<Value>& a, std::string& e) {
    const bool json = !a[1].is_str();
    const std::string body = json ? a[1].to_json_text() : a[1].as_str();
    return do_request(method, a[0].as_str(), &body, json, opt(a, 2), opt(a, 3), e);
}

Value fn_http_post(NativeCtx&, std::vector<Value>& a, std::string& e)  { return do_body_verb("post", a, e); }
Value fn_http_put(NativeCtx&, std::vector<Value>& a, std::string& e)   { return do_body_verb("put", a, e); }
Value fn_http_patch(NativeCtx&, std::vector<Value>& a, std::string& e) { return do_body_verb("patch", a, e); }

// RFC 3986 percent-encoding (a space is %20, not +), same as
// encoding.url_encode -- safe for a query string value.
Value fn_http_url_encode(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::str(lux::percent_encode(a[0].as_str()));
}

} // namespace

LUX_MODULE(http, {
    {"get",        "s|DD",  fn_http_get,    /*is_async=*/true},
    {"post",       "sx|DD", fn_http_post,   /*is_async=*/true},
    {"put",        "sx|DD", fn_http_put,    /*is_async=*/true},
    {"patch",      "sx|DD", fn_http_patch,  /*is_async=*/true},
    {"delete",     "s|DD",  fn_http_delete, /*is_async=*/true},
    {"url_encode", "s",     fn_http_url_encode},
})

} // namespace lux_script
