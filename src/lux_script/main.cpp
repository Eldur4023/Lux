// The Lux binary: it reads .lux files and serves.
#include <lux_script/project.hpp>
#include <lux_script/vm.hpp>
#include <lux_script/autotest.hpp>
#include <lux_script/db.hpp>
#include <lux_script/value.hpp>

#include <lux/app.hpp>
#include <lux/middleware.hpp>
#include <lux/logger.hpp>
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

std::shared_ptr<lux_script::Module> current_module() {
    std::lock_guard<std::mutex> lk(g_module_mutex);
    return g_module;
}

void publish_module(std::shared_ptr<lux_script::Module> m) {
    std::lock_guard<std::mutex> lk(g_module_mutex);
    g_module = std::move(m);
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
        "                  can be; turns off hot reload, same as --no-watch\n";
}

// Watches the compiled files and recompiles when it detects a change.
// If the new version does not compile, the error is printed and the previous
// one keeps serving: a typo never takes the server down.
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
        lux::log().info("reloaded: " + std::to_string(next->program.routes.size()) +
                           " route(s) — " + std::to_string(next->declarative_routes) +
                           " declarative, " + std::to_string(next->vm_routes) +
                           " with logic");
        lanzar_autotest(next);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    bool check_only = false, watch = true, verbose = false, native = false, json_output = false;
    int  port_override = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
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

    std::cout << "lux: " << inputs.size() << " file(s), "
              << mod->program.routes.size() << " route(s) — "
              << mod->declarative_routes << " declarative, "
              << mod->vm_routes << " with logic\n";
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
        // A partial (or total) degradation to bytecode is never a compile
        // error -- see the comment on Module::native_warning -- but it
        // should not pass in silence either.
        if (!mod->native_warning.empty())
            std::cerr << mod->native_warning << "\n";
    }
    if (check_only) return 0;

    publish_module(mod);

    const lux_script::AppDecl& cfg = mod->program.app;
    lux::App app;
    // One line per request, with its flush, costs close to 25% of the
    // throughput and multiplies median latency by 2.6: it is opt-in.
    // Startup, reloads and autotest are always shown.
    if (verbose) app.use(lux::logger());

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
    auto dispatch = [](lux::Request& req, lux::Response& res) -> lux::Task<void> {
        auto mod = current_module();
        if (!mod) { res.status(503).json_text(R"({"error":"no module loaded"})"); co_return; }

        auto match = mod->router.match(req.method, req.path);
        if (!match.found) {
            lux_script::Value::Dict d;
            d["error"] = lux_script::Value::str("Not Found");
            d["path"]  = lux_script::Value::str(req.path);
            res.status(404).json_text(lux_script::Value::dict(std::move(d)).to_json_text());
            co_return;
        }
        req.params = std::move(match.params);
        co_await match.handler(req, res);
    };
    app.any("/",  dispatch);
    app.any("/*", dispatch);

    // See App::set_route_probe's comment: router_ only ever holds the two
    // catch-all entries just above, so App::handle_request()'s own default
    // "is there a declared route here?" check (router_.match(...).found)
    // is always true for this engine and would permanently shadow every
    // static mount. Ask the module's REAL router instead — the same one
    // `dispatch` above queries to answer the request itself, so a static
    // mount and a live route can never disagree about which of them wins.
    app.set_route_probe([](const std::string& method, const std::string& path) {
        auto mod = current_module();
        return mod && mod->router.match(method, path).found;
    });

    // `on error` handlers: a single one is registered in the engine and the
    // dispatch by code is done by the live module, just as with the routes, so
    // that a hot reload reaches them too.
    app.on_error([](int code, lux::Request& req, lux::Response& res) {
        auto mod = current_module();
        if (!mod) return;

        auto it = mod->error_handlers.find(code);
        if (it == mod->error_handlers.end()) it = mod->error_handlers.find(0);
        if (it == mod->error_handlers.end()) return;   // no handler: whatever is there is left alone

        lux_script::NativeCtx ctx{req, res};
        ctx.error_code     = code;
        // The real runtime-error text (division by zero, an out-of-range
        // index, ...) that project.cpp/native_gen.cpp already put in
        // last_internal_error() right before writing their own 500 body --
        // gated by is_production_mode() HERE, the one place `error.message`
        // actually reaches the client, the same way the raw 500 body
        // already is. Before this, `error.message` was hardcoded to the
        // string "internal error" unconditionally (not gated by production
        // mode at all), so GUIDE.md's own `log.error(error.message)`
        // example never logged anything useful, even in development.
        // "invalid request" is unrelated to this and unchanged: a 4xx has
        // no VM error behind it to report -- it is the client's request
        // that was wrong, not a bug to surface detail about.
        if (res.status_code() >= 500) {
            ctx.error_message = lux_script::is_production_mode()
                ? "internal error" : lux_script::last_internal_error();
            // Cleared right after reading, not just after setting: a 500 a
            // handler returns directly (`return status(500)`, no VM crash
            // behind it) never touches last_internal_error() at all, and
            // without this it would otherwise inherit whatever an EARLIER,
            // unrelated request on this same thread last crashed with.
            lux_script::last_internal_error().clear();
        } else {
            ctx.error_message = "invalid request";
        }
        ctx.error_messages = &lux_script::last_validation_messages();

        lux_script::VM  vm;
        const lux_script::NativeDispatch native =
            mod->native ? mod->native->dispatch() : lux_script::NativeDispatch{};
        auto result = vm.start(*it->second, {}, ctx, &mod->functions, &native);
        if (result.status == lux_script::VM::Status::Error) {
            lux::log().error("on error " + std::to_string(code) + ": " + result.error);
            return;
        }
        if (result.status != lux_script::VM::Status::Done) return;   // it cannot suspend

        if (!ctx.response_written && !result.value.is_null())
            res.header("Content-Type", "application/json; charset=utf-8")
               .send(result.value.to_json_text());

        // The handler describes the failure; it cannot turn it into a success.
        res.status(code);
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

    app.run(port_override ? static_cast<uint16_t>(port_override)
                          : static_cast<uint16_t>(cfg.port));

    g_stop.store(true);
    if (watcher.joinable()) watcher.join();
    return 0;
}
