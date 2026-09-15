#include <lux/blocking_pool.hpp>
#include <thread>

namespace lux {

BlockingPool::~BlockingPool() { stop(); }

void BlockingPool::start(size_t core_workers, size_t max_workers) {
    if (!threads_.empty()) return;
    if (core_workers == 0) core_workers = 1;
    // +8, not the naive-looking "just multiply core count" -- measured
    // (see blocking_pool.hpp) to be the knee of the variance-vs-typical-
    // case-cost curve, not a round number picked for looks.
    if (max_workers < core_workers) max_workers = core_workers + 8;

    core_workers_ = core_workers;
    max_workers_  = max_workers;
    live_workers_ = core_workers;

    for (size_t i = 0; i < core_workers; ++i)
        threads_.emplace_back([this] { worker_loop(/*overflow=*/false); });
}

void BlockingPool::worker_loop(bool overflow) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++idle_workers_;
            if (overflow) {
                // Overflow workers only exist to soak up a burst: once one
                // has sat idle past the timeout with nothing to do, the
                // burst is over and it should give the thread back instead
                // of sitting there as permanent oversubscription -- that is
                // exactly the cost a fixed larger pool pays all the time
                // (measured: p99 nearly doubles at 8x core count vs core
                // count) for a benefit only bursts need.
                bool got = cv_.wait_for(lock, kOverflowIdleTimeout,
                                         [this] { return stopping_ || !jobs_.empty(); });
                if (!got || (stopping_ && jobs_.empty())) {
                    --idle_workers_;
                    --live_workers_;
                    return;
                }
            } else {
                cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_ && jobs_.empty()) {
                    --idle_workers_;
                    return;
                }
            }
            --idle_workers_;
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        // A handler that throws cannot take the worker down with it: the
        // coroutine it was posting a resume for would simply never resume,
        // leaking one connection instead of every synchronous route sharing
        // this pool.
        try { job(); } catch (...) {}
    }
}

void BlockingPool::submit(std::function<void()> job) {
    bool spawn_overflow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        jobs_.push(std::move(job));
        // Every idle core/overflow worker will wake for this (notify_one
        // below picks one), but if the backlog is already deeper than the
        // idle workers can be expected to drain -- i.e. this job would sit
        // behind others with nobody free to even start it -- and there is
        // still room under the cap, grow the pool by one right now instead
        // of making it wait for an existing worker to free up.
        if (jobs_.size() > idle_workers_ && live_workers_ < max_workers_) {
            ++live_workers_;
            spawn_overflow = true;
        }
    }
    cv_.notify_one();
    if (spawn_overflow)
        std::thread([this] { worker_loop(/*overflow=*/true); }).detach();
}

void BlockingPool::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
    // Overflow workers are detached: they see stopping_ on their own via
    // the same notify_all and exit themselves, nothing left to join here.
}

BlockingPool& blocking_pool() {
    static BlockingPool pool;
    return pool;
}

} // namespace lux
