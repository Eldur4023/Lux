#pragma once
#include <string>
#include <unordered_map>
#include <optional>
#include <memory>
#include <algorithm>
#include <cstdlib>
#include <lux/core/event_loop.hpp>
#include "cancel.hpp"
#include "cookies.hpp"
#include "percent_encoding.hpp"

namespace lux {

class Request {
public:
    std::string method;
    std::string path;
    std::string version;
    std::string body;
    std::string remote_ip;  // IPv4/IPv6 of the connected peer

    // Headers stored with lowercase keys
    std::unordered_map<std::string, std::string> headers;

    // Path params extracted by the router (e.g. /users/:id → params["id"])
    std::unordered_map<std::string, std::string> params;

    // Query string params (e.g. ?page=1&limit=20 → query["page"] = "1")
    std::unordered_map<std::string, std::string> query;

    // Pointer to the event loop for scheduling tasks
    core::EventLoop* loop = nullptr;

    // Single write on the connection socket.  Returns bytes written, or -1
    // with errno set (EAGAIN when the kernel buffer is full, EBADF after
    // close).  Set by HttpConnection::dispatch().  Used by SSE, the WebSocket
    // handshake, and WS frame writers — these paths bypass the normal response
    // pipeline, so they write through here rather than the buffered path.
    std::function<ssize_t(const char*, size_t)> _raw_write;

    // WebSocket mode: called by HttpConnection::do_read() with the bytes it
    // just decrypted/read from the socket.  Set by the ws() upgrade wrapper to
    // forward to WSState::feed().  Replacing the older _ws_on_readable (which
    // did its own ::read) so reads work over TLS too.
    std::function<void(const char*, size_t)> _ws_on_data;

    // WebSocket mode: queues one already-built frame for the socket, backed
    // by an EPOLLOUT-driven buffer instead of a single best-effort
    // ::write(). Set by HttpConnection::dispatch(); used by WSState::send_fn
    // instead of writing straight through _raw_write, so a frame that would
    // otherwise be torn mid-payload by backpressure gets queued and finished
    // in order rather than corrupting the stream.
    std::function<void(std::string)> _ws_queue_write;

    // Cancels HttpConnection's per-request timeout (kRequestTimeoutMs).  Set
    // by HttpConnection::dispatch(); called once by make_sse() (sse.hpp) and
    // by the ws() upgrade wrapper (app.hpp), the instant each writes its own
    // headers and the response stops being a bounded "reply by time X" and
    // becomes an open-ended stream instead. Without this, a healthy SSE/WS
    // connection gets a 408 dropped into the middle of its stream 30s after
    // it opened, timing or connection-count notwithstanding.
    std::function<void()> _cancel_request_timeout;

    // Forces the underlying connection closed. Used by SSEWriter when a
    // write partway through a frame fails to complete: from that point the
    // byte stream is desynced (the client has an incomplete frame with no
    // length prefix to tell it where the next one starts), so the
    // connection has to end rather than keep accepting more sse.send()
    // calls onto an already-corrupted stream.
    std::function<void()> _force_close;

    // Cancellation token — shared with the HttpConnection.
    // Cancelled when the connection closes (timeout, disconnect, write error).
    // Check in long-running handlers to exit early.
    std::shared_ptr<CancellationToken> cancel_token;

    // Convenience: true if the underlying connection has been closed.
    bool is_cancelled() const noexcept {
        return cancel_token && cancel_token->is_cancelled();
    }

    // Convenience: get a header by name (case-insensitive)
    std::optional<std::string> header(std::string name) const {
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        auto it = headers.find(name);
        if (it == headers.end()) return std::nullopt;
        return it->second;
    }

    // Convenience: get a query param with a default
    std::string query_param(const std::string& key, const std::string& def = "") const {
        auto it = query.find(key);
        return (it != query.end()) ? it->second : def;
    }

    // Convenience: get a cookie value by name. Parses the Cookie header lazily;
    // the result is cached on the first call so repeated lookups are O(1).
    std::optional<std::string> cookie(const std::string& name) const {
        if (!cookies_parsed_) {
            cookies_parsed_ = true;
            auto h = header("cookie");
            if (h) cookies_cache_ = parse_cookie_header(*h);
        }
        auto it = cookies_cache_.find(name);
        if (it == cookies_cache_.end()) return std::nullopt;
        return it->second;
    }

    // Parse an application/x-www-form-urlencoded body.
    // Returns an empty map if Content-Type doesn't match or body is empty.
    std::unordered_map<std::string, std::string> form() const {
        auto ct = header("content-type");
        if (!ct || ct->find("application/x-www-form-urlencoded") == std::string::npos)
            return {};
        std::unordered_map<std::string, std::string> out;
        parse_form_encoded(body, out);
        return out;
    }

    // Mutable cache for parsed Cookie header — only populated on first cookie().
    mutable std::unordered_map<std::string, std::string> cookies_cache_;
    mutable bool                                          cookies_parsed_ = false;
};

} // namespace lux
