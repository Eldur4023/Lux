#pragma once
#include <string>
#include <string_view>
#include <memory>
#include <unistd.h>
#include <cerrno>
#include "cancel.hpp"
#include "request.hpp"
#include "response.hpp"

namespace lux {

// ─── SSEWriter ────────────────────────────────────────────────────────────────
//
// Keeps an HTTP connection open and pushes text/event-stream events to the
// client.  Obtained via res.sse(req) — see Response::sse().
//
// Lifetime: headers are sent immediately by sse(); the writer remains valid
// until the client disconnects or the handler returns.  When the client
// disconnects, epoll fires EPOLLHUP → HttpConnection::close() → the shared
// CancellationToken is cancelled → is_open() returns false → the handler
// loop should exit naturally.
//
// Usage:
//   app.get("/events", [](Request& req, Response& res) -> Task<void> {
//       auto sse = res.sse(req);
//       int n = 0;
//       while (sse.is_open()) {
//           sse.send(std::to_string(n++));
//           co_await lux::sleep(1000);
//       }
//   });
//
class SSEWriter {
public:
    // Writes through req._raw_write, bypassing the buffered response pipeline
    // so each event goes out as soon as it is produced. force_close lets a
    // partial write (see raw_write()'s comment) actually tear the
    // connection down instead of merely reporting failure to a caller that,
    // per GUIDE.md's own example, is never shown checking send()'s return
    // value.
    SSEWriter(std::function<ssize_t(const char*, size_t)> writer,
              std::shared_ptr<CancellationToken> token,
              std::function<void()> force_close = nullptr)
        : writer_(std::move(writer)), token_(std::move(token))
        , force_close_(std::move(force_close)) {}

    SSEWriter(const SSEWriter&)            = delete;
    SSEWriter& operator=(const SSEWriter&) = delete;
    SSEWriter(SSEWriter&& o) noexcept
        : writer_(std::move(o.writer_)), token_(std::move(o.token_))
        , force_close_(std::move(o.force_close_)), desynced_(o.desynced_)
    {}
    SSEWriter& operator=(SSEWriter&&) = delete;

    // Send a "data: <text>\n\n" event.
    // Returns false if the connection is gone.
    bool send(std::string_view data) {
        return write_frame("", data, "");
    }

    // Send a named event: "event: <type>\ndata: <text>\n\n"
    // Optionally attach a last-event ID for browser auto-reconnect.
    bool send_event(std::string_view event_type, std::string_view data,
                    std::string_view id = "") {
        return write_frame(event_type, data, id);
    }

    // Send a comment (": <text>\n\n") — browsers ignore it; useful as a keepalive.
    bool ping(std::string_view comment = "") {
        std::string frame = ": ";
        frame += comment;
        frame += "\n\n";
        return raw_write(frame);
    }

    // True while the underlying connection is alive. Also false once a
    // partial write has left the byte stream desynced (see raw_write()) —
    // from the app's perspective this is indistinguishable from the
    // connection being gone, since nothing more can be safely sent on it.
    bool is_open() const { return token_ && !token_->is_cancelled() && !desynced_; }

private:
    std::function<ssize_t(const char*, size_t)> writer_;
    std::shared_ptr<CancellationToken>          token_;
    std::function<void()>                       force_close_;
    bool                                         desynced_ = false;

    // Strip CR/LF from a string_view to prevent SSE field injection.
    static std::string sanitize_field(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) if (c != '\r' && c != '\n') out += c;
        return out;
    }

    bool write_frame(std::string_view event, std::string_view data, std::string_view id) {
        if (!is_open()) return false;
        std::string frame;
        frame.reserve(data.size() + 48);
        if (!id.empty())    { frame += "id: ";    frame += sanitize_field(id);    frame += "\n"; }
        if (!event.empty()) { frame += "event: "; frame += sanitize_field(event); frame += "\n"; }
        // RFC 8895 §3.2: each line of multiline data needs its own "data:"
        // prefix. The EventSource line-parsing algorithm (WHATWG HTML
        // §9.2.6) treats CR, LF, AND CRLF each as one line-ending token --
        // splitting on '\n' alone left a bare '\r' inside `data` untouched,
        // and the browser's own parser still read it as a line break. That
        // let a caller who only sanitized against '\n' (or a value with an
        // embedded '\r' from anywhere upstream) inject a fake "event:" or a
        // second "data:" field into what this code emitted as a single
        // line: `send("hello\revent: admin\rdata: forged")` produced one
        // "data: hello" line to this function, but three separate SSE
        // fields to the browser. Scanning for whichever of \r, \n or \r\n
        // comes first — and consuming both bytes of a \r\n pair as ONE
        // boundary — matches that parser exactly, so every line break this
        // function does not itself insert ends up inside a "data: " line,
        // never at the start of a new field.
        std::string_view rem = data;
        while (true) {
            size_t cut = rem.size(), skip = 0;
            for (size_t i = 0; i < rem.size(); ++i) {
                if (rem[i] == '\n') { cut = i; skip = 1; break; }
                if (rem[i] == '\r') {
                    cut  = i;
                    skip = (i + 1 < rem.size() && rem[i + 1] == '\n') ? 2 : 1;
                    break;
                }
            }
            frame += "data: ";
            frame += rem.substr(0, cut);
            frame += "\n";
            if (cut == rem.size()) break;
            rem = rem.substr(cut + skip);
        }
        frame += "\n";  // blank line ends the event
        return raw_write(frame);
    }

    // Best-effort write.  EAGAIN with NOTHING sent yet → drop this event
    // (lossy but non-fatal: the stream is still byte-aligned, the client
    // will receive the next one normally).  Anything else that stops the
    // write partway through — EAGAIN after some bytes already went out, a
    // real errno, or a 0-byte write — is fatal: the client is left with an
    // incomplete frame and no way to tell where it ends, so the write
    // AFTER this one would land right on its tail and get parsed as part
    // of it. Previously this just returned false and left the actual
    // teardown to whenever epoll next noticed the socket was gone — which,
    // for a write path outside the normal EPOLLOUT-driven buffer (this one
    // bypasses write_buf_ entirely, see the class comment), could be a long
    // time, during which every subsequent sse.send() kept writing more
    // bytes onto an already-desynced stream instead of the corrupted
    // connection being closed. force_close_ (wired to the real
    // HttpConnection::close() via Request::_force_close) ends it outright
    // the moment this happens, and desynced_ makes is_open() reflect that
    // immediately rather than waiting for the token to catch up.
    bool raw_write(const std::string& frame) {
        if (!is_open()) return false;
        if (!writer_) return false;

        size_t written = 0;
        while (written < frame.size()) {
            ssize_t n = writer_(frame.data() + written, frame.size() - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                if ((errno == EAGAIN || errno == EWOULDBLOCK) && written == 0)
                    return false;  // clean drop: buffer full, stream still intact
                break;             // partial write or real error: fatal, see above
            }
            if (n == 0) break;
            written += static_cast<size_t>(n);
        }

        if (written == frame.size()) return true;

        desynced_ = true;
        if (force_close_) force_close_();
        return false;
    }
};

// ─── res.sse(req) — convenience free function ─────────────────────────────────
//
// Writes SSE headers to the socket immediately (bypassing the normal response
// pipeline) and marks the Response so finish_dispatch knows not to send again.
//
// Must be the first I/O operation on the response for this request.

inline SSEWriter make_sse(Response& res, const Request& req) {
    // Set SSE-specific headers (user may override Content-Type before calling)
    res.status(200)
       .header("Content-Type",     "text/event-stream")
       .header("Cache-Control",    "no-cache")
       .header("X-Accel-Buffering","no");  // disable nginx / proxy buffering

    res.header("Connection", "keep-alive");

    // Write the headers straight to the socket, bypassing write_buf_, so they
    // go out before the first event is produced.
    std::string headers = res.build_sse_headers();
    if (req._raw_write) {
        size_t written = 0;
        while (written < headers.size()) {
            ssize_t n = req._raw_write(headers.data() + written,
                                       headers.size() - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;  // connection gone; SSEWriter::is_open() → false
            }
            if (n == 0) break;
            written += static_cast<size_t>(n);
        }
    }

    // Tell finish_dispatch that headers are already out
    res.mark_sse_started();

    // From here on this is an open-ended stream, not a bounded
    // request/response: the connection's kRequestTimeoutMs budget (meant to
    // catch a handler or a slow client stalling a NORMAL reply) does not
    // apply to it, and leaving it armed would 408 a healthy stream out from
    // under itself 30s after it opened, mid-event, with no way for the
    // handler to see it coming.
    if (req._cancel_request_timeout) req._cancel_request_timeout();

    return SSEWriter(req._raw_write, req.cancel_token, req._force_close);
}

} // namespace lux
