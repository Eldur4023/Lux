// A process that outlives the request that started it -- a transcode, a
// long job a status page polls. start() returns an int handle at once;
// read()/write()/alive()/wait()/kill()/close() work on it from any later
// request. (os.run() is the run-to-completion form.) Entries live in a
// mutex-protected registry and are handed out as shared_ptr, so a close()
// racing a read() never frees what the read is using.
#include <lux_script/builtin_module.hpp>

#include "spawn.hpp"

#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>


namespace lux_script {

namespace {

// A single read() call is capped here regardless of what the caller passed
// as max_bytes: a .lux route is developer-authored, not attacker input,
// but there is no reason a single native call should be able to demand an
// arbitrarily large allocation either. 1 MB is generous for the kind of
// chunk an ffmpeg/ffprobe pipe actually produces between polls.
constexpr size_t kMaxReadChunk = 1 * 1024 * 1024;

// read()/wait() run on lux::blocking_pool() (is_async), a bounded shared
// pool -- unlike `await sleep()`, which the VM can suspend and resume via a
// timer without ever occupying a pool worker for the wait, a single
// read()/wait() call sleeps its OWN worker thread for up to `timeout_ms`
// with nothing else able to use it meanwhile. Capping the per-call timeout
// (the same protective role os.run()'s kRunTimeoutMs and http's kTimeoutMs
// already play) still leaves the documented "loop with a short timeout
// until it finishes" pattern fully intact -- a 6-hour job polled every 5s
// is 4320 short calls, not one long one.
constexpr long long kMaxTimeoutMs = 30'000;

// Poll step inside wait()'s loop -- see wait()'s own comment for why a
// single blocking waitpid() cannot be used here at all.
constexpr int kWaitPollStepMs = 50;

// ─── ProcRegistry ────────────────────────────────────────────────────────────

struct ProcEntry {
    // Guards everything below AND serializes the actual waitpid() call on
    // `pid`: two threads calling waitpid() on the same already-exited pid
    // concurrently is exactly the "waitpid on an already-reaped pid" UB
    // this exists to prevent -- see alive()/wait()/close(), all of which
    // take this before touching `reaped`/`exit_code` or calling waitpid().
    std::mutex mutex;
    pid_t      pid        = -1;
    int        stdout_fd  = -1;    // -1 unless started with stdout: "pipe"
    int        stdin_fd   = -1;    // -1 unless started with stdin: "pipe"
    bool       reaped     = false;
    int        exit_code  = -1;    // valid only once `reaped` is true
};

class ProcRegistry {
public:
    static ProcRegistry& instance() {
        static ProcRegistry r;
        return r;
    }

    // Random 53-bit ids (a handle cannot be guessed by counting), and a
    // handle nobody touched for an hour is closed -- closed, not killed.
    long long put(pid_t pid, int stdout_fd, int stdin_fd) {
        auto e = std::make_shared<ProcEntry>();
        e->pid       = pid;
        e->stdout_fd = stdout_fd;
        e->stdin_fd  = stdin_fd;
        for (long long stale : stale_ids()) close(stale);
        std::lock_guard<std::mutex> lock(mutex_);
        thread_local std::mt19937_64 rng{std::random_device{}()};
        long long id;
        do id = static_cast<long long>(rng() & ((1ULL << 53) - 1)) | 1; while (procs_.count(id));
        procs_.emplace(id, Slot{std::move(e), std::chrono::steady_clock::now()});
        return id;
    }

    // Kept alive for the caller's whole call via the returned shared_ptr,
    // even if close() erases it from `procs_` concurrently -- see the
    // module comment on why that is enough here, unlike rooms.cpp.
    std::shared_ptr<ProcEntry> get(long long handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = procs_.find(handle);
        if (it == procs_.end()) return nullptr;
        it->second.used = std::chrono::steady_clock::now();
        return it->second.entry;
    }

    void close(long long handle) {
        std::shared_ptr<ProcEntry> e;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = procs_.find(handle);
            if (it == procs_.end()) return;
            e = it->second.entry;
            procs_.erase(it);
        }
        std::lock_guard<std::mutex> elock(e->mutex);
        if (e->stdout_fd >= 0) { ::close(e->stdout_fd); e->stdout_fd = -1; }
        if (e->stdin_fd >= 0)  { ::close(e->stdin_fd);  e->stdin_fd  = -1; }
        if (e->reaped) return;
        int status = 0;
        pid_t r = ::waitpid(e->pid, &status, WNOHANG);
        if (r == e->pid) {
            e->reaped    = true;
            e->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return;
        }
        if (r == 0) {
            // Still running: close() must never kill anything by surprise
            // -- that is kill()'s job, on purpose, so a caller can free a
            // handle without deciding right then whether the process
            // should die too. Hand the pid to the sweep below instead of
            // leaving it unreaped forever: an orphaned zombie is fine for
            // an instant, forever is a real (if slow) resource leak.
            std::lock_guard<std::mutex> olock(orphan_mutex_);
            orphan_pids_.push_back(e->pid);
        }
        // r < 0 (ECHILD, ...): already reaped by something else, or the
        // pid never existed -- nothing left to track either way.
    }

    // Best-effort, non-blocking sweep of pids close() could not reap
    // immediately. Called opportunistically from start()/alive()/wait() --
    // any server that keeps calling those (which a live session inherently
    // does) never lets this grow unbounded; nothing here is load-bearing
    // for correctness if it is not called for a while, only for how
    // promptly a handful of harmless zombie entries get cleared.
    void reap_orphans() {
        std::lock_guard<std::mutex> lock(orphan_mutex_);
        for (size_t i = 0; i < orphan_pids_.size();) {
            int status = 0;
            pid_t r = ::waitpid(orphan_pids_[i], &status, WNOHANG);
            if (r != 0) orphan_pids_.erase(orphan_pids_.begin() + static_cast<long>(i));
            else        ++i;
        }
    }

private:
    struct Slot { std::shared_ptr<ProcEntry> entry; std::chrono::steady_clock::time_point used; };

    std::vector<long long> stale_ids() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<long long> out;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [id, slot] : procs_)
            if (now - slot.used > std::chrono::hours(1)) out.push_back(id);
        return out;
    }

    std::mutex                              mutex_;
    std::unordered_map<long long, Slot>     procs_;

    std::mutex           orphan_mutex_;
    std::vector<pid_t>   orphan_pids_;
};

// ─── start() ─────────────────────────────────────────────────────────────────

// start(cmd, args, {"stdout": "pipe" | "null" | path, "stderr": "null" |
// path, "stdin": "pipe" | "null", "cwd", "env"}). stdout defaults to a pipe
// (read() drains it), stderr to /dev/null. A stderr pipe is refused:
// nothing reads it, and a full pipe blocks the child forever.
Value fn_proc_start(NativeCtx&, std::vector<Value>& args, std::string& error) {
    spawn::Options o;
    o.out.kind = spawn::Stream::Pipe;
    if (args.size() > 2) {
        const auto& opts = args[2].as_dict();
        if (!spawn::read_common(opts, o, "proc.start", error)) return Value::null();
        auto stream = [&](const char* key, spawn::Stream& s, bool pipe_ok) {
            auto it = opts.find(key);
            if (it == opts.end()) return true;
            const std::string m = it->second.to_string();
            if (m == "pipe" && !pipe_ok) {
                error = std::string("proc.start(): options.") + key + " does not support \"pipe\" -- nothing in this module reads it; use a file path or \"null\"";
                return false;
            }
            s.kind = m == "pipe" ? spawn::Stream::Pipe : m == "null" ? spawn::Stream::Null : spawn::Stream::File;
            s.path = m;
            return true;
        };
        if (!stream("stdout", o.out, true) || !stream("stderr", o.err, false) || !stream("stdin", o.in, true))
            return Value::null();
    }
    ProcRegistry::instance().reap_orphans();

    spawn::Child c;
    if (!spawn::start(spawn::argv_of(args), o, c, error)) { error = "proc.start(): " + error; return Value::null(); }
    // Our ends non-blocking: read() never waits past its own poll(), and
    // write() hands over only what fits instead of blocking a thread.
    for (int fd : {c.out, c.in}) if (fd >= 0) fcntl(fd, F_SETFL, O_NONBLOCK);
    return Value::integer(ProcRegistry::instance().put(c.pid, c.out, c.in));
}

// ─── write() ─────────────────────────────────────────────────────────────────

// Bytes handed to the child's stdin (it needs stdin: "pipe"). Never blocks:
// with the pipe full it writes what fits, maybe 0, and the caller tries the
// rest later. write(h, null) closes stdin -- the EOF a filter waits for.
Value fn_proc_write(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto e = ProcRegistry::instance().get(args[0].as_int());
    if (!e) { error = "proc: unknown handle"; return Value::null(); }
    std::lock_guard<std::mutex> lock(e->mutex);
    if (e->stdin_fd < 0) { error = "proc.write(): this process has no stdin: \"pipe\" (or it was closed)"; return Value::null(); }
    if (args[1].is_null()) { ::close(e->stdin_fd); e->stdin_fd = -1; return Value::integer(0); }
    const std::string& data = args[1].as_str();
    const ssize_t n = ::write(e->stdin_fd, data.data(), data.size());
    if (n >= 0) return Value::integer(n);
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Value::integer(0);
    error = std::string("proc.write(): ") + std::strerror(errno);
    return Value::null();
}

// ─── alive() ─────────────────────────────────────────────────────────────────

// waitpid(WNOHANG): never blocks. If the child just exited, the exit code
// is cached on `e` right here so a later wait() never has to waitpid() a
// pid the kernel already reaped out from under it -- doing that a second
// time is not "returns nothing new", it is an actual error (ECHILD),
// because a reaped pid can be recycled by the kernel for an unrelated
// process the moment it is reaped.
Value fn_proc_alive(NativeCtx&, std::vector<Value>& args, std::string& error) {
    ProcRegistry::instance().reap_orphans();

    auto e = ProcRegistry::instance().get(args[0].as_int());
    if (!e) { error = "proc: unknown handle"; return Value::null(); }

    std::lock_guard<std::mutex> lock(e->mutex);
    if (e->reaped) return Value::boolean(false);

    int status = 0;
    pid_t r = ::waitpid(e->pid, &status, WNOHANG);
    if (r == e->pid) {
        e->reaped    = true;
        e->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return Value::boolean(false);
    }
    if (r < 0) {
        e->reaped    = true;
        e->exit_code = -1; // gone, but we do not know how -- ECHILD or similar
        return Value::boolean(false);
    }
    return Value::boolean(true); // r == 0: still running
}

// ─── read() ──────────────────────────────────────────────────────────────────

// poll() with `timeout_ms` as the deadline, then at most one read() --
// exactly the drain step os.cpp's run() already uses (os.cpp's own
// comment on its drain loop), just one call at a time instead of a loop
// that runs to EOF internally: the caller here IS the loop, across
// however many separate proc.read() calls it takes.
Value fn_proc_read(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const long long max_bytes  = args[1].as_int();
    const long long timeout_ms = args[2].as_int();
    if (max_bytes <= 0) { error = "proc.read(): max_bytes must be positive"; return Value::null(); }
    if (timeout_ms < 0 || timeout_ms > kMaxTimeoutMs) {
        error = "proc.read(): timeout must be between 0 and " + std::to_string(kMaxTimeoutMs) + "ms";
        return Value::null();
    }

    auto e = ProcRegistry::instance().get(args[0].as_int());
    if (!e) { error = "proc: unknown handle"; return Value::null(); }

    std::lock_guard<std::mutex> lock(e->mutex);
    if (e->stdout_fd < 0) {
        error = "proc.read(): this process was not started with stdout: \"pipe\"";
        return Value::null();
    }

    struct pollfd pfd { e->stdout_fd, POLLIN, 0 };
    int pr = poll(&pfd, 1, static_cast<int>(timeout_ms));
    if (pr < 0) {
        if (errno == EINTR) return Value::str(""); // caller retries, same as a plain timeout
        error = std::string("proc.read(): poll failed: ") + std::strerror(errno);
        return Value::null();
    }
    if (pr == 0) return Value::str(""); // timed out, no data yet -- process may still be running

    std::string buf(std::min<size_t>(static_cast<size_t>(max_bytes), kMaxReadChunk), '\0');
    ssize_t n = ::read(e->stdout_fd, buf.data(), buf.size());
    if (n > 0) { buf.resize(static_cast<size_t>(n)); return Value::str(std::move(buf)); }
    if (n == 0) return Value::null(); // EOF: the process closed stdout (exited, or closed it early)
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return Value::str(""); // spurious wake
    error = std::string("proc.read(): read failed: ") + std::strerror(errno);
    return Value::null();
}

// ─── wait() ──────────────────────────────────────────────────────────────────

// A plain blocking waitpid(pid, &status, 0) cannot honor a timeout at all
// -- it returns only when the child exits, however long that takes. This
// polls with WNOHANG in a short-sleep loop instead, exactly like os.cpp's
// run() polls its pipes rather than doing two sequential blocking reads.
Value fn_proc_wait(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const long long timeout_ms = args[1].as_int();
    if (timeout_ms < 0 || timeout_ms > kMaxTimeoutMs) {
        error = "proc.wait(): timeout must be between 0 and " + std::to_string(kMaxTimeoutMs) + "ms";
        return Value::null();
    }

    ProcRegistry::instance().reap_orphans();

    auto e = ProcRegistry::instance().get(args[0].as_int());
    if (!e) { error = "proc: unknown handle"; return Value::null(); }

    std::lock_guard<std::mutex> lock(e->mutex);
    if (e->reaped) return Value::integer(e->exit_code);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        pid_t r = ::waitpid(e->pid, &status, WNOHANG);
        if (r == e->pid) {
            e->reaped    = true;
            e->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return Value::integer(e->exit_code);
        }
        if (r < 0) {
            e->reaped    = true;
            e->exit_code = -1;
            return Value::integer(e->exit_code);
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return Value::null(); // still running when the deadline hit
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min<long long>(remaining, kWaitPollStepMs)));
    }
}

// ─── kill() ──────────────────────────────────────────────────────────────────

// Only signals -- never waits to see whether it actually died (that is
// what a follow-up wait() is for: "kill(); wait(h, 5000); if still alive,
// kill(SIGKILL)" is the documented pattern, not something this function
// does on the caller's behalf, since a caller that only ever wants to ask
// nicely -- SIGTERM, then its own retry policy -- should not be forced
// into this module's idea of how long is long enough).
Value fn_proc_kill(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const int sig = args.size() > 1 ? static_cast<int>(args[1].as_int()) : SIGTERM;

    auto e = ProcRegistry::instance().get(args[0].as_int());
    if (!e) { error = "proc: unknown handle"; return Value::null(); }

    std::lock_guard<std::mutex> lock(e->mutex);
    if (e->reaped) return Value::boolean(false); // already gone -- nothing to signal
    return Value::boolean(::kill(e->pid, sig) == 0);
}

// ─── close() ─────────────────────────────────────────────────────────────────

Value fn_proc_close(NativeCtx&, std::vector<Value>& args, std::string&) {
    ProcRegistry::instance().close(args[0].as_int());
    return Value::null();
}

} // namespace

LUX_MODULE(proc, {
    {"start", "s|ld", fn_proc_start},
    {"alive", "i",    fn_proc_alive},
    {"write", "iS",   fn_proc_write},
    // Both block their worker for up to timeout_ms (kMaxTimeoutMs).
    {"read",  "iii",  fn_proc_read,  /*is_async=*/true},
    {"wait",  "ii",   fn_proc_wait,  /*is_async=*/true},
    {"kill",  "i|i",  fn_proc_kill},
    {"close", "i",    fn_proc_close},
})

} // namespace lux_script
