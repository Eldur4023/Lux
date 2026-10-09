// The Lux binary: it reads .lux files and serves.
#include "../http/http_parser.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <malloc.h>
#include <lux_script/schedule.hpp>
#include <lux_script/project.hpp>
#include <lux_script/vm.hpp>
#include <lux_script/autotest.hpp>
#include <lux_script/db.hpp>
#include <lux_script/value.hpp>

#include <lux/app.hpp>
#include <lux/middleware.hpp>
#include <lux/logger.hpp>
#include <lux/tls.hpp>
#include <lux/openapi.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

namespace {

// The live module.  The dispatcher reads it on every request and the watcher
// replaces it on recompile: switching version is publishing a shared_ptr, not
// touching the router in use.
std::shared_ptr<lux_script::Module> g_module;
std::mutex                    g_module_mutex;
std::atomic<bool>             g_stop{false};
lux_script::AutotestOptions         g_autotest;

// The probe talks over the socket, so it cannot run on the thread that just
// reloaded: it is launched separately and left to finish on its own.
void lanzar_autotest(std::shared_ptr<lux_script::Module> mod) {
    if (!g_autotest.enabled || !mod) return;
    std::thread([mod] { lux_script::run_autotest(*mod, g_autotest); }).detach();
}

std::atomic<uint64_t>          g_module_gen{0};

// Every request asks for the module: a copy per thread, refreshed only when a
// reload bumps the generation, instead of a global mutex on every call. The
// reference is good until this thread's next call; keep a copy to hold it
// across a co_await.
const std::shared_ptr<lux_script::Module>& current_module() {
    thread_local std::shared_ptr<lux_script::Module> mine;
    thread_local uint64_t mine_gen = UINT64_MAX;
    if (const uint64_t g = g_module_gen.load(std::memory_order_acquire); g != mine_gen) {
        std::lock_guard<std::mutex> lk(g_module_mutex);
        mine = g_module;
        mine_gen = g;
    }
    return mine;
}

void publish_module(std::shared_ptr<lux_script::Module> m) {
    std::lock_guard<std::mutex> lk(g_module_mutex);
    g_module = std::move(m);
    g_module_gen.fetch_add(1, std::memory_order_release);
}

void usage() {
    std::cerr <<
        "usage: lux [options] <file.lux | files... | directory>\n"
        "\n"
        "  One file        compiles just that file\n"
        "  Several files   compiles just those\n"
        "  A directory     compiles every .lux inside it, recursively\n"
        "\n"
        "options:\n"
        "  --check         compile and exit, without starting the server\n"
        "  --json          like --check, but the diagnostics (if any) print to\n"
        "                  stdout as a JSON array [{file,line,col,message}],\n"
        "                  instead of the human-readable format -- meant for\n"
        "                  tooling (see editors/vscode-lux), not for reading\n"
        "  --port N        override the port from the app: block\n"
        "  --no-watch      do not watch files for changes\n"
        "  --verbose       log every incoming request to the console\n"
        "  --autotest      walk the endpoints on startup and on every reload\n"
        "  --autotest=all  also include POST/PUT/PATCH/DELETE\n"
        "  --native        compiles to native code (g++) the functions that\n"
        "                  can be; turns off hot reload, same as --no-watch\n"
        "\n"
        "       lux run <files|directory> -- <command> [args...]\n"
        "\n"
        "  Runs the `command \"name\":` block (after the `on start:` blocks)\n"
        "  once, as a script: os.argv(), os.input(), os.exit(code)\n"
        "\n"
        "       lux test <files|directory> [-- name-filter]\n"
        "\n"
        "  Starts the app on a free port and runs its `test \"name\":` blocks\n"
        "  against it (module `test`: ok, eq, contains, fail, base_url)\n"
        "\n"
        "       lux restore <replica> <out.db>\n"
        "\n"
        "  Rebuilds a sqlite database from a `replicate` target of its\n"
        "  sqlite: block (s3://..., sftp://... or a directory)\n";
}

// Watches the compiled files and recompiles when it detects a change.
// If the new version does not compile, the error is printed and the previous
// one keeps serving: a typo never takes the server down.
// "5 route(s) — 1 declarative, 4 with logic, 2 scheduled task(s)"
std::string route_summary(const lux_script::Module& m) {
    size_t declarative = 0, logic = 0, tasks = 0;
    for (const auto& r : m.route_report) {
        if (r.method == "EVERY") ++tasks;
        else if (r.path == "declarative") ++declarative;
        else ++logic;
    }
    std::string s = std::to_string(declarative + logic) + " route(s) — " + std::to_string(declarative) +
                    " declarative, " + std::to_string(logic) + " with logic";
    if (tasks) s += ", " + std::to_string(tasks) + " scheduled task(s)";
    return s;
}

void watch_loop(std::vector<fs::path> inputs) {
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (g_stop.load()) return;

        auto mod = current_module();
        if (!mod) continue;

        bool changed = false;
        std::error_code ec;
        for (const auto& [path, stamp] : mod->stamps) {
            auto now = fs::last_write_time(path, ec);
            if (!ec && now != stamp) { changed = true; break; }
        }
        if (!changed) continue;

        lux::log().info("changes detected: recompiling");

        // Grace period so the editor finishes writing the file.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        lux_script::DiagnosticBag diags;
        auto next = lux_script::compile(inputs, diags);
        if (!diags.empty()) {
            // The diagnostics point at the files of the failed attempt, not at
            // those of the live module: they have to be formatted with
            // next->files, which stays alive as long as `next` does.
            std::cerr << "\n" << lux_script::format_errors(diags, next->files)
                      << "reload cancelled: still serving the previous version\n\n";
            // The live module's mtimes are refreshed so as not to retry in a
            // loop over a file that is still broken.
            for (auto& [path, stamp] : mod->stamps) {
                auto now = fs::last_write_time(path, ec);
                if (!ec) stamp = now;
            }
            continue;
        }

        publish_module(next);
        lux::log().info("reloaded: " + route_summary(*next));
        lanzar_autotest(next);
    }
}

} // namespace

// `on start:` blocks still running: requests wait (503) until it is zero.
std::atomic<int> g_starting{0};

// One `every` block: armed on the main loop, never run twice at once.
struct ScheduledTask : std::enable_shared_from_this<ScheduledTask> {
    std::string              path, label;
    lux_script::EverySpec    spec;
    lux::DispatchFn          dispatch;
    bool                     once = false;      // `on start:`
    bool                     running = false;   // touched only on the loop's thread

    static long long now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // In steps of at most an hour: schedule_timer takes an int of ms, and
    // "1d" or a daily time is further away than that.
    void arm(lux::core::EventLoop& loop, long long due) {
        const int wait = static_cast<int>(std::clamp(due - now(), 0LL, 3'600'000LL));
        loop.schedule_timer(wait, [self = shared_from_this(), &loop, due] {
            if (now() < due) { self->arm(loop, due); return; }
            self->run(loop);
            self->arm(loop, self->spec.after(due, now()));
        });
    }

    void run(lux::core::EventLoop& loop) {
        if (running) {
            lux::log().warn("every \"", label, "\": the previous run has not finished, skipping this one");
            return;
        }
        running = true;
        auto req = std::make_shared<lux::Request>();
        auto res = std::make_shared<lux::Response>();
        req->method    = "EVERY";
        req->path      = path;
        req->remote_ip = "127.0.0.1";
        req->loop      = &loop;
        auto task = [](std::shared_ptr<ScheduledTask> self, std::shared_ptr<lux::Request> req,
                       std::shared_ptr<lux::Response> res) -> lux::Task<void> {
            try {
                co_await self->dispatch(*req, *res);
                if (res->status_code() >= 400)
                    lux::log().error("every \"", self->label, "\" failed: ", res->body());
            } catch (const std::exception& e) {
                lux::log().error("every \"", self->label, "\" failed: ", e.what());
            }
            self->running = false;
            if (self->once) g_starting.fetch_sub(1);
        }(shared_from_this(), req, res);
        auto h = task.detach();
        h.promise().loop = &loop;
        h.resume();
    }
};

// `lux run files -- name args`: runs the `on start:` blocks, then the
// `command "name":`, on an event loop of its own, and returns the exit code.
static int run_command(const lux_script::Module& m, lux::DispatchFn dispatch,
                       const std::string& name, std::vector<std::string> args) {
    std::vector<std::string> paths;
    std::string              cmd_path, known;
    for (const auto& r : m.program.routes) {
        if (r.method != "EVERY") continue;
        if (r.every == "start") paths.push_back(r.pattern);
        else if (r.every.rfind("cmd:", 0) == 0) {
            known += (known.empty() ? "" : ", ") + r.every.substr(4);
            if (r.every.substr(4) == name) cmd_path = r.pattern;
        }
    }
    if (cmd_path.empty()) {
        std::cerr << "lux run: " << (name.empty() ? "no command given" : "no command '" + name + "'")
                  << (known.empty() ? "; the project declares none (command \"name\":)" : "; available: " + known) << "\n";
        return 2;
    }
    paths.push_back(cmd_path);

    auto& sm = lux_script::script_mode();
    sm.on   = true;
    sm.args = std::move(args);

    lux::core::EventLoop loop;
    int  rc = 0;
    bool done = false;
    auto task = [](lux::DispatchFn dispatch, std::vector<std::string> paths, int& rc, bool& done,
                   lux::core::EventLoop& loop) -> lux::Task<void> {
        for (size_t i = 0; i < paths.size(); ++i) {
            lux::Request  req;
            lux::Response res;
            req.method = "EVERY"; req.path = paths[i]; req.remote_ip = "127.0.0.1"; req.loop = &loop;
            try {
                co_await dispatch(req, res);
            } catch (const std::exception& e) {
                std::cerr << "lux run: " << e.what() << "\n";
                rc = 1;
                break;
            }
            if (res.status_code() >= 400) {
                std::cerr << "lux run: failed: " << res.body() << "\n";
                rc = 1;
                break;
            }
            if (lux_script::script_mode().exit_code) break;   // os.exit() in the middle
            if (i + 1 == paths.size() && !res.body().empty()) std::cout << res.body() << "\n";
        }
        done = true;
        loop.stop();
    }(std::move(dispatch), std::move(paths), rc, done, loop);
    auto h = task.detach();
    h.promise().loop = &loop;
    h.resume();
    if (!done) loop.run();
    std::cout.flush();
    return rc ? rc : sm.exit_code;
}

std::atomic<int> g_test_rc{0};

// `lux test`: once the server is up and its `on start:` blocks are done, runs
// every `test "name":` (all, or those whose name contains `filter`), in the
// order written, then stops the server. Runs on a thread of its own.
static void run_tests(const lux_script::Module& m, lux::DispatchFn dispatch, std::string filter) {
    while (g_starting.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    struct T { std::string name, path; };
    std::vector<T> tests;
    for (const auto& r : m.program.routes)
        if (r.method == "EVERY" && r.every.rfind("test:", 0) == 0 && r.every.find(filter, 5) != std::string::npos)
            tests.push_back({r.every.substr(5), r.pattern});
    if (tests.empty()) std::cout << "lux test: no tests" << (filter.empty() ? "" : " matching '" + filter + "'") << "\n";

    lux::core::EventLoop loop;
    int  failed = 0;
    bool done = false;
    auto task = [](lux::DispatchFn dispatch, std::vector<T> tests, int& failed, bool& done,
                   lux::core::EventLoop& loop) -> lux::Task<void> {
        for (const auto& t : tests) {
            lux::Request  req;
            lux::Response res;
            req.method = "EVERY"; req.path = t.path; req.remote_ip = "127.0.0.1"; req.loop = &loop;
            lux_script::script_mode().failure.clear();
            std::string why;
            try {
                co_await dispatch(req, res);
                if (!lux_script::script_mode().failure.empty()) why = lux_script::script_mode().failure;
                else if (res.status_code() >= 400) why = res.body();
            } catch (const std::exception& e) {
                why = e.what();
            }
            if (why.empty()) std::cout << "  ok    " << t.name << "\n";
            else { ++failed; std::cout << "  FAIL  " << t.name << ": " << why << "\n"; }
            std::cout.flush();
        }
        done = true;
        loop.stop();
    }(std::move(dispatch), tests, failed, done, loop);
    auto h = task.detach();
    h.promise().loop = &loop;
    h.resume();
    if (!done) loop.run();
    std::cout << (failed ? "FAILED: " + std::to_string(failed) + " of " : "all ") << tests.size()
              << (failed ? " tests\n" : " tests passed\n");
    std::cout.flush();
    g_test_rc = failed ? 1 : 0;
    ::kill(::getpid(), SIGTERM);   // graceful shutdown of the server...
    // ...but the tests' own http client may hold a keep-alive connection to it, which the
    // drain would wait 30 s for: a moment's grace, then out with the result.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::_Exit(g_test_rc.load());
}

// Desktop apps (Drive Sync, Calendar, Mail): their shell links this file with LUX_EMBEDDED and
// calls lux_main() on a thread, so it serves the app exactly as `lux` does
// (every:, on start:, limits...) instead of keeping a second copy of this wiring.
#ifdef LUX_EMBEDDED
int lux_main(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    // glibc mmaps every allocation past 128 KB and unmaps it when freed: a
    // file read into a string or a big response paid a fresh zeroed mapping
    // per request, and each unmap interrupted every other thread (a TLB
    // shootdown). Up to 1 MB is served, and reused, from the heap.
    // ponytail: each arena may keep up to 8 MB of freed memory; lower
    // M_TRIM_THRESHOLD if resident size matters more than those faults.
#ifndef __ANDROID__   // glibc tuning knobs; bionic's allocator has neither
    mallopt(M_MMAP_THRESHOLD, 1 << 20);
    mallopt(M_TRIM_THRESHOLD, 8 << 20);
#endif
    if (argc >= 2 && std::string(argv[1]) == "restore")
        return lux_script::restore_main({argv + 2, argv + argc});

    std::vector<std::string> args;
    const bool run_mode  = argc >= 2 && std::string(argv[1]) == "run";
    const bool test_mode = argc >= 2 && std::string(argv[1]) == "test";
    std::string              command_name;
    std::vector<std::string> command_args;
    bool check_only = false, watch = true, verbose = false, native = false, json_output = false;
    int  port_override = 0;

    if (run_mode || test_mode) watch = false;
    for (int i = (run_mode || test_mode) ? 2 : 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((run_mode || test_mode) && a == "--") {   // everything after is the command and its arguments
            if (i + 1 < argc) command_name = argv[++i];
            for (++i; i < argc; ++i) command_args.push_back(argv[i]);
            break;
        }
        if      (a == "--check")    check_only = true;
        else if (a == "--json")     { check_only = true; json_output = true; }
        else if (a == "--no-watch") watch = false;
        else if (a == "--verbose")  verbose = true;
        else if (a == "--native")   { native = true; watch = false; }
        else if (a == "--autotest")     g_autotest.enabled = true;
        else if (a == "--autotest=all") { g_autotest.enabled = true; g_autotest.unsafe = true; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--port") {
            if (i + 1 >= argc) { std::cerr << "--port needs a number\n"; return 2; }
            port_override = std::atoi(argv[++i]);
        }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option: " << a << "\n";
            usage();
            return 2;
        }
        else args.push_back(a);
    }

    if (args.empty()) { usage(); return 2; }

    std::vector<fs::path> inputs;
    std::string error;
    if (!lux_script::resolve_inputs(args, inputs, error)) {
        if (json_output) {
            lux_script::Value::List one;
            lux_script::Value::Dict d;
            d["file"] = lux_script::Value::str("");
            d["line"] = lux_script::Value::integer(0);
            d["col"]  = lux_script::Value::integer(0);
            d["message"] = lux_script::Value::str("lux: " + error);
            one.push_back(lux_script::Value::dict(std::move(d)));
            std::cout << lux_script::Value::list(std::move(one)).to_json_text() << "\n";
            return 2;
        }
        std::cerr << "lux: " << error << "\n";
        return 2;
    }

    lux_script::DiagnosticBag diags;
    auto mod = lux_script::compile(inputs, diags, native);
    if (json_output) {
        // Un unico camino de salida, reusando el mismo Value/to_json_text()
        // que ya sirve cualquier respuesta HTTP -- nada de un serializador
        // JSON aparte solo para esto. Vacio ([]) en exito: el cliente LSP
        // (editors/vscode-lux) siempre puede parsear stdout como JSON, sin
        // distinguir "sin errores" de "no hay salida".
        lux_script::Value::List items;
        for (const auto& diag : diags.items()) {
            lux_script::Value::Dict d;
            d["file"] = lux_script::Value::str(diag.loc.file ? *diag.loc.file : std::string());
            d["line"] = lux_script::Value::integer(diag.loc.line);
            d["col"]  = lux_script::Value::integer(diag.loc.col);
            d["message"] = lux_script::Value::str(diag.message);
            items.push_back(lux_script::Value::dict(std::move(d)));
        }
        std::cout << lux_script::Value::list(std::move(items)).to_json_text() << "\n";
        return diags.empty() ? 0 : 1;
    }
    if (!diags.empty()) {
        std::cerr << lux_script::format_errors(diags, mod->files)
                  << "\n" << diags.size() << " error(s)\n";
        return 1;
    }

    if (!run_mode) std::cout << "lux: " << inputs.size() << " file(s), " << route_summary(*mod) << "\n";   // stdout is the command's
    if (native) {
        std::cout << "lux: --native: " << (mod->native ? mod->native->compiled() : 0)
                  << " function(s), " << (mod->native ? mod->native->routes_compiled() : 0)
                  << " route(s) compiled to native code\n";
        // --native phase 6: explicit diagnostic of
        // which path serves EACH route with logic -- the aggregate count
        // above does not say which ones fell back to bytecode, and --native
        // is precisely the mode where that matters. Declarative/ws/sse
        // routes are not listed: they never depend on --native (a
        // declarative one already avoids bytecode on its own; ws/sse do not
        // compile to native yet, so "bytecode" on them says nothing new).
        for (const auto& r : mod->route_report) {
            if (r.path == "declarative" || r.path == "ws" || r.path == "sse") continue;
            std::cout << "lux:   " << r.method << " " << r.pattern << " -> " << r.path << "\n";
        }
        for (const auto& [fn, why] : mod->native_why.funciones)
            std::cout << "lux:   fn " << fn << " -> bytecode" << (why.empty() ? "" : " (" + why + ")") << "\n";
        // A partial (or total) degradation to bytecode is never a compile
        // error -- see the comment on Module::native_warning -- but it
        // should not pass in silence either.
        if (!mod->native_warning.empty())
            std::cerr << mod->native_warning << "\n";
    }
    if (check_only) return 0;

    publish_module(mod);

    const lux_script::AppDecl& cfg = mod->program.app;
    const lux_script::TlsDecl& tls = mod->program.tls;
    lux::App app;
    // One line per request, with its flush, costs close to 25% of the
    // throughput and multiplies median latency by 2.6: it is opt-in.
    // Startup, reloads and autotest are always shown.
    if (cfg.log.present) {
        lux::LoggerOptions lo;
        lo.file = !cfg.log.file.empty();
        if (lo.file) {
            const std::filesystem::path p(cfg.log.file);
            lo.dir      = p.has_parent_path() ? p.parent_path().string() : ".";
            lo.filename = p.filename().string();
        }
        lo.max_file_size = cfg.log.max_size;
        lo.max_files     = cfg.log.keep;
        lo.console       = cfg.log.console;
        lo.level = cfg.log.level == "debug" ? lux::LogLevel::Debug : cfg.log.level == "warn" ? lux::LogLevel::Warn
                 : cfg.log.level == "error" ? lux::LogLevel::Error : cfg.log.level == "off" ? lux::LogLevel::Off
                 : lux::LogLevel::Info;
        lux::log().configure(lo);
    }
    if (verbose || cfg.log.access) app.use(lux::logger());

    // The database module pools have their own threads that resume handlers by
    // posting to the event loop.  They have to be stopped and waited for while
    // the loops are still alive; otherwise a worker finishing late —SQLite can
    // sit up to 5 s in its busy handler— posts to an already destroyed loop.
    app.on_before_stop([] { lux_script::DbRegistry::instance().shutdown(); });
    for (const auto& m : cfg.statics) app.serve_static(m.url_prefix, m.fs_root, m.spa);
    // /openapi.json and /docs are served from the live module, not from a copy
    // frozen at startup: that way a hot reload updates the documentation too.
    //
    if (cfg.docs) {
        app.get("/openapi.json", [](lux::Response& res) {
            auto mod = current_module();
            if (!mod) {
                res.status(503).json_text(R"({"error":"no module"})");
                return;
            }
            res.json_text(mod->openapi);
        });
        app.get("/docs", [](lux::Response& res) {
            res.html(lux::swagger_ui_html("/openapi.json"));
        });
    }
    if (cfg.health)  app.enable_health();
    if (cfg.metrics) app.enable_metrics();

    // The real routing is done by the module, which is what gets swapped on
    // hot path; in the engine a single catch-all entry is enough.  The static
    // mounts are resolved before reaching here.
    //
    // Two patterns are needed: the radix tree wildcard covers one or more
    // segments, so the root "/" does not fit in "/*".
    // Not a coroutine: it returns the route's own Task, and the request
    // holds the module alive for as long as that runs (a reload may publish
    // another meanwhile).
    auto dispatch = [](lux::Request& req, lux::Response& res) -> lux::Task<void> {
        static constexpr auto done = []() -> lux::Task<void> { co_return; };
        const auto& mod = current_module();
        if (g_starting.load() > 0 && req.method != "EVERY") {
            res.status(503).header("Retry-After", "1").json_text(R"({"error":"starting"})");
            return done();
        }
        if (!mod) { res.status(503).json_text(R"({"error":"no module loaded"})"); return done(); }

        auto match = mod->router.match(req.method, req.path);
        if (!match.found) {
            lux_script::Value::Dict d;
            d["error"] = lux_script::Value::str("Not Found");
            d["path"]  = lux_script::Value::str(req.path);
            res.status(404).json_text(lux_script::Value::dict(std::move(d)).to_json_text());
            return done();
        }
        req.params = std::move(match.params);
        req._hold  = mod;
        return (*match.handler)(req, res);
    };
    if (test_mode) {
        // Its own port, so a running instance of the app does not get in the way.
        if (!port_override) {
            const int s = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t len = sizeof a;
            if (s >= 0 && ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 &&
                ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0)
                port_override = ntohs(a.sin_port);
            if (s >= 0) ::close(s);
        }
        lux_script::script_mode().base_url = "http://127.0.0.1:" + std::to_string(port_override ? port_override : cfg.port);
    }
    if (run_mode) return run_command(*mod, dispatch, command_name, std::move(command_args));
    app.any("/",  dispatch);
    app.any("/*", dispatch);

    std::function<void(lux::core::EventLoop&)> boot;
    // `every` blocks: one timer each on the main loop, dispatched like a
    // request to the live module, so a hot reload changes what a task does
    // (a new task, or a new schedule, needs a restart).
    {
        std::vector<std::shared_ptr<ScheduledTask>> tasks;
        for (const auto& r : mod->program.routes) {
            if (r.method != "EVERY" || r.every.rfind("cmd:", 0) == 0 || r.every.rfind("test:", 0) == 0) continue;   // commands and tests only run from `lux run` / `lux test`
            auto t = std::make_shared<ScheduledTask>();
            t->path = r.pattern;
            t->label = r.every;
            t->dispatch = dispatch;
            if (r.every == "start") { t->once = true; g_starting.fetch_add(1); }
            else lux_script::parse_every_spec(r.every, t->spec);
            tasks.push_back(std::move(t));
        }
        if (!tasks.empty())
            boot = [tasks](lux::core::EventLoop& loop) {
                for (const auto& t : tasks) {
                    if (t->once) t->run(loop);
                    else t->arm(loop, t->spec.next(ScheduledTask::now()));
                }
            };
    }
    if (test_mode) {   // the tests start once the server is listening
        auto inner = boot;
        boot = [inner, mod, dispatch, filter = command_name](lux::core::EventLoop& loop) {
            if (inner) inner(loop);
            std::thread(run_tests, std::cref(*mod), lux::DispatchFn(dispatch), filter).detach();
        };
    }
    if (boot) app.on_start(boot);

    // See App::set_route_probe's comment: router_ only ever holds the two
    // catch-all entries just above, so App::handle_request()'s own default
    // "is there a declared route here?" check (router_.match(...).found)
    // is always true for this engine and would permanently shadow every
    // static mount. Ask the module's REAL router instead — the same one
    // `dispatch` above queries to answer the request itself, so a static
    // mount and a live route can never disagree about which of them wins.
    app.set_route_probe([](const std::string& method, const std::string& path) {
        const auto& mod = current_module();
        return mod && mod->router.match(method, path).found;
    });

    // `on error` handlers: a single one is registered in the engine and the
    // dispatch by code is done by the live module, just as with the routes, so
    // that a hot reload reaches them too.
    app.on_async_error([](int code, lux::Request& req, lux::Response& res) -> lux::Task<void> {
        const auto mod = current_module();   // a copy: it stays alive across the handler's awaits
        if (!mod) co_return;
        co_await lux_script::run_error_handler(*mod, code, req, res);
    });

    std::thread watcher;
    if (watch) watcher = std::thread(watch_loop, inputs);

    // The first pass waits for the server to be listening.
    if (g_autotest.enabled) {
        g_autotest.port = port_override ? static_cast<uint16_t>(port_override)
                                        : static_cast<uint16_t>(cfg.port);
        std::thread([mod] {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            lux_script::run_autotest(*mod, g_autotest);
        }).detach();
    }

    if (!test_mode && tls.present) {
        std::string tls_err = "tls: needs both cert and key for HTTPS";
        if (tls.cert.empty() || tls.key.empty() || !lux::tls::init(tls.cert, tls.key, tls_err)) {
            std::cerr << "https: " << tls_err << "\n";
            return 1;
        }
    }
    lux::http::g_max_body_size = cfg.max_body;
    lux::global_headers() = cfg.headers;
    app.run(test_mode ? std::string("127.0.0.1") : cfg.host, port_override ? static_cast<uint16_t>(port_override)
                                    : static_cast<uint16_t>(cfg.port));

    g_stop.store(true);
    if (watcher.joinable()) watcher.join();
    return test_mode ? g_test_rc.load() : 0;
}
