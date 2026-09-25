#include <lux/core/event_loop.hpp>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <climits>
#include <iostream>

namespace lux::core {

EpollLoop::EpollLoop() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        throw std::runtime_error(std::string("epoll_create1: ") + strerror(errno));

    // eventfd used to wake up epoll_wait from stop()
    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0)
        throw std::runtime_error(std::string("eventfd: ") + strerror(errno));

    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.fd  = wakeup_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev);
}

EpollLoop::~EpollLoop() {
    if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
    if (epoll_fd_  >= 0) ::close(epoll_fd_);
}

void EpollLoop::add(int fd, uint32_t events, Callback cb) {
    callbacks_[fd] = std::move(cb);

    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        callbacks_.erase(fd);
        std::cerr << "epoll_ctl ADD fd=" << fd << ": " << strerror(errno) << '\n';
    }
}

void EpollLoop::modify(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void EpollLoop::remove(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    callbacks_.erase(fd);
}

void EpollLoop::post(std::function<void()> cb) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push_back(std::move(cb));
    }
    uint64_t val = 1;
    (void)write(wakeup_fd_, &val, sizeof(val));
}

void EpollLoop::process_tasks() {
    std::vector<std::function<void()>> current_tasks;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        current_tasks.swap(task_queue_);
    }
    for (auto& task : current_tasks) {
        if (task) task();
    }
}

void EpollLoop::run() {
    running_ = true;
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    while (running_) {
        // We process tasks before waiting to handle anything posted before the loop
        process_tasks();
        
        int n = epoll_wait(epoll_fd_, events, kMaxEvents, next_timeout_ms());
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait: " << strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            // Wakeup fd → this will trigger next iteration to process_tasks()
            if (fd == wakeup_fd_) {
                uint64_t val;
                (void)read(wakeup_fd_, &val, sizeof(val));
                continue;
            }

            auto it = callbacks_.find(fd);
            if (it == callbacks_.end()) continue;

            // Make a local copy so the callback isn't destroyed if it
            // calls remove() on itself (e.g. connection close).
            Callback cb = it->second;
            cb(events[i].events);
        }
        run_timers();
    }
}

// Returns a timer id (> 0, never an fd). Loop thread only, like add().
int EpollLoop::schedule_timer(int ms, std::function<void()> cb) {
    do next_timer_id_ = next_timer_id_ == INT_MAX ? 1 : next_timer_id_ + 1;
    while (timer_deadline_.count(next_timer_id_));

    const auto deadline = Clock::now() + std::chrono::milliseconds(ms > 0 ? ms : 0);
    timers_.emplace(std::pair{deadline, next_timer_id_}, std::move(cb));
    timer_deadline_.emplace(next_timer_id_, deadline);
    return next_timer_id_;
}

void EpollLoop::cancel_timer(int id) {
    auto it = timer_deadline_.find(id);
    if (it == timer_deadline_.end()) return; // already fired or cancelled
    timers_.erase({it->second, id});
    timer_deadline_.erase(it);
}

// Fires every expired timer. Each is unlinked before its callback runs, so
// the callback may schedule or cancel timers (itself included) freely.
void EpollLoop::run_timers() {
    const auto now = Clock::now();
    while (!timers_.empty() && timers_.begin()->first.first <= now) {
        auto node = timers_.extract(timers_.begin());
        timer_deadline_.erase(node.key().second);
        node.mapped()();
    }
}

// epoll_wait timeout: -1 with no timers, else ms to the nearest one, rounded
// up so the loop doesn't wake a hair early and spin.
int EpollLoop::next_timeout_ms() const {
    if (timers_.empty()) return -1;
    const auto left = timers_.begin()->first.first - Clock::now();
    if (left <= Clock::duration::zero()) return 0;
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(left).count());
}

void EpollLoop::stop() {
    running_ = false;
    uint64_t val = 1;
    (void)write(wakeup_fd_, &val, sizeof(val));
}

} // namespace lux::core
