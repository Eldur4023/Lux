#pragma once
#include <chrono>
#include <functional>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <vector>
#include <atomic>
#include <mutex>

namespace lux::core {

// ── EpollLoop ─────────────────────────────────────────────────────────────────
//
// Default event loop backend — uses epoll + eventfd.  Timers live in an
// ordered map and set epoll_wait's timeout: no fd or syscall per timer, and
// every request arms two (header and request timeouts).
// All existing code refers to this as "EventLoop" via the alias below.

class EpollLoop {
public:
    EpollLoop();
    ~EpollLoop();

    using Callback = std::function<void(uint32_t events)>;

    void add   (int fd, uint32_t events, Callback cb);
    void modify(int fd, uint32_t events);
    void remove(int fd);

    void post(std::function<void()> cb);

    int  schedule_timer(int ms, std::function<void()> cb);
    void cancel_timer  (int tfd);

    void run();
    void stop();

private:
    void process_tasks();
    void run_timers();
    int  next_timeout_ms() const;

    int  epoll_fd_  = -1;
    int  wakeup_fd_ = -1;
    // Atomic because stop() writes it from ANOTHER thread —the shutdown one—
    // and the loop reads it on every turn.  As a plain bool it was a race, even
    // if in practice the compiler never bit it.
    std::atomic<bool> running_{false};

    using Clock = std::chrono::steady_clock;
    std::map<std::pair<Clock::time_point, int>, std::function<void()>> timers_;
    std::unordered_map<int, Clock::time_point> timer_deadline_;  // id -> key in timers_
    int next_timer_id_ = 0;

    std::unordered_map<int, Callback> callbacks_;
    std::vector<std::function<void()>> task_queue_;
    std::mutex queue_mutex_;
};

} // namespace lux::core

// ── Backend alias ─────────────────────────────────────────────────────────────
//
// When LUX_IO_URING is defined, EventLoop resolves to IoUringLoop.
// All connection and server code uses core::EventLoop without any changes.

#ifdef LUX_IO_URING
#  include "io_uring_loop.hpp"
   namespace lux::core { using EventLoop = IoUringLoop; }
#else
   namespace lux::core { using EventLoop = EpollLoop; }
#endif
