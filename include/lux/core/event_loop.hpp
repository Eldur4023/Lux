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
    // Ahead of everything else: run between one event and the next, not after
    // the whole batch and every task queued before it. For a handler holding
    // something others wait for -- a database transaction's write turn --
    // whose continuation, queued like any other, kept it held for ms.
    void post_urgent(std::function<void()> cb);

    int  schedule_timer(int ms, std::function<void()> cb);
    void cancel_timer  (int tfd);

    void run();
    void stop();

private:
    void process_tasks();
    void process_urgent();
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
    std::vector<decltype(callbacks_)::node_type> graveyard_;   // removed mid-dispatch, freed after it
    std::vector<std::function<void()>> task_queue_;
    std::vector<std::function<void()>> running_tasks_;   // swapped with task_queue_: both keep their capacity
    std::mutex queue_mutex_;
    std::atomic<bool> has_tasks_{false};   // lets process_tasks() skip the lock when idle
    std::vector<std::function<void()>> urgent_queue_;
    std::vector<std::function<void()>> running_urgent_;
    std::atomic<bool> has_urgent_{false};
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
