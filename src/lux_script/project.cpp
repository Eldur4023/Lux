#include <lux_script/project.hpp>
#include <lux_script/template.hpp>
#include <lux_script/lexer.hpp>
#include <lux_script/parser.hpp>
#include <lux_script/emitter.hpp>
#include <lux_script/vm.hpp>
#include <lux_script/auth.hpp>
#include <lux_script/db.hpp>
#include <lux_script/builtin_module.hpp>

#include <lux/request.hpp>
#include <lux/response.hpp>
#include <lux/task.hpp>
#include <lux/sse.hpp>
#include <lux/websocket.hpp>
#include <lux/multipart.hpp>
#include <lux/app.hpp>
#include <lux/logger.hpp>
#include <lux/blocking_pool.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lux_script {

// ─── Input ───────────────────────────────────────────────────────────────────

bool resolve_inputs(const std::vector<std::string>& args,
                    std::vector<fs::path>& out,
                    std::string& error) {
    std::error_code ec;

    for (const auto& a : args) {
        fs::path p(a);
        if (!fs::exists(p, ec)) {
            error = "does not exist: " + a;
            return false;
        }

        if (fs::is_directory(p, ec)) {
            size_t before = out.size();
            for (auto it = fs::recursive_directory_iterator(p, ec);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (it->is_regular_file(ec) && it->path().extension() == ".lux")
                    out.push_back(it->path());
            }
            if (out.size() == before) {
                error = "the directory contains no .lux files: " + a;
                return false;
            }
        } else {
            out.push_back(p);
        }
    }

    // Stable order so the diagnostics always come out the same.  The order does
    // not affect meaning: name resolution runs in two passes.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());

    if (out.empty()) { error = "no files to compile"; return false; }
    return true;
}

std::string format_errors(const DiagnosticBag& diags,
                          const std::vector<std::unique_ptr<SourceFile>>& files) {
    std::vector<const SourceFile*> raw;
    raw.reserve(files.size());
    for (const auto& f : files) raw.push_back(f.get());
    return diags.format(raw);
}

// ─── Literal evaluation ──────────────────────────────────────────────────────
//
// A declarative route can only contain constant values: if something shows up
// that has to be computed, the route needs the VM and that is another milestone.

namespace {

// Lux Script type name for an already built value.  Used where the data is
// constant and therefore its type is exact.
std::string lux_script_type_of(const Value& v) {
    switch (v.type()) {
        case Value::Type::Str:   return "string";
        case Value::Type::Int:   return "int";
        case Value::Type::Float: return "float";
        case Value::Type::Bool:  return "bool";
        case Value::Type::List:  return "List";
        case Value::Type::Dict:  return "Dict";
        default:                 return {};
    }
}



// The same, but producing a Value instead of an nlohmann tree.
//
// The difference matters for key order: nlohmann keeps its objects in a
// std::map and sorts them alphabetically, while the VM returns them in the
// order they are written.  If declarative routes used nlohmann, two routes from
// the same .lux would order differently.
bool const_eval_valor(const Expr& e, Value& out) {
    switch (e.kind) {
        case ExprKind::StringLit: out = Value::str(e.text);          return true;
        case ExprKind::IntLit:    out = Value::integer(e.int_value); return true;
        case ExprKind::FloatLit:  out = Value::real(e.float_value);  return true;
        case ExprKind::BoolLit:   out = Value::boolean(e.bool_value); return true;
        case ExprKind::NullLit:   out = Value::null();               return true;

        case ExprKind::ListLit: {
            Value::List l;
            l.reserve(e.items.size());
            for (const auto& item : e.items) {
                Value v;
                if (!const_eval_valor(*item, v)) return false;
                l.push_back(std::move(v));
            }
            out = Value::list(std::move(l));
            return true;
        }
        case ExprKind::DictLit: {
            Value::Dict d;
            for (const auto& entry : e.entries) {
                if (entry.key->kind != ExprKind::StringLit) return false;
                Value v;
                if (!const_eval_valor(*entry.value, v)) return false;
                d[entry.key->text] = std::move(v);
            }
            out = Value::dict(std::move(d));
            return true;
        }
        default:
            return false;
    }
}

// Name of the called function, if it is a direct call to an identifier.
const std::string* callee_name(const Expr& e) {
    if (e.kind != ExprKind::Call || !e.object) return nullptr;
    if (e.object->kind != ExprKind::Ident)     return nullptr;
    return &e.object->text;
}

// Replies with a hand-built JSON body.
void responder(lux::Response& res, int code, Value v) {
    res.status(code).json_text(v.to_json_text());
}

// Engine errors are always {"error": "..."} and sometimes carry a list of
// messages: two fixed shapes that need nothing more.
void responder_error(lux::Response& res, int code, const std::string& msg) {
    Value::Dict d;
    d["error"] = Value::str(msg);
    responder(res, code, Value::dict(std::move(d)));
}

void responder_error(lux::Response& res, int code, const std::string& msg,
                     const std::vector<std::string>& messages) {
    Value::List l;
    l.reserve(messages.size());
    for (const auto& m : messages) l.push_back(Value::str(m));
    Value::Dict d;
    d["error"]    = Value::str(msg);
    d["messages"] = Value::list(std::move(l));
    responder(res, code, Value::dict(std::move(d)));
}

using Action = std::function<void(lux::Request&, lux::Response&)>;

// Translates the `return <expr>` of a declarative route into a native action.
// Returns an empty Action and records the diagnostic if the expression needs
// runtime evaluation.
Action compile_return(const Expr& e, DiagnosticBag& diags,
                      const std::string& tpl_dir) {
    // return { ... }  /  return "literal"  → JSON body
    Value literal;
    if (const_eval_valor(e, literal)) {
        // The body is serialized HERE, once.  It used to keep the nlohmann tree
        // and call dump() on every request: a route resolved entirely at compile
        // time should not serialize anything on the hot path.
        std::string cuerpo = literal.to_json_text();
        return [cuerpo](lux::Request&, lux::Response& res) {
            res.header("Content-Type", "application/json; charset=utf-8").send(cuerpo);
        };
    }

    const std::string* fn = callee_name(e);
    if (!fn) {
        diags.error(e.loc, "this expression needs the VM, which is not "
                           "implemented yet; in milestone 1 a route can only "
                           "return a constant value or a native call "
                           "(render, text, html, json, status, redirect, send_file)");
        return {};
    }

    auto need_string = [&](size_t idx, const char* what) -> const std::string* {
        if (e.args.size() <= idx || e.args[idx].value->kind != ExprKind::StringLit) {
            diags.error(e.loc, std::string(*fn) + "() expects " + what +
                               " as a string literal");
            return nullptr;
        }
        return &e.args[idx].value->text;
    };

    if (*fn == "render") {
        const std::string* tpl = need_string(0, "the template name");
        if (!tpl) return {};

        // The named arguments are the template variables.  They are converted
        // HERE, once: the route is declarative, so all that is left on the hot
        // path is rendering.
        Value::Dict data;
        for (size_t i = 1; i < e.args.size(); ++i) {
            const Arg& a = e.args[i];
            if (a.name.empty()) {
                diags.error(a.loc, "after the template name, the arguments "
                                   "of render() are named: key=value");
                return {};
            }
            Value v;
            if (!const_eval_valor(*a.value, v)) {
                diags.error(a.loc, "non-constant value in render(): needs the VM");
                return {};
            }
            data[a.name] = std::move(v);
        }
        // The data is constant, so the page can be rendered IN FULL here: the
        // route is reduced to sending fixed bytes.  And the template errors come
        // out at compile time, like all the others.
        const std::string name = *tpl;
        if (name.find("..") != std::string::npos ||
            std::filesystem::path(name).is_absolute()) {
            diags.error(e.loc, "invalid template name: '" + name + "'");
            return {};
        }
        auto source = read_whole_file(std::filesystem::path(tpl_dir) / name);
        if (!source) {
            diags.error(e.loc, "template not found: '" + name + "' in " + tpl_dir);
            return {};
        }

        // Here the data is CONSTANT, so the type of each key is known exactly:
        // the template is checked against the real values.
        std::vector<TypedName> keys;
        std::vector<Value>        values;
        for (const auto& [k, v] : data) {
            keys.push_back({k, lux_script_type_of(v)});
            values.push_back(v);
        }

        Template tpl_c;
        if (!compile_template(*source, name, tpl_dir, keys, diags, tpl_c)) return {};

        lux::Request  fake_req;
        lux::Response fake_res;
        NativeCtx        ctx{fake_req, fake_res};
        std::string      html, err;
        if (!render_template(tpl_c, std::move(values), ctx, nullptr, html, err)) {
            diags.error(e.loc, "when rendering '" + name + "': " + err);
            return {};
        }
        return [html](lux::Request&, lux::Response& res) {
            res.header("Content-Type", "text/html; charset=utf-8").send(html);
        };
    }

    if (*fn == "text" || *fn == "html") {
        const std::string* s = need_string(0, "the content");
        if (!s) return {};
        std::string body = *s;
        bool is_html = (*fn == "html");
        return [body, is_html](lux::Request&, lux::Response& res) {
            if (is_html) res.html(body); else res.text(body);
        };
    }

    if (*fn == "send_file") {
        const std::string* p = need_string(0, "the file path");
        if (!p) return {};
        std::string path = *p;
        return [path](lux::Request&, lux::Response& res) {
            res.send_file(path);
        };
    }

    if (*fn == "status") {
        if (e.args.empty() || e.args[0].value->kind != ExprKind::IntLit) {
            diags.error(e.loc, "status() expects a numeric status code");
            return {};
        }
        int code = static_cast<int>(e.args[0].value->int_value);
        return [code](lux::Request&, lux::Response& res) {
            res.status(code).send("");
        };
    }

    if (*fn == "redirect") {
        const std::string* target = need_string(0, "the target");
        if (!target) return {};
        int code = 302;
        if (e.args.size() > 1) {
            if (e.args[1].value->kind != ExprKind::IntLit) {
                diags.error(e.loc, "the second argument of redirect() is the status code");
                return {};
            }
            code = static_cast<int>(e.args[1].value->int_value);
        }
        std::string to = *target;
        return [to, code](lux::Request&, lux::Response& res) {
            res.status(code).header("Location", to).send("");
        };
    }

    if (*fn == "json") {
        if (e.args.size() != 1) {
            diags.error(e.loc, "json() expects a single argument");
            return {};
        }
        Value v;
        if (!const_eval_valor(*e.args[0].value, v)) {
            diags.error(e.loc, "json() with a non-constant value: needs the VM");
            return {};
        }
        std::string cuerpo = v.to_json_text();
        return [cuerpo](lux::Request&, lux::Response& res) {
            res.header("Content-Type", "application/json; charset=utf-8").send(cuerpo);
        };
    }

    diags.error(e.loc, "unknown native function: '" + *fn + "'");
    return {};
}

// Extracts the :param / {param} names from the route pattern.
std::vector<std::string> pattern_params(const std::string& pattern) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == ':' || pattern[i] == '{') {
            char close = (pattern[i] == '{') ? '}' : '/';
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] != close) ++j;
            out.push_back(pattern.substr(i + 1, j - i - 1));
            i = j;
        } else ++i;
    }
    return out;
}


// The identifier of an asynchronous builtin is its index in the table; it is
// looked up once so as not to depend on the order.
int async_sleep_id() {
    static const int id = native_id("sleep");
    return id;
}

int async_ws_recv_id() {
    static const int id = native_id("__ws_recv");
    return id;
}

// The VM step cap resets on every suspension on purpose, so a legitimate
// sse/ws loop can live for hours (see kStepLimit in vm.hpp).  But that leaves a
// gap: `while true: await sleep(0)` suspends and resumes without advancing
// much, so it never accumulates steps between two suspensions and the cap never
// sees it — a single client could pin a whole thread by rescheduling a 0 ms
// timer over and over.  A 1 ms floor does not prevent it entirely, but it
// bounds it to ~1000 resumptions/s per connection instead of as many as the
// scheduler cares to give: no legitimate workload needs less than that.
//
// Upper-bounded too, not just floored: every call site immediately narrows
// this into an `int` for SleepAwaitable::ms (task.hpp) --
// `co_await lux::sleep(static_cast<int>(ms), ...)`. A ms value above
// INT_MAX (trivially reachable: the language's own int is 64-bit, and a
// route parameter or a hand-typed literal like `sleep(2147483648)` both
// produce one) wrapped to a NEGATIVE 32-bit int on that cast -- and
// SleepAwaitable::await_ready() treats ms <= 0 as "already elapsed",
// resuming the coroutine WITHOUT EVER SUSPENDING. A loop of the exact
// shape GUIDE.md's own SSE example uses (`while sse.open: await
// sleep(every)`) with an out-of-range `every` therefore never actually
// waits at all: confirmed against the real binary, a single request to
// that exact example route with every=2147483648 produced over a MILLION
// SSE events (27 MB) in 1.5 seconds instead of one every ~24 days -- the
// same failure mode as `sleep(0)` in a tight loop (see the 1ms floor
// above), just reached from the other end of the range instead of by
// asking for "no wait" directly.
long long clamp_sleep_ms(long long ms) {
    return std::clamp<long long>(ms, 1, std::numeric_limits<int>::max());
}

bool is_scalar(const std::string& t) {
    return t == "int" || t == "long" || t == "float" || t == "double" ||
           t == "bool" || t == "string";
}

int async_db_query_id() { static const int id = native_id("__db_query"); return id; }
int async_db_exec_id()  { static const int id = native_id("__db_exec");  return id; }
int async_db_begin_id()    { static const int id = native_id("__db_begin");    return id; }
int async_db_commit_id()   { static const int id = native_id("__db_commit");   return id; }
int async_db_rollback_id() { static const int id = native_id("__db_rollback"); return id; }
int async_db_last_id()     { static const int id = native_id("__db_last_id");  return id; }

bool is_db_await(int id) {
    return id == async_db_query_id()  || id == async_db_exec_id()  ||
           id == async_db_begin_id()  || id == async_db_commit_id() ||
           id == async_db_rollback_id() || id == async_db_last_id();
}

// Resolves a database suspension.
//
// The first argument is always the module name, which the emitter pushes.
// A failure comes back as {"error": ...} (db_failed, db.hpp) and drive_vm
// raises it at the `await`.
//
// Thin adapter over lux_script::await_db() (db.hpp/db.cpp) — the real
// logic (resolving the driver/pool, deciding the pinned worker, invoking
// DbAwaitable, updating pinned_workers/last_exec_workers) lives there,
// shared with the code that generates a --native route for the same thing:
// both paths run EXACTLY the same code, not a separate reproduction that
// could silently diverge (the same class of bug that already caused two
// critical fixes in this phase).
lux::Task<Value> run_db(const VM::Result& r, int op, lux::Request& req,
                           NativeCtx& ctx) {
    if (r.await_args.empty() || !r.await_args[0].is_str())
        co_return db_error("malformed query");

    const std::string mod = r.await_args[0].as_str();

    DbOp dbop = DbOp::Rollback;
    if      (op == async_db_query_id())  dbop = DbOp::Query;
    else if (op == async_db_exec_id())   dbop = DbOp::Exec;
    else if (op == async_db_last_id())   dbop = DbOp::LastId;
    else if (op == async_db_begin_id())  dbop = DbOp::Begin;
    else if (op == async_db_commit_id()) dbop = DbOp::Commit;

    std::string sql;
    std::vector<Value> params;
    if (dbop == DbOp::Query || dbop == DbOp::Exec) {
        if (r.await_args.size() < 2 || !r.await_args[1].is_str())
            co_return db_error("missing the SQL query");
        sql    = r.await_args[1].as_str();
        params = std::vector<Value>(r.await_args.begin() + 2, r.await_args.end());
    }

    Value v = co_await await_db(dbop, mod, req.loop, sql, std::move(params),
                                ctx.pinned_workers, ctx.last_exec_workers,
                                ctx.poisoned_db);
    co_return v;
}

// Resolves an `await <module>.<fn>(...)` suspension for an is_async native
// module function (os.run()/read_file()/write_file(), every http.*) --
// Op::CallAsyncModule's counterpart of run_db() above.
//
// The function is looked up the same way the SYNCHRONOUS Op::CallBuiltinModule
// path already does (builtin_module_function_at(), vm.cpp), and its own
// BuiltinModuleFn::fn is called exactly as-is, unchanged -- no separate
// "async" implementation to keep in sync with the sync one, only where it
// runs differs. Run on lux::blocking_pool() (BlockingAwaitable,
// blocking_pool.hpp), the SAME shared pool a synchronous no-`await` route
// already uses, instead of inline on the event loop thread: a slow
// os.run()/http.get() call now ties up a pool worker for its duration
// instead of the whole core.
//
// BlockingAwaitable's own contract ("the coroutine, and therefore req/res,
// is not touched by anyone else while suspended") is what makes it safe to
// hand `ctx` to the pool thread here even though it carries live
// lux::Request&/Response& references: control has fully passed to the
// worker until it posts back, and every is_async function today (verified:
// grepped for `ctx.` in os.cpp/http.cpp -- zero hits) ignores
// ctx entirely, taking it only for NativeFn's uniform signature. A future
// is_async function that DID read/write req/res from here would still be
// memory-safe under that same contract, just worth calling out since
// nothing else in the type system enforces it.
//
// A failure is left in `error`, and drive_vm raises it at the `await`.
lux::Task<Value> run_builtin_module_async(const VM::Result& r, lux::Request& req,
                                            NativeCtx& ctx, std::string& error) {
    const BuiltinModuleFn& fn = builtin_module_function_at(r.await_id);
    std::vector<Value> args = r.await_args;
    Value       out;
    // io_blocking_pool(), not the default blocking_pool(): this is an
    // is_async native module call (os.run(), http.*, a file read/write),
    // blocked on a subprocess or a socket for as long as its own timeout
    // allows -- not CPU-bound work. See io_blocking_pool()'s comment
    // (blocking_pool.hpp) for the starvation this fixes.
    co_await lux::BlockingAwaitable{req.loop, [&] {
        out = fn.call(ctx, args, error);
    }, &lux::io_blocking_pool()};
    co_return out;
}

// Closes the transactions the handler left open.
//
// Without this, a `return` halfway through or an error would leave the
// connection inside a transaction forever, and whoever took it from the pool
// next would inherit that state.
lux::Task<void> rollback_pending(NativeCtx& ctx, lux::Request& req) {
    co_await rollback_pending_db(ctx.pinned_workers, req.loop);
}

// Runs the VM's suspend/resume loop: every time it stops on an `await`, the
// real co_await happens here, on the engine, and the result is handed back.
// Shared by http, ws and sse routes; `ws` is only set for a ws route (the one
// place `await ws.recv()` can compile). Returns false if the client went away
// mid-sleep -- by then any open transaction has already been rolled back and
// the caller just stops.
lux::Task<bool> drive_vm(VM& vm, VM::Result& result, lux::Request& req, NativeCtx& ctx,
                         lux::WSConnection* ws = nullptr) {
    while (result.status == VM::Status::Suspended) {
        Value       produced = Value::null();
        std::string failed;   // a module or database call that failed: raised at the await
        if (result.await_is_module) {
            produced = co_await run_builtin_module_async(result, req, ctx, failed);
        }
        else if (ws && result.await_id == async_ws_recv_id()) {
            // The message enters the VM as the result of the `await`; null
            // means the connection closed. Unsolicited Pongs (the shape most
            // clients' own heartbeat uses -- RFC 6455 lets them arrive
            // unasked, and WSState queues every one) are skipped: `await
            // ws.recv()` returns application messages, and surfacing a Pong's
            // (often empty) payload made every idle heartbeat look like a
            // real message to the handler.
            std::optional<lux::WSMessage> msg;
            do {
                msg = co_await ws->recv();
            } while (msg && msg->is_pong());
            if (msg && !msg->is_close()) produced = Value::str(msg->data);
        }
        else if (is_db_await(result.await_id)) {
            produced = co_await run_db(result, result.await_id, req, ctx);
            db_failed(produced, failed);
        }
        else if (result.await_id == async_sleep_id()) {
            long long ms = result.await_args.empty() ? 0 : result.await_args[0].as_int();
            co_await lux::sleep(static_cast<int>(clamp_sleep_ms(ms)), req.loop, req.cancel_token);
            // sleep() wakes early if the client disconnects: stop, but an open
            // transaction must not be abandoned -- the connection would stay
            // pinned inside it and the next request on that worker would run
            // silently inside it (see rollback_pending_db, db.hpp).
            if (req.is_cancelled()) {
                co_await rollback_pending(ctx, req);
                co_return false;
            }
        }
        result = failed.empty() ? vm.resume(std::move(produced), ctx)
                                : vm.resume_error(std::move(failed), result.error_loc, ctx);
    }
    co_return true;
}

// "file:line:col" of a runtime error, or the route's own label if the VM has
// no precise location.
std::string error_at(const VM::Result& result, const std::string& where) {
    return result.error_loc.file
        ? *result.error_loc.file + ":" + std::to_string(result.error_loc.line) + ":" +
          std::to_string(result.error_loc.col)
        : where;
}

// ─── Classes ─────────────────────────────────────────────────────────────────

struct ClassInfo; // needed by ClassField below, defined fully further down

struct ClassField {
    std::string name;
    Type        type;   // scalar, List<scalar>, a class, or List<class>
    // Non-null exactly when `type` is a class, or a List whose element is
    // one -- resolved ONCE here, in build_classes(), so a consumer that
    // needs to recurse into the nested class's own fields (JSON body
    // validation below; OpenAPI schema generation) can follow this
    // directly instead of re-resolving the class name against the whole
    // ClassTable on every request.
    std::shared_ptr<ClassInfo> nested_class;
};

// A `validate:` rule already compiled: it receives the fields as locals, in the
// declaration order, and returns a boolean.
struct ClassRule {
    std::shared_ptr<Chunk> chunk;
    std::string            message;
};

struct ClassInfo {
    std::string             name;
    std::vector<ClassField> fields;
    std::vector<ClassRule>  rules;
};

using ClassTable = std::map<std::string, std::shared_ptr<ClassInfo>>;

// Whether `t` is a type a class field can legally hold: one of the four
// scalars, a List of one of those, a reference to another declared class,
// or a List of another declared class -- one level of nesting inside a
// List, never List<List<...>>/List<Dict<...>>, and Dict is not a field
// type at all (not asked for; adding it would mean solving the same
// "value fits the type" problem a second time for a shape nobody has
// needed yet).
//
// `known` doubles as both "which names are legal class references" AND, on
// a hit, the resolved shared_ptr the field's `nested_class` should point
// at -- see build_classes()'s two-pass structure below for why every
// class's ClassInfo already exists (if only as an empty shell) by the
// time any field is checked against it, which is what makes a FORWARD
// reference ("Season" declared before "Episode" in the file) resolve
// exactly like a backward one.
bool class_field_type_ok(const Type& t, const ClassTable& known,
                         std::shared_ptr<ClassInfo>& nested_out) {
    switch (t.kind()) {
        case Type::Kind::Int:
        case Type::Kind::Float:
        case Type::Kind::Bool:
        case Type::Kind::String:
            return true;
        case Type::Kind::Class: {
            auto it = known.find(t.class_name());
            if (it == known.end()) return false;
            nested_out = it->second;
            return true;
        }
        case Type::Kind::List: {
            const Type& elem = t.element();
            if (elem.is_optional()) return false;
            switch (elem.kind()) {
                case Type::Kind::Int:
                case Type::Kind::Float:
                case Type::Kind::Bool:
                case Type::Kind::String:
                    return true;
                case Type::Kind::Class: {
                    auto it = known.find(elem.class_name());
                    if (it == known.end()) return false;
                    nested_out = it->second;
                    return true;
                }
                default:
                    return false;
            }
        }
        default:
            return false;
    }
}

void build_classes(const Program& program, const FunctionSigs& fns,
                   const ClassSigs& sigs, const EnumSigs& enums,
                   const std::set<std::string>* imports,
                   ClassTable& out, DiagnosticBag& diags) {
    // Pass 1: an empty shell for every uniquely-named class, before any
    // field is looked at. A field referencing another class has to resolve
    // against something regardless of which of the two classes comes first
    // in the file -- without this, "Season" (with a field of type
    // "Episode") declared above "Episode" in the same file would see
    // "Episode" as unknown, a source-order dependency the language has no
    // other reason to impose.
    for (const auto& c : program.classes) {
        if (out.count(c.name)) continue; // duplicate; reported in pass 2, in file order
        auto info  = std::make_shared<ClassInfo>();
        info->name = c.name;
        out[c.name] = std::move(info);
    }

    std::set<std::string> processed;
    for (const auto& c : program.classes) {
        if (processed.count(c.name)) {
            diags.error(c.loc, "class '" + c.name + "' is already declared");
            continue;
        }
        processed.insert(c.name);

        auto& info = out[c.name]; // the shell pass 1 already created

        std::vector<TypedName> field_names;
        bool ok = true;

        for (const auto& f : c.fields) {
            Type t = Type::from_declared(f.type);
            std::shared_ptr<ClassInfo> nested;
            if (!class_field_type_ok(t, out, nested)) {
                diags.error(f.loc, "type '" + f.type.str() + "' as a field is not "
                                   "supported; a field can be int, long, float, "
                                   "double, bool, string, another class, or a List "
                                   "of one of those");
                ok = false;
                continue;
            }
            info->fields.push_back({f.name, t, std::move(nested)});
            field_names.push_back({f.name, f.type.name});
        }
        if (!ok) { info->fields.clear(); continue; }

        // Every rule is compiled against the class fields: if it mentions a name
        // that does not exist, the error comes out here and not in production.
        for (const auto& r : c.rules) {
            auto    chunk = std::make_shared<Chunk>();
            Emitter emitter(diags, &fns, &sigs, imports, nullptr, &enums);
            if (!emitter.emit_condition(*r.condition, field_names, *chunk)) continue;
            info->rules.push_back({chunk, r.message});
        }
    }
}

// Session signing/verification and JWT (sign_session, load_session,
// verify_jwt, AuthConfig, begin_auth, end_auth) live in
// include/lux_script/auth.hpp + src/lux_script/auth.cpp: any backend
// that runs Lux Script routes needs it, not just this VM — and it is
// also where the session-cookie
// expiry lives (`exp` signed alongside the payload, checked in
// load_session): see the comment there for why a session that never expires
// on its own was a real bug, not just a hardening nice-to-have.

// ─── Parameter binding ───────────────────────────────────────────────────────

enum class BindKind { Path, Query, Body, JsonBody, File, FileList };

struct ParamBind {
    BindKind    kind = BindKind::Path;
    std::string name;
    std::string type;          // scalar, or the class name if it is a Body
    bool        has_default = false;
    std::string default_text;
    std::shared_ptr<ClassInfo> cls;   // Body only
};

// std::stod() happily parses three spellings a route-param float must not
// accept: "nan", "inf"/"-inf" (case-insensitively) and hexadecimal float
// notation ("0x1p4") -- all three consume the whole string, so the
// full-consumption check below does not catch them. No FINITE decimal
// float can contain an 'x'/'X' (hex marker) or an 'n'/'N'/'i'/'I' (only
// "nan"/"inf" do), so rejecting any of those six characters up front is
// exact, not a heuristic. Shared with native_gen.cpp's
// lux_route_coerce_float, which must reject the exact same inputs.
bool looks_like_decimal_float(const std::string& text) {
    for (unsigned char c : text) {
        switch (c) {
            case 'x': case 'X': case 'n': case 'N': case 'i': case 'I':
                return false;
            default: break;
        }
    }
    return true;
}

// Converts the raw URL text to the declared type.  A malformed value is a 400:
// the client sent it wrong, it is not a server failure.
bool coerce(const std::string& text, const std::string& type, Value& out) {
    try {
        if (type == "string") { out = Value::str(text); return true; }
        if (type == "int" || type == "long") {
            // std::stoll() only requires a valid prefix, not the whole
            // string: "42abc" silently becomes 42, and "0x10" becomes 0 (it
            // reads the "0", stops at 'x', and reports success) since it
            // parses base 10 unless told otherwise. Requiring the full text
            // be consumed rejects both instead of quietly truncating input
            // the client sent wrong — README promises no implicit coercion
            // anywhere in the language; a route param is not an exception.
            size_t pos = 0;
            long long v = std::stoll(text, &pos);
            if (pos != text.size()) return false;
            out = Value::integer(v);
            return true;
        }
        if (type == "float" || type == "double") {
            if (!looks_like_decimal_float(text)) return false;
            size_t pos = 0;
            double v = std::stod(text, &pos);
            if (pos != text.size()) return false;
            out = Value::real(v);
            return true;
        }
        if (type == "bool") {
            // "on" is not a stylistic alternative to "true": it is the
            // literal string value HTML sends for a checked <input
            // type="checkbox"> with no explicit `value=` attribute (the
            // common case -- <input type="checkbox" name="remember"> with
            // nothing else), the same as "off" for one browsers occasionally
            // do send explicitly (a hidden mirror field, some JS form
            // libraries). A route declaring `bool remember` for the most
            // ordinary HTML form Lux Script can receive used to 400
            // unconditionally ("expected bool", "received": "on") for
            // every submission where the box was checked.
            if (text == "true"  || text == "1" || text == "on")  { out = Value::boolean(true);  return true; }
            if (text == "false" || text == "0" || text == "off") { out = Value::boolean(false); return true; }
            return false;
        }
    } catch (...) { return false; }
    return false;
}

// Checks that a JSON value fits the field's declared type.
// There is no conversion between families: a string in an int field is an
// error, not an attempt to parse.
//
// It is deliberately strict: a "30" is not good enough where an int was
// declared.  The body saying one thing and the class another is exactly what
// validation exists to catch.
//
// Recursive for a List field (checked element by element) and for a nested
// class field (checked field by field, against THAT class's own fields --
// `nested_class` is the same shared_ptr ClassField::nested_class already
// carries, resolved once in build_classes(), so this never has to look a
// class name back up in a ClassTable it was not even given). A List of a
// class recurses through both cases at once: `type` is List<Episode>, so
// each element goes through the Class branch below with the SAME
// `nested_class` (the list's element class, not a different one).
bool value_matches(const Value& v, const Type& type,
                   const std::shared_ptr<ClassInfo>& nested_class, Value& out) {
    switch (type.kind()) {
        case Type::Kind::String:
            if (!v.is_str()) return false;
            out = v;
            return true;
        case Type::Kind::Bool:
            if (!v.is_bool()) return false;
            out = v;
            return true;
        case Type::Kind::Int:
            if (!v.is_int()) return false;
            out = v;
            return true;
        case Type::Kind::Float:
            if (!v.is_num()) return false;
            out = Value::real(v.as_float());
            return true;
        case Type::Kind::List: {
            if (!v.is_list()) return false;
            Value::List result;
            result.reserve(v.as_list().size());
            for (const auto& elem : v.as_list()) {
                Value ev;
                if (!value_matches(elem, type.element(), nested_class, ev)) return false;
                result.push_back(std::move(ev));
            }
            out = Value::list(std::move(result));
            return true;
        }
        case Type::Kind::Class: {
            if (!v.is_dict() || !nested_class) return false;
            Value::Dict result;
            for (const auto& f : nested_class->fields) {
                auto it = v.as_dict().find(f.name);
                if (it == v.as_dict().end() || it->second.is_null()) {
                    if (!f.type.is_optional()) return false;
                    result[f.name] = Value::null();
                    continue;
                }
                Value fv;
                if (!value_matches(it->second, f.type, f.nested_class, fv)) return false;
                result[f.name] = std::move(fv);
            }
            out = Value::dict(std::move(result));
            return true;
        }
        default:
            return false;
    }
}

// Validates the parameters against the pattern and produces the binding plan.
bool bind_params(const RouteDecl& r, const ClassTable& classes,
                 std::vector<ParamBind>& out, DiagnosticBag& diags) {
    auto in_pattern = pattern_params(r.pattern);
    bool ok         = true;
    bool seen_body  = false;

    for (const auto& seg : in_pattern) {
        bool found = false;
        for (const auto& p : r.params) if (p.name == seg) { found = true; break; }
        if (!found) {
            diags.error(r.pattern_loc,
                        "the pattern declares ':" + seg + "' but no parameter binds it");
            ok = false;
        }
    }

    for (const auto& p : r.params) {
        bool in_path = std::find(in_pattern.begin(), in_pattern.end(), p.name)
                       != in_pattern.end();

        // File / List<File> bind to the multipart parts with that name.
        bool is_file      = (p.type.name == "File");
        bool is_file_list = (p.type.name == "List" && p.type.args.size() == 1 &&
                             p.type.args[0].name == "File");
        if (is_file || is_file_list) {
            if (in_path) {
                diags.error(p.loc, "'" + p.name + "' is in the route pattern, "
                                   "so it cannot be an uploaded file");
                ok = false;
                continue;
            }
            if (r.method == "GET" || r.method == "DELETE") {
                diags.error(p.loc, "a route " + r.method + " takes no body");
                ok = false;
                continue;
            }
            ParamBind fb;
            fb.kind = is_file ? BindKind::File : BindKind::FileList;
            fb.name = p.name;
            fb.type = p.type.name;
            out.push_back(std::move(fb));
            continue;
        }

        // `Json` binds to the whole request body too, but with no schema:
        // the parameter is whatever `json.parse()` produces (object, array,
        // string, number, bool, or null), unvalidated. For a body whose
        // shape is not fixed (a webhook payload, a proxy passthrough) a
        // `class` parameter is the wrong tool -- it demands a closed set of
        // named fields, all of which the caller must send -- and before
        // this there was no other typed way to accept "some JSON body" at
        // all. Shares `seen_body`/`in_path`/GET-DELETE with the `class`
        // case below: it is the same "one body per route" slot, just with
        // a different (or no) shape check on what fills it.
        if (p.type.name == "Json") {
            if (in_path) {
                diags.error(p.loc, "'" + p.name + "' is in the route pattern, "
                                   "so it cannot be of type 'Json'");
                ok = false;
                continue;
            }
            if (seen_body) {
                diags.error(p.loc, "there can only be one body parameter per route");
                ok = false;
                continue;
            }
            if (r.method == "GET" || r.method == "DELETE") {
                diags.error(p.loc, "a route " + r.method + " takes no body");
                ok = false;
                continue;
            }
            seen_body = true;
            out.push_back({BindKind::JsonBody, p.name, p.type.name, false, {}, nullptr});
            continue;
        }

        // A parameter whose type is a class binds to the request body: that is
        // the FastAPI idea, the body is one more typed parameter.
        auto it = classes.find(p.type.name);
        if (it != classes.end()) {
            if (in_path) {
                diags.error(p.loc, "'" + p.name + "' is in the route pattern, "
                                   "so it cannot be of type '" + p.type.name + "'");
                ok = false;
                continue;
            }
            if (seen_body) {
                diags.error(p.loc, "there can only be one body parameter per route");
                ok = false;
                continue;
            }
            if (r.method == "GET" || r.method == "DELETE") {
                diags.error(p.loc, "a route " + r.method + " takes no body");
                ok = false;
                continue;
            }
            seen_body = true;
            out.push_back({BindKind::Body, p.name, p.type.name, false, {}, it->second});
            continue;
        }

        ParamBind b;
        b.name = p.name;
        b.type = p.type.name;

        if (!is_scalar(p.type.name)) {
            diags.error(p.loc, "unknown type: '" + p.type.str() + "'");
            ok = false;
            continue;
        }
        if (p.type.optional) {
            diags.error(p.loc, "optional parameters are not implemented yet");
            ok = false;
            continue;
        }

        b.kind = in_path ? BindKind::Path : BindKind::Query;

        if (in_path && p.default_value) {
            diags.error(p.loc, "a path parameter cannot have a default value");
            ok = false;
            continue;
        }
        if (!in_path && p.default_value) {
            const Expr& d = *p.default_value;
            if      (d.kind == ExprKind::StringLit) b.default_text = d.text;
            else if (d.kind == ExprKind::IntLit)    b.default_text = std::to_string(d.int_value);
            else if (d.kind == ExprKind::BoolLit)   b.default_text = d.bool_value ? "true" : "false";
            else {
                diags.error(p.loc, "the default value must be a constant");
                ok = false;
                continue;
            }
            b.has_default = true;
        }

        out.push_back(std::move(b));
    }
    return ok;
}

// Recognizes the purely declarative shape: a single `return` resolved entirely
// at compile time.  These routes do not run a single bytecode step.
Action try_declarative(const RouteDecl& r, const std::string& tpl_dir) {
    // A route with guards can NEVER take the declarative path: the native
    // action does not run them, so it would skip the group's protection.
    if (!r.guards.empty())                                       return {};
    if (!r.params.empty())                                       return {};
    if (r.body.size() != 1)                                      return {};
    if (r.body[0]->kind != StmtKind::Return || !r.body[0]->value) return {};

    DiagnosticBag scratch;
    Action a = compile_return(*r.body[0]->value, scratch, tpl_dir);
    return scratch.empty() ? a : Action{};
}

// Builds the instance from the JSON body.
//
// A missing required field, or one with the wrong type, or a broken validate
// rule, is a 422 with the complete list of reasons: they are all reported at
// once, not just the first.  The handler never runs.
bool bind_body(const ClassInfo& ci, const FunctionTable* fns,
               lux::Request& req, lux::Response& res,
               NativeCtx& ctx, Value& out) {
    Value body;
    if (!Value::parse_json(req.body, body)) {
        responder_error(res, 400, "invalid JSON");
        return false;
    }
    if (!body.is_dict()) {
        responder_error(res, 422, "Validation failed",
                        {"the body must be a JSON object"});
        return false;
    }

    std::vector<std::string> messages;
    Value::Dict              fields;
    std::vector<Value>       ordered;

    for (const auto& f : ci.fields) {
        auto it = body.as_dict().find(f.name);
        if (it == body.as_dict().end() || it->second.is_null()) {
            if (!f.type.is_optional()) messages.push_back(f.name + ": required");
            fields[f.name] = Value::null();
            ordered.push_back(Value::null());
            continue;
        }
        Value v;
        if (!value_matches(it->second, f.type, f.nested_class, v)) {
            // base_name(), not to_string(): --native's own equivalent
            // message (native_gen.cpp's construir_clases(), CampoNativo::
            // ortografia) has always used the bare spelling with no `?`
            // suffix, and the native_route_shadow suite compares the two
            // byte for byte -- a class with a List<X>/nested-class field
            // never reaches --native's version of this message at all (see
            // class_field_type_ok()'s comment), so this only has to agree
            // with native_gen.cpp for the scalar/optional-scalar case,
            // which base_name() does exactly.
            messages.push_back(f.name + ": expected " + f.type.base_name());
            fields[f.name] = Value::null();
            ordered.push_back(Value::null());
            continue;
        }
        fields[f.name] = v;
        ordered.push_back(std::move(v));
    }

    // The rules only run if the fields are sound: evaluating them over missing
    // values would give type errors instead of the useful message.
    if (messages.empty()) {
        thread_local VM rule_vm;
        for (const auto& rule : ci.rules) {
            VM::Result r = rule_vm.start(*rule.chunk, ordered, ctx, fns);
            if (r.status != VM::Status::Done) {
                lux::log().error("validate of " + ci.name + ": " + r.error);
                responder_error(res, 500, r.error);
                return false;
            }
            if (!r.value.truthy()) messages.push_back(rule.message);
        }
    }

    if (!messages.empty()) {
        // They are left within reach of this same request's `on error 422`.
        last_validation_messages() = messages;
        responder_error(res, 422, "Validation failed", messages);
        return false;
    }

    out = Value::dict(std::move(fields));
    return true;
}

// Fills the parameter slots from the request.
// Returns false, with the response already written, if some value does not fit.
// A File in the language is the metadata plus the index of its part in
// ctx.uploads; the bytes do not travel inside the Value.
Value make_file_value(const lux::MultipartPart& part, size_t index) {
    Value::Dict d;
    d["name"]         = Value::str(part.name);
    d["filename"]     = Value::str(part.filename);
    d["content_type"] = Value::str(part.content_type);
    d["size"]         = Value::integer(static_cast<long long>(part.body.size()));
    d["__idx"]        = Value::integer(static_cast<long long>(index));
    return Value::dict(std::move(d));
}

bool prepare_args(const std::vector<ParamBind>& binds, const FunctionTable* fns,
                  lux::Request& req, lux::Response& res,
                  NativeCtx& ctx, std::vector<Value>& out) {
    // They are cleared on entry: a 422 the handler writes by hand must not
    // inherit the messages of an earlier validation on this thread.
    last_validation_messages().clear();
    out.reserve(binds.size());

    // Lazily parsed on first use inside the loop below -- see its comment.
    std::optional<std::unordered_map<std::string, std::string>> form_data;

    for (const auto& b : binds) {
        if (b.kind == BindKind::File || b.kind == BindKind::FileList) {
            if (!ctx.uploads) {
                responder_error(res, 400, "expected multipart/form-data");
                return false;
            }
            Value::List matches;
            for (size_t i = 0; i < ctx.parts->size(); ++i) {
                const auto& part = (*ctx.parts)[i];
                if (part.name == b.name && !part.filename.empty())
                    matches.push_back(make_file_value(part, i));
            }

            if (b.kind == BindKind::FileList) {
                out.push_back(Value::list(std::move(matches)));
                continue;
            }
            if (matches.empty()) {
                responder_error(res, 422, "Validation failed",
                                {b.name + ": the file is missing"});
                return false;
            }
            out.push_back(matches[0]);
            continue;
        }

        if (b.kind == BindKind::Body) {
            Value v;
            if (!bind_body(*b.cls, fns, req, res, ctx, v)) return false;
            out.push_back(std::move(v));
            continue;
        }

        if (b.kind == BindKind::JsonBody) {
            Value v;
            if (!Value::parse_json(req.body, v)) {
                responder_error(res, 400, "invalid JSON");
                return false;
            }
            out.push_back(std::move(v));
            continue;
        }

        std::string raw;
        bool present = false;

        if (b.kind == BindKind::Path) {
            auto it = req.params.find(b.name);
            if (it != req.params.end()) { raw = it->second; present = true; }
        } else {
            auto it = req.query.find(b.name);
            if (it != req.query.end()) { raw = it->second; present = true; }
            else {
                // A field of an application/x-www-form-urlencoded body is
                // bound the same way a query param is -- this used to fall
                // straight through to "absent" for every scalar parameter
                // on a route whose client posted an HTML <form> without
                // enctype="multipart/form-data" (the DEFAULT enctype: a
                // plain `<form method=post>` sends urlencoded, not
                // multipart), even though req.form() -- available to the
                // handler by hand -- read that exact field correctly the
                // whole time. Parsed at most once per request (not once per
                // parameter): req.form() itself is cheap to call again (it
                // returns {} immediately when the content-type does not
                // match, never touching the body), but re-parsing the same
                // urlencoded body on every one of a route's N scalar
                // parameters was not.
                if (!form_data) form_data = req.form();
                auto fit = form_data->find(b.name);
                if (fit != form_data->end()) { raw = fit->second; present = true; }
                else if (ctx.uploads && ctx.parts) {
                    // A text field of a multipart form is one more part, just like a
                    // File, only without a filename.  This branch did not exist
                    // before: a scalar parameter on a route with a File always came
                    // out empty, with no error, because only the query string was read.
                    for (const auto& part : *ctx.parts) {
                        if (part.name == b.name && part.filename.empty()) {
                            raw = part.body; present = true; break;
                        }
                    }
                }
            }
            if (!present && b.has_default) { raw = b.default_text; present = true; }
            // An HTML <input type="number"> (or <input type="date">, etc.)
            // left blank submits its name with an EMPTY value
            // (`?page=`), not omitted entirely -- indistinguishable from
            // "present" above. For a non-string type that empty text can
            // never coerce (there is no valid int/float/bool spelled ""),
            // so a route declaring `int page = 1` 400'd on the single most
            // common way a number field reaches "no value entered": a
            // client leaving it blank, exactly the case `= 1` exists to
            // paper over. `string` is deliberately excluded: an empty
            // string IS a valid string value (`?q=` legitimately means
            // "search for nothing"), so only the actually-uncoercible
            // types fall back here.
            if (present && raw.empty() && b.type != "string" && b.has_default) {
                raw = b.default_text;
            }
        }

        Value v;
        if (!present) {
            // Genuinely absent: no value in the query/path/multipart AND no
            // `= default` in the signature (that case already turned into
            // `present = true` above, with `raw` set to the default text).
            // This used to fill in the type's zero value (""/false/0)
            // instead, silently -- the exact same "missing" a File field or
            // a class body field already turns into a 422 for, minus the
            // 422. A route like `post endpoint("/login", string name)`
            // called with NO `name` at all logged the user in as "" rather
            // than rejecting the request: GUIDE.md's own parameter table
            // says "A name that does not appear [in the pattern] -> Query
            // string" with no mention of it being optional by default, and
            // `= value` is presented as the ONLY way to make one optional
            // ("Default value if missing from the query") -- so a
            // parameter with no `=` was always meant to be required, the
            // same as a class field with no `?`.
            responder_error(res, 422, "Validation failed", {b.name + ": required"});
            return false;
        } else if (!coerce(raw, b.type, v)) {
            Value::Dict d;
            d["error"]    = Value::str("invalid parameter");
            d["param"]    = Value::str(b.name);
            d["expected"] = Value::str(b.type);
            d["received"] = Value::str(raw);
            responder(res, 400, Value::dict(std::move(d)));
            return false;
        }
        out.push_back(std::move(v));
    }
    return true;
}

// ─── OpenAPI from the AST ────────────────────────────────────────────────────
//
// The C++ version of this was 347 lines of metaprogramming that deduced the
// schema by building a default T{} and inspecting the resulting JSON.  With an
// AST in front of you, the names and the types are already there: it is a walk
// over declarations.

std::string openapi_type(const std::string& t) {
    if (t == "int" || t == "long")        return "integer";
    if (t == "float" || t == "double")    return "number";
    if (t == "bool")                      return "boolean";
    return "string";
}

// The Lux Script pattern uses :name; OpenAPI uses {name}.
std::string openapi_path(const std::string& pattern) {
    std::string out;
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == ':') {
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] != '/') ++j;
            out += "{" + pattern.substr(i + 1, j - i - 1) + "}";
            i = j;
        } else {
            out += pattern[i++];
        }
    }
    return out;
}

// Shortcuts to assemble the document without drowning in Value::Dict.
Value jstr(std::string v) { return Value::str(std::move(v)); }

Value jobj(std::initializer_list<std::pair<const char*, Value>> fields) {
    Value::Dict d;
    d.reserve(fields.size());
    for (const auto& [k, v] : fields) d[k] = v;
    return Value::dict(std::move(d));
}

// The full JSON-Schema fragment for a class field -- unlike openapi_type()
// above (a bare type name, still exactly right for a route/query param,
// which stays scalar-only), a field can now be a List or a reference to
// another class's own schema, so this builds the whole nested shape
// instead of one type string. A List<Episode> becomes
// {"type":"array","items":{"$ref":"#/components/schemas/Episode"}}; a
// bare Episode field becomes just the $ref -- both point at the SAME
// schema build_openapi() below already emits once per class, not a
// second copy inlined here.
Value openapi_field_schema(const Type& t) {
    if (t.kind() == Type::Kind::List)
        return jobj({{"type", jstr("array")}, {"items", openapi_field_schema(t.element())}});
    if (t.kind() == Type::Kind::Class)
        return jobj({{"$ref", jstr("#/components/schemas/" + t.class_name())}});
    return jobj({{"type", jstr(openapi_type(t.base_name()))}});
}

// The document is built and serialized ONCE, when the .lux is compiled.  It
// used to keep the tree and call dump() on every request to /openapi.json,
// which is hot-path work for something that does not change.
std::string build_openapi(const Program& program, const ClassTable& classes) {
    Value::Dict doc;
    doc["openapi"] = jstr("3.0.3");
    doc["info"]    = jobj({
        {"title",   jstr(program.app.name.empty() ? "Lux API" : program.app.name)},
        {"version", jstr(program.app.version.empty() ? "0.1.0" : program.app.version)},
    });

    // Classes are published as reusable schemas.  The `validate:` messages are
    // attached as the description: they are the real rules the server applies,
    // so they document better than any separate text.
    Value::Dict schemas;
    for (const auto& [name, info] : classes) {
        Value::Dict props;
        Value::List required;

        for (const auto& f : info->fields) {
            props[f.name] = openapi_field_schema(f.type);
            if (!f.type.is_optional()) required.push_back(jstr(f.name));
        }

        Value::Dict schema;
        schema["type"]       = jstr("object");
        schema["properties"] = Value::dict(std::move(props));
        if (!required.empty()) schema["required"] = Value::list(std::move(required));

        if (!info->rules.empty()) {
            std::string desc = "Validation rules:";
            for (const auto& r : info->rules) desc += "\n- " + r.message;
            schema["description"] = jstr(std::move(desc));
        }
        schemas[name] = Value::dict(std::move(schema));
    }
    if (!schemas.empty())
        doc["components"] = jobj({{"schemas", Value::dict(std::move(schemas))}});

    Value::Dict paths;

    for (const auto& r : program.routes) {
        // Streaming routes do not fit OpenAPI 3.0: they are advertised as GET
        // with the response they really return, without faking a schema.
        if (r.method == "EVERY") continue;   // a scheduled task, not an endpoint
        std::string method = r.method;
        if (method == "SSE" || method == "WS") method = "GET";
        if (method == "*")                     method = "get";
        for (auto& c : method) c = static_cast<char>(::tolower((unsigned char)c));

        auto        in_pattern = pattern_params(r.pattern);
        Value::List params;
        std::string body_class;

        bool json_body = false;
        for (const auto& p : r.params) {
            if (classes.count(p.type.name)) { body_class = p.type.name; continue; }
            if (p.type.name == "Json") { json_body = true; continue; }
            if (p.type.name == "File" ||
                (p.type.name == "List" && !p.type.args.empty() &&
                 p.type.args[0].name == "File")) {
                body_class = "__multipart";
                continue;
            }

            const bool in_path = std::find(in_pattern.begin(), in_pattern.end(), p.name)
                                 != in_pattern.end();
            params.push_back(jobj({
                {"name",     jstr(p.name)},
                {"in",       jstr(in_path ? "path" : "query")},
                {"required", Value::boolean(in_path)},
                {"schema",   jobj({{"type", jstr(openapi_type(p.type.name))}})},
            }));
        }

        Value::Dict op;
        if (!params.empty()) op["parameters"] = Value::list(std::move(params));

        if (body_class == "__multipart") {
            op["requestBody"] = jobj({
                {"required", Value::boolean(true)},
                {"content",  jobj({{"multipart/form-data",
                                    jobj({{"schema", jobj({{"type", jstr("object")}})}})}})},
            });
        } else if (json_body) {
            // No fixed schema to $ref: `Json` accepts any JSON document, so
            // the OpenAPI schema is deliberately empty (`{}`, OpenAPI's own
            // spelling of "any value") rather than guessing `object`, which
            // would wrongly reject a top-level array/string/number body.
            op["requestBody"] = jobj({
                {"required", Value::boolean(true)},
                {"content",  jobj({{"application/json", jobj({{"schema", jobj({})}})}})},
            });
        } else if (!body_class.empty()) {
            op["requestBody"] = jobj({
                {"required", Value::boolean(true)},
                {"content",  jobj({{"application/json",
                    jobj({{"schema", jobj({{"$ref",
                        jstr("#/components/schemas/" + body_class)}})}})}})},
            });
        }

        Value::Dict responses;
        if (r.method == "SSE") {
            responses["200"] = jobj({
                {"description", jstr("Event stream")},
                {"content",     jobj({{"text/event-stream", Value::dict()}})},
            });
            op["summary"] = jstr("Server-Sent Events");
        } else if (r.method == "WS") {
            responses["101"] = jobj({{"description", jstr("Cambio a WebSocket")}});
            op["summary"]    = jstr("WebSocket");
        } else {
            responses["200"] = jobj({{"description", jstr("OK")}});
        }
        // Only the codes the server really produces are declared.
        if (!body_class.empty() && body_class != "__multipart")
            responses["422"] = jobj({{"description", jstr("Validation failed")}});
        if (!r.guards.empty())
            responses["403"] = jobj({{"description", jstr("Group guard not passed")}});

        // Every declared `on error` code is advertised on all routes.
        for (const auto& e : program.errors)
            if (e.code >= 400)
                responses[std::to_string(e.code)] =
                    jobj({{"description", jstr("Manejador propio")}});

        op["responses"] = Value::dict(std::move(responses));

        // Several methods can share a path, so it accumulates onto whatever was
        // already there instead of overwriting it.
        Value& entrada = paths[openapi_path(r.pattern)];
        if (!entrada.is_dict()) entrada = Value::dict();
        entrada.as_dict()[method] = Value::dict(std::move(op));
    }

    doc["paths"] = Value::dict(std::move(paths));
    return Value::dict(std::move(doc)).to_json_text();
}

// `Response` is not a class the checker knows about, and it never can be:
// send_file()/text()/html()/json()/status()/redirect() all write directly
// to ctx.res as a side effect and return Value::null() to whoever called
// them (see call_method()/fn_send_file() et al, natives.cpp) -- there is
// no runtime value that represents "a response", by design (GUIDE.md §7:
// "Everything goes out through return. There is no response object to
// carry around."). Before this check existed, `fn Response? try_serve():
// ... return send_file(path).header(...)` compiled silently and corrupted
// the real response at runtime: try_serve()'s return value is always
// null (send_file() returns null; .header()'s call_method() branch passes
// its receiver straight through), so a caller doing `r == null ? ... : r`
// to tell "no file" from "served" always took the "no file" branch --
// and then wrote a SECOND, conflicting response (its own status()/return)
// on top of the first one, ending up with the second call's status and
// body but the first call's leftover headers (nothing clears headers
// between two response-writing calls in the same request). Caught here
// instead, at the declaration, before any of that can happen.
bool mentions_response_type(const TypeRef& t) {
    if (t.name == "Response") return true;
    for (const auto& a : t.args) if (mentions_response_type(a)) return true;
    return false;
}

// Functions are compiled before routes and handlers, and they all see the
// complete table: that way they can call each other regardless of the order
// they were declared in or the file they are in.
// Fills FnSig from a parameter list, checking that no required one comes after
// one with a default value.
FnSig make_sig(size_t index, const std::vector<Param>& params, const TypeRef& return_type,
              DiagnosticBag& diags) {
    FnSig sig;
    sig.index    = index;
    sig.return_type = Type::from_declared(return_type);
    if (mentions_response_type(return_type))
        diags.error(return_type.loc,
            "'Response' cannot be used as a return type: there is no response value to "
            "hold. send_file()/text()/html()/json()/status()/redirect()/render() write "
            "the response directly and always return null to their own caller -- write "
            "the response inline in the route itself instead of returning it from a "
            "helper function.");
    bool seen_default = false;
    for (const auto& p : params) {
        sig.defaults.push_back(p.default_value.get());
        if (mentions_response_type(p.type))
            diags.error(p.loc,
                "'Response' cannot be used as a parameter type: there is no response "
                "value to pass in. See the same note on a 'Response' return type.");
        if (p.default_value) seen_default = true;
        else {
            if (seen_default)
                diags.error(p.loc, "a parameter without a default cannot come "
                                   "after one that has one");
            ++sig.required;
        }
    }
    return sig;
}

// Just the member-name sets `Color.RED`-style access checks against --
// nothing to resolve by index the way a function/class needs, since a
// member compiles straight to its own name as a string constant (EnumDecl's
// comment, ast.hpp).
EnumSigs build_enum_signatures(Module& mod, DiagnosticBag& diags) {
    EnumSigs out;
    for (const auto& e : mod.program.enums) {
        if (out.count(e.name)) {
            diags.error(e.loc, "enum '" + e.name + "' is already declared");
            continue;
        }
        out[e.name] = std::set<std::string>(e.members.begin(), e.members.end());
    }
    return out;
}

// Methods and constructors are compiled as functions with `this` as the first
// parameter, so they go into the same table as standalone functions.
ClassSigs build_class_signatures(Module& mod, DiagnosticBag& diags) {
    ClassSigs out;

    for (const auto& c : mod.program.classes) {
        if (out.count(c.name)) continue;      // duplicate already reported
        ClassSig sig;
        for (const auto& f : c.fields) sig.fields.push_back(f.name);

        for (const auto& m : c.methods) {
            size_t idx = mod.functions.size();
            mod.functions.push_back(std::make_shared<Chunk>());
            sig.methods[m.name] = make_sig(idx, m.params, m.return_type, diags);
        }
        for (const auto& ct : c.ctors) {
            size_t idx = mod.functions.size();
            mod.functions.push_back(std::make_shared<Chunk>());
            sig.ctors[ct.params.size()] = idx;
        }

        // With no declared constructor, the full mapping one is offered: every
        // field in order.
        if (sig.ctors.empty() && !c.fields.empty()) {
            size_t idx = mod.functions.size();
            mod.functions.push_back(std::make_shared<Chunk>());
            sig.ctors[c.fields.size()] = idx;
        }
        out[c.name] = std::move(sig);
    }
    return out;
}

void emit_class_bodies(Module& mod, const ClassSigs& classes, const FunctionSigs& fns,
                       const EnumSigs& enums, DiagnosticBag& diags) {
    const std::set<std::string>* imports = &mod.program.imports;
    for (const auto& c : mod.program.classes) {
        auto it = classes.find(c.name);
        if (it == classes.end()) continue;
        const ClassSig& sig = it->second;

        for (const auto& m : c.methods) {
            auto ms = sig.methods.find(m.name);
            if (ms == sig.methods.end()) continue;
            Emitter emitter(diags, &fns, &classes, imports, nullptr, &enums);
            emitter.emit_method(c.name, m, *mod.functions[ms->second.index]);
        }
        for (const auto& ct : c.ctors) {
            auto cs = sig.ctors.find(ct.params.size());
            if (cs == sig.ctors.end()) continue;
            Emitter emitter(diags, &fns, &classes, imports, nullptr, &enums);
            emitter.emit_ctor(c.name, sig.fields, ct, *mod.functions[cs->second]);
        }

        // The implicit constructor: a synthetic CtorDecl with one parameter per
        // field, in declaration order.
        if (c.ctors.empty() && !c.fields.empty()) {
            CtorDecl implicito;
            implicito.loc = c.loc;
            for (const auto& f : c.fields) {
                Param p;
                p.loc  = f.loc;
                p.name = f.name;
                p.type = f.type;
                implicito.params.push_back(std::move(p));
            }
            auto cs = sig.ctors.find(c.fields.size());
            if (cs != sig.ctors.end()) {
                Emitter emitter(diags, &fns, &classes, imports, nullptr, &enums);
                emitter.emit_ctor(c.name, sig.fields, implicito,
                                  *mod.functions[cs->second]);
            }
        }
    }
}

FunctionSigs build_function_signatures(Module& mod, DiagnosticBag& diags) {
    FunctionSigs index;

    for (const auto& f : mod.program.functions) {
        if (native_id(f.name) >= 0) {
            diags.error(f.loc, "'" + f.name + "' is a builtin: choose another name");
            continue;
        }
        if (index.count(f.name)) continue;   // the parser already reported the duplicate

        index[f.name] = make_sig(mod.functions.size(), f.params, f.return_type, diags);
        mod.functions.push_back(std::make_shared<Chunk>());
    }

    return index;
}

// True if `code` contains an Op::CallFunction targeting a callee (looked
// up by index into `functions`, exactly how the opcode's own operand
// works) whose `has_await` is already true. Every user function, method
// AND constructor call in the whole language compiles to this one opcode
// (see Emitter::emit_call's UserFunctionCall/ConstructorCall/
// ClassMethodCall cases, which all funnel into it) with `call_index`
// being the callee's index into that SAME FunctionTable, so this one
// check covers everything a route/function body can call.
//
// Used two ways below: iterated to a fixed point over mod.functions
// itself (functions can call each other, in any order, including mutual
// recursion), and as a single, non-iterated check for a route's own
// chunk once that fixed point has already settled -- nothing ever calls
// a route, so there is no further propagation needed past that one check.
bool calls_something_that_awaits(const Chunk& code, const FunctionTable& functions) {
    for (const auto& in : code.code) {
        if (in.op != Op::CallFunction) continue;
        size_t callee = in.operand >> 8;
        if (callee < functions.size() && functions[callee]->has_await) return true;
    }
    return false;
}

// Propagates has_await through the call graph of every standalone
// function, class method and constructor in the program.
//
// Emitter::emit_call already sets has_await correctly for a chunk whose
// OWN bytecode calls something async directly (CallAsync/CallAsyncModule/
// DbModuleCall) -- what it cannot see is a function that merely CALLS
// another function that awaits something, however many calls deep. Left
// unfixed, that chunk's has_await stays false, and project.cpp's plain
// HTTP route dispatch (the ONE place that branches on it -- ws/sse routes
// always use a resumable VM regardless, see their own dispatch code
// further down) wrongly assumes vm.start() can only return Done/Error and
// hands the call to a blocking-pool worker's own, unrelated thread_local
// VM instead of the per-request one actually capable of being resumed.
// The VM still suspends correctly there -- nothing is listening for it:
// the thread_local VM's suspended frame is silently abandoned (leaking
// its state into whatever request that pool worker handles NEXT), and the
// route's own, never-actually-started VM::resume() on an empty frame
// stack falls straight through to a default Done/null result. The
// caller's `int v = await helper()` (or a bare `return helper()`) sees a
// null value where the callee's real, computed return value should have
// been -- not a crash, which is exactly what let this hide as long as it
// did.
void propagate_has_await(FunctionTable& functions) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& chunk : functions) {
            if (!chunk->has_await && calls_something_that_awaits(*chunk, functions)) {
                chunk->has_await = true;
                changed = true;
            }
        }
    }
}

// Bodies, once EVERYTHING callable has a signature -- classes included.
// This used to be the second half of build_functions() itself, with
// `classes`/`enums` never passed to Emitter at all (always nullptr): a
// standalone `fn` could not construct a class instance or call a method on
// one ("unknown function: 'Item'" for a class named Item), not because the
// language forbids it -- nothing about a Value::Dict cares whether the
// code building it is a route handler or a plain function -- but because
// this half ran before build_class_signatures()/build_enum_signatures()
// even existed yet, a purely historical ordering accident (the comment
// this replaces called it "on purpose", which was true only in the sense
// that nobody had gone back to fix it once classes/enums got their own
// signature-then-body split). Splitting this the same way removes the
// asymmetry: a route/method already resolves ConstructorCall/
// ClassMethodCall/enum values fine, and now a standalone function does too.
void emit_function_bodies(Module& mod, const FunctionSigs& fns, const ClassSigs& classes,
                          const EnumSigs& enums, DiagnosticBag& diags) {
    for (const auto& f : mod.program.functions) {
        auto it = fns.find(f.name);
        if (it == fns.end()) continue;
        Emitter emitter(diags, &fns, &classes, &mod.program.imports, nullptr, &enums);
        emitter.emit_function(f, *mod.functions[it->second.index]);
    }
}

void build_error_handlers(Module& mod, const FunctionSigs& fns,
                          const ClassSigs& sigs, const EnumSigs& enums,
                          DiagnosticBag& diags) {
    // An error handler can render a page too.
    TemplateCtx pctx{mod.program.app.templates_dir, &mod.templates};
    for (const auto& e : mod.program.errors) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter emitter(diags, &fns, &sigs, &mod.program.imports, &pctx, &enums);
        if (!emitter.emit_error_handler(e, *chunk)) continue;
        mod.error_handlers[e.code] = std::move(chunk);
    }
}

void build_routes(Module& mod, const ClassTable& classes, const AuthConfig& auth,
                  const FunctionSigs& fns, const ClassSigs& sigs,
                  const EnumSigs& enums, DiagnosticBag& diags) {
    // The Module owns the table and outlives any in-flight request: the
    // dispatcher keeps its shared_ptr alive while the handler runs.
    const FunctionTable* fn_table = &mod.functions;
    // --native (phase 2): resolved once, like fn_table -- valid because
    // compile() already finished compiling native code before this is
    // called (see the comment on Module::native, project.hpp).
    const NativeDispatch native_table = mod.native ? mod.native->dispatch() : NativeDispatch{};
    // Templates live in the module, like the functions: the pointer is stable
    // as long as the module is, and the reload swap changes both at the same
    // time.
    const std::vector<Template>* tpl_table = &mod.templates;

    // Context the emitters need to compile the templates they find in a
    // render().
    TemplateCtx pctx{mod.program.app.templates_dir, &mod.templates};

    for (size_t ridx = 0; ridx < mod.program.routes.size(); ++ridx) {
        const auto& r = mod.program.routes[ridx];
        if (!r.origins.empty() && r.method != "WS")
            diags.error(r.loc, "origins() is only valid on ws routes");

        // ── ws routes ────────────────────────────────────────────────────────
        // The RFC 6455 handshake is done by the engine; here the VM is only
        // driven with the connection already established.
        if (r.method == "WS") {
            if (r.origins.empty()) {
                diags.error(r.loc, "a ws route needs origins(...): without an allowlist "
                                   "any site can open the connection "
                                   "from your user's browser");
                continue;
            }

            std::vector<ParamBind> ws_binds;
            if (!bind_params(r, classes, ws_binds, diags)) continue;
            bool body_param = false;
            for (const auto& b : ws_binds)
                if (b.kind == BindKind::Body || b.kind == BindKind::JsonBody) body_param = true;
            if (body_param) {
                diags.error(r.loc, "a ws route takes no body");
                continue;
            }

            auto    ws_chunk = std::make_shared<Chunk>();
            Emitter ws_emitter(diags, &fns, &sigs, &mod.program.imports, &pctx);
            if (!ws_emitter.emit_route(r, *ws_chunk)) continue;

            ++mod.vm_routes;
            std::string ws_where = "WS " + r.pattern;
            lux::App::WSOptions opts;
            opts.allowed_origins = r.origins;

            mod.route_report.push_back({r.method, r.pattern, "ws"});
            mod.router.add_internal("GET", r.pattern,
                // Parameters are validated BEFORE the RFC 6455 handshake,
                // not inside it: make_ws_handler() below writes the 101
                // Switching Protocols response the instant it runs, and
                // once that is on the wire there is no way to instead
                // answer a 400/422 for a missing or malformed query
                // parameter -- the connection is already a WebSocket as
                // far as the client is concerned. Confirmed against the
                // real binary: a ws route with a required string parameter
                // and no value supplied upgraded to 101 and then just sat
                // there silently forever (prepare_args() failed and
                // co_returned only AFTER the handshake already ran) --
                // indistinguishable, from the client's side, from a server
                // that accepted the connection and simply has nothing to
                // say. This outer handler runs prepare_args() first,
                // against the ordinary HTTP request/response pair (a
                // failure here writes a normal 400/422 exactly like any
                // other route), and only calls into make_ws_handler()'s
                // upgrade logic -- with the already-validated args moved
                // in -- once that succeeds.
                //
                // This does mean the Origin allowlist check (inside
                // make_ws_handler itself) now runs AFTER parameter
                // validation instead of before it, where it used to run
                // first. Weighed deliberately, not missed: coerce()'ing a
                // handful of scalar query params is in-memory, side-
                // effect-free, and no slower than the query-string parsing
                // every request already does regardless of origin, so a
                // disallowed origin cannot use this to make the server do
                // meaningfully more work than before it is rejected; and
                // the 422 it gets back names a parameter the request it
                // just sent already had to be built around, not something
                // origin-gating was ever hiding from it. Swapping the two
                // checks to preserve the old order would need
                // make_ws_handler's origin logic duplicated or factored out
                // on its own -- not worth it for a difference this small.
                [ws_chunk, ws_binds, ws_where, auth, fn_table, native_table, tpl_table,
                 opts](lux::Request& req, lux::Response& res) -> lux::Task<void> {
                    NativeCtx           pre_ctx{req, res};
                    std::vector<Value>  args;
                    if (!prepare_args(ws_binds, fn_table, req, res, pre_ctx, args)) co_return;

                    co_await lux::App::make_ws_handler(
                        [ws_chunk, ws_where, auth, fn_table, native_table, tpl_table,
                         args = std::move(args)]
                        (lux::WSConnection conn, lux::Request& req,
                         lux::Response& res) mutable -> lux::Task<void> {
                        NativeCtx    ctx{req, res};
                ctx.templates = tpl_table;
                ctx.functions  = fn_table;
                        SessionState session;
                        Value        claims = Value::dict();
                        begin_auth(auth, req, session, claims, ctx);
                        ctx.ws              = &conn;
                        ctx.response_written = true;   // the upgrade already replied

                        VM         vm;
                        VM::Result result = vm.start(*ws_chunk, std::move(args), ctx, fn_table, &native_table);

                        if (!co_await drive_vm(vm, result, req, ctx, &conn)) co_return;

                        // See the identical comment on the sse route below:
                        // this is the only place left that can close a
                        // transaction the handler opened and never closed
                        // itself, on EITHER a normal end (Done) or an error.
                        co_await rollback_pending(ctx, req);

                        if (result.status == VM::Status::Error)
                            lux::log().error(error_at(result, ws_where) + ": " + result.error);
                        },
                        opts)(req, res);   // opts is copied, not moved: this whole
                                           // outer handler (and its captured opts)
                                           // is invoked once per incoming WS
                                           // request, not once ever -- moving it
                                           // would leave every connection after
                                           // the first with an empty allowlist.
                },
                /*head_alias=*/false);
            continue;
        }

        // ── sse routes ───────────────────────────────────────────────────────
        // The stream is opened before starting the VM and closed when the
        // handler ends; there is no final response to write.
        if (r.method == "SSE") {
            std::vector<ParamBind> sse_binds;
            if (!bind_params(r, classes, sse_binds, diags)) continue;
            for (const auto& b : sse_binds) {
                if (b.kind == BindKind::Body || b.kind == BindKind::JsonBody) {
                    diags.error(r.loc, "an sse route takes no body");
                    break;
                }
            }

            auto    sse_chunk = std::make_shared<Chunk>();
            Emitter sse_emitter(diags, &fns, &sigs, &mod.program.imports, &pctx);
            if (!sse_emitter.emit_route(r, *sse_chunk)) continue;

            ++mod.vm_routes;
            std::string sse_where = "SSE " + r.pattern;
            mod.route_report.push_back({r.method, r.pattern, "sse"});
            mod.router.add_internal("GET", r.pattern,
                [sse_chunk, sse_binds, sse_where, auth, fn_table, native_table, tpl_table](lux::Request& req,
                                                        lux::Response& res)
                    -> lux::Task<void> {
                    NativeCtx    ctx{req, res};
                ctx.templates = tpl_table;
                ctx.functions  = fn_table;
                    SessionState session;
                    Value        claims = Value::dict();
                    begin_auth(auth, req, session, claims, ctx);

                    std::vector<Value> args;
                    if (!prepare_args(sse_binds, fn_table, req, res, ctx, args)) co_return;

                    // make_sse already writes the stream headers, so the
                    // response counts as sent from this point on.
                    auto writer = lux::make_sse(res, req);
                    ctx.sse             = &writer;
                    ctx.response_written = true;

                    VM         vm;
                    VM::Result result = vm.start(*sse_chunk, std::move(args), ctx, fn_table, &native_table);

                    if (!co_await drive_vm(vm, result, req, ctx)) co_return;

                    // Every OTHER exit from an sse route runs through here --
                    // the stream ending normally (Done) or the handler
                    // erroring out (Error) -- and neither one had ANY
                    // rollback at all before this: a `sqlite.begin()` with no
                    // matching commit()/rollback() (by design, on error, or
                    // just a bug in the handler) left that connection pinned
                    // with an open transaction for the rest of the process's
                    // life, since sse routes have no later point that could
                    // call it. Same reasoning as ws above and as the bytecode
                    // HTTP route below.
                    co_await rollback_pending(ctx, req);

                    if (result.status == VM::Status::Error)
                        lux::log().error(error_at(result, sse_where) + ": " + result.error);
                },
                /*head_alias=*/false);
            continue;
        }

        // The pattern is ALWAYS checked, before the declarative shortcut: a
        // route without logic can declare ':id' and not bind it too, and that
        // compiler promise cannot depend on which path the route takes.
        for (const auto& seg : pattern_params(r.pattern)) {
            bool recogido = false;
            for (const auto& p : r.params) if (p.name == seg) recogido = true;
            if (!recogido)
                diags.error(r.pattern_loc, "the pattern declares ':" + seg +
                                           "' but no parameter binds it");
        }

        // Level 1: declarative route → native action, zero bytecode.
        if (Action a = try_declarative(r, mod.program.app.templates_dir)) {
            ++mod.declarative_routes;
            mod.route_report.push_back({r.method, r.pattern, "declarative"});
            mod.router.add_internal(r.method, r.pattern,
                [a](lux::Request& req, lux::Response& res) -> lux::Task<void> {
                    a(req, res);
                    co_return;
                });
            continue;
        }

        // Level 2: route with logic → bytecode on the VM.
        //
        // bind_params validates the parameters (a default on a path one,
        // File out of place, more than one body...) ALWAYS, regardless of
        // which level ends up serving the route -- level 1.5 does not use
        // `binds` directly (the native handler does its own binding,
        // identical in the rules but over C++ types instead of Value), but
        // a program with an invalid pattern/parameter has to fail
        // compilation all the same, and a route generate_native_route()
        // accepted is, by construction, a STRICTER subset of what
        // bind_params allows (no default, no File, no body) -- it should
        // never fail here if it already passed there.
        std::vector<ParamBind> binds;
        if (!bind_params(r, classes, binds, diags)) continue;

        // Nivel 1.5: ruta compilada nativamente (Fase 4/5, --native). Mismo
        // criterio de "todo o nada" que una funcion: generate_native_route()
        // (native_gen.cpp), invocado durante compile() -> compile_native(),
        // ya decidio si esta ruta entera es representable (parametros
        // escalares de patron/query sin valor por defecto, cuerpo sin
        // sesion/JWT/render/contenedores, `await` limitado a `sleep`) -- si
        // lo es, mod.native trae el puntero ya resuelto por dlsym() y aqui
        // solo hace falta invocarlo. La funcion generada hace su PROPIO
        // binding de parametros (lee req.params/req.query ella misma) y
        // escribe la respuesta directamente sobre `res`: no pasa por
        // bind_params/prepare_args/begin_auth ni por el VM.
        //
        // Una ruta con `await` (RutaNativa::asincrona) se genero como
        // lux::Task<void> de verdad -- su firma YA coincide exactamente
        // con Handler (ver types.hpp), asi que se registra directa, sin
        // ningun envoltorio: envolverla en otra corrutina que la
        // co_await-ara solo anadiria un frame sin necesidad.
        if (mod.native && ridx < mod.native->rutas_async_por_indice.size() &&
            mod.native->rutas_async_por_indice[ridx]) {
            ++mod.vm_routes;
            mod.route_report.push_back({r.method, r.pattern, "native (async)"});
            mod.router.add_internal(r.method, r.pattern, mod.native->rutas_async_por_indice[ridx]);
            continue;
        }
        if (mod.native && ridx < mod.native->rutas_por_indice.size() &&
            mod.native->rutas_por_indice[ridx]) {
            auto fn = mod.native->rutas_por_indice[ridx];
            ++mod.vm_routes; // cuenta como ruta con logica, aunque no pase por el VM
            mod.route_report.push_back({r.method, r.pattern, "native"});
            mod.router.add_internal(r.method, r.pattern,
                [fn](lux::Request& req, lux::Response& res) -> lux::Task<void> {
                    // No `await` anywhere in this route (that is exactly what
                    // landed it in rutas_por_indice instead of
                    // rutas_async_por_indice above): its whole body runs to
                    // completion in one call, so it is safe to hand the
                    // entire thing to the shared blocking pool instead of
                    // running it inline and stalling this connection's event
                    // loop -- and everyone else queued behind it -- for
                    // however long it takes. See blocking_pool.hpp.
                    // The validation messages an `on error` handler reads are
                    // thread_local: filled on the pool thread, they are
                    // carried back to this one.
                    std::vector<std::string> messages;
                    co_await lux::BlockingAwaitable{req.loop, [&] {
                        fn(req, res);
                        messages = last_validation_messages();
                    }};
                    last_validation_messages() = std::move(messages);
                });
            continue;
        }

        // Nivel 2: ruta con logica → bytecode sobre el VM.
        auto    chunk = std::make_shared<Chunk>();
        Emitter emitter(diags, &fns, &sigs, &mod.program.imports, &pctx, &enums);
        if (!emitter.emit_route(r, *chunk)) continue;

        // A route is never itself called from elsewhere, so unlike
        // mod.functions (propagate_has_await(), already run by the time
        // this runs) it needs no fixed point of its own -- just one check
        // against a callee table that has already fully settled.
        if (calls_something_that_awaits(*chunk, mod.functions)) chunk->has_await = true;

        ++mod.vm_routes;
        bool needs_upload = false;
        for (const auto& b : binds)
            if (b.kind == BindKind::File || b.kind == BindKind::FileList)
                needs_upload = true;

        std::string where = r.method + " " + r.pattern;
        mod.route_report.push_back({r.method, r.pattern,
            ridx < mod.native_why.rutas.size() && !mod.native_why.rutas[ridx].empty()
                ? "bytecode (" + mod.native_why.rutas[ridx] + ")" : "bytecode"});
        mod.router.add_internal(r.method, r.pattern,
            [chunk, binds, where, auth, needs_upload, fn_table, native_table, tpl_table](lux::Request& req, lux::Response& res)
                -> lux::Task<void> {
                NativeCtx    ctx{req, res};
                ctx.templates = tpl_table;
                ctx.functions  = fn_table;
                SessionState session;
                Value        claims = Value::dict();
                begin_auth(auth, req, session, claims, ctx);

                // The multipart body is only parsed if some slot asks for it.
                std::vector<lux::MultipartPart> parts;
                if (needs_upload) {
                    if (auto p = lux::parse_multipart(req)) {
                        parts       = std::move(*p);
                        ctx.parts   = &parts;
                        ctx.uploads = true;
                    }
                }

                std::vector<Value> args;
                if (!prepare_args(binds, fn_table, req, res, ctx, args)) co_return;

                // The VM lives in this coroutine's frame, not in the thread: two
                // handlers suspended at once on the same core each have their own
                // stack and locals.  The ones that cannot suspend reuse one per
                // thread and save the two allocations.
                thread_local VM shared_vm;
                VM  own_vm;

                VM::Result result;
                if (chunk->has_await) {
                    result = own_vm.start(*chunk, std::move(args), ctx, fn_table, &native_table);
                } else {
                    // No `await` anywhere in this chunk (that is what
                    // has_await means): vm.start() is guaranteed to return
                    // Done/Error on this one call, never Suspended -- the
                    // resume loop below simply will not run for this
                    // branch -- so the whole call is safe to hand to the
                    // shared blocking pool instead of running it inline and
                    // stalling this connection's event loop, and everyone
                    // else queued behind it, for however long the bytecode
                    // takes (VM interpretation has no JIT: this is where it
                    // hurts most). See blocking_pool.hpp. shared_vm being
                    // thread_local means it becomes "one VM per pool
                    // worker" here rather than "one per event loop" -- same
                    // amortization, just relative to whichever thread pool
                    // actually runs it.
                    co_await lux::BlockingAwaitable{req.loop, [&] {
                        result = shared_vm.start(*chunk, std::move(args), ctx, fn_table, &native_table);
                    }};
                }

                // Only the has_await branch can suspend, so own_vm -- not
                // shared_vm -- is the one whose frame is live across it.
                if (!co_await drive_vm(own_vm, result, req, ctx)) co_return;

                if (result.status == VM::Status::Error) {
                    std::string at = error_at(result, where);
                    lux::log().error(at + ": " + result.error);
                    // The 500 below ends the request without ever reaching
                    // the rollback_pending() call a few lines down (that
                    // one only runs on success) -- a runtime error INSIDE a
                    // transaction (the exact case this branch exists for)
                    // would otherwise leave the connection that opened it
                    // pinned with the transaction still open, and the next
                    // unrelated request routed to that same worker would run
                    // silently inside it.
                    co_await rollback_pending(ctx, req);
                    last_internal_error() = result.error;
                    Value::Dict d;
                    if (is_production_mode()) {
                        d["error"] = Value::str("Internal Server Error");
                    } else {
                        d["error"] = Value::str(result.error);
                        d["at"]    = Value::str(at);
                    }
                    responder(res, 500, Value::dict(std::move(d)));
                    co_return;
                }

                co_await rollback_pending(ctx, req);
                end_auth(auth, session, res);

                if (ctx.response_written) co_return;
                if (result.value.is_null()) { res.status(204).send(""); co_return; }
                res.header("Content-Type", "application/json; charset=utf-8")
                   .send(result.value.to_json_text());
            });
    }
}

} // namespace

// ─── Compilation ─────────────────────────────────────────────────────────────

std::shared_ptr<Module> compile(const std::vector<fs::path>& inputs,
                                DiagnosticBag& diags, bool native) {
    auto mod = std::make_shared<Module>();
    std::error_code ec;

    for (const auto& path : inputs) {
        auto src  = std::make_unique<SourceFile>();
        src->path = path.string();

        std::ifstream f(path, std::ios::binary);
        if (!f) {
            diags.error({}, "cannot read: " + src->path);
            continue;
        }
        std::ostringstream ss; ss << f.rdbuf();
        src->text = ss.str();

        auto stamp = fs::last_write_time(path, ec);
        if (!ec) mod->stamps.emplace_back(path, stamp);

        mod->files.push_back(std::move(src));
    }

    // Pass 1: lex and parse every file onto the same Program.
    for (const auto& src : mod->files) {
        Lexer  lexer(*src, diags);
        Parser parser(lexer.tokenize(), diags);
        parser.parse_into(mod->program);
    }

    // A relative templates/static/database path in the `.lux` source is
    // resolved against the DIRECTORY OF THAT SOURCE FILE, not the
    // process's current working directory -- launching `lux
    // /srv/blog/app.lux` from somewhere else entirely (a cron job, a
    // systemd unit with no WorkingDirectory=, an IDE's own default cwd)
    // used to make `templates "./templates"` fail to find anything
    // ("template not found") and `sqlite: file "./blog.db"` silently
    // open/create an EMPTY database in whatever directory the process
    // happened to start in -- no error, just the app quietly acting as
    // if every row it had ever written was gone. An already-absolute
    // path (or one built from env(), which a deploy controls
    // deliberately) is never touched. Runs once, right after parsing,
    // before module activation reads `modules["sqlite"]["file"]` below
    // or anything else reads `templates_dir`/a static mount's `fs_root`.
    if (diags.empty() && mod->program.app.loc.file) {
        fs::path base = fs::path(*mod->program.app.loc.file).parent_path();
        auto resolve = [&](std::string& path) {
            if (path.empty() || fs::path(path).is_absolute() || base.empty()) return;
            // ":memory:" (SQLite's own convention for a private in-memory
            // database, recognized by sqlite3_open() itself) and a
            // "file:...?..." SQLite URI filename are not filesystem paths
            // at all -- resolving either against `base` would corrupt it
            // into a literal, on-disk file with that exact odd name.
            if (path == ":memory:" || path.rfind("file:", 0) == 0) return;
            path = (base / path).lexically_normal().string();
        };
        resolve(mod->program.app.templates_dir);
        for (auto& m : mod->program.app.statics) resolve(m.fs_root);
        auto sqlite_it = mod->program.app.modules.find("sqlite");
        if (sqlite_it != mod->program.app.modules.end()) {
            auto file_it = sqlite_it->second.find("file");
            if (file_it != sqlite_it->second.end()) resolve(file_it->second);
        }
    }

    // Modules are checked before anything else: importing one this binary does
    // not carry, or using it unconfigured, has to be said plainly.
    if (diags.empty()) {
        auto& reg      = DbRegistry::instance();
        auto& mod_reg  = BuiltinModuleRegistry::instance();
        for (const auto& m : mod->program.imports) {
            if (reg.has(m)) {
                auto it = mod->program.app.modules.find(m);
                if (it == mod->program.app.modules.end()) {
                    diags.error(mod->program.app.loc,
                                "'import " + m + "' without its '" + m + ":' in app:");
                    continue;
                }
                std::string err;
                if (!reg.activate(m, it->second, err)) diags.error(mod->program.app.loc, err);
                continue;
            }
            if (mod_reg.has(m)) {
                // Native modules (NATIVE-MODULES.md), unlike DB drivers, do
                // not require an `<name>: { ... }` block in `app:` -- most
                // need no configuration at all (hash does not), so an
                // absent block is not an error, just no options.
                auto it = mod->program.app.modules.find(m);
                static const std::map<std::string, std::string> sin_opciones;
                std::string err;
                if (!mod_reg.activate(m, it == mod->program.app.modules.end() ? sin_opciones
                                                                              : it->second,
                                      err))
                    diags.error(mod->program.app.loc, err);
                continue;
            }
            auto available_mods = reg.available();
            for (const auto& d : mod_reg.available()) available_mods.push_back(d);
            std::string list_;
            for (const auto& d : available_mods) list_ += (list_.empty() ? "" : ", ") + d;
            diags.error({}, "module '" + m + "' is not compiled into this binary" +
                            (list_.empty() ? "" : "; available: " + list_));
        }
    }

    // Pass 2: resolve classes and build the route table.  Classes go first
    // because routes bind against them; within each pass the file order does
    // not matter.
    if (diags.empty()) {
        // Order: first the signatures of everything callable —standalone
        // functions, methods and constructors— and only then the bodies.  That
        // way anyone can call anyone regardless of declaration order -- a
        // standalone function's OWN body is no exception: it needs
        // ClassSigs/EnumSigs in hand before it compiles, exactly like a
        // route or a method does, to construct an instance or use an enum
        // value itself.
        auto fns   = build_function_signatures(*mod, diags);
        mod->function_sigs = fns;
        auto enums = build_enum_signatures(*mod, diags);
        auto sigs  = build_class_signatures(*mod, diags);

        emit_function_bodies(*mod, fns, sigs, enums, diags);
        emit_class_bodies(*mod, sigs, fns, enums, diags);

        // See propagate_has_await()'s own comment for the bug this closes:
        // every function/method/constructor now has an accurate has_await,
        // including one that only awaits transitively (by calling another
        // one that does) -- needed before a single route gets emitted
        // below, since a route's own has_await is decided by looking at
        // THIS table (calls_something_that_awaits()), not recomputed from
        // scratch.
        propagate_has_await(mod->functions);

        // --native (Fase 3): despues de las firmas/cuerpos de clase (necesita
        // ClassSigs para constructores/metodos) y antes de construir rutas,
        // para que puedan capturar mod->native.get() ya resuelto -- ver el
        // comentario sobre BuiltinModule en project.hpp. Solo si lo demas
        // compilo limpio: no tiene sentido invocar g++ sobre un programa que
        // de todas formas no se va a publicar.
        if (native && diags.empty())
            mod->native = compile_native(mod->program, fns, sigs, ".lux-native",
                                          mod->native_warning, &mod->native_why, &mod->functions, &enums);
        if (mod->native && mod->native->bind) mod->native->bind(&mod->functions, &mod->templates);

        ClassTable classes;
        build_classes(mod->program, fns, sigs, enums, &mod->program.imports, classes, diags);

        AuthConfig auth;
        auth.session_secret  = mod->program.app.session_secret;
        auth.session_max_age = mod->program.app.session_max_age;
        auth.session_secure  = mod->program.app.session_secure;
        auth.jwt_secret      = mod->program.app.jwt_secret;
        auth.jwt_issuer      = mod->program.app.jwt_issuer;

        if (diags.empty()) build_routes(*mod, classes, auth, fns, sigs, enums, diags);
        if (diags.empty()) build_error_handlers(*mod, fns, sigs, enums, diags);
        if (diags.empty()) mod->openapi = build_openapi(mod->program, classes);
    }

    // Templates go into `stamps` too, alongside the `.lux` files above: without
    // this, main.cpp's watch_loop() only ever saw the `.lux` files it compiled,
    // so editing a template (templates/index.html, .../layout.html — anything
    // {% include %}-d too, since this scans the whole directory rather than
    // trying to track exactly which files a given render()/include chain
    // touched) had NO effect until something ALSO touched a `.lux` file.
    // Whole-directory rather than "only what render() actually opened": the
    // dependency between a `.lux` route and the templates it can reach through
    // {% include %} is not tracked anywhere durable enough to replay here, and
    // over-watching a few unrelated files in the same folder costs nothing —
    // it triggers one more harmless recompile, not a wrong one.
    if (!mod->program.app.templates_dir.empty()) {
        std::error_code walk_ec;
        for (const auto& entry :
             fs::recursive_directory_iterator(mod->program.app.templates_dir, walk_ec)) {
            if (walk_ec) break;
            if (!entry.is_regular_file(walk_ec)) continue;
            auto stamp = fs::last_write_time(entry.path(), ec);
            if (!ec) mod->stamps.emplace_back(entry.path(), stamp);
        }
    }

    // It is always returned: the caller checks diags.empty() to decide whether
    // to publish it.  See the note in project.hpp about SourceLoc lifetimes.
    return mod;
}

} // namespace lux_script
