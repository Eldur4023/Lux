#include <lumen/blocking_pool.hpp>
#include <thread>

namespace lumen {

BlockingPool::~BlockingPool() { stop(); }

void BlockingPool::start(size_t workers) {
    if (!threads_.empty()) return;
    if (workers == 0) workers = 1;

    for (size_t i = 0; i < workers; ++i) {
        threads_.emplace_back([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                    if (stopping_ && jobs_.empty()) return;
                    job = std::move(jobs_.front());
                    jobs_.pop();
                }
                // A handler that throws cannot take the worker down with it:
                // the coroutine it was posting a resume for would simply
                // never resume, leaking one connection instead of every
                // synchronous route sharing this pool.
                try { job(); } catch (...) {}
            }
        });
    }
}

void BlockingPool::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
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
}

BlockingPool& blocking_pool() {
    static BlockingPool pool;
    return pool;
}

} // namespace lumen
