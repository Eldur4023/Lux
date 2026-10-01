#pragma once
#include <string>
#include <unordered_map>
#include <string_view>
#include <lux/mime.hpp>
#include <fstream>
#include <filesystem>
#include <memory>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unistd.h>
#include <cerrno>
#include <iostream>
#include <utility>
#include <vector>
#include "cookies.hpp"

namespace lux {

// Headers added to every response (set once at startup from `app: headers:`).
inline std::vector<std::pair<std::string, std::string>>& global_headers() {
    static std::vector<std::pair<std::string, std::string>> h;
    return h;
}


class Response {
    struct State {
        int         status_code = 200;
        std::string body;
        // In the order they were set; a flat vector, since a response has a
        // handful and a hash map paid a node for each.
        std::vector<std::pair<std::string, std::string>> headers;
        // Set-Cookie is the one HTTP response header that legally appears
        // multiple times — keep them in a separate list so they survive the
        // map serialisation.
        std::vector<std::string> cookies;
        // sendfile path: when set, build() emits only headers; the connection
        // uses sendfile(2) to stream the file body directly to the socket.
        std::string     sendfile_path;
        std::string     error_message;   // status(code, "why"): what `error.message` shows
        std::uintmax_t  sendfile_size = 0;
        // Already open (the static mounts resolve it with openat2): the
        // connection sends this one instead of opening the path again.
        // Owned: closed here unless the connection takes it.
        int             sendfile_fd   = -1;
        State() = default;
        State(const State&) = delete;
        ~State() { if (sendfile_fd >= 0) ::close(sendfile_fd); }
        // 0 = no active range (serve the whole file). Set only by
        // partial_content(), once http_connection.cpp has already validated
        // a Range header against this exact file's size.
        std::uintmax_t  sendfile_range_length = 0;

        // SSE / WebSocket mode: headers already written directly to the socket;
        // finish_dispatch must not send a second response.
        bool sse_started    = false;
        bool ws_started     = false;
        // Set when any body-writing method is first called.
        // Lets handlers and middlewares check res.is_committed() before writing.
        bool body_committed = false;
        // The route returned this response itself -- a body with its own
        // status (`return {...}.status(409)`): `on error` handlers leave it.
        bool route_body     = false;
    };
    std::shared_ptr<State> state_;

public:
    Response() : state_(std::make_shared<State>()) {}

    // ── Builder methods ───────────────────────────────────────────────────────

    Response& status(int code) {
        state_->status_code = code;
        return *this;
    }

    // Takes away the already written body and returns it, leaving the response
    // free to write another one.
    //
    // It exists for the error handlers: when one fires, the default body is
    // already committed, so without this any res.json() or res.render() of the
    // handler would be silently discarded.  Returning it allows putting it
    // back if the handler decides to write nothing.
    //
    // It touches neither the status code nor the headers, and is a no-op on a
    // response in SSE or WebSocket mode, where bytes were already sent.
    std::string take_body() {
        if (state_->sse_started || state_->ws_started) return {};
        std::string old = std::move(state_->body);
        state_->body.clear();
        state_->sendfile_path.clear();
        state_->sendfile_size = 0;
        if (state_->sendfile_fd >= 0) { ::close(state_->sendfile_fd); state_->sendfile_fd = -1; }
        state_->sendfile_range_length = 0;
        state_->body_committed = false;
        return old;
    }

    // Puts back a body taken with take_body().
    Response& restore_body(std::string body) {
        if (state_->sse_started || state_->ws_started) return *this;
        state_->body = std::move(body);
        state_->body_committed = true;
        return *this;
    }

    Response& header(std::string key, std::string value) {
        // Strip CR/LF from both key and value to prevent HTTP response splitting.
        auto strip_crlf = [](std::string& s) {
            s.erase(std::remove_if(s.begin(), s.end(),
                [](char c){ return c == '\r' || c == '\n'; }), s.end());
        };
        strip_crlf(key);
        strip_crlf(value);

        // Content-Length is computed by build()/build_sse_headers() from the
        // actual body/file size, and this framework never emits
        // Transfer-Encoding (no chunked support) -- so both are always
        // framing information, never content a handler legitimately states.
        // Without this, `res.header("Content-Length", "0")` or
        // `res.header("Transfer-Encoding", "chunked")` would put a second,
        // handler-controlled value for one of these next to (or, for
        // Content-Length, silently replacing) the framework's own, and a
        // proxy in front of Lux that resolves the resulting ambiguity
        // differently than the framework's own client does is exactly the
        // setup a request-smuggling attack needs.
        if (iequals(key, "content-length") || iequals(key, "transfer-encoding")) {
            std::cerr << "[lux] Response.header(\"" << key << "\", ...) ignored -- "
                         "this header is controlled by the framework, not a handler\n";
            return *this;
        }

        set_header(std::move(key), std::move(value));
        return *this;
    }

    // The value set under exactly `key`, or null.
    const std::string* header_value(std::string_view key) const {
        for (const auto& [k, v] : state_->headers) if (k == key) return &v;
        return nullptr;
    }

    // Set a cookie.  Multiple cookies can be set on one response — each emits
    // its own Set-Cookie header (the map storage in headers_ only allows one).
    //
    //   res.cookie("session", token, {.secure = true, .same_site = SameSite::Strict});
    //
    Response& cookie(std::string name, std::string value, CookieOptions opts = {}) {
        state_->cookies.push_back(
            build_set_cookie(std::move(name), std::move(value), std::move(opts)));
        return *this;
    }

    // Convenience: delete a cookie by name (sets Max-Age=0).
    Response& clear_cookie(std::string name, CookieOptions opts = {}) {
        opts.max_age = 0;
        state_->cookies.push_back(
            build_set_cookie(std::move(name), "", std::move(opts)));
        return *this;
    }

    Response& text(std::string body) {
        return commit("text", std::move(body), "text/plain; charset=utf-8");
    }

    // The argument is ALWAYS the response body, never a filename.
    //
    // This used to sniff the content: a string ending in ".html"/".htm" with
    // no '<' in it was taken for a template name and read off disk from
    // templates_dir instead of being sent.  That made the behaviour depend on
    // the *value*, so a handler doing html(query("c")) — content the client
    // controls — flipped between "send this string" and "read this file"
    // based on what the client sent, and `?c=page.html` came back with the
    // raw source of a template rather than the text.  Nothing in the language
    // or the C++ API documented that branch, and no caller used it: templates
    // go through render() (resolved to a compile-time index, so a name can
    // never come from a request) and files through send_file().
    Response& html(std::string content) {
        return commit("html", std::move(content), "text/html; charset=utf-8");
    }

    // Already serialized JSON body.  This used to take an nlohmann tree and
    // call dump(); now whoever has the data writes it directly, which is one
    // materialization less.
    Response& json_text(std::string body) {
        return commit("json_text", std::move(body), "application/json; charset=utf-8");
    }

    Response& send(std::string body) { return commit("send", std::move(body), nullptr); }

    // Zero-copy static file: instead of reading the file into the body,
    // record the path and let the connection layer use sendfile(2).
    // Content-Type should be set by the caller before calling send_file().
    //
    // WARNING: if `path` is derived from user input, use serve_file_from()
    // instead — it enforces a root directory and resolves symlinks safely.
    Response& send_file(const std::filesystem::path& path) {
        // Reject paths with ".." components to prevent directory traversal.
        // Internal callers (serve_file_from, serve_from_mount) pass canonical
        // paths so this check is a no-op for them.
        for (const auto& comp : path) {
            if (comp == "..") return fail(403, "Forbidden");
        }
        std::error_code ec;
        auto sz = std::filesystem::file_size(path, ec);
        if (ec) { state_->status_code = 500; state_->body = "Cannot stat file"; return *this; }
        return send_file(path, sz);
    }

    // Overload for callers that already have the file size — skips the extra stat(2).
    // Advertises Range support up front, on the FIRST (non-ranged) response
    // too -- a client has to see this before it knows it may ask for a range.
    Response& send_file(const std::filesystem::path& path, std::uintmax_t known_size) {
        // Without a Content-Type, `nosniff` (sent by default) leaves the
        // browser nothing to go on: an image downloads instead of showing.
        if (!has_header_ci(state_->headers, "Content-Type"))
            header("Content-Type", mime_for_ext(path.extension().string()));
        state_->sendfile_path = path.string();
        state_->sendfile_size = known_size;
        state_->body_committed = true;
        header("Accept-Ranges", "bytes");
        return *this;
    }

    // send_file() for a file the caller already has open: `fd` is taken over.
    // The caller has set Content-Type.
    Response& send_file_fd(int fd, std::string path, std::uintmax_t size) {
        state_->sendfile_path  = std::move(path);
        state_->sendfile_size  = size;
        state_->body_committed = true;
        header("Accept-Ranges", "bytes");
        if (state_->sendfile_fd >= 0) ::close(state_->sendfile_fd);
        state_->sendfile_fd = fd;
        return *this;
    }
    int take_sendfile_fd() { return std::exchange(state_->sendfile_fd, -1); }

    // Marks this sendfile response as a 206 for a range ALREADY VALIDATED
    // by the caller (http_connection.cpp's finish_dispatch() is the only
    // place that ever sees the request's Range header, so it is the only
    // caller). This only touches status/headers -- it does not move a
    // single byte itself; sendfile(2) still does the actual streaming,
    // using the offset/remaining finish_dispatch() already set on the
    // connection before calling this.
    Response& partial_content(std::uintmax_t range_start, std::uintmax_t range_end) {
        state_->status_code = 206;
        state_->sendfile_range_length = range_end - range_start + 1;
        header("Content-Range", "bytes " + std::to_string(range_start) + "-"
                                + std::to_string(range_end) + "/"
                                + std::to_string(state_->sendfile_size));
        return *this;
    }

    // Safe file serving with path-traversal and symlink protection.
    // Fully resolves root/user_path (following all symlinks) and rejects
    // anything that resolves outside root.  Use this instead of send_file()
    // when user_path comes from request input.
    //
    //   res.serve_file_from("./uploads", req.param("name").value_or(""));
    //
    Response& serve_file_from(const std::filesystem::path& root,
                               const std::filesystem::path& user_path) {
        namespace fs = std::filesystem;
        std::error_code ec;
        // canonical() resolves ALL symlinks (unlike weakly_canonical), which
        // prevents symlink-swap attacks where a symlink inside root points to
        // a file outside root.
        auto canonical_root = fs::canonical(root, ec);
        if (ec) return fail(500, "Internal Server Error");
        auto canonical_file = fs::canonical(canonical_root / user_path, ec);
        if (ec) return fail(404, "Not Found");
        // 4-iterator mismatch: a file resolving ABOVE root has fewer
        // components than root, and the 3-iterator form would read past its end.
        auto [ri, fi] = std::mismatch(canonical_root.begin(), canonical_root.end(),
                                       canonical_file.begin(), canonical_file.end());
        if (ri != canonical_root.end()) return fail(403, "Forbidden");
        return send_file(canonical_file);
    }

    // ── Framework-internal ───────────────────────────────────────────────────


    int                    status_code()    const { return state_->status_code; }
    const std::string&     body()           const { return state_->body; }
    const std::vector<std::string>&
                           cookies()        const { return state_->cookies; }
    const std::string&     sendfile_path()  const { return state_->sendfile_path; }
    std::uintmax_t         sendfile_size()  const { return state_->sendfile_size; }
    bool                   is_committed()   const { return state_->body_committed; }
    bool                   route_body()     const { return state_->route_body; }
    const std::string&     error_message()  const { return state_->error_message; }
    Response&              set_error_message(std::string m) { state_->error_message = std::move(m); return *this; }
    Response&              mark_route_body()      { state_->route_body = true; return *this; }
    bool                   sse_started()    const { return state_->sse_started; }
    void                   mark_sse_started()    { state_->sse_started = true; }
    bool                   ws_started()     const { return state_->ws_started; }
    void                   mark_ws_started()     { state_->ws_started  = true; }
    std::string            content_type()   const {
        const std::string* v = header_value("Content-Type");
        return v ? *v : "";
    }

    // Headers-only build for SSE: no Content-Length (streaming, length unknown).
    std::string build_sse_headers() const {
        std::string out;
        append_status_line(out);
        emit_headers(out);
        out += "\r\n";
        return out;
    }

    std::string build() const {
        std::string out = build_head();
        if (state_->sendfile_path.empty()) { out.reserve(out.size() + state_->body.size()); out += state_->body; }
        return out;
    }

    // build() without the body, which the connection sends from where it is.
    std::string build_head() const {
        // Content-Length: use file size when sendfile is in play, or the
        // range's length instead when partial_content() set one -- a 206
        // sends fewer bytes than the file's own size, and Content-Length
        // has to say so, not the full file size.
        const bool inline_body = state_->sendfile_path.empty();
        auto clen = inline_body
                    ? state_->body.size()
                    : static_cast<std::size_t>(state_->sendfile_range_length
                                                ? state_->sendfile_range_length
                                                : state_->sendfile_size);
        std::string out;
        out.reserve(256);
        append_status_line(out);
        out += "Content-Length: ";
        out += std::to_string(clen);
        out += "\r\n";
        emit_headers(out);
        out += "\r\n";
        return out;
    }

private:
    // Sets the body once; a second body write is logged and ignored.
    Response& commit(const char* who, std::string body, const char* content_type) {
        if (state_->body_committed) {
            std::cerr << "[lux] Response." << who
                      << "() called after body already committed — ignoring\n";
            return *this;
        }
        if (content_type) header("Content-Type", content_type);
        state_->body = std::move(body);
        state_->body_committed = true;
        return *this;
    }

    Response& fail(int code, const char* error) {
        state_->status_code = code;
        state_->body = std::string(R"({"error":")") + error + "\"}";
        set_header("Content-Type", "application/json; charset=utf-8");
        return *this;
    }

    // HTTP header field names are case-insensitive (RFC 7230 §3.2), but
    // state_->headers is keyed by whatever exact case a handler passed to
    // header() — needed so header_value() still hands back what the caller
    // set (native_route_shadow.cpp's own test looks up "Location" by that
    // exact case). A handler that sets "x-frame-options" (all lowercase)
    // is, semantically, setting the SAME header as kDefaults'
    // "X-Frame-Options" below, but an exact-case map lookup does not know
    // that: both ended up on the wire as two separate, contradictory
    // header lines instead of the handler's value winning outright.
    static bool iequals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }
    static bool has_header_ci(
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::string_view name) {
        for (const auto& [k, v] : headers) if (iequals(k, name)) return true;
        return false;
    }
    // Replaces the value under exactly `key` (the map it used to be did the same).
    void set_header(std::string key, std::string value) {
        for (auto& [k, v] : state_->headers)
            if (k == key) { v = std::move(value); return; }
        if (state_->headers.empty()) state_->headers.reserve(8);
        state_->headers.emplace_back(std::move(key), std::move(value));
    }

    // Every response the framework sends gets this baseline of hardening
    // headers, unless the handler already set one explicitly — a handler
    // that wants to frame its own content (res.header("X-Frame-Options",
    // "SAMEORIGIN")) or set its own Referrer-Policy always wins, this only
    // fills gaps left by handlers that set neither.  Content-Security-Policy
    // is deliberately NOT defaulted: it is inline-script/style dependent per
    // app, and a wrong default would silently break pages rather than
    // protect them.
    void append_status_line(std::string& out) const {
        out += "HTTP/1.1 ";
        out += std::to_string(state_->status_code);
        out += ' ';
        out += reason_phrase(state_->status_code);
        out += "\r\n";
    }

    // Headers plus Set-Cookie lines.
    void emit_headers(std::string& out) const {
        static constexpr std::pair<const char*, const char*> kDefaults[] = {
            {"X-Content-Type-Options", "nosniff"},
            {"X-Frame-Options",        "DENY"},
            {"Referrer-Policy",        "strict-origin-when-cross-origin"},
        };
        auto line = [&out](std::string_view k, std::string_view v) {
            out += k; out += ": "; out += v; out += "\r\n";
        };
        for (const auto& [k, v] : state_->headers) line(k, v);
        // app: headers: -- on every response, under what the handler set.
        const auto& configured = global_headers();
        for (const auto& [k, v] : configured)
            if (!has_header_ci(state_->headers, k)) line(k, v);
        for (const auto& [k, v] : kDefaults)
            if (!has_header_ci(state_->headers, k) && !has_header_ci(configured, k)) line(k, v);
        for (const auto& c : state_->cookies) line("Set-Cookie", c);
    }

    static const char* reason_phrase(int code) noexcept {
        switch (code) {
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
            case 204: return "No Content";
            case 206: return "Partial Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 303: return "See Other";
            case 304: return "Not Modified";
            case 307: return "Temporary Redirect";
            case 308: return "Permanent Redirect";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 408: return "Request Timeout";
            case 409: return "Conflict";
            case 410: return "Gone";
            case 413: return "Content Too Large";
            case 415: return "Unsupported Media Type";
            case 416: return "Range Not Satisfiable";
            case 422: return "Unprocessable Entity";
            case 429: return "Too Many Requests";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            case 504: return "Gateway Timeout";
            default:  return "Unknown";
        }
    }
};

} // namespace lux
