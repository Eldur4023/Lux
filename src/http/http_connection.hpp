#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <sys/epoll.h>
#include <chrono>
#include <cstdint>
#include "http_parser.hpp"
#include <lux/core/event_loop.hpp>
#include "../../include/lux/types.hpp"
#include "../../include/lux/cancel.hpp"

struct ssl_st;   // OpenSSL's SSL and BIO, only used with LUX_TLS
struct bio_st;

namespace lux::http {

class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    HttpConnection(int fd, core::EventLoop& loop, lux::DispatchFn dispatch,
                   std::shared_ptr<std::atomic<int>> conn_count = nullptr,
                   std::shared_ptr<std::atomic<int>> loop_count = nullptr);
    ~HttpConnection();

    void start();
    void on_event(uint32_t events);

private:
    int                fd_;
    core::EventLoop&   loop_;
    lux::DispatchFn dispatch_;
    std::shared_ptr<std::atomic<int>>          conn_count_;   // decremented on close()
    std::shared_ptr<std::atomic<int>>          loop_count_;   // this loop's share, too
    std::shared_ptr<lux::CancellationToken> cancel_token_; // one per request
    HttpParser         parser_;
    bool               closed_         = false;
    // What epoll is watching: re-arming the same thing after every response
    // was an epoll_ctl per request for nothing.
    // HTTPS (LUX_TLS, lux::tls::init): every socket I/O goes through io_read()/
    // io_write(), which behave like read(2)/write(2) -- a TLS "want read/write"
    // is EAGAIN -- so the callers do not care which one they have. null = plain.
    ssl_st*            ssl_            = nullptr;
    bio_st*            tls_in_         = nullptr;  // ciphertext read off the socket, for OpenSSL (owned by ssl_)
    bool               tls_want_out_   = false;  // the handshake needs EPOLLOUT, not a response
    bool               tls_drained_    = false;  // the last socket read was short: empty until the next edge
    bool    corked_         = false;  // TCP_CORK while a TLS file response goes out
    void    cork(bool on);
    ssize_t io_read(void* buf, size_t n);
    ssize_t io_write(const void* buf, size_t n);
    ssize_t tls_ret(int r, bool reading);
    bool    drain();   // writes write_buf_ out: true = all sent, false = blocked or closed
    void    free_tls();
    uint32_t           armed_          = EPOLLIN;   // as tcp_server adds it
    void arm(uint32_t events) {
        if (events == armed_) return;
        armed_ = events;
        loop_.modify(fd_, events | EPOLLET);
    }

    // Weak reference to the current request — used in WebSocket mode to route
    // do_read() bytes into the WS frame parser instead of the HTTP parser.
    std::weak_ptr<lux::Request> current_req_;

    // ── WebSocket send queue limit ────────────────────────────────────────────
    // Frames queued for a peer that is not draining them: past this the
    // connection is closed instead of growing without bound.
    static constexpr size_t kMaxResponseBytes = 16 * 1024 * 1024; // 16 MB

    // ── Write buffer ─────────────────────────────────────────────────────────
    // Non-blocking writes: if send buffer is full (EAGAIN), data is queued here
    // and flushed when EPOLLOUT fires.  Using an offset avoids O(n) erases.
    std::string write_buf_;
    size_t      write_offset_ = 0;
    bool        keep_alive_   = false;  // stored here so on_write_complete can act

    // ── Pipelining serialisation ─────────────────────────────────────────────
    // While a request's response is being produced and written, no second
    // dispatch may run on the same connection (write_buf_ is shared, timers
    // are per-connection).  The parser is paused on each message_complete;
    // any trailing bytes that arrived in the same TCP segment are saved here
    // and replayed in on_write_complete once the previous response is out.
    // pending_buf_ is capped to bound memory under a buggy/abusive pipeliner.
    std::string                pending_buf_;
    bool                       in_flight_ = false;
    static constexpr size_t    kMaxPendingBuf = 64 * 1024;  // 64 KB pipelined

    // A synchronous handler replies INSIDE parser_.feed(): llhttp llama a
    // on_message_complete, which dispatches, and the response is written in
    // full before the callback returns HPE_PAUSED.  If the cycle close ran
    // there, it would resume() a pause that does not exist yet and the
    // connection would stay paused forever.  So it is deferred until feed()
    // returns and the pause is in place.
    bool in_parser_     = false;
    bool cycle_pending_ = false;

    // ── Timeouts ──────────────────────────────────────────────────────────────
    // kIdleTimeoutMs: a connection with no request in progress (just
    //   accepted, or kept alive after a response) is closed after this
    //   long, silently, as nginx does (keepalive_timeout, 75s). It was the
    //   header timeout, 5s, with a 408: a user who read a page for more
    //   than 5s sent the next click into a closing socket and got that 408
    //   as its answer (measured: every page kind failing at 50k users).
    // kHeaderTimeoutMs: from a request's first byte (a partial read left it
    //   unfinished); fires 408 if complete headers are not received within
    //   this window (Slowloris defence).
    //   Cancelled in on_headers_complete() -- the llhttp callback that fires
    //   the instant headers finish parsing, NOT in dispatch(), which only
    //   runs once the BODY has fully arrived too. Cancelling in dispatch()
    //   charged a slow request BODY against the header timeout's much
    //   shorter budget: a client sending headers instantly but a large body
    //   slowly got a bogus "Request Header Timeout" the moment kHeaderTimeoutMs
    //   elapsed, even though the headers had long since arrived.
    // kRequestTimeoutMs: an INACTIVITY timeout, not a fixed total-duration
    //   budget. Armed in on_headers_complete(), right after the header timer
    //   is cancelled, and re-armed (refresh_request_timeout()) on every
    //   read()/write()/sendfile() call that makes forward progress -- so it
    //   only fires 408 after kRequestTimeoutMs with NO progress at all, not
    //   merely because reading the body, running the handler and writing
    //   the response together happened to take longer than that. A fixed
    //   budget starting at headers-complete cannot tell a stalled
    //   connection from a large upload/download that is still moving, just
    //   slowly (a rate-limited client, a big send_file()) -- it 408s both
    //   alike. Refreshing on progress fixes that without giving up the
    //   protection: a connection that goes fully quiet (no bytes either
    //   way) for the whole window still gets cut.
    //   Cancelled in on_write_complete() for a normal request/response, or
    //   early via cancel_request_timeout() the instant a response turns
    //   into an open-ended stream (SSE headers written, or the WS upgrade
    //   completes) -- neither has a "finishes eventually" shape at all, so
    //   the request timeout does not apply to them past that point; leaving
    //   it armed would 408 a healthy SSE/WS stream mid-flight regardless of
    //   how much data was flowing.
    //
    // They never run at once, so they share one deadline and one timer:
    // arming moves the deadline, and the timer, firing before it, sleeps
    // again. Only a deadline moved EARLIER reschedules it (idle -> header,
    // on a partial request: rare): scheduling and cancelling a timer per
    // request cost an allocation and two map updates each, four times over.
    // A request that arrives whole goes idle -> request, later: no reschedule.
    static constexpr int kIdleTimeoutMs    = 60'000;
    static constexpr const char* kIdleMsg  = "idle";   // closes, answers nothing
    static constexpr int kHeaderTimeoutMs  = 5'000;
    static constexpr int kRequestTimeoutMs = 30'000;
    static constexpr const char* kHeaderTimeoutMsg  = "Request Header Timeout";
    static constexpr const char* kRequestTimeoutMsg = "Request Timeout";
    std::chrono::steady_clock::time_point deadline_{};
    const char* deadline_msg_ = nullptr;   // null: no deadline
    int         timer_        = -1;
    std::chrono::steady_clock::time_point timer_at_{};   // when timer_ fires

    // Peer address, resolved on the first request: it cannot change for the
    // life of the socket, and keep-alive would otherwise pay getpeername +
    // inet_ntop on every request.
    std::string peer_ip_;
    bool        peer_ip_done_ = false;

    // ── sendfile state ────────────────────────────────────────────────────────
    // When serving static files, we skip the read-into-buffer step and stream
    // directly from the file descriptor to the socket.  The connection sends
    // the HTTP headers via the normal write_buf_ path, then transitions to
    // do_sendfile() once the headers are fully flushed.
    int    file_fd_        = -1;
    off_t  file_offset_    = 0;
    size_t file_remaining_ = 0;

    // ── WebSocket outbound queue ────────────────────────────────────────────
    // Once the handshake completes, individual frames are written straight to
    // the socket (queue_ws_write()) instead of going through write_buf_ / the
    // request-response cycle — a WS session has no "one response" shape and
    // is full-duplex, so on_write_complete()'s keep-alive/timeout bookkeeping
    // does not apply to it.
    //
    // A write that would block is buffered here and drained by do_ws_write()
    // on EPOLLOUT, exactly like write_buf_ is for HTTP responses — a frame
    // either reaches the wire whole or the connection closes, it is never
    // torn in the middle.  Capped so a peer that stops reading (deliberately
    // or not) cannot grow this without bound; the cap sits at kMaxResponseBytes
    // so one legitimate large ws.send() still fits without fragmenting.
    std::string ws_write_buf_;
    size_t      ws_write_offset_ = 0;

    void do_read();
    void do_write();
    void do_ws_write();
    void queue_ws_write(std::string frame);
    void do_sendfile();
    void on_write_complete();
    void on_headers_complete();
    void refresh_request_timeout();
    void cancel_request_timeout();
    // 408 `msg` and close once `ms` pass without being moved or cleared.
    void set_deadline(int ms, const char* msg);
    void arm_between_requests();   // idle, or header if a request has begun
    void schedule_deadline(int ms);
    void on_deadline();

    // Feeds bytes to the HTTP parser and stashes any unconsumed tail (the
    // next pipelined request, or a WS client's first frame). False = closed.
    bool feed_parser(const char* data, size_t n);

    // Response cycle close: resumes the parser, replays anything that arrived
    // by pipelining, rearms the header timer and EPOLLIN.
    void finish_cycle();

    // Begin writing `data`; buffers any unsent remainder and arms EPOLLOUT.
    void send_response(std::string data, const std::string* body = nullptr);
    void send_error(int code, const char* msg);
    // Sends `r` with "Connection: close" and drops keep-alive.
    void send_and_close(lux::Response& r);
    void close();

    void dispatch(ParsedRequest req);
    static void bind_stream(lux::Request& req);
    void finish_dispatch(lux::Request& request, lux::Response& response);
};

} // namespace lux::http
