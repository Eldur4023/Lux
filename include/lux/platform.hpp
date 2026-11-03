#pragma once
// OS layer of the networking core. Header-only, no virtual calls: on Linux
// every helper is an inline one-liner over the exact syscall the code used
// before, so it compiles to the same calls. The Windows side (Winsock,
// CRT file descriptors, WSAPoll-based loop in core/event_loop.cpp) lives
// behind `#ifdef _WIN32`.
//
// Conventions kept on both systems so the callers do not care:
//   * a socket is an `int` (Windows SOCKET handles are small kernel handles
//     that fit; sock_open/sock_accept refuse one that does not);
//   * socket helpers return -1 and set `errno` (EAGAIN, EINTR, ECONNRESET...)
//     like read(2)/write(2) -- on Windows the Winsock error is mapped;
//   * a file is a CRT descriptor (_open/_read), never mixed with a socket.

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  include <process.h>
#  include <atomic>

static_assert(sizeof(off_t) == 8, "build with -D_FILE_OFFSET_BITS=64 (CMake does it)");

// epoll's event bits, so connection code keeps its vocabulary. Only the
// Windows event loop interprets them; EPOLLET is accepted and ignored (the
// loop is level-triggered, and the connections already read until EAGAIN).
#  ifndef EPOLLIN
#    define EPOLLIN  0x001u
#    define EPOLLOUT 0x004u
#    define EPOLLERR 0x008u
#    define EPOLLHUP 0x010u
#    define EPOLLET  0x80000000u
#  endif

#else  // Linux

#  include <fcntl.h>
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/epoll.h>
#  include <sys/mman.h>
#  include <sys/sendfile.h>
#  include <sys/socket.h>
#  include <sys/uio.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>

#endif

namespace lux::plat {

#ifdef _WIN32

// ── Winsock ───────────────────────────────────────────────────────────────────

// WSAStartup, once per process; cheap to call again.
inline void net_init() {
    static const int once = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d); }();
    (void)once;
}

inline int map_wsa_error(int e) {
    switch (e) {
        case WSAEWOULDBLOCK:
        case WSAEINPROGRESS:    return EAGAIN;
        case WSAEINTR:          return EINTR;
        case WSAEMFILE:         return EMFILE;
        case WSAENOBUFS:        return ENOBUFS;
        case WSAECONNRESET:     return ECONNRESET;
        case WSAECONNABORTED:   return ECONNABORTED;
        case WSAENOTCONN:       return ENOTCONN;
        case WSAESHUTDOWN:
        case WSAENETRESET:      return EPIPE;
        case WSAENOTSOCK:
        case WSAEBADF:          return EBADF;
        case WSAEADDRINUSE:     return EADDRINUSE;
        case WSAEACCES:         return EACCES;
        case WSAEINVAL:         return EINVAL;
        default:                return EIO;
    }
}

// Copies the last Winsock error into errno (call right after a failure).
inline void fix_errno() { errno = map_wsa_error(WSAGetLastError()); }

// Text of the last Winsock error.
inline std::string err_str() {
    const int e = WSAGetLastError();
    char buf[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, static_cast<DWORD>(e), 0, buf, sizeof buf, nullptr);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) --n;
    return n ? std::string(buf, n) : "winsock error " + std::to_string(e);
}

inline int sock_close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }

inline void sock_nonblock(int fd) { u_long nb = 1; ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &nb); }

// SOCKET -> int, or -1 (closed) if the handle does not fit.
inline int to_fd(SOCKET s) {
    if (s == INVALID_SOCKET) { fix_errno(); return -1; }
    if (static_cast<SOCKET>(static_cast<int>(s)) != s) { ::closesocket(s); errno = EMFILE; return -1; }
    ::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
    sock_nonblock(static_cast<int>(s));
    return static_cast<int>(s);
}

inline int sock_open(int family) {
    net_init();
    return to_fd(::socket(family, SOCK_STREAM, IPPROTO_TCP));
}

inline int sock_accept(int lfd, sockaddr* a, socklen_t* l) {
    return to_fd(::accept(static_cast<SOCKET>(lfd), a, l));
}

inline ssize_t sock_send(int fd, const void* b, size_t n, int flags = 0) {
    const int r = ::send(static_cast<SOCKET>(fd), static_cast<const char*>(b),
                         static_cast<int>(n > INT_MAX ? INT_MAX : n), flags);
    if (r == SOCKET_ERROR) { fix_errno(); return -1; }
    return r;
}
inline ssize_t sock_write(int fd, const void* b, size_t n) { return sock_send(fd, b, n, 0); }

inline ssize_t sock_read(int fd, void* b, size_t n) {
    const int r = ::recv(static_cast<SOCKET>(fd), static_cast<char*>(b),
                         static_cast<int>(n > INT_MAX ? INT_MAX : n), 0);
    if (r == SOCKET_ERROR) { fix_errno(); return -1; }
    return r;
}

// Head + body in one call (writev with two buffers).
inline ssize_t sock_writev2(int fd, const void* a, size_t al, const void* b, size_t bl) {
    WSABUF v[2];
    v[0].buf = const_cast<char*>(static_cast<const char*>(a));
    v[0].len = static_cast<ULONG>(al > 0x7fffffff ? 0x7fffffff : al);
    v[1].buf = const_cast<char*>(static_cast<const char*>(b));
    v[1].len = static_cast<ULONG>(bl > 0x7fffffff ? 0x7fffffff : bl);
    DWORD sent = 0;
    if (::WSASend(static_cast<SOCKET>(fd), v, 2, &sent, 0, nullptr, nullptr) == SOCKET_ERROR) {
        fix_errno();
        return -1;
    }
    return static_cast<ssize_t>(sent);
}

// No MSG_MORE on Windows: the head goes out on its own.
inline ssize_t sock_send_more(int fd, const void* b, size_t n) { return sock_send(fd, b, n, 0); }

// Connected loopback pair: sv[0] is the read end, sv[1] the write end; both
// non-blocking. Replaces eventfd / pipe2 (a Winsock socket is the only thing
// WSAPoll can wait on).
inline bool socketpair_loopback(int sv[2]) {
    net_init();
    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    SOCKET c = INVALID_SOCKET, s = INVALID_SOCKET;
    auto fail = [&] {
        ::closesocket(ls);
        if (c != INVALID_SOCKET) ::closesocket(c);
        if (s != INVALID_SOCKET) ::closesocket(s);
        return false;
    };
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    int alen = sizeof a;
    if (::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) == SOCKET_ERROR ||
        ::getsockname(ls, reinterpret_cast<sockaddr*>(&a), &alen) == SOCKET_ERROR ||
        ::listen(ls, 1) == SOCKET_ERROR) return fail();
    c = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == INVALID_SOCKET) return fail();
    if (::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a) == SOCKET_ERROR) return fail();
    s = ::accept(ls, nullptr, nullptr);
    if (s == INVALID_SOCKET) return fail();
    // The one that connected to our listener must be our own client, not a
    // stranger that raced us to the ephemeral port.
    sockaddr_in cl{}, pe{};
    int l1 = sizeof cl, l2 = sizeof pe;
    if (::getsockname(c, reinterpret_cast<sockaddr*>(&cl), &l1) == SOCKET_ERROR ||
        ::getpeername(s, reinterpret_cast<sockaddr*>(&pe), &l2) == SOCKET_ERROR ||
        cl.sin_port != pe.sin_port) return fail();
    ::closesocket(ls);
    const int r = to_fd(s), w = to_fd(c);
    if (r < 0 || w < 0) { if (r >= 0) sock_close(r); if (w >= 0) sock_close(w); return false; }
    int one = 1;
    ::setsockopt(static_cast<SOCKET>(w), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&one), sizeof one);
    sv[0] = r;
    sv[1] = w;
    return true;
}

// Signal pipe (graceful shutdown): the same loopback pair.
inline int     pipe_open (int p[2])                    { return socketpair_loopback(p) ? 0 : -1; }
inline ssize_t pipe_read (int fd, void* b, size_t n)   { return sock_read(fd, b, n); }
inline ssize_t pipe_write(int fd, const void* b, size_t n) { return sock_write(fd, b, n); }
inline int     pipe_close(int fd)                      { return sock_close(fd); }

// ── Files (CRT descriptors) ───────────────────────────────────────────────────

inline int file_open_ro(const char* path) { return ::_open(path, _O_RDONLY | _O_BINARY | _O_NOINHERIT); }

inline ssize_t pread(int fd, void* b, size_t n, off_t off) {
    if (::_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return ::_read(fd, b, static_cast<unsigned>(n > 0x7fffffff ? 0x7fffffff : n));
}

// sendfile(2) stand-in: reads a chunk and sends it. Not zero-copy
// (TransmitFile would need an overlapped socket); advances *off by what the
// socket took, so a short send just re-reads next time.
inline ssize_t sendfile(int sock, int file, off_t* off, size_t count) {
    char buf[64 * 1024];
    const ssize_t r = pread(file, buf, count < sizeof buf ? count : sizeof buf, *off);
    if (r <= 0) return r;
    const ssize_t s = sock_send(sock, buf, static_cast<size_t>(r), 0);
    if (s > 0) *off += s;
    return s;
}

// A temp file that vanishes when closed (and unmapped), named under `dir`.
inline int tmpfile_open(const std::string& dir) {
    static std::atomic<unsigned> counter{0};
    const std::string name = dir + "\\lux-spool-" + std::to_string(::_getpid()) + "-"
                           + std::to_string(counter.fetch_add(1)) + ".tmp";
    return ::_open(name.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY | _O_TEMPORARY | _O_NOINHERIT,
                   _S_IREAD | _S_IWRITE);
}

// Read-only view of the whole file; nullptr on failure. The mapping handle is
// closed at once: the view keeps the section (and so the file) alive.
inline void* map_ro(int fd, size_t n) {
    (void)n;
    HANDLE h = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE m = ::CreateFileMappingA(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) return nullptr;
    void* p = ::MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    ::CloseHandle(m);
    return p;
}
inline void unmap(void* p, size_t) { ::UnmapViewOfFile(p); }

// ── Misc ──────────────────────────────────────────────────────────────────────

inline void set_thread_name(const char*) {}
inline void usleep(unsigned us) { ::Sleep((us + 999) / 1000); }
inline void local_time(const std::time_t* t, std::tm* out) { ::localtime_s(out, t); }

#else  // ── Linux: the same names over the calls that were always there ─────────

inline void net_init() {}
inline void fix_errno() {}
inline std::string err_str() { return std::strerror(errno); }

inline int sock_close(int fd) { return ::close(fd); }
inline int sock_open(int family) { return ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); }
inline int sock_accept(int lfd, sockaddr* a, socklen_t* l) {
    return ::accept4(lfd, a, l, SOCK_NONBLOCK | SOCK_CLOEXEC);
}
inline ssize_t sock_read (int fd, void* b, size_t n)       { return ::read(fd, b, n); }
inline ssize_t sock_write(int fd, const void* b, size_t n) { return ::write(fd, b, n); }
inline ssize_t sock_send_more(int fd, const void* b, size_t n) { return ::send(fd, b, n, MSG_MORE); }
inline ssize_t sock_writev2(int fd, const void* a, size_t al, const void* b, size_t bl) {
    iovec iov[2] = {{const_cast<void*>(a), al}, {const_cast<void*>(b), bl}};
    return ::writev(fd, iov, 2);
}

inline int     pipe_open (int p[2])                        { return ::pipe2(p, O_CLOEXEC | O_NONBLOCK); }
inline ssize_t pipe_read (int fd, void* b, size_t n)       { return ::read(fd, b, n); }
inline ssize_t pipe_write(int fd, const void* b, size_t n) { return ::write(fd, b, n); }
inline int     pipe_close(int fd)                          { return ::close(fd); }

inline int file_open_ro(const char* path) { return ::open(path, O_RDONLY | O_CLOEXEC); }
inline ssize_t pread(int fd, void* b, size_t n, off_t off) { return ::pread(fd, b, n, off); }
inline ssize_t sendfile(int sock, int file, off_t* off, size_t count) { return ::sendfile(sock, file, off, count); }
inline int tmpfile_open(const std::string& dir) { return ::open(dir.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600); }
inline void* map_ro(int fd, size_t n) {
    void* m = ::mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0);
    return m == MAP_FAILED ? nullptr : m;
}
inline void unmap(void* p, size_t n) { ::munmap(p, n); }

inline void set_thread_name(const char* name) { pthread_setname_np(pthread_self(), name); }
inline void usleep(unsigned us) { ::usleep(us); }
inline void local_time(const std::time_t* t, std::tm* out) { ::localtime_r(t, out); }

#endif

} // namespace lux::plat
