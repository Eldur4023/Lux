// System-access module (NATIVE-MODULES.md): environment variables, paths,
// file I/O, and running external commands -- Lux Script's answer to "we
// could use OS/system libraries like Python has" (os, os.path, subprocess).
// Zero third-party dependencies: everything here is C++17 <filesystem> and
// POSIX, so, like `hash`/`csv`, this module is unconditionally compiled in
// (builtin_module.cpp), no cmake option needed.
//
// Synchronous by default (NATIVE-MODULES.md §2) -- most of this module pays
// nothing for that (env lookups, path string manipulation, a `stat()` are
// all microseconds). Two functions are real, not theoretical, exceptions,
// exactly the same shape `http`'s comment already documents, and are marked
// `is_async` (BuiltinModuleFn::is_async) below so `await` runs them on
// lux::blocking_pool() instead of the event loop thread:
//   - read_file()/write_file() do real disk I/O -- normally fast (page-cache
//     backed) but not bounded, same cost class the project already accepts
//     for static file serving (app.cpp) and template loading (template.cpp).
//   - run() launches and waits on an external process, whose duration is
//     entirely outside Lux's control -- bounded here by a fixed timeout
//     (see kRunTimeoutMs) so a hung child cannot pin its worker forever,
//     the same mitigation http.* uses for a hung remote server.
//
// Security posture, stated plainly rather than left implicit: this module
// trusts the caller exactly as much as Python's `os`/`open()`/`subprocess`
// do -- there is no path sandboxing (no configured "root" a path is
// confined to, unlike static file serving's canonical-root check in
// app.cpp) and run() takes a command plus an argv LIST, never a shell
// string, specifically so it can never be tricked into shell metacharacter
// injection the way `os.system("cmd " + user_input)` can in Python. Passing
// unsanitized user input as a path or as an argv element is exactly as
// dangerous here as it is in Python, and exactly as much the caller's
// responsibility to avoid.
#include <lux_script/builtin_module.hpp>

#include <spawn.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

extern char** environ;

namespace lux_script {

namespace {

namespace fs = std::filesystem;

// ─── Environment & paths ────────────────────────────────────────────────────

Value fn_os_getenv(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.getenv() expects a name"; return Value::null(); }
    const char* v = std::getenv(args[0].as_str().c_str());
    if (v) return Value::str(std::string(v));
    // A default is a normal, expected outcome (matching Python's
    // os.getenv(name, default)), not an error -- and with no default,
    // an unset variable is null, not an error either: the whole point of
    // getenv() over a hypothetical getenv_required() is to let the script
    // decide what an absent variable means.
    if (args.size() > 1 && args[1].is_str()) return Value::str(args[1].as_str());
    return Value::null();
}

Value fn_os_cwd(NativeCtx&, std::vector<Value>&, std::string& error) {
    std::error_code ec;
    fs::path p = fs::current_path(ec);
    if (ec) { error = "os.cwd(): " + ec.message(); return Value::null(); }
    return Value::str(p.string());
}

// Variadic, min 1 arg, no cap -- os.path_join("a", "b", "c") -> "a/b/c",
// matching Python's os.path.join(*parts).
Value fn_os_path_join(NativeCtx&, std::vector<Value>& args, std::string& error) {
    fs::path out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (!args[i].is_str()) { error = "os.path_join(): every argument must be a string"; return Value::null(); }
        out /= args[i].as_str();
    }
    return Value::str(out.string());
}

Value fn_os_path_exists(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.path_exists() expects a path"; return Value::null(); }
    std::error_code ec;
    return Value::boolean(fs::exists(fs::path(args[0].as_str()), ec));
}

// A dangling symlink is neither -- is_directory()/is_regular_file() both
// come back false for one, matching what those two functions already do
// with a broken symlink (they follow it, find nothing, and report false
// rather than throwing), not a special case handled here.
Value fn_os_is_dir(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.is_dir() expects a path"; return Value::null(); }
    std::error_code ec;
    return Value::boolean(fs::is_directory(fs::path(args[0].as_str()), ec));
}

Value fn_os_is_file(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.is_file() expects a path"; return Value::null(); }
    std::error_code ec;
    return Value::boolean(fs::is_regular_file(fs::path(args[0].as_str()), ec));
}

// -1 rather than null (unlike read_file()): the result is an int, and null
// would force every caller to check for null BEFORE it could compare/add
// the size -- the same reason remove_file() below returns false rather
// than null when the file was already gone. -1 is never a real size, so it
// stays unambiguous. A directory (fs::file_size() fails on one, `ec` gets
// set) is -1 too, not some arbitrary filesystem-reported size.
Value fn_os_file_size(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.file_size() expects a path"; return Value::null(); }
    std::error_code ec;
    auto sz = fs::file_size(fs::path(args[0].as_str()), ec);
    return Value::integer(ec ? -1 : static_cast<long long>(sz));
}

Value fn_os_mtime_ms(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.mtime_ms() expects a path"; return Value::null(); }
    std::error_code ec;
    auto ftime = fs::last_write_time(fs::path(args[0].as_str()), ec);
    if (ec) return Value::integer(-1);
    // file_time_type's epoch is unspecified pre-C++20 but IS the system
    // clock's epoch from C++20 on (the guarantee this project already
    // depends on -- CMakeLists.txt requires C++20): clock_cast to
    // system_clock is the standard, portable way to get a real Unix
    // timestamp out of it, not the file_clock::to_sys() shim some libstdc++
    // versions add ad hoc.
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch());
    return Value::integer(static_cast<long long>(ms.count()));
}

Value fn_os_path_basename(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.path_basename() expects a path"; return Value::null(); }
    return Value::str(fs::path(args[0].as_str()).filename().string());
}

Value fn_os_path_dirname(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.path_dirname() expects a path"; return Value::null(); }
    return Value::str(fs::path(args[0].as_str()).parent_path().string());
}

Value fn_os_path_abs(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.path_abs() expects a path"; return Value::null(); }
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(args[0].as_str()), ec);
    if (ec) { error = "os.path_abs(): " + ec.message(); return Value::null(); }
    // lexically_normal collapses "a/./b/../c" into "a/c" -- absolute() alone
    // does not, and a caller asking for an absolute path almost always wants
    // it in the form they can compare/display, not one still carrying "..".
    return Value::str(p.lexically_normal().string());
}

// ─── File I/O ───────────────────────────────────────────────────────────────

// Missing/unreadable is null, not an error: the same "absent is a normal
// outcome the script decides about" reasoning as getenv() above, and
// consistent with the project's existing precedent for "not found" (a DB
// query with no matching row already comes back as an empty List, not a
// thrown error -- see GUIDE.md's "no encontrado" pattern) rather than every
// absence being a hard failure. A genuine system error (permission denied on
// a path that DOES exist, for instance) still goes through `error`.
Value fn_os_read_file(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.read_file() expects a path"; return Value::null(); }
    const std::string& path = args[0].as_str();
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) return Value::null();
    std::ifstream f(path, std::ios::binary);
    if (!f) return Value::null();
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) { error = "os.read_file(): could not read '" + path + "'"; return Value::null(); }
    return Value::str(ss.str());
}

Value fn_os_write_file(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.write_file() expects a path"; return Value::null(); }
    if (!args[1].is_str()) { error = "os.write_file() expects the content as a string"; return Value::null(); }
    std::ofstream f(args[0].as_str(), std::ios::binary | std::ios::trunc);
    if (!f) { error = "os.write_file(): could not open '" + args[0].as_str() + "' for writing"; return Value::null(); }
    f << args[1].as_str();
    if (f.bad()) { error = "os.write_file(): write failed for '" + args[0].as_str() + "'"; return Value::null(); }
    return Value::boolean(true);
}

// Not recursive/not a directory listing tool -- one level, like Python's
// os.listdir(), entry names only (not full paths): the caller path_joins
// with the original directory if it wants full paths, exactly as Python
// leaves it to the caller too.
Value fn_os_list_dir(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.list_dir() expects a path"; return Value::null(); }
    const std::string& path = args[0].as_str();
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) return Value::null();
    Value::List out;
    for (const auto& entry : fs::directory_iterator(path, ec))
        out.push_back(Value::str(entry.path().filename().string()));
    if (ec) { error = "os.list_dir(): " + ec.message(); return Value::null(); }
    return Value::list(std::move(out));
}

// Files only -- mirroring Python's own os.remove()/os.rmdir() split
// (and shutil.rmtree() as the separate, explicitly-named "yes, really,
// recursively" operation). Directory removal is remove_dir(), below,
// which keeps the same split: empty-only by default, recursive only on
// explicit request.
Value fn_os_remove_file(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.remove_file() expects a path"; return Value::null(); }
    const std::string& path = args[0].as_str();
    std::error_code ec;
    if (!fs::exists(path, ec)) return Value::boolean(false); // already gone: not an error
    if (!fs::is_regular_file(path, ec)) {
        error = "os.remove_file(): '" + path + "' is not a regular file (use os.remove_dir() for directories)";
        return Value::null();
    }
    bool removed = fs::remove(path, ec);
    if (ec) { error = "os.remove_file(): " + ec.message(); return Value::null(); }
    return Value::boolean(removed);
}

// Creates intermediate directories too, like Python's os.makedirs() (not
// the stricter os.mkdir(), which fails if the parent is missing) -- the
// more forgiving of the two is the more useful default for a web app
// ensuring an upload/cache directory exists before writing into it.
Value fn_os_make_dir(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.make_dir() expects a path"; return Value::null(); }
    std::error_code ec;
    bool created = fs::create_directories(args[0].as_str(), ec);
    if (ec) { error = "os.make_dir(): " + ec.message(); return Value::null(); }
    return Value::boolean(created);
}

// Empty-only by default -- mirroring Python's os.rmdir() vs shutil.rmtree()
// split from remove_file()'s own comment above, but as one function with
// an explicit second argument rather than two names: a directory
// legitimately might or might not be empty, and the caller already has to
// know which one it means before calling this. Without `recursive=true`,
// an unexpectedly non-empty directory fails loudly with a message that
// says exactly how to actually remove it, instead of a plain function
// silently taking everything inside it along for the ride.
Value fn_os_remove_dir(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.remove_dir() expects a path"; return Value::null(); }
    const std::string& path = args[0].as_str();
    bool recursive = false;
    if (args.size() > 1) {
        if (!args[1].is_bool()) { error = "os.remove_dir(): the second argument is a bool (recursive)"; return Value::null(); }
        recursive = args[1].as_bool();
    }
    std::error_code ec;
    if (!fs::exists(path, ec)) return Value::boolean(false); // already gone: not an error
    if (!fs::is_directory(path, ec)) {
        error = "os.remove_dir(): '" + path + "' is not a directory (use os.remove_file() for files)";
        return Value::null();
    }
    if (recursive) {
        auto removed = fs::remove_all(path, ec);
        if (ec) { error = "os.remove_dir(): " + ec.message(); return Value::null(); }
        return Value::boolean(removed > 0);
    }
    bool removed = fs::remove(path, ec);
    if (ec) {
        if (ec == std::errc::directory_not_empty) {
            error = "os.remove_dir(): '" + path + "' is not empty -- pass true as the "
                    "second argument to remove it and everything inside";
            return Value::null();
        }
        error = "os.remove_dir(): " + ec.message();
        return Value::null();
    }
    return Value::boolean(removed);
}

// Files only, like remove_file()/make_dir() above -- a recursive directory
// copy (shutil.copytree()'s equivalent) is a separate, bigger decision
// (what an already-existing destination tree means) not exposed yet.
// fs::copy_file() rather than read_file()+write_file(): that pair loads
// the WHOLE file into memory as a Lux Script string first, which for a
// large media file is a needless extra copy that a plain file-to-file
// copy at the OS level does not pay.
Value fn_os_copy_file(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.copy_file() expects the source path"; return Value::null(); }
    if (!args[1].is_str()) { error = "os.copy_file() expects the destination path as the second argument"; return Value::null(); }
    const std::string& from = args[0].as_str();
    const std::string& to   = args[1].as_str();
    bool overwrite = false;
    if (args.size() > 2) {
        if (!args[2].is_bool()) { error = "os.copy_file(): the third argument is a bool (overwrite)"; return Value::null(); }
        overwrite = args[2].as_bool();
    }
    std::error_code ec;
    if (!fs::is_regular_file(from, ec)) {
        error = "os.copy_file(): '" + from + "' is not a regular file";
        return Value::null();
    }
    auto opts = overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none;
    bool copied = fs::copy_file(from, to, opts, ec);
    if (ec) { error = "os.copy_file(): " + ec.message(); return Value::null(); }
    return Value::boolean(copied);
}

// Renames/moves a file or directory, working across filesystems too --
// which a plain fs::rename() (a single rename(2) syscall) cannot do: it
// fails with EXDEV the moment source and destination are on different
// mount points, and that split is *common*, not an edge case, for exactly
// the app this was requested for (a downloads volume and a media library
// volume are routinely separate mounts in a container/NAS setup). The
// fast, atomic rename(2) is still tried FIRST and used whenever it works
// (same filesystem: instant, and nothing is ever partially moved); only
// on EXDEV does this fall back to copy-then-delete-the-source, mirroring
// Python's shutil.move() for the same reason it exists. That fallback is
// NOT atomic -- a crash between the copy and the delete leaves both
// copies on disk -- an inherent limitation of moving across filesystems
// at all, not something this function does worse than any other.
Value fn_os_move(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.move() expects the source path"; return Value::null(); }
    if (!args[1].is_str()) { error = "os.move() expects the destination path as the second argument"; return Value::null(); }
    const std::string& from = args[0].as_str();
    const std::string& to   = args[1].as_str();

    std::error_code ec;
    if (!fs::exists(from, ec)) {
        error = "os.move(): '" + from + "' does not exist";
        return Value::null();
    }

    fs::rename(from, to, ec);
    if (!ec) return Value::boolean(true);
    if (ec != std::errc::cross_device_link) {
        error = "os.move(): " + ec.message();
        return Value::null();
    }

    // Crossing filesystems: copy first, only remove the source once the
    // copy fully succeeded, so a failed/interrupted copy never loses the
    // original.
    bool is_dir = fs::is_directory(from, ec);
    if (is_dir) {
        fs::copy(from, to, fs::copy_options::recursive, ec);
    } else {
        fs::copy_file(from, to, ec);
    }
    if (ec) { error = "os.move(): " + ec.message(); return Value::null(); }

    if (is_dir) fs::remove_all(from, ec);
    else        fs::remove(from, ec);
    if (ec) {
        error = "os.move(): copied to '" + to + "' but could not remove the original "
                "'" + from + "': " + ec.message();
        return Value::null();
    }
    return Value::boolean(true);
}

// ─── Process spawning ───────────────────────────────────────────────────────

// Bounded the same way http.*'s remote-server call is (http.cpp):
// there is no way to interrupt a blocking wait from outside once it starts,
// so a hard ceiling has to exist before the child is even spawned. 15s
// matches http's own timeout for the same reason -- neither is trying to
// be a job queue for long-running work, both exist to keep an event-loop
// thread from being pinned indefinitely by something outside Lux's
// control.
constexpr int kRunTimeoutMs = 15000;

// posix_spawn(), not fork()+exec(): fork() in an already-multithreaded
// process only allows async-signal-safe calls in the child before exec(),
// which rules out almost all of the C++ standard library -- posix_spawn()
// exists specifically so a multithreaded program never has to get that
// right by hand. Never goes through a shell (no posix_spawn "-c", argv is
// built directly from the caller's list) -- see the module-level comment on
// why that is a deliberate security property, not just an implementation
// detail.
Value fn_os_run(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "os.run() expects a command"; return Value::null(); }
    const std::string command = args[0].as_str();

    std::vector<std::string> argv_storage;
    argv_storage.push_back(command);
    if (args.size() > 1) {
        if (!args[1].is_list()) { error = "os.run(): second argument must be a List of strings"; return Value::null(); }
        for (const auto& a : args[1].as_list()) {
            if (!a.is_str()) { error = "os.run(): every element of the argument list must be a string"; return Value::null(); }
            argv_storage.push_back(a.as_str());
        }
    }
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& s : argv_storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0) { error = "os.run(): could not create a pipe"; return Value::null(); }
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        error = "os.run(): could not create a pipe";
        return Value::null();
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[1]);

    pid_t pid = -1;
    int rc = posix_spawnp(&pid, command.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);
    close(err_pipe[1]);

    if (rc != 0) {
        close(out_pipe[0]); close(err_pipe[0]);
        error = "os.run(): could not start '" + command + "': " + std::strerror(rc);
        return Value::null();
    }

    // Drain both pipes with a deadline -- poll() rather than two blocking
    // reads in sequence, so a child that writes slowly to stderr while
    // stdout sits idle (or vice versa) cannot stall one read while the
    // other pipe's buffer fills and blocks the CHILD instead. On timeout,
    // the child is killed outright: there is no partial/best-effort result
    // to salvage from a process that overran the bound.
    std::string out_data, err_data;
    bool out_open = true, err_open = true;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRunTimeoutMs);
    bool timed_out = false;

    while (out_open || err_open) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { timed_out = true; break; }

        struct pollfd fds[2];
        int nfds = 0;
        int out_idx = -1, err_idx = -1;
        if (out_open) { fds[nfds] = {out_pipe[0], POLLIN, 0}; out_idx = nfds++; }
        if (err_open) { fds[nfds] = {err_pipe[0], POLLIN, 0}; err_idx = nfds++; }

        int pr = poll(fds, static_cast<nfds_t>(nfds), static_cast<int>(remaining));
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) { timed_out = true; break; }

        if (out_idx >= 0 && (fds[out_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0) out_data.append(buf, static_cast<size_t>(n));
            else out_open = false;
        }
        if (err_idx >= 0 && (fds[err_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(err_pipe[0], buf, sizeof(buf));
            if (n > 0) err_data.append(buf, static_cast<size_t>(n));
            else err_open = false;
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        error = "os.run(): '" + command + "' timed out after " + std::to_string(kRunTimeoutMs) + "ms";
        return Value::null();
    }
    waitpid(pid, &status, 0);

    Value::Dict out;
    out["status"] = Value::integer(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    out["stdout"] = Value::str(std::move(out_data));
    out["stderr"] = Value::str(std::move(err_data));
    return Value::dict(std::move(out));
}

class OsModule : public BuiltinModule {
public:
    const char* name() const override { return "os"; }

    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"getenv",       1, 2, fn_os_getenv},
            {"cwd",          0, 0, fn_os_cwd},
            {"path_join",    1, -1, fn_os_path_join},
            {"path_exists",  1, 1, fn_os_path_exists},
            {"is_dir",       1, 1, fn_os_is_dir},
            {"is_file",      1, 1, fn_os_is_file},
            {"file_size",    1, 1, fn_os_file_size},
            {"mtime_ms",     1, 1, fn_os_mtime_ms},
            {"path_basename",1, 1, fn_os_path_basename},
            {"path_dirname", 1, 1, fn_os_path_dirname},
            {"path_abs",     1, 1, fn_os_path_abs},
            // is_async: real, unbounded disk/process I/O -- see the module
            // comment at the top of this file and BuiltinModuleFn::is_async.
            {"read_file",    1, 1, fn_os_read_file,  /*is_async=*/true},
            {"write_file",   2, 2, fn_os_write_file, /*is_async=*/true},
            {"list_dir",     1, 1, fn_os_list_dir},
            {"remove_file",  1, 1, fn_os_remove_file},
            {"remove_dir",   1, 2, fn_os_remove_dir},
            {"make_dir",     1, 1, fn_os_make_dir},
            // is_async: real disk I/O moving file CONTENT, same class as
            // read_file/write_file above -- not just metadata like
            // remove_dir/make_dir/rename's own fast path.
            {"copy_file",    2, 3, fn_os_copy_file,   /*is_async=*/true},
            {"move",         2, 2, fn_os_move,        /*is_async=*/true},
            {"run",          1, 2, fn_os_run,        /*is_async=*/true},
        };
        return fns;
    }
};

} // namespace

LUX_REGISTER_MODULE(OsModule)

} // namespace lux_script
