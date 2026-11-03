#pragma once
// Starting a child process, shared by os.run() and proc.start().
//
// posix_spawn, not fork+exec: in a multithreaded process the child of a
// fork may only make async-signal-safe calls before exec, which rules out
// nearly all of C++. Never through a shell -- argv goes straight to the
// program, so no metacharacter in an argument is ever interpreted.
//
// On Windows: CreateProcessW with anonymous pipes. The child inherits exactly
// its three standard handles (a PROC_THREAD_ATTRIBUTE_HANDLE_LIST), the
// arguments are quoted with the CommandLineToArgvW rules, and everything is
// converted from UTF-8. The small Fd/Pid helpers below hide the difference
// from os.run() and proc.*.
#include <lux_script/value.hpp>
#include <lux_script/platform_os.hpp>

#ifdef _WIN32
#  include <algorithm>
#  include <thread>
#else
#  include <spawn.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <cerrno>
#endif

#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
extern char** environ;
#endif

namespace lux_script::spawn {

// Where a child's stdin/stdout/stderr goes. For stdin, File is read from.
struct Stream {
    enum Kind { Null, Pipe, File } kind = Null;
    std::string path;
};

struct Options {
    std::string cwd;                                      // empty = ours
    std::vector<std::pair<std::string, Value>> env;       // set (string) or unset (null)
    Stream in, out, err;
};

#ifdef _WIN32
using Fd  = HANDLE;
using Pid = HANDLE;   // a process handle
inline const Fd  kNoFd  = INVALID_HANDLE_VALUE;
inline const Pid kNoPid = nullptr;
#else
using Fd  = int;
using Pid = pid_t;
inline constexpr Fd  kNoFd  = -1;
inline constexpr Pid kNoPid = -1;
#endif
inline bool open_fd(Fd f) { return f != kNoFd; }

struct Child {
    Pid pid = kNoPid;
    Fd  in = kNoFd, out = kNoFd, err = kNoFd;   // our end of each Pipe stream
};

// {"cwd": "...", "env": {"K": "v", "UNSET_ME": null}} out of an options
// Dict; stream fields are the caller's to read.
inline bool read_common(const Value::Dict& opts, Options& o, const char* fn, std::string& error) {
    if (auto it = opts.find("cwd"); it != opts.end()) {
        if (!it->second.is_str()) { error = std::string(fn) + "(): cwd must be a string"; return false; }
        o.cwd = it->second.as_str();
    }
    if (auto it = opts.find("env"); it != opts.end()) {
        if (!it->second.is_dict()) { error = std::string(fn) + "(): env must be a Dict"; return false; }
        for (const auto& [k, v] : it->second.as_dict()) {
            if (!v.is_str() && !v.is_null()) { error = std::string(fn) + "(): env values are strings (or null to unset)"; return false; }
            o.env.emplace_back(k, v);
        }
    }
    return true;
}

#ifdef _WIN32

inline std::string last_error() { return plat::win_error(GetLastError()); }

// One argument, quoted so CommandLineToArgvW (and the MSVC runtime) gives it back unchanged.
inline std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        for (; i < a.size() && a[i] == L'\\'; ++i) ++backslashes;
        if (i == a.size()) { r.append(backslashes * 2, L'\\'); break; }
        if (a[i] == L'"') { r.append(backslashes * 2 + 1, L'\\'); r.push_back(L'"'); }
        else              { r.append(backslashes, L'\\'); r.push_back(a[i]); }
    }
    r.push_back(L'"');
    return r;
}

inline bool ieq(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// The child's environment block: ours minus every key the options touch
// (names are case-insensitive here), plus what they set. Empty = inherit.
inline std::wstring env_block(const Options& o) {
    if (o.env.empty()) return {};
    std::vector<std::wstring> entries;
    if (wchar_t* e = GetEnvironmentStringsW()) {
        for (const wchar_t* p = e; *p; p += wcslen(p) + 1) {
            const std::wstring kv(p);
            const size_t eq = kv.find(L'=', 1);   // "=C:=C:\\" style entries start with '='
            const std::wstring key = kv.substr(0, eq);
            bool touched = false;
            for (const auto& [k, _] : o.env) touched |= ieq(plat::widen(k), key);
            if (!touched) entries.push_back(kv);
        }
        FreeEnvironmentStringsW(e);
    }
    for (const auto& [k, v] : o.env)
        if (v.is_str()) entries.push_back(plat::widen(k) + L"=" + plat::widen(v.as_str()));
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& e : entries) { block += e; block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}

// argv[0] is the program, looked up in PATH (".exe" is appended when it has no extension).
inline bool start(const std::vector<std::string>& argv, const Options& o, Child& c, std::string& error) {
    std::vector<HANDLE> ours, theirs;
    auto cleanup = [&] { for (HANDLE h : ours) CloseHandle(h); for (HANDLE h : theirs) CloseHandle(h); };

    // A .bat/.cmd would be run through cmd.exe, which re-parses the arguments: that is a shell.
    {
        std::string prog = argv[0];
        for (auto& ch : prog) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (prog.ends_with(".bat") || prog.ends_with(".cmd")) {
            error = "could not start '" + argv[0] + "': batch files need a shell, which run/start never use";
            return false;
        }
    }

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};   // inheritable: the child's ends
    auto wire = [&](const Stream& s, bool reading, Fd& our_end, HANDLE& child_end) {
        if (s.kind == Stream::Pipe) {
            HANDLE r, w;
            if (!CreatePipe(&r, &w, &sa, 1 << 16)) return false;
            our_end   = reading ? w : r;
            child_end = reading ? r : w;
            SetHandleInformation(our_end, HANDLE_FLAG_INHERIT, 0);   // only the child's end is inherited
            ours.push_back(our_end);
            theirs.push_back(child_end);
            return true;
        }
        const wchar_t* path = s.kind == Stream::File ? nullptr : L"NUL";
        const std::wstring wpath = s.kind == Stream::File ? plat::widen(s.path) : std::wstring();
        child_end = CreateFileW(path ? path : wpath.c_str(),
                                reading ? GENERIC_READ : FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                                reading ? OPEN_EXISTING : (s.kind == Stream::File ? OPEN_ALWAYS : OPEN_EXISTING),
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (child_end == INVALID_HANDLE_VALUE) return false;
        theirs.push_back(child_end);
        return true;
    };
    HANDLE cin_h = nullptr, cout_h = nullptr, cerr_h = nullptr;
    if (!wire(o.in, true, c.in, cin_h) || !wire(o.out, false, c.out, cout_h) || !wire(o.err, false, c.err, cerr_h)) {
        error = "could not start '" + argv[0] + "': " + last_error();
        cleanup();
        c = Child{};
        return false;
    }

    std::wstring cmdline;
    for (size_t i = 0; i < argv.size(); ++i) { if (i) cmdline.push_back(L' '); cmdline += quote_arg(plat::widen(argv[i])); }
    const std::wstring env = env_block(o), cwd = plat::widen(o.cwd);

    // Inherit exactly the three standard handles (deduplicated: the list rejects repeats).
    std::vector<HANDLE> inherit{cin_h, cout_h, cerr_h};
    std::sort(inherit.begin(), inherit.end());
    inherit.erase(std::unique(inherit.begin(), inherit.end()), inherit.end());
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    bool ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
              UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                        inherit.size() * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = cin_h;
    si.StartupInfo.hStdOutput = cout_h;
    si.StartupInfo.hStdError = cerr_h;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    DWORD err = 0;
    if (ok) {
        ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                            env.empty() ? nullptr : const_cast<wchar_t*>(env.c_str()),
                            cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi) != FALSE;
        if (!ok) err = GetLastError();
    } else err = GetLastError();
    if (attr_size) DeleteProcThreadAttributeList(attrs);
    for (HANDLE h : theirs) CloseHandle(h);   // only the child needs its ends
    if (!ok) {
        for (HANDLE h : ours) CloseHandle(h);
        c = Child{};
        error = "could not start '" + argv[0] + "': " + plat::win_error(err);
        return false;
    }
    CloseHandle(pi.hThread);
    c.pid = pi.hProcess;
    return true;
}

#else

// argv[0] is the program, looked up in PATH.
inline bool start(const std::vector<std::string>& argv, const Options& o, Child& c, std::string& error) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    std::vector<int> ours, theirs;

    // O_CLOEXEC on both ends: another thread spawning at the same moment
    // must not inherit them -- an unrelated child holding our pipe's write
    // end open would keep this one's reader waiting for an EOF that never
    // comes.
    auto wire = [&](const Stream& s, int target, int& our_end) {
        if (s.kind == Stream::Pipe) {
            int p[2];
            if (pipe2(p, O_CLOEXEC) != 0) return false;
            const bool reading = target == STDIN_FILENO;
            our_end = reading ? p[1] : p[0];
            const int child_end = reading ? p[0] : p[1];
            ours.push_back(our_end);
            theirs.push_back(child_end);
            posix_spawn_file_actions_adddup2(&fa, child_end, target);
        } else {
            const char* path = s.kind == Stream::File ? s.path.c_str() : "/dev/null";
            const int flags  = target == STDIN_FILENO ? O_RDONLY
                             : s.kind == Stream::File ? O_WRONLY | O_CREAT | O_APPEND : O_WRONLY;
            posix_spawn_file_actions_addopen(&fa, target, path, flags, 0644);
        }
        return true;
    };
    bool ok = wire(o.in, STDIN_FILENO, c.in) && wire(o.out, STDOUT_FILENO, c.out) &&
              wire(o.err, STDERR_FILENO, c.err);
#ifndef __ANDROID__   // ponytail: bionic (API 28) has no usable addchdir_np, so `cwd` is ignored on Android
    if (ok && !o.cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, o.cwd.c_str());
#endif

    // The environment: ours, minus every key the options touch, plus what
    // they set.
    std::vector<std::string> env_storage;
    for (char** e = environ; *e; ++e) {
        const std::string_view kv(*e);
        const auto key = kv.substr(0, kv.find('='));
        bool touched = false;
        for (const auto& [k, _] : o.env) touched |= k == key;
        if (!touched) env_storage.emplace_back(kv);
    }
    for (const auto& [k, v] : o.env)
        if (v.is_str()) env_storage.push_back(k + "=" + v.as_str());
    std::vector<char*> envp, args;
    for (auto& s : env_storage) envp.push_back(s.data());
    envp.push_back(nullptr);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    int rc = ok ? posix_spawnp(&c.pid, argv[0].c_str(), &fa, nullptr, args.data(), envp.data()) : errno;
    posix_spawn_file_actions_destroy(&fa);
    for (int fd : theirs) ::close(fd);   // only the child needs its ends
    if (rc != 0) {
        for (int fd : ours) ::close(fd);
        c = Child{};
        error = "could not start '" + argv[0] + "': " + std::strerror(rc);
        return false;
    }
    return true;
}

#endif

// ─── what proc.* and os.run() do with a started child ───────────────────────

inline void close_fd(Fd f) {
#ifdef _WIN32
    CloseHandle(f);
#else
    ::close(f);
#endif
}

// A process handle (Windows only) is released once its exit code is known.
inline void release_pid(Pid p) {
#ifdef _WIN32
    CloseHandle(p);
#else
    (void)p;
#endif
}

// Our end of a pipe never blocks in read()/write() after this. Windows: reads
// are already gated by PeekNamedPipe (a no-wait read end would return errors
// instead of waiting), so only the write end needs the mode.
inline void set_nonblock(Fd f, bool write_end) {
#ifdef _WIN32
    if (!write_end) return;
    DWORD mode = PIPE_NOWAIT;
    SetNamedPipeHandleState(f, &mode, nullptr, nullptr);
#else
    (void)write_end;
    fcntl(f, F_SETFL, O_NONBLOCK);
#endif
}

// Waits up to timeout_ms for `f` to have data to read, or hit EOF. 1 = readable
// (read() will not block), 0 = timed out (or interrupted), -1 = failed (see sys_error()).
inline int wait_readable(Fd f, int timeout_ms) {
#ifdef _WIN32
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(f, nullptr, 0, nullptr, &avail, nullptr)) return 1;   // broken pipe = EOF
        if (avail) return 1;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return 0;
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 5)));
    }
#else
    struct pollfd pfd { f, POLLIN, 0 };
    const int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) return errno == EINTR ? 0 : -1;
    return pr == 0 ? 0 : 1;
#endif
}

inline std::string sys_error() {
#ifdef _WIN32
    return last_error();
#else
    return std::strerror(errno);
#endif
}

// One read: n > 0 bytes, 0 = EOF, -1 = error, -2 = nothing now / interrupted. Call after wait_readable().
inline long read_some(Fd f, void* buf, size_t cap) {
#ifdef _WIN32
    DWORD got = 0;
    if (!ReadFile(f, buf, static_cast<DWORD>(std::min<size_t>(cap, 1 << 20)), &got, nullptr))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return got ? static_cast<long>(got) : 0;
#else
    const ssize_t n = ::read(f, buf, cap);
    if (n >= 0) return static_cast<long>(n);
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? -2 : -1;
#endif
}

// Writes what fits without blocking: >= 0 bytes taken (0 = pipe full), -1 = error (closed, ...).
inline long write_some(Fd f, const char* data, size_t len) {
#ifdef _WIN32
    // A non-blocking pipe write may take all of a chunk or none of it, so go chunk by chunk.
    size_t done = 0;
    while (done < len) {
        DWORD wrote = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - done, 4096));
        if (!WriteFile(f, data + done, chunk, &wrote, nullptr)) return done ? static_cast<long>(done) : -1;
        if (wrote == 0) break;
        done += wrote;
    }
    return static_cast<long>(done);
#else
    const ssize_t n = ::write(f, data, len);
    if (n >= 0) return static_cast<long>(n);
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
#endif
}

// How a finished child maps to the exit code scripts see: its code, or -1 when
// it was killed by a signal (Windows: terminated by us, or crashed with an NTSTATUS code).
// try_reap: 1 = finished (code set), 0 = still running, -1 = gone and unknowable (code = -1).
inline int try_reap(Pid p, int& code) {
#ifdef _WIN32
    DWORD c = 0;
    if (WaitForSingleObject(p, 0) == WAIT_TIMEOUT) return 0;
    if (!GetExitCodeProcess(p, &c)) { code = -1; return -1; }
    code = c >= 0xC0000000u ? -1 : static_cast<int>(c);
    return 1;
#else
    int status = 0;
    const pid_t r = ::waitpid(p, &status, WNOHANG);
    if (r == p) { code = WIFEXITED(status) ? WEXITSTATUS(status) : -1; return 1; }
    if (r < 0)  { code = -1; return -1; }
    return 0;
#endif
}

// Blocks until it exits; the exit code as try_reap() gives it.
inline int wait_exit(Pid p) {
#ifdef _WIN32
    WaitForSingleObject(p, INFINITE);
    int code = -1;
    try_reap(p, code);
    return code;
#else
    int status = 0;
    ::waitpid(p, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// Posix: kill(pid, sig). Windows has no signals: any `sig` terminates the process outright
// (exit code reported as -1, like a signal death).
inline bool kill_proc(Pid p, int sig) {
#ifdef _WIN32
    (void)sig;
    return TerminateProcess(p, 0xC0000001u) != FALSE;
#else
    return ::kill(p, sig) == 0;
#endif
}

// argv out of (command, [args...]) as the Lux functions receive them.
inline std::vector<std::string> argv_of(const std::vector<Value>& a) {
    std::vector<std::string> argv{a[0].as_str()};
    if (a.size() > 1 && a[1].is_list())
        for (const Value& v : a[1].as_list()) argv.push_back(v.to_string());
    return argv;
}

} // namespace lux_script::spawn
