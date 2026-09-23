#pragma once
#include <coroutine>
#include <exception>
#include <utility>
#include <functional>
#include <memory>
#include <iostream>
#include <lux/core/event_loop.hpp>
#include "cancel.hpp"

namespace lux {

// ─── Task<T> ─────────────────────────────────────────────────────────────────
//
// Coroutine return type. Supports:
//   - co_return value
//   - co_await sleep(ms, loop)
//   - co_await Task<U>          (chained tasks via symmetric transfer)
//
// Lifetime: the coroutine frame self-destructs via the event loop after
// completing, as long as loop is set on the promise before resuming.
// Use detach() to transfer ownership from the Task wrapper to the promise.

template<typename T>
struct Task {
    struct promise_type {
        using value_type = T;

        T               result;
        std::exception_ptr exception;
        std::function<void(T)> on_complete;   // called when the task finishes
        std::coroutine_handle<> continuation; // outer coroutine waiting on us
        core::EventLoop* loop = nullptr;      // for deferred self-destruction

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        // After the coroutine finishes:
        //   - if there is a continuation (co_await chain), resume it
        //   - otherwise schedule self-destruction via the event loop
        struct FinalAwaitable {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept
            {
                if (auto c = h.promise().continuation) return c;  // symmetric transfer

                // No continuation — schedule destruction on the event loop
                if (h.promise().loop) {
                    auto raw = std::coroutine_handle<>(h);
                    h.promise().loop->post([raw]() mutable { raw.destroy(); });
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaitable final_suspend() noexcept { return {}; }

        void return_value(T value) {
            result = std::move(value);
            if (on_complete) on_complete(result);
        }
        void unhandled_exception() {
            exception = std::current_exception();
            if (!continuation) {
                try { std::rethrow_exception(exception); }
                catch (const std::exception& e) {
                    std::cerr << "[lux] unhandled exception in detached task: " << e.what() << '\n';
                } catch (...) {
                    std::cerr << "[lux] unknown exception in detached task\n";
                }
            }
        }
    };

    std::coroutine_handle<promise_type> handle;

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    Task(Task&& o) noexcept : handle(std::exchange(o.handle, nullptr)) {}
    Task(const Task&) = delete;
    ~Task() { if (handle) handle.destroy(); }

    // Transfer ownership to the promise (stops ~Task from destroying the frame)
    std::coroutine_handle<promise_type> detach() {
        return std::exchange(handle, nullptr);
    }

    bool done() const { return handle && handle.done(); }

    T get_result() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        return std::move(handle.promise().result);
    }

    // ── Awaiter interface (for co_await Task<T> inside another coroutine) ────
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> outer) noexcept {
        // Store the outer coroutine as our continuation; FinalAwaitable resumes it
        handle.promise().continuation = outer;
        handle.resume(); // start the inner task
    }

    T await_resume() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        return std::move(handle.promise().result);
    }
};

// ─── Task<void> ──────────────────────────────────────────────────────────────

template<>
struct Task<void> {
    struct promise_type {
        using value_type = void;

        std::exception_ptr exception;
        std::function<void()> on_complete;
        std::coroutine_handle<> continuation;
        core::EventLoop* loop = nullptr;

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaitable {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept
            {
                if (auto c = h.promise().continuation) return c;

                if (h.promise().loop) {
                    auto raw = std::coroutine_handle<>(h);
                    h.promise().loop->post([raw]() mutable { raw.destroy(); });
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaitable final_suspend() noexcept { return {}; }

        void return_void() {
            if (on_complete) on_complete();
        }
        void unhandled_exception() {
            exception = std::current_exception();
            if (!continuation) {
                try { std::rethrow_exception(exception); }
                catch (const std::exception& e) {
                    std::cerr << "[lux] unhandled exception in detached task: " << e.what() << '\n';
                } catch (...) {
                    std::cerr << "[lux] unknown exception in detached task\n";
                }
            }
        }
    };

    std::coroutine_handle<promise_type> handle;

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    Task(Task&& o) noexcept : handle(std::exchange(o.handle, nullptr)) {}
    Task(const Task&) = delete;
    ~Task() { if (handle) handle.destroy(); }

    std::coroutine_handle<promise_type> detach() {
        return std::exchange(handle, nullptr);
    }

    bool done() const { return handle && handle.done(); }

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> outer) noexcept {
        handle.promise().continuation = outer;
        handle.resume();
    }
    void await_resume() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
    }
};

// ─── SleepAwaitable ──────────────────────────────────────────────────────────
// Non-blocking sleep: suspends the coroutine and resumes it after `ms`
// milliseconds via the event loop's timerfd (one-shot, no extra threads).
//
// If a CancellationToken is supplied (via the thread-local or explicitly) and
// the connection is closed before the timer fires, the coroutine is resumed
// immediately so it can check is_cancelled() and exit rather than waiting out
// the full duration (avoids zombie coroutines on slow-path handlers).

struct SleepAwaitable {
    int ms;
    core::EventLoop*                    loop;
    std::weak_ptr<CancellationToken>    token;

    bool await_ready() const noexcept { return ms <= 0; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        if (!loop) { h.resume(); return; }

        // If the token is ALREADY cancelled before the timer is even
        // armed, the resume must happen ASYNCHRONOUSLY (via loop->post),
        // never from within this call. A ws/sse handler's loop shape is
        // `while ws.open: ... await sleep(ms)` -- it re-checks is_cancelled()
        // only AFTER the await returns, not before calling sleep() again.
        // If this resumed h synchronously, the coroutine would run straight
        // through to the next sleep() call, hit this exact branch again
        // (the token is still cancelled), and recurse on the same C++ call
        // stack with no bound: what looks like a bounded per-iteration loop
        // at the Lux Script level is actually unbounded native recursion,
        // and it ends in a stack-overflow SIGSEGV that takes the whole
        // process down, not just this one connection. Posting breaks the
        // stack on every iteration instead, the same way a real epoll_wait
        // round-trip would.
        if (auto t = token.lock(); t && t->is_cancelled()) {
            loop->post([h]() mutable { if (!h.done()) h.resume(); });
            return;
        }

        // Schedule the timer.  The callback clears the wake slot first so
        // cancel() can't fire it again after the timer has already won.
        int tfd = loop->schedule_timer(ms, [h, tok = token]() mutable {
            if (auto t = tok.lock()) t->clear_wake();
            if (!h.done()) h.resume();
        });

        // Register the early-wake callback for a cancellation that arrives
        // WHILE this timer is pending (the common case: the connection
        // closes mid-sleep). This is not the synchronous-recursion hazard
        // above -- cancel() is invoked from HttpConnection::close(), which
        // is not itself running inside this coroutine's own call chain, so
        // resuming h here does not grow this coroutine's call stack.
        if (auto t = token.lock()) {
            t->set_wake([h, tfd, l = loop]() mutable {
                l->cancel_timer(tfd);
                if (!h.done()) h.resume();
            });
        }
    }

    void await_resume() const noexcept {}
};

// Thread-locals set by HttpConnection::dispatch() for the current request.
namespace detail {
    inline thread_local core::EventLoop*             current_loop  = nullptr;
    inline thread_local std::weak_ptr<CancellationToken> current_token = {};
}

// sleep(ms, loop) — explicit loop (backward-compat / advanced use)
inline SleepAwaitable sleep(int ms, core::EventLoop* loop) {
    return {ms, loop, detail::current_token};
}

// sleep(ms, loop, token) — explicit loop AND token.
//
// Required for any handler that can call sleep() more than once across a
// suspension boundary -- a ws/sse route's `while ... : await sleep(ms)`
// loop is exactly this shape. detail::current_token is set ONCE, inside
// HttpConnection::dispatch(), and every OTHER connection dispatched on this
// same event-loop thread overwrites it in between. A loop that re-enters
// sleep() after its first suspension reads whatever connection most
// recently dispatched on this thread, not its own -- on a busy server that
// is essentially always a different (and possibly already-closed)
// connection's token, so is_cancelled() checks against a token that says
// nothing about this connection at all. Passing the token this coroutine
// actually owns (Request::cancel_token) makes the awaitable correct
// regardless of what else this thread dispatches while it sleeps.
inline SleepAwaitable sleep(int ms, core::EventLoop* loop,
                            std::weak_ptr<CancellationToken> token) {
    return {ms, loop, std::move(token)};
}

// sleep(ms) — uses thread-locals set for the current request (no args needed)
inline SleepAwaitable sleep(int ms) {
    return {ms, detail::current_loop, detail::current_token};
}

} // namespace lux
