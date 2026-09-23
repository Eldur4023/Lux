// Live-handle subprocess module (NATIVE-MODULES.md): start a subprocess and
// keep talking to it across multiple calls -- unlike os.run() (os.cpp),
// which spawns, drains both pipes to completion, and waits, all inside one
// blocking call. That shape covers a command that runs to completion in
// under os.run()'s own 15s ceiling and hands its whole output back at
// once; it has no way to express "start this now, keep it running for the
// next two hours, and let a LATER, DIFFERENT request read from it, check
// on it, or kill it" -- exactly the shape a transcoding session (ffmpeg
// writing HLS segments while a viewer's browser polls playback, a seek
// killing and restarting it, a background re-encode a status page polls
// for hours) actually needs. `start()` returns an opaque `int` handle
// immediately, without waiting for the child to write anything or exit;
// `read()`/`alive()`/`wait()`/`kill()`/`close()` operate on that handle
// from whatever later request needs to.
//
// This is the exact same cross-request-lifetime problem `rooms` (rooms.cpp)
// solves for WebSocket connections, solved the same way: a mutex-protected
// registry keyed by an opaque int handle (ProcRegistry here, the same
// pattern `csv`/`pdf`'s HandleTable already uses for their own handles).
// It needs LESS care than rooms.cpp's weak_ptr/lifetime tracking, though:
// a WSConnection can vanish out from under a room's member list on its
// own (the browser tab closes) with nothing else involved, so rooms.cpp
// has to hold only a WEAK reference and cope with it going stale at any
// moment. A pid_t + fd have no such owner -- they stay valid until THIS
// module explicitly closes them, never because something else decided to
// let go, so a plain shared_ptr the registry hands out under its own lock
// (kept alive for the duration of one call, exactly the csv.cpp/pdf.cpp
// HandleTable::get() pattern) is enough.
#include <lux_script/builtin_module.hpp>

#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

extern char** environ;

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
    bool       reaped     = false;
    int        exit_code  = -1;    // valid only once `reaped` is true
};

class ProcRegistry {
public:
    static ProcRegistry& instance() {
        static ProcRegistry r;
        return r;
    }

    int put(pid_t pid, int stdout_fd) {
        auto e = std::make_shared<ProcEntry>();
        e->pid       = pid;
        e->stdout_fd = stdout_fd;
        std::lock_guard<std::mutex> lock(mutex_);
        int id = next_id_++;
        procs_.emplace(id, std::move(e));
        return id;
    }

    // Kept alive for the caller's whole call via the returned shared_ptr,
    // even if close() erases it from `procs_` concurrently -- see the
    // module comment on why that is enough here, unlike rooms.cpp.
    std::shared_ptr<ProcEntry> get(int handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = procs_.find(handle);
        return it == procs_.end() ? nullptr : it->second;
    }

    void close(int handle) {
        std::shared_ptr<ProcEntry> e;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = procs_.find(handle);
            if (it == procs_.end()) return;
            e = it->second;
            procs_.erase(it);
        }
        std::lock_guard<std::mutex> elock(e->mutex);
        if (e->stdout_fd >= 0) { ::close(e->stdout_fd); e->stdout_fd = -1; }
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
    std::mutex                                   mutex_;
    std::unordered_map<int, std::shared_ptr<ProcEntry>> procs_;
    int                                           next_id_ = 1;

    std::mutex           orphan_mutex_;
    std::vector<pid_t>   orphan_pids_;
};

// ─── start() ─────────────────────────────────────────────────────────────────

// posix_spawn(), not fork()+exec() -- same reason os.cpp's run() gives:
// fork() in an already-multithreaded process only allows async-signal-safe
// calls in the child before exec(). Never goes through a shell (argv is
// built directly from the caller's List, exactly like os.run()) -- same
// deliberate anti-injection property, not an oversight.
Value fn_proc_start(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "proc.start() expects a command"; return Value::null(); }
    const std::string command = args[0].as_str();

    std::vector<std::string> argv_storage;
    argv_storage.push_back(command);
    if (args.size() > 1) {
        if (!args[1].is_list()) {
            error = "proc.start(): second argument must be a List of strings";
            return Value::null();
        }
        for (const auto& a : args[1].as_list()) {
            if (!a.is_str()) {
                error = "proc.start(): every element of the argument list must be a string";
                return Value::null();
            }
            argv_storage.push_back(a.as_str());
        }
    }

    std::string stdout_mode = "pipe";
    std::string stderr_mode = "null";
    if (args.size() > 2) {
        if (!args[2].is_dict()) {
            error = "proc.start(): third argument must be a Dict";
            return Value::null();
        }
        const auto& opts = args[2].as_dict();
        if (auto it = opts.find("stdout"); it != opts.end()) {
            if (!it->second.is_str()) { error = "proc.start(): options.stdout must be a string"; return Value::null(); }
            stdout_mode = it->second.as_str();
        }
        if (auto it = opts.find("stderr"); it != opts.end()) {
            if (!it->second.is_str()) { error = "proc.start(): options.stderr must be a string"; return Value::null(); }
            stderr_mode = it->second.as_str();
        }
    }
    // Only stdout can ever be read back (proc.read() has no "which stream"
    // argument): a stderr pipe nobody drains fills its kernel buffer and
    // then blocks the CHILD's writes to it forever, a silent hang with no
    // native-side symptom to point at. Rejecting it here is a compile-time-
    // shaped error at the one place that can still catch it -- runtime,
    // since `options` is a plain Dict, not something the type checker sees.
    if (stderr_mode == "pipe") {
        error = "proc.start(): options.stderr does not support \"pipe\" -- nothing in "
                "this module reads it; use a file path or \"null\"";
        return Value::null();
    }

    ProcRegistry::instance().reap_orphans();

    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& s : argv_storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    int stdout_read_fd = -1, stdout_write_fd = -1;
    if (stdout_mode == "pipe") {
        // O_CLOEXEC here (not just the addclose() file actions below, which
        // only apply to THIS child): without it, another proc.start()/
        // os.run() call racing on a different thread can fork (via ITS OWN
        // posix_spawn()) in the window before this one's posix_spawn() runs
        // below, inheriting these fds into an unrelated child that then
        // holds this pipe's write end open indefinitely -- same failure
        // mode as os.run()'s identical fix, see its comment (os.cpp).
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) { error = "proc.start(): could not create a pipe"; return Value::null(); }
        // Non-blocking on OUR (read) end only -- pipe(2) gives each end its
        // own open file description, so this has no effect on the CHILD's
        // write end (dup2'd from fds[1] below), which stays perfectly
        // normal/blocking from the child's point of view. Without this,
        // read() (see fn_proc_read) could block past its own poll() call
        // in the window between poll() saying "readable" and read()
        // actually running -- see that function's comment.
        if (fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0) {
            close(fds[0]); close(fds[1]);
            error = "proc.start(): could not set the pipe non-blocking";
            return Value::null();
        }
        stdout_read_fd  = fds[0];
        stdout_write_fd = fds[1];
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    if (stdout_mode == "pipe") {
        posix_spawn_file_actions_adddup2(&actions, stdout_write_fd, STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, stdout_read_fd);
        posix_spawn_file_actions_addclose(&actions, stdout_write_fd);
    } else if (stdout_mode == "null") {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_mode.c_str(),
                                         O_WRONLY | O_CREAT | O_APPEND, 0644);
    }

    if (stderr_mode == "null") {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_mode.c_str(),
                                         O_WRONLY | O_CREAT | O_APPEND, 0644);
    }

    pid_t pid = -1;
    int   rc  = posix_spawnp(&pid, command.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (stdout_write_fd >= 0) close(stdout_write_fd); // only the child needs the write end

    if (rc != 0) {
        if (stdout_read_fd >= 0) close(stdout_read_fd);
        error = "proc.start(): could not start '" + command + "': " + std::strerror(rc);
        return Value::null();
    }

    return Value::integer(ProcRegistry::instance().put(pid, stdout_read_fd));
}

// ─── alive() ─────────────────────────────────────────────────────────────────

// waitpid(WNOHANG): never blocks. If the child just exited, the exit code
// is cached on `e` right here so a later wait() never has to waitpid() a
// pid the kernel already reaped out from under it -- doing that a second
// time is not "returns nothing new", it is an actual error (ECHILD),
// because a reaped pid can be recycled by the kernel for an unrelated
// process the moment it is reaped.
Value fn_proc_alive(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) { error = "proc.alive() expects a handle"; return Value::null(); }
    ProcRegistry::instance().reap_orphans();

    auto e = ProcRegistry::instance().get(static_cast<int>(args[0].as_int()));
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
    if (!args[0].is_int()) { error = "proc.read() expects a handle"; return Value::null(); }
    if (!args[1].is_int()) { error = "proc.read() expects max_bytes"; return Value::null(); }
    if (!args[2].is_int()) { error = "proc.read() expects a timeout in milliseconds"; return Value::null(); }
    const long long max_bytes  = args[1].as_int();
    const long long timeout_ms = args[2].as_int();
    if (max_bytes <= 0) { error = "proc.read(): max_bytes must be positive"; return Value::null(); }
    if (timeout_ms < 0 || timeout_ms > kMaxTimeoutMs) {
        error = "proc.read(): timeout must be between 0 and " + std::to_string(kMaxTimeoutMs) + "ms";
        return Value::null();
    }

    auto e = ProcRegistry::instance().get(static_cast<int>(args[0].as_int()));
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
    if (!args[0].is_int()) { error = "proc.wait() expects a handle"; return Value::null(); }
    if (!args[1].is_int()) { error = "proc.wait() expects a timeout in milliseconds"; return Value::null(); }
    const long long timeout_ms = args[1].as_int();
    if (timeout_ms < 0 || timeout_ms > kMaxTimeoutMs) {
        error = "proc.wait(): timeout must be between 0 and " + std::to_string(kMaxTimeoutMs) + "ms";
        return Value::null();
    }

    ProcRegistry::instance().reap_orphans();

    auto e = ProcRegistry::instance().get(static_cast<int>(args[0].as_int()));
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
    if (!args[0].is_int()) { error = "proc.kill() expects a handle"; return Value::null(); }
    int sig = SIGTERM;
    if (args.size() > 1) {
        if (!args[1].is_int()) { error = "proc.kill(): signal must be an int"; return Value::null(); }
        sig = static_cast<int>(args[1].as_int());
    }

    auto e = ProcRegistry::instance().get(static_cast<int>(args[0].as_int()));
    if (!e) { error = "proc: unknown handle"; return Value::null(); }

    std::lock_guard<std::mutex> lock(e->mutex);
    if (e->reaped) return Value::boolean(false); // already gone -- nothing to signal
    return Value::boolean(::kill(e->pid, sig) == 0);
}

// ─── close() ─────────────────────────────────────────────────────────────────

Value fn_proc_close(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) { error = "proc.close() expects a handle"; return Value::null(); }
    ProcRegistry::instance().close(static_cast<int>(args[0].as_int()));
    return Value::null();
}

class ProcModule : public BuiltinModule {
public:
    const char* name() const override { return "proc"; }

    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"start", 1, 3, fn_proc_start},
            {"alive", 1, 1, fn_proc_alive},
            // is_async: both block the calling worker for up to
            // `timeout_ms` -- see kMaxTimeoutMs's comment.
            {"read",  3, 3, fn_proc_read,  /*is_async=*/true},
            {"wait",  2, 2, fn_proc_wait,  /*is_async=*/true},
            {"kill",  1, 2, fn_proc_kill},
            {"close", 1, 1, fn_proc_close},
        };
        return fns;
    }
};

} // namespace

LUX_REGISTER_MODULE(ProcModule)

} // namespace lux_script
