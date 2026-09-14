#pragma once
#include <chrono>
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
//
// Core count is not the right size in EITHER direction, measured, not
// assumed: pinned at core count, a burst of concurrent CPU-bound requests
// above that count has nowhere to go but wait -- rare, but when it happens
// the wait can be tens of milliseconds (confirmed: 10 trials, p99 CV 4.5%,
// but max CV 50.8%, one outlier of 137ms against a typical 40-70ms). Pinned
// much higher instead (tested at 8x core count) closes that gap (max CV
// 8.5%) but nearly DOUBLES the typical case (p99 30ms -> 56ms) -- that many
// permanently-live threads fighting the same core count for CPU adds real
// scheduling overhead to every request, not just the rare bursty one.
//
// So: a small set of CORE workers (sized to the core count) that live for
// the process's whole lifetime, plus a SMALL, capped set of OVERFLOW
// workers spun up on demand when the queue backs up faster than idle core
// workers can drain it, exiting on their own after sitting idle past
// kOverflowIdleTimeout. Shape borrowed from tokio's own blocking pool (a
// fixed async worker count, plus an elastic, keep-alive-timed pool for
// spawn_blocking) -- but NOT its size: tokio defaults that pool's cap to
// 512, unbounded in practice. Tried matching that shape here first,
// uncapped-ish (8x core count) -- under SUSTAINED concurrent load (as
// opposed to a genuinely brief, then-idle burst) it grew almost as large as
// a permanently-oversubscribed fixed pool and paid almost the same typical-
// case cost (p99 52ms vs the naive fixed-128 test's 56ms, both far worse
// than core-count-only's 30ms) for no real variance advantage over a much
// smaller cap. The app.cpp call site caps it at core_count+8 instead: most
// of the max-CV improvement (50.8% -> 14.9%, measured, not assumed) for a
// much smaller typical-case cost (p99 30ms -> 39ms) than either extreme.
// See app.cpp for the actual numbers this was picked from.
class BlockingPool {
public:
    ~BlockingPool();

    void start(size_t core_workers, size_t max_workers = 0);
    void submit(std::function<void()> job);
    void stop();

    // Core worker count -- used by BlockingAwaitable to check "has anyone
    // started this pool at all", not affected by overflow workers coming
    // and going.
    size_t size() const { return core_workers_; }

private:
    static constexpr auto kOverflowIdleTimeout = std::chrono::seconds(2);

    void worker_loop(bool overflow);

    std::vector<std::thread>          threads_;   // core workers, joined in stop()
    std::queue<std::function<void()>> jobs_;
    std::mutex                        mutex_;
    std::condition_variable           cv_;
    bool                               stopping_     = false;
    size_t                             core_workers_ = 0;
    size_t                             max_workers_  = 0;
    size_t                             live_workers_ = 0;  // core + overflow, mutex-guarded
    size_t                             idle_workers_ = 0;  // currently waiting for a job
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
