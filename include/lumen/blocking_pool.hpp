#pragma once
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <lumen/core/event_loop.hpp>

namespace lumen {

// ─── BlockingPool ──────────────────────────────────────────────────────────
//
// A shared pool of worker threads for route handlers that have no `await`
// at all -- pure CPU-bound work (counting primes, hashing, whatever) that
// would otherwise run inline on whichever core's event loop accepted the
// connection, blocking it from serving anyone else for as long as the work
// takes.
//
// Modeled directly on lumen_script::DbPool (db.hpp/db.cpp), minus the
// per-worker pinning a DB transaction needs: this is CPU work, not a
// stateful connection, so any free worker can take any job. That is
// precisely the fix for the imbalance a plain per-core event loop has: today
// a synchronous request is stuck on whatever loop happened to accept it, so
// an unlucky loop that got 4-5 concurrent CPU-bound requests queues them one
// after another while other loops sit idle. A SHARED queue drained by
// however many workers self-balances -- the same effect Axum/Actix get from
// running their equivalent routes on a work-stealing blocking-thread pool
// (tokio::task::spawn_blocking), confirmed by reading their handler source
// directly rather than assumed.
class BlockingPool {
public:
    ~BlockingPool();

    void start(size_t workers);
    void submit(std::function<void()> job);
    void stop();

    size_t size() const { return threads_.size(); }

private:
    std::vector<std::thread>       threads_;
    std::queue<std::function<void()>> jobs_;
    std::mutex                     mutex_;
    std::condition_variable        cv_;
    bool                           stopping_ = false;
};

// One pool for the whole process -- every event loop's synchronous routes
// share it, which is the point: it is what lets a busy loop's overflow work
// get picked up by an idle worker instead of queuing behind requests already
// running on that loop.
BlockingPool& blocking_pool();

// ─── BlockingAwaitable ───────────────────────────────────────────────────────
//
// co_await BlockingAwaitable{loop, [&]{ ...cpu-bound work, may touch req/res... }};
//
// Submits `work` to the shared pool and suspends the calling coroutine;
// when `work` finishes, resumes it back on `loop` -- the SAME event loop
// that owns this connection's Request/Response/socket, via
// EventLoop::post(), exactly like DbAwaitable does for database calls. The
// coroutine (and therefore req/res) is not touched by anyone else while
// suspended, so it is safe for `work` to read/write them from the pool
// thread: control has fully passed to the worker until it posts back.
struct BlockingAwaitable {
    core::EventLoop*     loop;
    std::function<void()> work;

    // If nobody has started the pool -- App::listen() always does before
    // serving real traffic, but an embedder calling into lumen_script
    // directly, or a test that drives a route's Task<void> by hand with no
    // event loop at all, may not have -- submitting would queue a job no
    // thread will ever drain and the coroutine would hang forever waiting
    // for a resume that never comes. Running inline here instead preserves
    // the guarantee a no-`await` chunk/native route has always had: its
    // Task<void> completes within a single resume(), no event loop
    // required. That guarantee is exactly what several tests (see
    // tests/native_route_shadow.cpp) are built on.
    bool await_ready() {
        if (blocking_pool().size() == 0) {
            work();
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h) {
        auto* l = loop;
        auto  w = work;
        blocking_pool().submit([l, h, w]() mutable {
            w();
            l->post([h]() mutable { h.resume(); });
        });
    }

    void await_resume() const noexcept {}
};

} // namespace lumen
