#include "../include/lux/app.hpp"
#include "../include/lux/logger.hpp"
#include "../include/lux/metrics.hpp"
#include "../include/lux/request.hpp"
#include "../include/lux/response.hpp"
#include "../include/lux/task.hpp"
#include "../include/lux/blocking_pool.hpp"
#include "../include/lux/percent_encoding.hpp"

#include <lux/core/event_loop.hpp>
#include "core/tcp_server.hpp"

#include <sys/epoll.h>

#include <csignal>
#include <sched.h>
#include <iostream>
#include <memory>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>
#include <mutex>
#include <functional>
#include <sstream>
#include <string_view>
#include <utility>
#include <iomanip>
#include <unistd.h>
#include <fcntl.h>

#include <lux/mime.hpp>

namespace lux {

namespace {


// Weak ETag from mtime + size: "mtime-size" hex-encoded.
static std::string make_etag(const std::filesystem::file_time_type& mtime,
                              std::uintmax_t size) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  mtime.time_since_epoch()).count();
    std::ostringstream ss;
    ss << '"' << std::hex << ns << '-' << size << '"';
    return ss.str();
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

// Returns true and fills res if a static mount covers this path.
// Sets ETag, Cache-Control, and honours If-None-Match for 304 responses.
static bool try_serve_static(
    const std::vector<App::StaticMount>& mounts,
    const Request& req,
    Response& res)
{
    namespace fs = std::filesystem;
    auto fail = [&](int code, const char* body) {
        res.status(code).json_text(body);
        return true;
    };
    for (const auto& m : mounts) {
        if (req.path.rfind(m.prefix, 0) != 0) continue;
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
            req.path.size() > plen && req.path[plen] != '/') continue;

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
        static const std::string kWellKnown = "/.well-known/";
        bool is_well_known = rel.compare(0, kWellKnown.size(), kWellKnown) == 0;
        if (!is_well_known) {
            for (size_t i = 0; i < rel.size(); ++i) {
                if (rel[i] == '/' && i + 1 < rel.size() && rel[i + 1] == '.') return fail(404, R"({"error":"Not Found"})");
            }
        }

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

        // ── ETag ──────────────────────────────────────────────────────────────
        std::error_code mtime_ec, size_ec;
        auto mtime    = fs::last_write_time(canonical_file, mtime_ec);
        auto filesize = fs::file_size(canonical_file, size_ec);
        if (mtime_ec || size_ec) return fail(500, R"({"error":"Cannot stat file"})");
        std::string etag = make_etag(mtime, filesize);

        // ── Cache-Control ─────────────────────────────────────────────────────
        // Hashed filenames (e.g. app.abc123ef.js) → immutable for 1 year.
        // Detects a hash segment: last component after '.' or '-' is ≥8 hex chars.
        // Everything else → must-revalidate with short max-age.
        const std::string& ext  = canonical_file.extension().string();
        const std::string  stem = canonical_file.stem().string();
        auto is_hex_hash = [](const std::string& s) -> bool {
            auto pos = s.find_last_of(".-");
            if (pos == std::string::npos) return false;
            const auto seg = s.substr(pos + 1);
            if (seg.size() < 8) return false;
            return std::all_of(seg.begin(), seg.end(),
                               [](unsigned char c){ return std::isxdigit(c); });
        };
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
        const char* cache_ctrl = canonical_file.filename() == "index.html"
            ? "no-cache"
            : is_hex_hash(stem)
                ? "public, max-age=31536000, immutable"
                : "public, max-age=3600, must-revalidate";

        const char* mime = mime_for_ext(ext);  // lux/mime.hpp
        res.header("ETag",          etag);
        res.header("Cache-Control", cache_ctrl);
        res.header("Content-Type",  mime);

        // ── 304 Not Modified ──────────────────────────────────────────────────
        auto inm = req.header("if-none-match");
        if (inm && *inm == etag) {
            res.status(304).send("");
            return true;
        }

        // ── Serve via sendfile(2) — zero-copy ─────────────────────────────────
        res.send_file(canonical_file, filesize);
        return true;
    }
    return false;
}

// ── Graceful shutdown ─────────────────────────────────────────────────────────
// Signal handler writes one byte to a pipe; the event loop thread reads it
// and runs the actual drain logic.  This keeps the handler async-signal-safe:
// only write(2) and _Exit(2) are used — both appear in the POSIX safe list.
//
// g_signal_pipe[0] = read end (monitored by epoll on the main loop)
// g_signal_pipe[1] = write end (written by the signal handler)
static int                   g_signal_pipe[2] = {-1, -1};
static std::function<void()> g_initiate_drain;
static volatile sig_atomic_t g_signal_count   = 0;

static void signal_handler(int) {
    if (++g_signal_count >= 2) {
        static const char msg[] = "\nForced exit.\n";
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
        std::_Exit(1);
    }
    static const char msg[] = "\nShutting down gracefully... (CTRL+C again to force)\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    // Wake the event loop — write is async-signal-safe.
    if (g_signal_pipe[1] >= 0) {
        char byte = 1;
        (void)write(g_signal_pipe[1], &byte, 1);
    }
}

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
        auto rmatch = router_.match(req.method, req.path);
        bool route_exists = rmatch.found && !rmatch.via_wildcard;
        if (!route_exists && route_probe_) route_exists = route_probe_(req.method, req.path);
        if (!route_exists && try_serve_static(static_mounts_, req, res)) co_return;
    }

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
                co_await match.handler(req, res);
            } else {
                res.status(404).json_text(R"({"error":"Not Found"})");
            }
        }
    };
    co_await (*call_next)(0);

    // Error handlers run after the full chain, while we still own req/res.
    // Async handlers take precedence over sync handlers for the same code.
    if (res.status_code() >= 400) {
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
    std::signal(SIGPIPE, SIG_IGN);

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
    {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (::sched_getaffinity(0, sizeof(mask), &mask) == 0)
            num_threads = static_cast<unsigned>(CPU_COUNT(&mask));
    }
    if (num_threads == 0) num_threads = std::thread::hardware_concurrency();
    num_threads = std::max(1u, num_threads);

    // Shared pool for route handlers with no `await` at all: pure CPU-bound
    // work (see BlockingAwaitable, blocking_pool.hpp) that would otherwise
    // run inline on whichever core's loop accepted the connection, blocking
    // it from serving anyone else meanwhile. Core-count core workers plus a
    // small measured-not-guessed overflow ceiling (BlockingPool::start's own
    // default, core+8) -- see blocking_pool.hpp for the actual numbers this
    // was picked from: it is the knee of a real variance-vs-typical-case-cost
    // curve, not a round number.
    blocking_pool().start(num_threads);

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
    if (::pipe2(g_signal_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        throw std::runtime_error(std::string("pipe2: ") + strerror(errno));

    core::EventLoop main_loop;
    {
        std::lock_guard<std::mutex> lk(all_mutex);
        all_loops.push_back(&main_loop);
    }

    g_initiate_drain = [this, &main_loop, shared_conn_count, &all_loops, &all_servers, &all_mutex]() {
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
                      while (::read(g_signal_pipe[0], buf, sizeof(buf)) > 0) {}
                      if (g_initiate_drain) g_initiate_drain();
                  });

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Worker threads (cores 1..N-1) ─────────────────────────────────────────
    // Each thread runs its own EventLoop + TcpServer.  SO_REUSEPORT lets the
    // kernel distribute incoming connections evenly across all workers.
    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (unsigned i = 1; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            core::EventLoop loop;
            core::TcpServer server(host, port, loop, dispatch,
                                   max_connections_, shared_conn_count);
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
    core::TcpServer main_server(host, port, main_loop, dispatch,
                                max_connections_, shared_conn_count);
    {
        std::lock_guard<std::mutex> lk(all_mutex);
        all_servers.push_back(&main_server);
    }

    const char* scheme = "http";
    log().info("Lux running on ", scheme, "://", host, ':', port,
               " (threads=", num_threads, ", press CTRL+C to quit)");

    main_loop.run();

    for (auto& t : threads) t.join();

    // Restore default signal disposition BEFORE closing the pipe.  Otherwise
    // a stray SIGINT/SIGTERM between the close and the SIG_DFL reset would
    // run signal_handler with g_signal_pipe[1] either invalid or already
    // reassigned to an unrelated fd opened in another thread.
    std::signal(SIGINT,  SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);

    Metrics::instance().active_connections_ = nullptr;
    g_initiate_drain = nullptr;
    g_signal_count   = 0;
    if (g_signal_pipe[0] >= 0) { ::close(g_signal_pipe[0]); g_signal_pipe[0] = -1; }
    if (g_signal_pipe[1] >= 0) { ::close(g_signal_pipe[1]); g_signal_pipe[1] = -1; }
}

} // namespace lux
