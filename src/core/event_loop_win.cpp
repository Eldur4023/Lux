// Windows half of EpollLoop (the rest -- tasks, timers, stop -- is shared and
// lives in event_loop.cpp). Same public interface and same contract as the
// epoll one, with different machinery:
//
//   * WSAPoll over the registered sockets, level-triggered. The connection
//     code reads/accepts until EAGAIN, so level-triggered only costs it a
//     spurious wakeup, never a lost one; EPOLLET is ignored.
//   * The wakeup is a loopback socket pair (plat::socketpair_loopback)
//     instead of an eventfd: WSAPoll can only wait on sockets.
//   * The poll array is rebuilt only when the interest set changes (add /
//     modify / remove), not on every turn.
//
// WSAPoll is O(registered sockets) per call, where epoll is O(ready). That is
// the price of a dependency-free backend; IOCP (or vendoring wepoll) is the
// upgrade if Windows ever needs to carry c10k.
#ifdef _WIN32

#include <lux/core/event_loop.hpp>

#include <stdexcept>
#include <iostream>

namespace lux::core {

EpollLoop::EpollLoop() {
    int sv[2];
    if (!plat::socketpair_loopback(sv))
        throw std::runtime_error("wakeup socket pair: " + plat::err_str());
    wake_r_ = sv[0];
    wake_w_ = sv[1];
}

EpollLoop::~EpollLoop() {
    if (wake_r_ >= 0) plat::sock_close(wake_r_);
    if (wake_w_ >= 0) plat::sock_close(wake_w_);
}

void EpollLoop::add(int fd, uint32_t events, Callback cb) {
    // An existing entry may be the callback running right now: parked.
    if (auto it = callbacks_.find(fd); it != callbacks_.end())
        graveyard_.push_back(callbacks_.extract(it));
    callbacks_.emplace(fd, std::move(cb));
    interest_[fd] = events;
    pfds_dirty_ = true;
}

void EpollLoop::modify(int fd, uint32_t events) {
    if (auto it = interest_.find(fd); it != interest_.end() && it->second != events) {
        it->second  = events;
        pfds_dirty_ = true;
    }
}

void EpollLoop::remove(int fd) {
    interest_.erase(fd);
    pfds_dirty_ = true;
    // Events already collected for this fd in the current turn must not reach
    // whatever socket reuses the number before they are dispatched.
    for (auto& r : ready_) if (r.fd == fd) r.fd = -1;
    // Parked, not destroyed: remove() is usually called from inside this very
    // callback (a connection closing itself).
    if (auto it = callbacks_.find(fd); it != callbacks_.end())
        graveyard_.push_back(callbacks_.extract(it));
}

// One byte down the loopback pair. A full buffer means a wakeup is already
// pending, which is all that is needed.
void EpollLoop::wake() {
    const char b = 1;
    (void)plat::sock_write(wake_w_, &b, 1);
}

void EpollLoop::run() {
    running_ = true;

    while (running_) {
        // We process tasks before waiting to handle anything posted before the loop
        process_tasks();

        if (pfds_dirty_) {
            pfds_.clear();
            pfds_.reserve(interest_.size() + 1);
            WSAPOLLFD w{};
            w.fd     = static_cast<SOCKET>(wake_r_);
            w.events = POLLRDNORM;
            pfds_.push_back(w);
            for (const auto& [fd, ev] : interest_) {
                WSAPOLLFD p{};
                p.fd     = static_cast<SOCKET>(fd);
                p.events = static_cast<SHORT>(((ev & EPOLLIN) ? POLLRDNORM : 0) |
                                              ((ev & EPOLLOUT) ? POLLWRNORM : 0));
                pfds_.push_back(p);
            }
            pfds_dirty_ = false;
        }

        const int n = ::WSAPoll(pfds_.data(), static_cast<ULONG>(pfds_.size()), next_timeout_ms());
        if (n == SOCKET_ERROR) {
            plat::fix_errno();
            if (errno == EINTR) continue;
            std::cerr << "WSAPoll: " << plat::err_str() << '\n';
            break;
        }

        // Collect first: a callback may add/remove and rebuild pfds_.
        ready_.clear();
        if (n > 0) {
            if (pfds_[0].revents) {   // wakeup: the next turn runs process_tasks()
                char buf[64];
                while (plat::sock_read(wake_r_, buf, sizeof buf) > 0) {}
            }
            for (size_t i = 1; i < pfds_.size(); ++i) {
                const SHORT re = pfds_[i].revents;
                if (!re) continue;
                const uint32_t want = interest_.count(static_cast<int>(pfds_[i].fd))
                                          ? interest_[static_cast<int>(pfds_[i].fd)] : 0;
                uint32_t ev = 0;
                if (re & (POLLRDNORM | POLLRDBAND)) ev |= EPOLLIN;
                if (re & POLLWRNORM)                ev |= EPOLLOUT;
                // A hangup is reported as readable when the caller reads: its
                // read() then sees EOF / the reset and closes, after taking any
                // data still queued, like epoll's EPOLLIN|EPOLLHUP.
                if (re & POLLHUP) ev |= (want & EPOLLIN) ? EPOLLIN : EPOLLHUP;
                if (re & (POLLERR | POLLNVAL)) ev |= EPOLLERR;
                ready_.push_back({static_cast<int>(pfds_[i].fd), ev});
            }
        }

        for (size_t i = 0; i < ready_.size(); i++) {
            const int fd = ready_[i].fd;
            if (fd < 0) continue;   // removed by an earlier callback this turn

            process_urgent();
            auto it = callbacks_.find(fd);
            if (it == callbacks_.end()) continue;

            // Called in place: a remove() or add() on this fd from inside
            // parks the callback in graveyard_ instead of destroying it.
            it->second(ready_[i].events);
        }
        ready_.clear();
        graveyard_.clear();
        run_timers();
    }
}

} // namespace lux::core

#endif // _WIN32
