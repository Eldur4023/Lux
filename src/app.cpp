#include "../include/lux/app.hpp"
#include "../include/lux/logger.hpp"
#include "../include/lux/metrics.hpp"
#include "../include/lux/request.hpp"
#include "../include/lux/response.hpp"
#include "../include/lux/task.hpp"
#include "../include/lux/tls.hpp"
#include "../include/lux/blocking_pool.hpp"
#include "../include/lux/percent_encoding.hpp"

#include <lux/core/event_loop.hpp>
#include "core/tcp_server.hpp"

#include <lux/platform.hpp>

#include <csignal>
#ifndef _WIN32
#include <sched.h>
#include <pthread.h>
#endif
#include <iostream>
#include <memory>
#include <filesystem>
#include <thread>
#include <charconv>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <functional>
#include <string_view>
#include <utility>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/resource.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/openat2.h>
#endif

#include <lux/mime.hpp>
#include <lux/http_cache.hpp>
#include <list>
#include <unordered_map>

namespace lux {

namespace {


// Weak ETag from mtime + size: "mtime-size" hex-encoded.
static std::string make_etag(const std::filesystem::file_time_type& mtime,
                              std::uintmax_t size) {
    // Hex of the bits, as the ostream this replaced printed them: libstdc++'s
    // file_clock counts from 2174, so today's times are negative.
    const auto ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        mtime.time_since_epoch()).count());
    char buf[48];
    char* p = buf;
    *p++ = '"';
    p = std::to_chars(p, buf + sizeof buf, ns, 16).ptr;
    *p++ = '-';
    p = std::to_chars(p, buf + sizeof buf, static_cast<uint64_t>(size), 16).ptr;
    *p++ = '"';
    return std::string(buf, p);
}

// True if `candidate` lives inside `root` (root is a component-wise prefix
// of candidate). Both must already be canonical/weakly-canonical paths.
// Four-iterator mismatch: a candidate with FEWER components than root (a
// symlink to a shallower directory) must not be walked past its end.
static bool path_is_within(const std::filesystem::path& root,
                            const std::filesystem::path& candidate) {
    return std::mismatch(root.begin(), root.end(),
                         candidate.begin(), candidate.end()).first == root.end();
}

// The mount covering `path`: the longest prefix (prepare() sorts them) that
// ends on a path-segment boundary.
static const App::StaticMount* mount_for(const std::vector<App::StaticMount>& mounts,
                                         const std::string& path) {
    for (const auto& m : mounts) {
        if (path.rfind(m.prefix, 0) != 0) continue;
        const size_t plen = m.prefix.size();
        // A prefix ending in '/' (almost always the root mount, "/") already
        // consumes the separator itself, so anything after it is fair game
        // with no further check. A prefix WITHOUT a trailing '/' ("/static")
        // still needs this: without it, "/static" would also match a sibling
        // that merely starts with the same letters ("/staticky"), since
        // rfind() above only checked the prefix, not a path-segment boundary.
        // Missing the '/'-ending case entirely used to mean a root mount
        // (`static "/" -> "./dist"`) only ever matched the exact path "/" --
        // every other file under it, even ones that exist, fell straight
        // through to 404, since m.prefix[1] never lines up with req.path[1]
        // for any longer path.
        if (m.prefix.back() != '/' &&
            path.size() > plen && path[plen] != '/') continue;
        return &m;
    }
    return nullptr;
}

#ifndef _WIN32   // openat2 is Linux-only: Windows serves statics through the portable path below

// root/rel resolved by the kernel without leaving root: openat2's
// RESOLVE_BENEATH refuses any "..", absolute path or symlink that would
// (EXDEV). One call where canonical() + weakly_canonical() + prefix checks
// took ~30 (a readlink per component, three times over).
// 0 with `st` filled and the file open in `fd` (O_NONBLOCK: a FIFO must not
// hang the loop), 403 (escapes root), 404, or -1: no openat2 here (Linux <
// 5.6, or a seccomp profile that predates it).
static int open_beneath(int root_fd, const std::string& rel, struct stat& st, int& fd) {
#ifdef __ANDROID__
    // Android's app seccomp filter kills the process (SIGSYS) on openat2 instead of answering ENOSYS:
    // take the portable path straight away.
    (void)root_fd; (void)rel; (void)st; fd = -1;
    return -1;
#endif
    open_how how{};
    how.flags   = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    fd = static_cast<int>(::syscall(SYS_openat2, root_fd, rel.empty() ? "." : rel.c_str(),
                                    &how, sizeof how));
    if (fd < 0) return errno == ENOSYS || errno == EPERM ? -1 : errno == EXDEV ? 403 : 404;
    if (::fstat(fd, &st) == 0) return 0;
    ::close(fd);
    fd = -1;
    return 404;
}

// The mount's root, open, per thread. Re-opened once a second, so a deploy
// that swaps a symlink in its path (current -> releases/N) is followed
// within one: opening it per request walked the whole path every time.
static int root_fd_for(const App::StaticMount& m) {
    struct Cached { const App::StaticMount* m; int fd; std::chrono::steady_clock::time_point at; };
    thread_local std::vector<Cached> cache;
    const auto now = std::chrono::steady_clock::now();
    for (auto& c : cache) {
        if (c.m != &m) continue;
        if (now - c.at < std::chrono::seconds(1) && c.fd >= 0) return c.fd;
        if (c.fd >= 0) ::close(c.fd);
        c.fd = ::open(m.root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        c.at = now;
        return c.fd;
    }
    cache.push_back({&m, ::open(m.root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC), now});
    return cache.back().fd;
}

#endif // !_WIN32

// A hashed asset name (app.abc123ef.js): the last '.'/'-' segment is >= 8 hex digits
// with at least one a-f (report-20261104.css is a date, not a hash).
static bool is_hex_hash(std::string_view s) {
    const auto pos = s.find_last_of(".-");
    if (pos == std::string_view::npos) return false;
    const auto seg = s.substr(pos + 1);
    if (seg.size() < 8) return false;
    bool letter = false;
    for (unsigned char c : seg) {
        if (!std::isxdigit(c)) return false;
        letter |= std::isalpha(c) != 0;
    }
    return letter;
}

// Content-Type and Cache-Control of the static file `full` (a path).
// Hashed filenames (e.g. app.abc123ef.js) → immutable for 1 year.
// Everything else → must-revalidate with short max-age.
static void static_class(const std::string& full, std::string& ct, const char*& cache_ctrl) {
    // Name, extension and stem by hand: std::filesystem split the whole
    // path into components for each.
    const std::string_view name = std::string_view(full).substr(full.rfind('/') + 1);
    const size_t dot = name.rfind('.');
    const std::string_view ext  = dot == std::string_view::npos || dot == 0 ? std::string_view() : name.substr(dot);
    const std::string_view stem = name.substr(0, name.size() - ext.size());
    // An index.html — whether requested directly, served for a bare
    // directory, or reached through the `spa` fallback — is the one
    // static file whose CONTENT changes on every deploy without its
    // NAME changing (that is exactly what the hashed-asset names it
    // references are for), so it needs the opposite of the two rules
    // above: revalidate on every load, not just once an hour. Without
    // this, a client could keep the OLD index.html — pointing at
    // hashed bundles a deploy already deleted — for up to an hour
    // after a release. `no-cache` (which, despite the name, still lets
    // the browser cache the file — it just forces the ETag
    // revalidation below on every load instead of skipping it for
    // max-age) costs one cheap 304 round trip per navigation, not a
    // full re-download.
    cache_ctrl = name == "index.html"
        ? "no-cache"
        : is_hex_hash(stem)
            ? "public, max-age=31536000, immutable"
            : "public, max-age=3600, must-revalidate";
    ct = mime_for_ext(ext);   // lux/mime.hpp
}

// The validators of one representation of a static file.
struct StaticMeta { std::string etag, last_modified; std::time_t mtime = 0; };

// Does an Accept-Encoding value list `tok` with a non-zero q?
static bool accepts_encoding(std::string_view ae, std::string_view tok) {
    auto trim = [](std::string_view t) {
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
        while (!t.empty() && (t.back()  == ' ' || t.back()  == '\t')) t.remove_suffix(1);
        return t;
    };
    while (!ae.empty()) {
        const size_t comma = ae.find(',');
        std::string_view item = ae.substr(0, comma);
        ae = comma == std::string_view::npos ? std::string_view() : ae.substr(comma + 1);
        const size_t semi = item.find(';');
        const std::string_view name = trim(item.substr(0, semi));
        if (name.size() != tok.size() ||
            !std::equal(name.begin(), name.end(), tok.begin(),
                        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; })) continue;
        if (semi == std::string_view::npos) return true;
        const std::string_view q = trim(item.substr(semi + 1));
        return !(q.size() > 1 && (q[0] == 'q' || q[0] == 'Q') && q.find_first_of("123456789") == std::string_view::npos);
    }
    return false;
}

// ETag, Cache-Control, Content-Type, Last-Modified (and Content-Encoding / Vary) of a static
// response; true if the request's validators make it a 304 (already answered).
static bool static_headers(const Request& req, Response& res, const std::string& ct, const char* cc,
                           const StaticMeta& meta, const char* encoding, bool vary) {
    res.header("ETag",          meta.etag);
    res.header("Cache-Control", cc);
    res.header("Content-Type",  ct);
    res.header("Last-Modified", meta.last_modified);
    if (encoding) res.header("Content-Encoding", encoding);
    if (vary)     res.header("Vary", "Accept-Encoding");

    // RFC 9110 §13.1.3: If-Modified-Since is only looked at without an If-None-Match.
    bool fresh = false;
    if (auto inm = req.header("if-none-match")) fresh = etag_listed(*inm, meta.etag);
    else if (auto ims = req.header("if-modified-since")) {
        std::time_t t = 0;
        fresh = parse_http_date(*ims, t) && meta.mtime <= t;
    }
    if (fresh) res.status(304).send("");
    return fresh;
}

// The portable path (no openat2): the file by its path.
static void send_static(const Request& req, Response& res, std::string file,
                        const std::filesystem::file_time_type& mtime, std::uintmax_t filesize) {
    std::string ct; const char* cc;
    static_class(file, ct, cc);
#ifdef __ANDROID__   // libc++'s file_clock::to_sys yields a 128-bit duration; its file clock counts from the Unix epoch
    const std::time_t t = std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
#else
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::file_clock::to_sys(mtime));
#endif
    if (static_headers(req, res, ct, cc, {make_etag(mtime, filesize), http_date(t), t}, nullptr, false)) return;
    res.send_file(file, filesize);   // zero-copy: sendfile(2)
}

#ifndef _WIN32

// Files this small are kept in memory, per thread: a hit is one hash lookup (and, once a second,
// a few fstatat) instead of openat2 + fstat + the file read/sendfile.
constexpr size_t kSmallFile = 64 * 1024, kSmallCacheCap = 16u << 20;

// What identifies a file's content for the cache: a change in any of it drops the entry.
struct FileSig {
    dev_t dev; ino_t ino; time_t sec; long nsec; off_t size;
    explicit FileSig(const struct stat& st = {}) : dev(st.st_dev), ino(st.st_ino), sec(st.st_mtim.tv_sec),
                                                   nsec(st.st_mtim.tv_nsec), size(st.st_size) {}
    bool operator==(const FileSig& o) const {
        return dev == o.dev && ino == o.ino && sec == o.sec && nsec == o.nsec && size == o.size;
    }
};

constexpr const char* kSuffix[3]   = {"", ".br", ".gz"};          // identity, brotli, gzip
constexpr const char* kEncoding[3] = {nullptr, "br", "gzip"};

struct SmallEntry {
    std::string key;                       // mount root + resolved path under it
    std::string body[3];
    StaticMeta  meta[3];
    FileSig     sig[3];
    bool        has[3] = {true, false, false};   // which variants exist (precompressed .br / .gz)
    std::string ct, cc;
    std::chrono::steady_clock::time_point checked_at;
    size_t      bytes = 0;
};

class SmallCache {
public:
    SmallEntry* find(const std::string& key) {
        const auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        lru_.splice(lru_.begin(), lru_, it->second);
        return &*it->second;
    }
    void erase(const std::string& key) {
        const auto it = map_.find(key);
        if (it == map_.end()) return;
        bytes_ -= it->second->bytes;
        lru_.erase(it->second);
        map_.erase(it);
    }
    void put(SmallEntry e) {
        erase(e.key);
        bytes_ += e.bytes;
        lru_.push_front(std::move(e));
        map_[lru_.front().key] = lru_.begin();
        while (bytes_ > kSmallCacheCap && lru_.size() > 1) erase(std::string(lru_.back().key));
    }
private:
    std::list<SmallEntry> lru_;   // most recently used first
    std::unordered_map<std::string, std::list<SmallEntry>::iterator> map_;
    size_t bytes_ = 0;
};

// Only plain relative names are cached: "a/../index.html" or "x//y" would be aliases of
// one file, and the cache must not grow with URLs.
static bool plain_rel(std::string_view f) {
    while (!f.empty()) {
        const size_t slash = f.find('/');
        const auto part = f.substr(0, slash);
        if (part.empty() || part == "." || part == "..") return false;
        f = slash == std::string_view::npos ? std::string_view() : f.substr(slash + 1);
    }
    return true;
}

static std::filesystem::file_time_type file_time_of(const struct stat& st) {
    return std::chrono::file_clock::from_sys(
        std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds(st.st_mtim.tv_sec) + std::chrono::nanoseconds(st.st_mtim.tv_nsec))));
}

// The validators of file variant `i` (0 = the file, 1 = its .br, 2 = its .gz).
static StaticMeta meta_of(const struct stat& st, int i) {
    StaticMeta sm{make_etag(file_time_of(st), static_cast<std::uintmax_t>(st.st_size)),
                  http_date(st.st_mtim.tv_sec), st.st_mtim.tv_sec};
    if (i) { sm.etag.pop_back(); sm.etag += i == 1 ? "-br\"" : "-gz\""; }
    return sm;
}

// Answers from the cache; false on a miss (or an entry the disk no longer matches).
static bool serve_small(SmallCache& cache, int root_fd, const std::string& key, const std::string& file,
                        const Request& req, Response& res) {
    SmallEntry* e = cache.find(key);
    if (!e) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now - e->checked_at >= std::chrono::seconds(1)) {
        // At most once a second per entry, as root_fd_for does for the root: the file and its
        // variants must be the very ones the entry was made from, and absent ones still absent.
        for (int i = 0; i < 3; ++i) {
            struct stat st{};
            const bool exists = ::fstatat(root_fd, (file + kSuffix[i]).c_str(), &st, 0) == 0;
            if (exists != e->has[i] || (exists && !(FileSig(st) == e->sig[i]))) { cache.erase(key); return false; }
        }
        e->checked_at = now;
    }
    int pick = 0;
    if (const std::string* ae = req.header("accept-encoding")) {
        if (e->has[1] && accepts_encoding(*ae, "br")) pick = 1;
        else if (e->has[2] && accepts_encoding(*ae, "gzip")) pick = 2;
    }
    if (static_headers(req, res, e->ct, e->cc.c_str(), e->meta[pick], kEncoding[pick], e->has[1] || e->has[2])) return true;
    res.header("Accept-Ranges", pick ? "none" : "bytes");
    res.send(e->body[pick]);
    return true;
}

#endif // !_WIN32

// Serves `req` from mount `m`: the file, a 304, or the error.
// Sets ETag, Cache-Control, and honours If-None-Match for 304 responses.
static void serve_from_mount(const App::StaticMount& m, const Request& req, Response& res) {
    namespace fs = std::filesystem;
    auto fail = [&](int code, const char* body) {
        res.status(code).json_text(body);
    };
    {
        const size_t plen = m.prefix.size();
        std::string rel = lux::percent_decode(req.path.substr(plen), false);
        if (rel.empty() || rel.front() != '/') rel = '/' + rel;

        // Block dotfiles: any path component starting with '.' (e.g. .env,
        // .git/config, .htaccess) — common misconfiguration in deployments.
        // We check the URL-decoded relative path so %2E bypasses are caught.
        //
        // EXCEPT /.well-known/ (RFC 8615): a fixed, standardized,
        // intentionally-public directory. ACME's HTTP-01 domain validation
        // (RFC 8555 §8.3) serves its challenge response from exactly
        // /.well-known/acme-challenge/<token> over plain HTTP,
        // unauthenticated, by design — and a static mount is the only way
        // to serve it at all, since the path is fixed by the CA, not
        // something an app route can be written for ahead of time.
        // Blocking every dotfile unconditionally left no way to pass that
        // validation through Lux at all.
        // A decoded %00 would cut the path the kernel sees short of the name
        // the MIME type is taken from.
        if (rel.find('\0') != std::string::npos) return fail(404, R"({"error":"Not Found"})");
        static const std::string kWellKnown = "/.well-known/";
        bool is_well_known = rel.compare(0, kWellKnown.size(), kWellKnown) == 0;
        if (!is_well_known) {
            for (size_t i = 0; i < rel.size(); ++i) {
                if (rel[i] == '/' && i + 1 < rel.size() && rel[i + 1] == '.') return fail(404, R"({"error":"Not Found"})");
            }
        }

#ifndef _WIN32
        if (const int root_fd = root_fd_for(m); root_fd >= 0) {
            struct stat st{};
            int fd = -1;
            std::string file = rel.substr(1);
            // A Range request is the connection's business (sendfile + offsets): no cache.
            const bool ranged = req.header("range") != nullptr;
            thread_local SmallCache cache;
            if (!ranged) {
                // Cached under the resolved name: a directory's is its index.html.
                std::string cfile = file.empty() || file.back() == '/' ? file + "index.html" : file;
                if (serve_small(cache, root_fd, m.root + '/' + cfile, cfile, req, res)) return;
            }
            int r;
            if (file.empty() || file.back() == '/') {
                // A directory by definition (the mount root, "docs/"): straight to
                // its index.html, one openat2 + fstat + close less per request.
                std::string index = file + "index.html";
                r = open_beneath(root_fd, index, st, fd);
                if (r == 0) file = std::move(index);   // regular file or not: checked below
            } else {
                r = open_beneath(root_fd, file, st, fd);
                if (r == 0 && S_ISDIR(st.st_mode)) {
                    struct stat ist{};
                    int ifd = -1;
                    std::string index = file.empty() ? "index.html" : file + "/index.html";
                    if (open_beneath(root_fd, index, ist, ifd) == 0) {
                        if (S_ISREG(ist.st_mode)) { ::close(fd); fd = ifd; file = std::move(index); st = ist; }
                        else ::close(ifd);
                    }
                }
            }
            if (r > 0 || (r == 0 && !S_ISREG(st.st_mode))) {
                if (fd >= 0) { ::close(fd); fd = -1; }
                if (r == 403 || !m.spa) return fail(r == 403 ? 403 : 404, r == 403 ? R"({"error":"Forbidden"})" : R"({"error":"Not Found"})");
                // SPA fallback: index.html for unknown paths, so client-side
                // routers (React Router, Vue Router...) can handle the URL.
                r = open_beneath(root_fd, "index.html", st, fd);
                if (r == 0 && !S_ISREG(st.st_mode)) { ::close(fd); fd = -1; r = 404; }
                if (r > 0) return fail(r, r == 403 ? R"({"error":"Forbidden"})" : R"({"error":"Not Found"})");
                file = "index.html";
            }
            if (r == 0) {
                const auto size = static_cast<std::uintmax_t>(st.st_size);
                std::string ct; const char* cc;
                static_class(file, ct, cc);

                // Precompressed variants (app.js.br, app.js.gz), if no older than the file.
                struct Alt { int fd = -1; struct stat st{}; bool ok = false; } alt[3];
                auto open_alt = [&](int i) {
                    Alt& a = alt[i];
                    if (open_beneath(root_fd, file + kSuffix[i], a.st, a.fd) == 0 && S_ISREG(a.st.st_mode) &&
                        FileSig(a.st).sec * 1000000000LL + FileSig(a.st).nsec >= FileSig(st).sec * 1000000000LL + FileSig(st).nsec)
                        a.ok = true;
                    else if (a.fd >= 0) { ::close(a.fd); a.fd = -1; }
                };
                const std::string* ae = ranged ? nullptr : req.header("accept-encoding");
                bool cache_it = !ranged && size <= kSmallFile && plain_rel(file);
                if (cache_it) { open_alt(1); open_alt(2); }   // the entry records them all
                else if (ae) {
                    // A big file without variants is asked again on every request: remember the
                    // answer for a second instead of failing two openat2 each time.
                    thread_local std::unordered_map<std::string, std::chrono::steady_clock::time_point> no_variant;
                    const auto now = std::chrono::steady_clock::now();
                    const std::string vkey = m.root + '/' + file;
                    const auto nv = no_variant.find(vkey);
                    if (nv == no_variant.end() || now - nv->second >= std::chrono::seconds(1)) {
                        if (accepts_encoding(*ae, "br")) open_alt(1);
                        if (!alt[1].ok && accepts_encoding(*ae, "gzip")) open_alt(2);
                        if (!alt[1].ok && !alt[2].ok) {
                            if (no_variant.size() >= 1024) no_variant.clear();
                            no_variant[vkey] = now;
                        } else if (nv != no_variant.end()) no_variant.erase(vkey);
                    }
                }
                int pick = 0;
                if (ae) {
                    if (alt[1].ok && accepts_encoding(*ae, "br")) pick = 1;
                    else if (alt[2].ok && accepts_encoding(*ae, "gzip")) pick = 2;
                }
                auto stat_of = [&](int i) -> const struct stat& { return i ? alt[i].st : st; };
                auto fd_of   = [&](int i) -> int& { return i ? alt[i].fd : fd; };
                auto close_all = [&](int keep) {
                    for (int i = 0; i < 3; ++i) if (i != keep && fd_of(i) >= 0) { ::close(fd_of(i)); fd_of(i) = -1; }
                };

                if (cache_it) {
                    SmallEntry e;
                    e.key = m.root + '/' + file;
                    for (int i = 0; i < 3 && cache_it; ++i) {
                        if (i && !alt[i].ok) continue;
                        const auto n = static_cast<size_t>(stat_of(i).st_size);
                        e.body[i].resize(n);
                        cache_it = n <= kSmallFile && ::pread(fd_of(i), e.body[i].data(), n, 0) == static_cast<ssize_t>(n);
                        e.has[i] = true;
                        e.meta[i] = meta_of(stat_of(i), i);
                        e.sig[i] = FileSig(stat_of(i));
                        e.bytes += n;
                    }
                    if (cache_it) {
                        e.ct = ct; e.cc = cc;
                        e.checked_at = std::chrono::steady_clock::now();
                        e.bytes += e.key.size() + 512;
                        close_all(-1);
                        const std::string key = e.key;
                        cache.put(std::move(e));
                        serve_small(cache, root_fd, key, file, req, res);
                        return;
                    }
                }

                const bool vary = alt[1].ok || alt[2].ok;
                if (static_headers(req, res, ct, cc, meta_of(stat_of(pick), pick), kEncoding[pick], vary)) {
                    close_all(-1);
                    return;
                }
                const int sfd = std::exchange(fd_of(pick), -1);
                close_all(-1);
                res.send_file_fd(sfd, m.root + '/' + file + kSuffix[pick], static_cast<std::uintmax_t>(stat_of(pick).st_size));
                if (pick) res.header("Accept-Ranges", "none");   // the bytes are the encoded ones
                return;
            }
            // r == -1: no openat2, resolved the portable way below.
        }
#endif

        // canonical() for the root resolves any symlinks inside the serve root
        // itself (e.g. if m.root is itself a symlink to /var/www).  Required
        // so the mismatch check below compares fully-resolved paths.
        std::error_code root_ec;
        auto canonical_root = fs::canonical(m.root, root_ec);
        if (root_ec) return fail(500, R"({"error":"Server misconfiguration"})");

        fs::path file = canonical_root / rel.substr(1);

        // First pass: weakly_canonical catches ".." traversal even when the
        // target file does not exist yet (needed for the 404 branch below).
        std::error_code ec;
        auto preliminary = fs::weakly_canonical(file);
        if (!path_is_within(canonical_root, preliminary)) return fail(403, R"({"error":"Forbidden"})");

        auto canonical_file = preliminary;

        auto status = fs::status(preliminary, ec);

        // A request for a directory (`/docs`, `/docs/`) serves that
        // directory's OWN index.html, same as every other static file
        // server (nginx, Apache, `python -m http.server`...) — not a 404
        // and not (for an `spa` mount) the ROOT index.html, which would
        // silently swap in the wrong page instead of the directory's real
        // one. Falls through to the branches below when there is no
        // index.html here: a directory with nothing to serve is still
        // either a 404 or, for `spa`, the root fallback.
        if (!ec && fs::is_directory(status)) {
            std::error_code dir_ec;
            auto dir_index = fs::canonical(preliminary / "index.html", dir_ec);
            if (!dir_ec && fs::is_regular_file(fs::status(dir_index)) &&
                path_is_within(canonical_root, dir_index)) {
                preliminary = dir_index;
                status = fs::status(preliminary, ec);
            }
        }

        if (ec || !fs::is_regular_file(status)) {
            // SPA fallback: serve index.html for unknown paths so client-side
            // routers (React Router, Vue Router, etc.) can handle the URL.
            if (m.spa) {
                canonical_file = fs::canonical(canonical_root / "index.html", ec);
                if (ec || !fs::is_regular_file(fs::status(canonical_file))) return fail(404, R"({"error":"Not Found"})");
                // index.html itself may be a symlink pointing outside the root.
                // Re-check that the resolved path still lives inside canonical_root
                // so a misconfigured/compromised dist directory cannot exfiltrate
                // arbitrary files via the SPA fallback.
                if (!path_is_within(canonical_root, canonical_file)) return fail(403, R"({"error":"Forbidden"})");
            } else {
                return fail(404, R"({"error":"Not Found"})");
            }
        } else {
            // File exists: fully resolve symlinks and re-check traversal.
            // The first pass caught ".." sequences; this pass catches symlinks
            // that point outside the root (e.g. uploads/evil -> /etc/passwd).
            canonical_file = fs::canonical(preliminary, ec);
            if (ec) return fail(404, R"({"error":"Not Found"})");
            if (!path_is_within(canonical_root, canonical_file)) return fail(403, R"({"error":"Forbidden"})");
        }

        std::error_code mtime_ec, size_ec;
        auto mtime    = fs::last_write_time(canonical_file, mtime_ec);
        auto filesize = fs::file_size(canonical_file, size_ec);
        if (mtime_ec || size_ec) return fail(500, R"({"error":"Cannot stat file"})");
        // '/'-separated UTF-8-ish string on both: send_static() splits the name on '/'.
        send_static(req, res, canonical_file.generic_string(), mtime, filesize);
    }
}

// ── Graceful shutdown ─────────────────────────────────────────────────────────
// Signal handler writes one byte to a pipe; the event loop thread reads it
// and runs the actual drain logic.  This keeps the handler async-signal-safe:
// only write(2) and _Exit(2) are used — both appear in the POSIX safe list.
//
// g_signal_pipe[0] = read end (monitored by epoll on the main loop)
// g_signal_pipe[1] = write end (written by the signal handler)
static int                   g_signal_pipe[2] = {-1, -1};   // plat::pipe_open (a loopback socket pair on Windows)
static std::function<void()> g_initiate_drain;
static volatile sig_atomic_t g_signal_count   = 0;

static void signal_handler(int) {
    g_signal_count = g_signal_count + 1;
    if (g_signal_count >= 2) {
        static const char msg[] = "\nForced exit.\n";
        (void)write(2, msg, sizeof(msg) - 1);
        std::_Exit(1);
    }
    static const char msg[] = "\nShutting down gracefully... (CTRL+C again to force)\n";
    (void)write(2, msg, sizeof(msg) - 1);
    // Wake the event loop — write is async-signal-safe.
    if (g_signal_pipe[1] >= 0) {
        char byte = 1;
        (void)plat::pipe_write(g_signal_pipe[1], &byte, 1);
    }
}

#ifdef _WIN32
// Ctrl-C / Ctrl-Break / console close / logoff / shutdown: same handler, run
// by Windows on a thread of its own (so it only touches the signal pipe).
static BOOL WINAPI console_ctrl_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT: case CTRL_BREAK_EVENT: case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT: case CTRL_SHUTDOWN_EVENT:
            signal_handler(0);
            return TRUE;
        default: return FALSE;
    }
}
#endif

} // anonymous namespace

// ── App::run ──────────────────────────────────────────────────────────────────

// ── App::prepare ─────────────────────────────────────────────────────────────
// Sorts the static mounts once (idempotent).

void App::prepare() {
    if (prepared_) return;
    prepared_ = true;
    std::sort(static_mounts_.begin(), static_mounts_.end(),
              [](const StaticMount& a, const StaticMount& b) {
                  return a.prefix.size() > b.prefix.size();
              });
}

// ── App::handle_request ───────────────────────────────────────────────────────
// Full middleware + router pipeline as a coroutine.
// Used by run() (via the DispatchFn) and by TestClient for in-process testing.

Task<void> App::handle_request(Request& req, Response& res) {

    // Static file mounts bypass the middleware chain — but only for a path
    // that has no explicitly declared route of its own. A broad mount like
    // `static "/" -> "./dist" spa` (the exact shape GUIDE.md recommends for
    // an SPA's dist folder) matches every path by prefix, so without this
    // check it silently swallowed EVERY GET/HEAD request the moment ANY
    // root or wide-prefix static mount existed — including one with a real
    // handler, answered instead with the mount's own 404 (or, worse, the
    // SPA's index.html) and the actual route never ran. A route the
    // developer wrote by hand takes precedence over a directory dump by
    // construction; the static mount is the fallback for "nothing else
    // claims this", not the other way around.
    if (req.method == "GET" || req.method == "HEAD") {
        // Two sources of "yes, a real route claims this", checked together:
        //   - router_ itself, EXCLUDING a match that only succeeded via a
        //     wildcard segment (`via_wildcard`) — App's own concrete routes
        //     (enable_health()/enable_metrics()/the docs endpoints, or any
        //     plain app.get()/post()/etc. from the C++ API) match this way,
        //     but so would the Lux Script engine's two blanket
        //     any("/",...)/any("/*",...) catch-alls if via_wildcard were
        //     not excluded — that would make this always true again for
        //     that engine and silently re-disable every static mount, the
        //     exact bug the via_wildcard field exists to keep fixed.
        //   - route_probe_, when set: the Lux Script engine's OWN router
        //     (mod->router, invisible to router_ above) for its actual
        //     declared routes -- see App::set_route_probe()'s comment.
        // Neither alone is enough: router_ misses the live module's routes,
        // and the probe alone (the previous version of this fix) missed
        // health/docs/metrics, which live on router_, not the module --
        // confirmed against the real binary: a root SPA mount answered
        // /health, /docs and /openapi.json with index.html instead of
        // reaching any of them.
        if (const StaticMount* m = mount_for(static_mounts_, req.path)) {
            auto rmatch = router_.match(req.method, req.path);
            bool route_exists = rmatch.found && !rmatch.via_wildcard;
            if (!route_exists && route_probe_) route_exists = route_probe_(req.method, req.path);
            if (!route_exists) { serve_from_mount(*m, req, res); co_return; }
        }
    }

    // No middleware (the Lux Script engine without --verbose): straight to
    // the route, without the chain's shared_ptr, std::function and frames.
    if (middlewares_.empty()) {
        auto match = router_.match(req.method, req.path);
        if (match.found) {
            req.params = std::move(match.params);
            co_await (*match.handler)(req, res);
        } else {
            res.status(404).json_text(R"({"error":"Not Found"})");
        }
    } else {
    // ── Async middleware chain ─────────────────────────────────────────────
    // call_next lives in a shared_ptr so NextFn closures that outlive this
    // coroutine frame (e.g. during shutdown) don't dangle on the function
    // object. advanced is also heap-allocated for the same reason.
    using CallNext = std::function<Task<void>(size_t)>;
    auto call_next = std::make_shared<CallNext>();
    // The lambda is stored INSIDE *call_next, so it holds a weak reference:
    // capturing the shared_ptr here would make the object own itself, and the
    // refcount would never reach zero -- one leaked control block and closure
    // per request. The NextFn handed to the middleware does take a strong
    // reference, which is what keeps it from dangling if it outlives us.
    *call_next = [this, &req, &res,
                  weak = std::weak_ptr<CallNext>(call_next)](size_t i) -> Task<void> {
        auto self = weak.lock();
        if (!self) co_return;
        if (i < middlewares_.size()) {
            auto advanced = std::make_shared<bool>(false);
            co_await middlewares_[i](req, res,
                [self, advanced, i]() -> Task<void> {
                    if (*advanced) co_return;
                    *advanced = true;
                    co_await (*self)(i + 1);
                });
        } else {
            auto match = router_.match(req.method, req.path);
            if (match.found) {
                req.params = std::move(match.params);
                co_await (*match.handler)(req, res);
            } else {
                res.status(404).json_text(R"({"error":"Not Found"})");
            }
        }
    };
    co_await (*call_next)(0);
    }

    // Error handlers run after the full chain, while we still own req/res.
    // Async handlers take precedence over sync handlers for the same code.
    // Not over a body the route returned with its own status: a route that
    // answers `{"error": "duplicate", ...}.status(409)` has already said what
    // the client should see (a global handler replaced it with its own).
    if (res.status_code() >= 400 && !res.route_body()) {
        int code = res.status_code();

        // The default body is already written and marked as committed.  An
        // error handler exists precisely to replace it, so it is taken away
        // before handing over control; without this, its res.json() or
        // res.render() would be silently ignored.
        //
        // If the handler writes nothing, the original body is put back: not
        // having written one cannot mean ending up with no response.
        auto guarded = [&res](auto&& fn) {
            std::string saved = res.take_body();
            fn();
            if (!res.is_committed()) res.restore_body(std::move(saved));
        };

        auto ait = async_error_handlers_.find(code);
        if (ait != async_error_handlers_.end()) {
            std::string saved = res.take_body();
            co_await ait->second(code, req, res);
            if (!res.is_committed()) res.restore_body(std::move(saved));
        } else if (catchall_async_error_handler_) {
            std::string saved = res.take_body();
            co_await catchall_async_error_handler_(code, req, res);
            if (!res.is_committed()) res.restore_body(std::move(saved));
        } else {
            auto it = error_handlers_.find(code);
            if (it != error_handlers_.end()) {
                guarded([&] { it->second(code, req, res); });
            } else if (catchall_error_handler_) {
                guarded([&] { catchall_error_handler_(code, req, res); });
            }
        }
    }
}

// ── App::run ──────────────────────────────────────────────────────────────────

void App::run(const std::string& host, uint16_t port) {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    prepare();  // sorts the static mounts

    // ── Build the async dispatch function ─────────────────────────────────────
    // Returns handle_request() directly — no extra coroutine frame.
    DispatchFn dispatch = [this](Request& req, Response& res) {
        return handle_request(req, res);
    };

    // ── Multi-core: one event loop per hardware thread ────────────────────────
    // hardware_concurrency() counts the machine's cores, not the ones this
    // process can use: it ignores the affinity mask and the cgroup cpu limit.
    // In a container with 2 cores assigned out of a 64-core machine,
    // levantaria 64 event loops sobre 2 cores.
    unsigned num_threads = 0;
#ifndef _WIN32
    {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (::sched_getaffinity(0, sizeof(mask), &mask) == 0)
            num_threads = static_cast<unsigned>(CPU_COUNT(&mask));
    }
#endif
    if (num_threads == 0) num_threads = std::thread::hardware_concurrency();
    // LUX_THREADS=N: fixed size for the event loops and both pools (a desktop shell does not need one per core).
    if (const char* e = std::getenv("LUX_THREADS"); e && *e) {
        long n = std::strtol(e, nullptr, 10);
        if (n > 0 && n <= 1024) num_threads = static_cast<unsigned>(n);
    }
    num_threads = std::max(1u, num_threads);

    // LUX_PIN_THREADS=1: loop i runs on the i-th cpu this process may use, and its listener asks the kernel for the
    // connections whose packets arrive there (SO_INCOMING_CPU): the packet, the socket and the loop that serves it
    // then share a cpu, which is what a NIC with receive-side scaling can give. Off by default: pinned threads do
    // not share a busy host well, and on loopback (every packet is received on the sender's cpu) it changes nothing.
    std::vector<int> pin_cpus;
#ifndef _WIN32
    if (const char* e = std::getenv("LUX_PIN_THREADS"); e && *e && *e != '0') {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (::sched_getaffinity(0, sizeof(mask), &mask) == 0)
            for (int c = 0; c < CPU_SETSIZE; ++c) if (CPU_ISSET(c, &mask)) pin_cpus.push_back(c);
    }
#endif
    auto pin_to = [&pin_cpus](unsigned index) -> int {
#ifndef _WIN32
        if (pin_cpus.empty()) return -1;
        const int cpu = pin_cpus[index % pin_cpus.size()];
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(cpu, &one);
#ifdef __ANDROID__   // bionic has no pthread_setaffinity_np; pid 0 is the calling thread
        return ::sched_setaffinity(0, sizeof(one), &one) == 0 ? cpu : -1;
#else
        return ::pthread_setaffinity_np(::pthread_self(), sizeof(one), &one) == 0 ? cpu : -1;
#endif
#else
        (void)index;
        return -1;
#endif
    };

    // Shared pool for route handlers with no `await` at all: pure CPU-bound
    // work (see BlockingAwaitable, blocking_pool.hpp) that would otherwise
    // run inline on whichever core's loop accepted the connection, blocking
    // it from serving anyone else meanwhile. Core-count core workers plus a
    // small measured-not-guessed overflow ceiling (BlockingPool::start's own
    // default, core+8) -- see blocking_pool.hpp for the actual numbers this
    // was picked from: it is the knee of a real variance-vs-typical-case-cost
    // curve, not a round number.
    blocking_pool().start(num_threads, 0, std::chrono::milliseconds(10));

    // Separate pool for is_async native module calls (os.run(), http.*,
    // read_file()/write_file()) -- see io_blocking_pool()'s comment
    // (blocking_pool.hpp) for why sharing blocking_pool() above starved
    // unrelated requests behind a burst of slow ones. These workers spend
    // nearly all their time blocked on a subprocess or a socket, not a CPU
    // core, so a much bigger ceiling than the CPU-bound pool's costs little:
    // a handful of core-sized permanent workers for the common case, with
    // plenty of overflow room (self-retiring after 2s idle, same as the
    // other pool) for a burst of concurrent slow calls to not queue up
    // behind each other.
    io_blocking_pool().start(num_threads, num_threads * 16);

    // Descriptors: the soft limit up to the hard one (often 1024 of
    // 524288), as Go does at startup -- at 1024 the server ran out of them
    // near a thousand connections. The connection cap follows from it
    // unless it was set.
#ifdef _WIN32
    // No descriptor limit to raise (WSAPoll is not bound by FD_SETSIZE).
    if (max_connections_ <= 0) max_connections_ = 10'000;
#else
    {
        rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            ::setrlimit(RLIMIT_NOFILE, &rl);
            ::getrlimit(RLIMIT_NOFILE, &rl);
        }
        // The kernel grows the descriptor table by doubling it, and every doubling stalls all the
        // threads of the process (expand_files waits for an RCU grace period): seen at ~260k open
        // sockets as 20 ms in accept4 and a loop frozen for a second. Duplicating a descriptor onto a
        // high number makes it that big once, now, while nothing is running; the table never shrinks.
        {
            const rlim_t top = rl.rlim_cur == RLIM_INFINITY ? rlim_t(1) << 19 : std::min<rlim_t>(rl.rlim_cur, rlim_t(1) << 19);
            if (top > 4096) {
                const int fd = ::fcntl(0, F_DUPFD_CLOEXEC, static_cast<int>(top) - 1);
                if (fd >= 0) ::close(fd);
            }
        }
        if (max_connections_ <= 0) {
            const rlim_t fds = rl.rlim_cur == RLIM_INFINITY ? rlim_t(1) << 20 : rl.rlim_cur;
            max_connections_ = static_cast<int>(std::min<rlim_t>(
                fds > 2048 ? fds - 1024 : fds * 3 / 4, 1 << 30));
        }
    }
#endif

    // Shared connection counter — enforces max_connections_ across all threads.
    auto shared_conn_count = std::make_shared<std::atomic<int>>(0);
    Metrics::instance().active_connections_ = shared_conn_count.get();

    std::vector<core::EventLoop*>  all_loops;
    std::vector<core::TcpServer*>  all_servers;
    std::mutex                     all_mutex;

    // ── Graceful shutdown ─────────────────────────────────────────────────────
    // On first SIGINT/SIGTERM:
    //   1. Signal handler writes one byte to g_signal_pipe[1] (async-signal-safe).
    //   2. The main epoll loop detects it and runs g_initiate_drain on its thread:
    //      stop accepting + poll every 100 ms until connections drain or 30 s elapse.
    // On second signal: std::_Exit(1) — see signal_handler above.
    g_signal_count = 0;
    if (plat::pipe_open(g_signal_pipe) < 0)
        throw std::runtime_error("signal pipe: " + plat::err_str());

    core::EventLoop main_loop;
    {
        std::lock_guard<std::mutex> lk(all_mutex);
        all_loops.push_back(&main_loop);
    }

    g_initiate_drain = [this, &main_loop, shared_conn_count, &all_loops, &all_servers, &all_mutex]() {
        shutting_down().store(true, std::memory_order_release);   // lets SSE streams end (see cancel.hpp)
        {
            // Each server stops accepting ON ITS OWN loop.  Doing it from here
            // —which is the main loop's thread— touched the handler map of the
            // other N loops while they were reading it.  Four lines below
            // post() is already used for its own work: it is the same
            // mechanism, applied to everyone.
            //
            // The lag is one loop turn, in which a worker could still accept a
            // connection.  It does not matter: the drain waits up to 30 seconds
            // for none to be left.
            std::lock_guard<std::mutex> lk(all_mutex);
            for (auto* s : all_servers) s->pedir_parada();
        }

        main_loop.post([this, &main_loop, shared_conn_count, &all_loops, &all_mutex]() {
            using Clock = std::chrono::steady_clock;
            auto deadline = Clock::now() + std::chrono::seconds(30);

            auto fn = std::make_shared<std::function<void()>>();
            *fn = [this, fn, &main_loop, shared_conn_count, deadline,
                   &all_loops, &all_mutex]() mutable {
                bool timed_out = Clock::now() >= deadline;
                int  remaining = shared_conn_count->load(std::memory_order_acquire);

                if (remaining == 0 || timed_out) {
                    if (timed_out && remaining > 0)
                        log().warn("shutdown: grace period expired — ",
                                   remaining, " connection(s) dropped");
                    else
                        log().info("shutdown: all connections drained");
                    // Last moment when the loops are still alive: here goes
                    // the shutdown of anything with its own threads posting to
                    // them.  If it were done later, those threads would write
                    // destruido.
                    if (before_stop_) before_stop_();

                    std::lock_guard<std::mutex> lk(all_mutex);
                    for (auto* l : all_loops) l->stop();
                    return;
                }
                main_loop.schedule_timer(100, *fn);
            };
            (*fn)();
        });
    };

    // Register the signal pipe with the main event loop.
    // When the signal handler fires it writes a byte here; the loop thread
    // calls g_initiate_drain safely without any async-signal-safe concerns.
    main_loop.add(g_signal_pipe[0], EPOLLIN,
                  [](uint32_t) {
                      char buf[16];
                      while (plat::pipe_read(g_signal_pipe[0], buf, sizeof(buf)) > 0) {}
                      if (g_initiate_drain) g_initiate_drain();
                  });

#ifdef _WIN32
    ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    // the shell (and run_tests) std::raise(SIGTERM): the CRT delivers it to this handler
    std::signal(SIGTERM, signal_handler);
#else
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    // ── Worker threads (cores 1..N-1) ─────────────────────────────────────────
    // Each thread runs its own EventLoop + TcpServer.  SO_REUSEPORT spreads
    // the accepts; the group then evens out the connections themselves.
    // SO_REUSEPORT lets every loop own a listener; Windows has none, so only
    // the main loop listens and hands connections to the workers.
#ifdef _WIN32
    constexpr bool kWorkersListen = false;
#else
    constexpr bool kWorkersListen = true;
#endif
    auto group = std::make_shared<core::TcpServer::Group>();
    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (unsigned i = 1; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            plat::set_thread_name("lux-loop");
            const int cpu = pin_to(i);
            core::EventLoop loop;
            core::TcpServer server(host, port, loop, dispatch,
                                   max_connections_, shared_conn_count, group,
                                   /*listen_here=*/kWorkersListen);
            if (cpu >= 0) server.prefer_cpu(cpu);
            {
                std::lock_guard<std::mutex> lk(all_mutex);
                all_loops.push_back(&loop);
                all_servers.push_back(&server);
            }
            loop.run();
            {
                // Remove from tracking after the loop exits so stop_accepting()
                // is never called on a destroyed server.
                std::lock_guard<std::mutex> lk(all_mutex);
                all_loops.erase(std::remove(all_loops.begin(), all_loops.end(), &loop), all_loops.end());
                all_servers.erase(std::remove(all_servers.begin(), all_servers.end(), &server), all_servers.end());
            }
        });
    }

    // ── Main thread (core 0) ──────────────────────────────────────────────────
    const int main_cpu = pin_to(0);
    core::TcpServer main_server(host, port, main_loop, dispatch,
                                max_connections_, shared_conn_count, group);
    if (main_cpu >= 0) main_server.prefer_cpu(main_cpu);
    {
        std::lock_guard<std::mutex> lk(all_mutex);
        all_servers.push_back(&main_server);
    }

    const char* scheme = tls::enabled() ? "https" : "http";
    log().info("Lux running on ", scheme, "://", host, ':', port,
               " (threads=", num_threads, ", press CTRL+C to quit)");

    if (on_start_) on_start_(main_loop);
    main_loop.run();

    for (auto& t : threads) t.join();

    // Restore default signal disposition BEFORE closing the pipe.  Otherwise
    // a stray SIGINT/SIGTERM between the close and the SIG_DFL reset would
    // run signal_handler with g_signal_pipe[1] either invalid or already
    // reassigned to an unrelated fd opened in another thread.
#ifdef _WIN32
    ::SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    std::signal(SIGTERM, SIG_DFL);
#else
    std::signal(SIGINT,  SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
#endif

    Metrics::instance().active_connections_ = nullptr;
    g_initiate_drain = nullptr;
    g_signal_count   = 0;
    if (g_signal_pipe[0] >= 0) { plat::pipe_close(g_signal_pipe[0]); g_signal_pipe[0] = -1; }
    if (g_signal_pipe[1] >= 0) { plat::pipe_close(g_signal_pipe[1]); g_signal_pipe[1] = -1; }
}

} // namespace lux
