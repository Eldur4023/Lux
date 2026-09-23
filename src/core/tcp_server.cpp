#include "tcp_server.hpp"
#include "../http/http_connection.hpp"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <memory>
#include <iostream>

namespace lux::core {

TcpServer::TcpServer(const std::string& host, uint16_t port,
                     EventLoop& loop, lux::DispatchFn dispatch,
                     int max_connections,
                     std::shared_ptr<std::atomic<int>> conn_count)
    : loop_(loop)
    , dispatch_(std::move(dispatch))
    , max_connections_(max_connections)
    , conn_count_(conn_count ? std::move(conn_count)
                             : std::make_shared<std::atomic<int>>(0))
{
    // IPv4 is used when the host says so (0.0.0.0 or a v4 IP)
    // and IPv6 only when explicitly asked for (::)
    bool use_ipv6 = (host == "::" || host.find(':') != std::string::npos);
    int family    = use_ipv6 ? AF_INET6 : AF_INET;

    listen_fd_ = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error(std::string("socket: ") + strerror(errno));

    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (use_ipv6) {
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons(port);
        const char* h = (host == "::") ? "::" : host.c_str();
        if (inet_pton(AF_INET6, h, &addr.sin6_addr) != 1)
            throw std::runtime_error("invalid IPv6 host: " + host);
        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error(std::string("bind: ") + strerror(errno));
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (host == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            // inet_pton returns 0 for malformed input; inet_addr would silently
            // accept some malformed strings or return INADDR_NONE, causing a
            // confusing bind on broadcast/all-ones.
            if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
                throw std::runtime_error("invalid IPv4 host: " + host);
        }
        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error(std::string("bind: ") + strerror(errno));
    }

    if (listen(listen_fd_, SOMAXCONN) < 0)
        throw std::runtime_error(std::string("listen: ") + strerror(errno));

    register_listen_fd();
}

void TcpServer::register_listen_fd() {
    loop_.add(listen_fd_, EPOLLIN, [this](uint32_t) { on_accept(); });
}

TcpServer::~TcpServer() {
    stop_accepting();
}

void TcpServer::pedir_parada() {
    loop_.post([this] { stop_accepting(); });
}

void TcpServer::stop_accepting() {
    if (listen_fd_ >= 0) {
        loop_.remove(listen_fd_);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void TcpServer::on_accept() {
    while (true) {
        sockaddr_storage addr{};
        socklen_t addrlen = sizeof(addr);

        int client_fd = accept4(listen_fd_, (sockaddr*)&addr, &addrlen,
                                SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;

            // EMFILE/ENFILE (this process or the whole system is out of file
            // descriptors) and ENOBUFS/ENOMEM (kernel out of network buffer
            // memory) are all "try again once something frees up", not "this
            // socket is broken" -- but there is still a connection sitting
            // in the kernel's accept backlog that accept4() just refused to
            // hand us, so listen_fd_ stays readable and epoll_wait() reports
            // it ready again immediately. Breaking out of this loop (as
            // every other error does, correctly) does not help: the OUTER
            // event loop just calls on_accept() again on its very next
            // epoll_wait(), which fails accept4() again, forever -- a tight
            // epoll_wait/accept4 spin at whatever rate the kernel lets those
            // two syscalls run, logging one line each time. Confirmed
            // against the real binary: `ulimit -n 128` plus a client that
            // opens ~80 connections produced 2.1 MILLION log lines (62 MB)
            // in under two seconds, one CPU core pegged the whole time.
            //
            // The fix used by nginx/libevent for this exact failure mode:
            // stop listening for a short backoff instead of spinning, so
            // whatever is holding the other fds/buffers open gets a chance
            // to let go of them (a burst of connections finishing, GC in
            // some other process, etc.) before this socket is readable
            // again. register_listen_fd() re-arms it when the timer fires;
            // if stop_accepting() runs first (shutdown), listen_fd_ is -1 by
            // then and the callback below is a no-op instead of resurrecting
            // a closed listener.
            if (errno == EMFILE || errno == ENFILE ||
                errno == ENOBUFS || errno == ENOMEM) {
                std::cerr << "accept4: " << strerror(errno)
                          << " -- pausing this listener for 500ms\n";
                loop_.remove(listen_fd_);

                // schedule_timer() itself needs a NEW fd (timerfd_create()) --
                // exactly the resource EMFILE just said this process has
                // none of left. When it can't get one, EpollLoop::
                // schedule_timer() falls back to loop_.post(cb) -- queuing
                // `rearm` to run on this loop's very next tick, immediately,
                // instead of after 500ms. That is the right general policy
                // for every OTHER caller (still eventually fire, rather than
                // silently drop the callback), but here it defeats the whole
                // point: re-arming listen_fd_ right away, while it is still
                // readable (the connection that triggered EMFILE is still
                // sitting in the kernel's accept backlog) and fds are still
                // exhausted, hits EMFILE again immediately, tries to
                // schedule ANOTHER timer, fails to get an fd for THAT one
                // too, forever -- confirmed against the real binary: relying
                // on schedule_timer() alone here still produced over a
                // million log lines and pegged a core, no better than
                // having no backoff at all.
                //
                // usleep() cannot fail this way -- it needs no fd -- so it
                // is the real fallback whenever schedule_timer() reports (by
                // returning < 0) that its own callback already got queued
                // for immediate, undelayed execution instead of in 500ms.
                // `armed` guards against BOTH paths calling register_listen_fd():
                // schedule_timer()'s post() fallback only ever queues its
                // callback (post() does not run it inline), so the manual
                // path below -- usleep() then rearm() -- always completes
                // and sets the flag before that queued callback gets a
                // chance to run on this same single-threaded loop; without
                // the guard, the queued call would fire a SECOND
                // register_listen_fd() on an fd already back in epoll's
                // interest set, which fails with EEXIST and (see
                // EpollLoop::add()) erases the callback that first call just
                // installed -- listen_fd_ would stay in epoll's set forever,
                // ready and spuriously waking this thread, with no callback
                // left to answer it: this listener would silently stop
                // accepting connections for the rest of the process's life.
                auto armed = std::make_shared<bool>(false);
                auto rearm = [this, armed] {
                    if (*armed) return;
                    *armed = true;
                    if (listen_fd_ >= 0) register_listen_fd();
                };
                if (loop_.schedule_timer(500, rearm) < 0) {
                    usleep(500'000);
                    rearm();
                }
                return;
            }

            std::cerr << "accept4: " << strerror(errno) << '\n';
            break;
        }

        // Reject if at connection limit — send 503 and close immediately.
        int current = conn_count_->fetch_add(1, std::memory_order_relaxed);
        if (current >= max_connections_) {
            conn_count_->fetch_sub(1, std::memory_order_relaxed);
            // Write a minimal 503 without allocating an HttpConnection.
            // Body is exactly 32 bytes — keep Content-Length in sync.
            const char resp[] =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 32\r\n"
                "Connection: close\r\n\r\n"
                "{\"error\":\"Too many connections\"}";
            (void)::write(client_fd, resp, sizeof(resp) - 1);
            ::close(client_fd);
            continue;
        }

        auto conn = std::make_shared<http::HttpConnection>(
            client_fd, loop_, dispatch_, conn_count_);

        conn->start();

        loop_.add(client_fd, EPOLLIN, [conn](uint32_t events) {
            conn->on_event(events);
        });
    }
}

} // namespace lux::core
