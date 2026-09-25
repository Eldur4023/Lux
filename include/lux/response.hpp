#pragma once
#include <string>
#include <unordered_map>
#include <sstream>
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

class Response {
    struct State {
        int         status_code = 200;
        std::string body;
        std::unordered_map<std::string, std::string> headers;
        // Set-Cookie is the one HTTP response header that legally appears
        // multiple times — keep them in a separate list so they survive the
        // map serialisation.
        std::vector<std::string> cookies;
        // sendfile path: when set, build() emits only headers; the connection
        // uses sendfile(2) to stream the file body directly to the socket.
        std::string     sendfile_path;
        std::uintmax_t  sendfile_size = 0;
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
        std::string lower = key;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower == "content-length" || lower == "transfer-encoding") {
            std::cerr << "[lux] Response.header(\"" << key << "\", ...) ignored -- "
                         "this header is controlled by the framework, not a handler\n";
            return *this;
        }

        state_->headers[std::move(key)] = std::move(value);
        return *this;
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
        // Internal callers (serve_file_from, try_serve_static) pass canonical
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
        state_->sendfile_path = path.string();
        state_->sendfile_size = known_size;
        state_->body_committed = true;
        header("Accept-Ranges", "bytes");
        return *this;
    }

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
    const std::unordered_map<std::string, std::string>&
                           headers_map()    const { return state_->headers; }
    const std::vector<std::string>&
                           cookies()        const { return state_->cookies; }
    const std::string&     sendfile_path()  const { return state_->sendfile_path; }
    std::uintmax_t         sendfile_size()  const { return state_->sendfile_size; }
    bool                   is_committed()   const { return state_->body_committed; }
    bool                   sse_started()    const { return state_->sse_started; }
    void                   mark_sse_started()    { state_->sse_started = true; }
    bool                   ws_started()     const { return state_->ws_started; }
    void                   mark_ws_started()     { state_->ws_started  = true; }
    std::string            content_type()   const {
        auto it = state_->headers.find("Content-Type");
        return (it != state_->headers.end()) ? it->second : "";
    }

    // Headers-only build for SSE: no Content-Length (streaming, length unknown).
    std::string build_sse_headers() const {
        std::ostringstream os;
        os << "HTTP/1.1 " << state_->status_code
           << ' ' << reason_phrase(state_->status_code) << "\r\n";
        emit_headers(os);
        for (const auto& c : state_->cookies)
            os << "Set-Cookie: " << c << "\r\n";
        os << "\r\n";
        return os.str();
    }

    std::string build() const {
        std::ostringstream os;
        os << "HTTP/1.1 " << state_->status_code
           << ' ' << reason_phrase(state_->status_code) << "\r\n";
        // Content-Length: use file size when sendfile is in play, or the
        // range's length instead when partial_content() set one -- a 206
        // sends fewer bytes than the file's own size, and Content-Length
        // has to say so, not the full file size.
        auto clen = state_->sendfile_path.empty()
                    ? state_->body.size()
                    : static_cast<std::size_t>(state_->sendfile_range_length
                                                ? state_->sendfile_range_length
                                                : state_->sendfile_size);
        os << "Content-Length: " << clen << "\r\n";
        emit_headers(os);
        for (const auto& c : state_->cookies)
            os << "Set-Cookie: " << c << "\r\n";
        os << "\r\n";
        // Body only for normal (non-sendfile) responses
        if (state_->sendfile_path.empty())
            os << state_->body;
        return os.str();
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
        state_->headers["Content-Type"] = "application/json; charset=utf-8";
        return *this;
    }

    // HTTP header field names are case-insensitive (RFC 7230 §3.2), but
    // state_->headers is keyed by whatever exact case a handler passed to
    // header() — needed so headers_map() still hands back what the caller
    // set (native_route_shadow.cpp's own test looks up "Location" by that
    // exact case). A handler that sets "x-frame-options" (all lowercase)
    // is, semantically, setting the SAME header as kDefaults'
    // "X-Frame-Options" below, but an exact-case map lookup does not know
    // that: both ended up on the wire as two separate, contradictory
    // header lines instead of the handler's value winning outright.
    static bool has_header_ci(
        const std::unordered_map<std::string, std::string>& headers,
        const char* name) {
        for (const auto& [k, v] : headers) {
            if (k.size() != std::strlen(name)) continue;
            bool eq = true;
            for (size_t i = 0; i < k.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(k[i])) !=
                    std::tolower(static_cast<unsigned char>(name[i]))) { eq = false; break; }
            }
            if (eq) return true;
        }
        return false;
    }

    // Every response the framework sends gets this baseline of hardening
    // headers, unless the handler already set one explicitly — a handler
    // that wants to frame its own content (res.header("X-Frame-Options",
    // "SAMEORIGIN")) or set its own Referrer-Policy always wins, this only
    // fills gaps left by handlers that set neither.  Content-Security-Policy
    // is deliberately NOT defaulted: it is inline-script/style dependent per
    // app, and a wrong default would silently break pages rather than
    // protect them.
    void emit_headers(std::ostringstream& os) const {
        static constexpr std::pair<const char*, const char*> kDefaults[] = {
            {"X-Content-Type-Options", "nosniff"},
            {"X-Frame-Options",        "DENY"},
            {"Referrer-Policy",        "strict-origin-when-cross-origin"},
        };
        for (const auto& [k, v] : state_->headers)
            os << k << ": " << v << "\r\n";
        for (const auto& [k, v] : kDefaults)
            if (!has_header_ci(state_->headers, k))
                os << k << ": " << v << "\r\n";
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
