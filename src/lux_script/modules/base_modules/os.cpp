// Environment, paths, files and running commands -- Python's os, os.path,
// shutil, glob, tempfile, mimetypes and subprocess.run, in one module.
//
// Most of it is a stat() or string work and runs inline. What moves file
// content or waits on a process is is_async (runs on the I/O pool, needs
// `await`). No sandboxing: a path or an argument built from user input is
// as dangerous here as in Python -- run() at least never uses a shell.
#include <lux_script/builtin_module.hpp>
#include <lux/mime.hpp>

#include "spawn.hpp"

#include <fcntl.h>
#include <termios.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace lux_script {

namespace {

namespace fs = std::filesystem;

Value fail(std::string& error, const std::string& fn, const std::string& what) {
    error = "os." + fn + "(): " + what;
    return Value::null();
}

// ─── Environment & paths ────────────────────────────────────────────────────

// Unset is null (or the default): the script decides what absence means.
Value fn_getenv(NativeCtx&, std::vector<Value>& a, std::string&) {
    const char* v = std::getenv(a[0].as_str().c_str());
    return v ? Value::str(v) : a.size() > 1 ? a[1] : Value::null();
}

Value fn_cwd(NativeCtx&, std::vector<Value>&, std::string& error) {
    std::error_code ec;
    fs::path p = fs::current_path(ec);
    return ec ? fail(error, "cwd", ec.message()) : Value::str(p.string());
}

Value fn_path_join(NativeCtx&, std::vector<Value>& a, std::string&) {
    fs::path out;
    for (const Value& v : a) out /= v.to_string();
    return Value::str(out.string());
}

Value fn_path_basename(NativeCtx&, std::vector<Value>& a, std::string&) { return Value::str(fs::path(a[0].as_str()).filename().string()); }
Value fn_path_dirname(NativeCtx&, std::vector<Value>& a, std::string&)  { return Value::str(fs::path(a[0].as_str()).parent_path().string()); }
// ".jpg" for "a/b.JPG" -> ".JPG"; "" when there is none.
Value fn_path_ext(NativeCtx&, std::vector<Value>& a, std::string&)      { return Value::str(fs::path(a[0].as_str()).extension().string()); }
Value fn_mime(NativeCtx&, std::vector<Value>& a, std::string&)          { return Value::str(lux::mime_for_ext(fs::path(a[0].as_str()).extension().string())); }

// Absolute and normalised: "a/./b/../c" -> "/cwd/a/c".
Value fn_path_abs(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::error_code ec;
    fs::path p = fs::absolute(a[0].as_str(), ec);
    return ec ? fail(error, "path_abs", ec.message()) : Value::str(p.lexically_normal().string());
}

// A dangling symlink is neither a file nor a directory.
Value fn_path_exists(NativeCtx&, std::vector<Value>& a, std::string&) { std::error_code ec; return Value::boolean(fs::exists(a[0].as_str(), ec)); }
Value fn_is_dir(NativeCtx&, std::vector<Value>& a, std::string&)      { std::error_code ec; return Value::boolean(fs::is_directory(a[0].as_str(), ec)); }
Value fn_is_file(NativeCtx&, std::vector<Value>& a, std::string&)     { std::error_code ec; return Value::boolean(fs::is_regular_file(a[0].as_str(), ec)); }

// -1, never a real size, for a missing path or a directory.
Value fn_file_size(NativeCtx&, std::vector<Value>& a, std::string&) {
    std::error_code ec;
    auto n = fs::file_size(a[0].as_str(), ec);
    return Value::integer(ec ? -1 : static_cast<long long>(n));
}

Value fn_mtime_ms(NativeCtx&, std::vector<Value>& a, std::string&) {
    std::error_code ec;
    auto t = fs::last_write_time(a[0].as_str(), ec);
    if (ec) return Value::integer(-1);
#ifdef __ANDROID__
    // libc++ (NDK) has no clock_cast; its file_clock counts from the Unix epoch on Linux/Android.
    return Value::integer(std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count());
#else
    return Value::integer(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch()).count());
#endif
}

// ─── Files ──────────────────────────────────────────────────────────────────

// A missing file is null, not an error. One open, one fstat and reads
// straight into a string of the file's size: no stream, no copies.
Value fn_read_file(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const std::string& path = a[0].as_str();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Value::null();
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); return Value::null(); }
    // Read straight into the string, without zero-filling it first.
    std::string out;
    size_t got = 0;
    bool failed = false;
    out.resize_and_overwrite(static_cast<size_t>(st.st_size), [&](char* buf, size_t cap) {
        while (got < cap) {
            const ssize_t n = ::read(fd, buf + got, cap - got);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { failed = true; break; }
            if (n == 0) break;
            got += static_cast<size_t>(n);
        }
        return got;
    });
    char more[16384];   // what it grew by since the fstat (or a /proc file's size 0)
    while (!failed && got == out.size()) {
        const ssize_t n = ::read(fd, more, sizeof more);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) failed = true;
        if (n <= 0) break;
        out.append(more, static_cast<size_t>(n));
        got += static_cast<size_t>(n);
    }
    if (failed) { ::close(fd); return fail(error, "read_file", "could not read '" + path + "'"); }
    ::close(fd);
    out.resize(got);
    return Value::str(std::move(out));
}

Value write(std::vector<Value>& a, std::string& error, bool append) {
    const char* fn = append ? "append_file" : "write_file";
    std::ofstream f(a[0].as_str(), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!f) return fail(error, fn, "could not open '" + a[0].as_str() + "' for writing");
    f << a[1].as_str();
    return f.flush() ? Value::boolean(true) : fail(error, fn, "write failed for '" + a[0].as_str() + "'");
}
Value fn_write_file(NativeCtx&, std::vector<Value>& a, std::string& e)  { return write(a, e, false); }
Value fn_append_file(NativeCtx&, std::vector<Value>& a, std::string& e) { return write(a, e, true); }

// Entry names, one level (Python's os.listdir); list_dir(dir, true) walks
// the whole tree and gives paths relative to `dir`. null if not a directory.
Value fn_list_dir(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const fs::path root = a[0].as_str();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return Value::null();
    Value::List out;
    auto add = [&](const fs::directory_entry& e) { out.push_back(Value::str(e.path().lexically_relative(root).string())); };
    if (a.size() > 1 && a[1].as_bool())
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) add(*it);
    else
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) add(*it);
    return ec ? fail(error, "list_dir", ec.message()) : Value::list(std::move(out));
}

// glob("uploads/*.jpg"): the matching paths, sorted. The pattern's last
// part is matched (shell rules: * ? [...]) inside the directory before it.
Value fn_glob(NativeCtx&, std::vector<Value>& a, std::string&) {
    const fs::path pattern = a[0].as_str();
    const fs::path dir = pattern.has_parent_path() ? pattern.parent_path() : ".";
    const std::string name = pattern.filename().string();
    std::vector<std::string> hits;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string f = it->path().filename().string();
        if (fnmatch(name.c_str(), f.c_str(), FNM_PERIOD) == 0)
            hits.push_back(pattern.has_parent_path() ? (dir / f).string() : f);
    }
    std::sort(hits.begin(), hits.end());
    Value::List out;
    for (auto& h : hits) out.push_back(Value::str(std::move(h)));
    return Value::list(std::move(out));
}

// false if it was already gone. Files only; directories are remove_dir().
Value fn_remove_file(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const std::string& path = a[0].as_str();
    std::error_code ec;
    if (!fs::exists(path, ec)) return Value::boolean(false);
    if (!fs::is_regular_file(path, ec)) return fail(error, "remove_file", "'" + path + "' is not a regular file (use os.remove_dir() for directories)");
    const bool removed = fs::remove(path, ec);
    return ec ? fail(error, "remove_file", ec.message()) : Value::boolean(removed);
}

// {total, free, used}: bytes of the filesystem that holds `path`; `free` is
// what a non-root user may still write.
Value fn_disk_usage(NativeCtx&, std::vector<Value>& a, std::string& error) {
    struct statvfs v{};
    if (::statvfs(a[0].as_str().c_str(), &v) != 0) return fail(error, "disk_usage", std::strerror(errno));
    const long long total = static_cast<long long>(v.f_blocks) * static_cast<long long>(v.f_frsize);
    const long long free_ = static_cast<long long>(v.f_bavail) * static_cast<long long>(v.f_frsize);
    Value::Dict d;
    d["total"] = Value::integer(total);
    d["free"]  = Value::integer(free_);
    d["used"]  = Value::integer(total - static_cast<long long>(v.f_bfree) * static_cast<long long>(v.f_frsize));
    return Value::dict(std::move(d));
}

// A permission mode: an octal string ("640", "0750") or an int taken as is.
bool parse_mode(const Value& v, mode_t& out) {
    if (v.is_int()) { out = static_cast<mode_t>(v.as_int()); return v.as_int() >= 0 && v.as_int() <= 07777; }
    const std::string& s = v.as_str();
    char* end = nullptr;
    const unsigned long m = std::strtoul(s.c_str(), &end, 8);
    out = static_cast<mode_t>(m);
    return !s.empty() && *end == '\0' && m <= 07777;
}

Value fn_chmod(NativeCtx&, std::vector<Value>& a, std::string& error) {
    mode_t m;
    if (!parse_mode(a[1], m)) return fail(error, "chmod", "the mode is an octal string such as \"640\"");
    if (::chmod(a[0].as_str().c_str(), m) != 0) return fail(error, "chmod", std::strerror(errno));
    return Value::boolean(true);
}

// Sets the process umask; returns the previous one as an octal string.
Value fn_umask(NativeCtx&, std::vector<Value>& a, std::string& error) {
    mode_t m;
    if (!parse_mode(a[0], m)) return fail(error, "umask", "the mask is an octal string such as \"027\"");
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04o", static_cast<unsigned>(::umask(m)));
    return Value::str(buf);
}

// ── commands (`lux run files -- name args`) ─────────────────────────────────

// The arguments after the command's name.
Value fn_argv(NativeCtx&, std::vector<Value>&, std::string&) {
    Value::List out;
    for (const auto& a : script_mode().args) out.push_back(Value::str(a));
    return Value::list(std::move(out));
}

// Ends the command with this exit code (from any function, like abort()).
Value fn_exit(NativeCtx&, std::vector<Value>& a, std::string& error) {
    if (!script_mode().on) return fail(error, "exit", "only a `command` run with `lux run` can exit; a request ends with return or abort()");
    script_mode().exit_code = a.empty() ? 0 : static_cast<int>(a[0].as_int());
    error = kAbortMessage;
    return Value::null();
}

Value fn_print(NativeCtx&, std::vector<Value>& a, std::string&) {
    std::cout << a[0].to_string() << '\n';
    return Value::null();
}

Value fn_eprint(NativeCtx&, std::vector<Value>& a, std::string&) {
    std::cerr << a[0].to_string() << '\n';
    return Value::null();
}

// A line from stdin, without its newline; null at end of input. The prompt
// goes to stderr so stdout stays clean for a pipe.
Value read_line(const std::string& prompt, bool secret) {
    if (!prompt.empty()) { std::cerr << prompt; std::cerr.flush(); }
    termios old{};
    const bool tty = secret && ::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) { termios t = old; t.c_lflag &= ~static_cast<tcflag_t>(ECHO); ::tcsetattr(STDIN_FILENO, TCSANOW, &t); }
    std::string line;
    const bool ok = static_cast<bool>(std::getline(std::cin, line));
    if (tty) { ::tcsetattr(STDIN_FILENO, TCSANOW, &old); std::cerr << '\n'; }
    return ok ? Value::str(std::move(line)) : Value::null();
}

Value fn_input(NativeCtx&, std::vector<Value>& a, std::string&)        { return read_line(a.empty() ? "" : a[0].as_str(), false); }
Value fn_input_secret(NativeCtx&, std::vector<Value>& a, std::string&) { return read_line(a.empty() ? "" : a[0].as_str(), true); }

// Parents too, like os.makedirs(). false if it already existed.
Value fn_make_dir(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::error_code ec;
    const bool created = fs::create_directories(a[0].as_str(), ec);
    return ec ? fail(error, "make_dir", ec.message()) : Value::boolean(created);
}

// Empty-only unless the second argument is true -- a directory that turns
// out not to be empty fails loudly instead of taking its contents along.
Value fn_remove_dir(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const std::string& path = a[0].as_str();
    std::error_code ec;
    if (!fs::exists(path, ec)) return Value::boolean(false);
    if (!fs::is_directory(path, ec)) return fail(error, "remove_dir", "'" + path + "' is not a directory (use os.remove_file() for files)");
    if (a.size() > 1 && a[1].as_bool()) {
        const auto n = fs::remove_all(path, ec);
        return ec ? fail(error, "remove_dir", ec.message()) : Value::boolean(n > 0);
    }
    const bool removed = fs::remove(path, ec);
    if (ec == std::errc::directory_not_empty)
        return fail(error, "remove_dir", "'" + path + "' is not empty -- pass true as the second argument to remove it and everything inside");
    return ec ? fail(error, "remove_dir", ec.message()) : Value::boolean(removed);
}

// Files only; the third argument allows overwriting.
Value fn_copy_file(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(a[0].as_str(), ec)) return fail(error, "copy_file", "'" + a[0].as_str() + "' is not a regular file");
    const auto mode = a.size() > 2 && a[2].as_bool() ? fs::copy_options::overwrite_existing : fs::copy_options::none;
    const bool copied = fs::copy_file(a[0].as_str(), a[1].as_str(), mode, ec);
    return ec ? fail(error, "copy_file", ec.message()) : Value::boolean(copied);
}

// A file or a whole directory, across filesystems too: rename(2) first
// (instant, atomic); on EXDEV, copy and then delete the source, like
// shutil.move() -- only once the copy fully succeeded.
Value fn_move(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const std::string &from = a[0].as_str(), &to = a[1].as_str();
    std::error_code ec;
    if (!fs::exists(from, ec)) return fail(error, "move", "'" + from + "' does not exist");
    fs::rename(from, to, ec);
    if (!ec) return Value::boolean(true);
    if (ec != std::errc::cross_device_link) return fail(error, "move", ec.message());
    const bool dir = fs::is_directory(from, ec);
    if (dir) fs::copy(from, to, fs::copy_options::recursive, ec);
    else     fs::copy_file(from, to, ec);
    if (ec) return fail(error, "move", ec.message());
    if (dir) fs::remove_all(from, ec); else fs::remove(from, ec);
    return ec ? fail(error, "move", "copied to '" + to + "' but could not remove '" + from + "': " + ec.message())
              : Value::boolean(true);
}

Value fn_temp_dir(NativeCtx&, std::vector<Value>&, std::string& error) {
    std::error_code ec;
    fs::path p = fs::temp_directory_path(ec);
    return ec ? fail(error, "temp_dir", ec.message()) : Value::str(p.string());
}

// A new, empty file only this process can read (0600), with the given
// suffix (".pdf"); the caller removes it.
Value fn_temp_file(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::error_code ec;
    const std::string suffix = a.empty() ? "" : a[0].as_str();
    std::string path = (fs::temp_directory_path(ec) / ("lux-XXXXXX" + suffix)).string();
    const int fd = mkstemps(path.data(), static_cast<int>(suffix.size()));
    if (fd < 0) return fail(error, "temp_file", std::strerror(errno));
    ::close(fd);
    return Value::str(path);
}

// ─── run() ──────────────────────────────────────────────────────────────────

constexpr long long kDefaultTimeoutMs = 15'000, kMaxTimeoutMs = 120'000;

// run(cmd, args, {"cwd", "env>s", "input", "timeout_ms"}) -> {status,
// stdout, stderr}. `input` is written to the child's stdin while its
// output is read, in one poll loop: a child that fills its stdout before
// reading all its stdin cannot deadlock the two against each other (and one
// that exits without reading it is an EPIPE, not a signal: app.cpp ignores
// SIGPIPE). Past the timeout it is killed.
Value fn_run(NativeCtx&, std::vector<Value>& a, std::string& error) {
    spawn::Options o;
    o.out.kind = o.err.kind = spawn::Stream::Pipe;
    std::string input;
    long long timeout = kDefaultTimeoutMs;
    if (a.size() > 2) {
        const auto& opts = a[2].as_dict();
        if (!spawn::read_common(opts, o, "os.run", error)) return Value::null();
        if (auto it = opts.find("input"); it != opts.end()) { input = it->second.to_string(); o.in.kind = spawn::Stream::Pipe; }
        if (auto it = opts.find("timeout_ms"); it != opts.end()) {
            if (!it->second.is_int() || it->second.as_int() < 1 || it->second.as_int() > kMaxTimeoutMs)
                return fail(error, "run", "timeout_ms must be between 1 and " + std::to_string(kMaxTimeoutMs));
            timeout = it->second.as_int();
        }
    }
    spawn::Child c;
    const auto argv = spawn::argv_of(a);
    if (!spawn::start(argv, o, c, error)) return fail(error, "run", error);

    std::string out, err;
    size_t written = 0;
    char buf[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    bool timed_out = false;
    while (c.out >= 0 || c.err >= 0) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { timed_out = true; break; }
        pollfd fds[3];
        int n = 0;
        for (int fd : {c.out, c.err}) if (fd >= 0) fds[n++] = {fd, POLLIN, 0};
        if (c.in >= 0) fds[n++] = {c.in, POLLOUT, 0};
        if (poll(fds, static_cast<nfds_t>(n), static_cast<int>(left)) < 0 && errno != EINTR) break;
        for (int i = 0; i < n; ++i) {
            if (!fds[i].revents) continue;
            if (fds[i].fd == c.in) {
                const ssize_t w = ::write(c.in, input.data() + written, input.size() - written);
                if (w > 0) written += static_cast<size_t>(w);
                if (w < 0 || written == input.size()) { ::close(c.in); c.in = -1; }   // EOF for the child
                continue;
            }
            int& fd = fds[i].fd == c.out ? c.out : c.err;
            const ssize_t r = ::read(fd, buf, sizeof buf);
            if (r > 0) (fd == c.out ? out : err).append(buf, static_cast<size_t>(r));
            else { ::close(fd); fd = -1; }
        }
    }
    for (int fd : {c.in, c.out, c.err}) if (fd >= 0) ::close(fd);
    int status = 0;
    if (timed_out) kill(c.pid, SIGKILL);
    waitpid(c.pid, &status, 0);
    if (timed_out) return fail(error, "run", "'" + argv[0] + "' timed out after " + std::to_string(timeout) + "ms");

    Value::Dict d;
    d["status"] = Value::integer(WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    d["stdout"] = Value::str(std::move(out));
    d["stderr"] = Value::str(std::move(err));
    return Value::dict(std::move(d));
}

} // namespace

LUX_MODULE(os, {
    {"getenv",        "s|x",  fn_getenv},
    {"cwd",           "",     fn_cwd},
    {"path_join",     "x*>s",   fn_path_join},
    {"path_basename", "s>s",    fn_path_basename},
    {"path_dirname",  "s>s",    fn_path_dirname},
    {"path_ext",      "s>s",    fn_path_ext},
    {"path_abs",      "s>s",    fn_path_abs},
    {"path_exists",   "s>b",    fn_path_exists},
    {"is_dir",        "s>b",    fn_is_dir},
    {"is_file",       "s>b",    fn_is_file},
    {"file_size",     "s>i",    fn_file_size},
    {"mtime_ms",      "s>i",    fn_mtime_ms},
    {"mime",          "s>s",    fn_mime},
    {"list_dir",      "s|b",  fn_list_dir},
    {"glob",          "s>l",    fn_glob},
    {"remove_file",   "s>b",    fn_remove_file},
    {"remove_dir",    "s|b>b",  fn_remove_dir},
    {"make_dir",      "s>b",    fn_make_dir},
    {"disk_usage",    "s>d",    fn_disk_usage},
    {"argv",          ">l",     fn_argv},
    {"exit",          "|i",     fn_exit},
    {"print",         "x",      fn_print},
    {"eprint",        "x",      fn_eprint},
    {"chmod",         "sx>b",   fn_chmod},
    {"umask",         "x>s",    fn_umask},
    {"temp_dir",      ">s",     fn_temp_dir},
    {"temp_file",     "|s>s",   fn_temp_file},
    // Moving file content or waiting on a process: off the event loop.
    {"read_file",     "s",    fn_read_file,   /*is_async=*/true},
    {"write_file",    "ss>b",   fn_write_file,  /*is_async=*/true},
    {"append_file",   "ss>b",   fn_append_file, /*is_async=*/true},
    {"copy_file",     "ss|b>b", fn_copy_file,   /*is_async=*/true},
    {"move",          "ss>b",   fn_move,        /*is_async=*/true},
    {"run",           "s|ld>d", fn_run,         /*is_async=*/true},
    // Waiting on the terminal: off the event loop.
    {"input",         "|s",     fn_input,        /*is_async=*/true},
    {"input_secret",  "|s",     fn_input_secret, /*is_async=*/true},
})

} // namespace lux_script
