# Native modules — extending Lux Script beyond the language core

> How `import <name>` grows beyond the database drivers (`sqlite`/`postgres`/`mysql`) into a
> general mechanism for adding capability to Lux Script — `hash`, `csv`, `pdf` and `http`
> today, more tomorrow — and a step-by-step guide for building one. Companion to
> [GUIDE.md](GUIDE.md) (using an application once it is built) — native *compilation*
> (`--native`) is a different thing, covered elsewhere in the codebase's own comments.

---

## 1. What this is, and what it deliberately is not

Python has `hashlib`, `csv`, `smtplib` — a large standard library, installable extensions on
top of it (`pip install`), and a stable C-extension ABI that lets either ship as compiled code.
Lux has none of that yet. This document is the first step: a way to add a capability to
Lux Script that is more than a language keyword and less than a full package ecosystem.

Three shapes were on the table:

1. **Generalize the existing compiled-in pattern.** A module broader than `DbDriver`'s
   query/exec shape, but still compiled *into* the `lux` binary at CMake time, exactly like
   `sqlite`/`postgres`/`mysql` are today. Reuses the type-checked, `--native`-compatible
   infrastructure that already exists; using a new module still means rebuilding `lux`.
2. **True dynamic loading**, `.so` files discovered at runtime with no rebuild — the actually
   Python-like answer. Needs a stable ABI between modules and the compiler, a way to describe a
   module's types *before* it is loaded, versioning, probably a package manager. Bigger than
   `--native` was.
3. **A couple of hardcoded builtins**, added straight to the compiler the way `crypto`/`auth`
   already are — no new mechanism, but nothing a Lux *user* could do without patching the
   compiler either.

**This document builds (1).** It is the pragmatic middle: real extensibility, without taking on
a second, much larger project (dynamic loading) before the shape of a "module" is even proven.
Nothing here forecloses (2) later — see §6.

---

## 2. Why not just reuse `DbDriver`?

`DbDriver` (`db.hpp`) already proves that "an `import`ed, compiled-in capability" works —
`DbRegistry` gates it on `import`, `#ifdef LUX_SQLITE`-style options gate it on cmake. It was
the obvious first thing to try extending. It does not fit:

- Every `DbDriver` method takes `(worker, sql-string, vector<Value> args)` and returns rows or
  an affected-count. That is exactly right for "talk to a database" and meaningless for
  "compute a hash" or "add a page to a PDF" — there is no SQL string, no worker/connection
  pool, no rows.
- Every `DbDriver` call is `await`ed — DB modules always suspend onto a worker-thread pool
  (`DbPool`) so a slow query does not pin the event loop. A hash of a short string, or most
  other CPU-bound library calls, does not need that: forcing `await` on every module call would
  be asking every module author to reason about a coroutine/worker-pool dance their module does
  not need.

So a native module (`BuiltinModule`, `include/lux_script/builtin_module.hpp`) is a sibling of
`DbDriver`, not a specialization of it: **synchronous** (no worker pool, no `await`, functions
run inline on whatever thread calls them — the event loop thread, same as any other builtin),
and its functions are a flat, arbitrary, named set instead of a fixed query/exec/tx verb list.

## 3. Architecture

### 3.1 The interface

```cpp
struct BuiltinModuleFn {
    std::string name;         // "sha256" -> used as `hash.sha256(...)`
    int         min_args;
    int         max_args;     // -1 = no limit
    NativeFn    fn;           // Value(*)(NativeCtx&, vector<Value>&, string&) --
                               // the EXACT signature every other builtin already uses
                               // (natives.hpp) -- a module function IS a builtin,
                               // just namespaced under an import instead of always present.
    bool        is_async;     // runs on the I/O pool; `await` required
    std::string sig;          // argument types, e.g. "ss|i"; min/max come from it

    Value call(NativeCtx&, std::vector<Value>&, std::string& error) const;  // checks sig, then fn
};

class BuiltinModule {
public:
    virtual const char*                         name() const = 0;
    virtual const std::vector<BuiltinModuleFn>&  functions() const = 0;
    virtual bool configure(const std::map<std::string, std::string>& options,
                           std::string& error) { return true; }  // most modules need nothing
};
```

A module is usually just a name and a table, written with `LUX_MODULE(name, {{"fn", "sig",
fn_ptr}, ...})`; only one that needs `configure()` writes the class. The signature is checked
once, in `call()`, which both dispatch sites (the VM for a synchronous call, the async driver
for an awaited one) go through — so a function body never re-checks its argument types, and
every type error reads the same way.

Reusing `NativeFn`'s calling convention is what lets the compiler's existing checking/emission
machinery stay almost untouched — a module function is dispatched exactly like `len()` or
`state.incr()` are, just resolved through a different table (§3.3).

### 3.2 The registry

`BuiltinModuleRegistry` (`builtin_module.hpp`/`.cpp`) mirrors `DbRegistry` in shape (`has(name)`
/ `available()` back the same "not compiled into this binary; available: ..." error in
`project.cpp`; `activate()` just runs `configure()` once, with the `app:` block if there is one
— unlike a DB module, a native module's `app:` block is **optional**, since most need no
configuration at all), but differs in one deliberate way: **it does not list its modules by
hand.**

A module registers itself: its `.cpp` ends with `LUX_REGISTER_MODULE(ItsClassName)`, a macro
(`builtin_module.hpp`) that defines a namespace-scope object whose constructor — run before
`main()`, like every namespace-scope object's — pushes a factory closure into a Meyer's-
singleton list (`detail::module_factories()`). `BuiltinModuleRegistry`'s constructor just walks
that list, calls each factory, and reads the resulting module's own `name()` to key it in
`slots_` — so there is no second string (an old `slots_["hash"] = ...` literal) that could drift
from what the class itself reports. §5 covers what this means in practice: adding a
dependency-free module needs zero edits to this file, or to `CMakeLists.txt`.

The one thing this trades away is automatic for free: `lux_script` is a plain `STATIC` library
(an `ar` archive), and archives link member-by-member — the linker keeps an object file only if
something already-included already references one of its symbols. A module whose only
externally-visible effect is `LUX_REGISTER_MODULE`'s namespace-scope object (true of every
module, by design: the class itself lives in an anonymous namespace) is exactly the kind of
"nothing references it" member a plain link would silently drop, registration and all.
`CMakeLists.txt`'s `lux_link_script()` function is the fix: it wraps `lux_script` in
`-Wl,--whole-archive`/`-Wl,--no-whole-archive` wherever it is linked, which keeps every member
of *that* archive regardless of whether anything already references it. Every target that links
`lux_script` uses it, with one documented exception (`test_placeholders` — see the comment next
to it in `CMakeLists.txt`, it `#include`s a driver `.cpp` directly and would collide with its own
copy under `--whole-archive`).

It also builds a flat table, once, concatenating every registered module's `functions()` in
registration order — `id_of("hash", "sha256")` resolves a `(module, function)` pair to a single
stable integer, the module-call counterpart of `native_id()`.

### 3.3 Compile time: one new branch, one new `IrCallShape`

`Emitter::check_call()` (`emitter.cpp`) already had a well-established shape for "member call
on a known object" (`sse.send(...)`, `state.incr(...)`, `sqlite.query(...)`) — the code that
decides which of the 9 call shapes (`ir.hpp`, `IrCallShape`) an expression is. A native module
call gets its own, checked *before* the reserved-object branch (`is_reserved_object()`) so the
two never overlap:

1. Is `obj` a name `BuiltinModuleRegistry::instance().has()` recognizes? If not, fall through —
   this might be a reserved object or a DB module instead.
2. Was it `import`ed (`imports_->count(obj)`)? If not: `"missing 'import hash' in order to use
   'hash.sha256'"` — the same message shape a missing DB import already gives.
3. Does the function exist (`BuiltinModuleRegistry::find()`)? Arity checked against its
   `min_args`/`max_args` — the same discipline `ReservedMemberCall` already applies, **not** a
   deeper per-argument type check. That matches the existing convention across the whole
   checker: a plain `fn` call today only checks argument *count*, not each argument's type
   against the callee's declared parameters (`UserFunctionCall`) — native module calls are held
   to the same bar, not a stricter one invented just for them.
4. Was it `await`ed? That is now an *error* ("is not asynchronous"), the mirror image of a DB
   call's check — module calls are never async (§2).
5. Build an `IrCallShape::NativeModuleCall` node. `call_index` is the flat id from §3.2 —
   resolved once, at compile time, so nothing has to look a function up by name at runtime.
   `type` is set to `Type::json()` (§3.4).

Bytecode emission (`Emitter::emit_call()`) mirrors `CallNative`'s case almost exactly — push
the arguments, emit one opcode with `(id << 8) | argc` — except the id space is
`BuiltinModuleRegistry`'s flat table, not `kNatives`, so it needs its own opcode,
`Op::CallBuiltinModule` (`bytecode.hpp`). The VM handler (`vm.cpp`) is a copy of `CallNative`'s
with one line changed: `builtin_module_function_at(id)` instead of `native_at(id)`.

### 3.4 Why every module call has type `Json`, and why that is not a shortcut

> Superseded: module functions now declare a return type (the `>` of their signature) and
> `--native` compiles their calls (§6). What follows is how the first cut stayed correct.

`hash.sha256()` always returns a `string`, in reality — but its `IrExpr::type` is set to
`Type::json()`, not `Type::primitive(Kind::String)`. This is deliberate, not a shortcut taken
for lack of time: `Comprobador::tipo_provable()` (`native_gen.cpp`, the analysis `--native`
runs to decide what it can safely turn into C++) has no case for `NativeModuleCall` — it is not
in the `if (e.call_shape == X)` chain that function is, so it simply never matches, and
`tipo_provable()` returns `std::nullopt` for it, the same conservative "cannot prove it" result
every construct gets before native support for it is written. `Type::json()` is what the
*bytecode* side (the checker, arity checking, `es_valor_json()` for "can this go inside a
`return {...}`") needs to accept a module call inside a JSON-returning route without claiming a
concrete C++ representation `--native` cannot back up.

The practical result, confirmed against the real binary before writing this down: a route using
`hash.sha256(...)` compiles and runs correctly today, and `--native --check` reports it served
`-> bytecode`, cleanly, with no partial or broken code generation:

```
$ lux app.lux --native --check
lux: --native: 0 function(s), 0 route(s) compiled to native code
lux:   GET /hash/:s -> bytecode
```

Giving a module native support later is additive, exactly like every `--native` phase has
been so far: teach `tipo_provable()` a case for `NativeModuleCall` (probably
keyed by `call_index`, the way `db_query_id()`/`db_exec_id()` already are for DB calls) and
`Generador::expr()` how to emit a direct C++ call instead of going through `Value`. Nothing
about the mechanism in this document needs to change for that to happen — it is deferred
because proving the *mechanism* first, with a module too small to need it, was more important
than optimizing before there was anything to measure.

## 4. Three modules, three points proven

**`hash`** (`src/lux_script/modules/base_modules/hash.cpp`) is deliberately the *smallest* module that could
exercise the mechanism: zero external dependencies (three thin wrappers over `crypto.hpp`,
which already existed for session/JWT signing), stateless, no configuration. Adding it required
no change to `kNatives`, no change to `DbDriver`, and no change to anything `--native`-specific
beyond "gracefully do not support it yet."

```lux
import hash

get endpoint("/hash/:s", string s):
    return { "sha256": hash.sha256(s) }
```

```
$ curl localhost:8080/hash/hello
{"sha256":"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"}
```

Matches Python's `hashlib.sha256(b"hello").hexdigest()` byte for byte — checked directly, not
assumed.

**`csv`** (`src/lux_script/modules/base_modules/csv.cpp`) proved the harder case §5.2 originally left open:
*state*. It parses CSV text into an internal table and hands back an opaque `int` handle instead
of the data itself — `filter_eq`/`filter_gt`/`sort_by`/`select`/`slice` each take a handle and
return a *new* one, `rows`/`get`/`sum`/`mean`/`group_sum`/`to_csv` read one without consuming
it, `close` frees it. Twenty-two functions, still zero external dependencies, still no core
mechanism change — see §5.2 for the pattern that made that true, filled in once there was a
real module to draw it from rather than guess at it.

```lux
import csv

get endpoint("/report"):
    int h = csv.parse(request_body_or_file_contents)
    int in_madrid = csv.filter_eq(h, "city", "madrid")
    return { "avg_age": csv.mean(in_madrid, "age"), "by_city": csv.group_sum(h, "city", "age") }
```

Pandas-*shaped*, deliberately not pandas-*equivalent*: Lux Script has no function values, so
there is no `df[df.age > 18]` — `filter_gt(h, "age", 18)` is the honest equivalent a language
without closures can actually offer (§5.2 argues this is a fair trade, not a shortfall).

**`pdf`** (`src/lux_script/modules/base_modules/pdf.cpp`) proved the third case: a module with a real
external dependency (cairo's PDF surface — already liberally licensed and already installed
almost everywhere that does graphics work, so no new library had to be vetted). It surfaced a
genuine bug in the `--native` build path that neither `hash` nor `csv` could have (§5.2).

```lux
import pdf

get endpoint("/invoice/:id"):
    int doc = pdf.create(595, 842)
    pdf.set_font(doc, "Sans", true, false)
    pdf.text(doc, 50, 50, "Invoice #" + str(id), 20)
    return { "pdf_base64": pdf.to_base64(doc) }
```

Verified as an actual PDF, not just a response that looks like one: decoded straight from the
route's own base64 output and checked against real tooling —`file`(1) reports "PDF document,
version 1.7", `pdfinfo` reports the right page count and page size, `pdftotext` recovers the
exact text placed on each page, and `pdftoppm` rasterizes it to confirm the shapes and colors
land where they were drawn, not just that *something* got written.

**`http`** (`src/lux_script/modules/base_modules/http.cpp`) — outbound `GET`/`POST`/`PUT`/`PATCH`/`DELETE`,
built on libcurl — proved the fourth case: a dependency that could not be a narrow "if it's not
there, skip the module" story, because it forces a real, deliberate exception to a stated
project principle.

```lux
import http

get endpoint("/weather/:city", string city):
    Json r = http.get("https://api.example.com/weather?city=" + city)
    return r["body"]
```

`README.md`/`CMakeLists.txt` commit Lux to never linking TLS — but that principle is about not
being an *inbound* TLS terminator ("TLS belongs to the reverse proxy"); it says nothing about
outbound calls, because there is no reverse proxy sitting between Lux and a third-party HTTPS
API to delegate to. Without real TLS, `http.get()` could not reach almost any API written since
~2018. The resolution — discussed with, and chosen by, whoever owns that principle rather than
quietly overridden — is libcurl: it brings its own mature TLS backend, used here with its secure
defaults never made configurable to "off" from a route (`CURLOPT_SSL_VERIFYPEER`/
`CURLOPT_SSL_VERIFYHOST` always on). The original principle stays intact for the inbound/server
side, which is what it was actually protecting.

A response comes back as `{"status", "headers", "body"}` — `body` parsed as JSON when the
response looks like JSON (the same `Value::parse_json()` every incoming request body already
goes through, applied symmetrically), the raw text otherwise. A request body follows the same
rule in reverse: a `string` argument is sent exactly as given; anything else (a `Dict`, a
`List`) is JSON-serialized automatically with `Content-Type: application/json` set, unless the
caller's own headers already set one.

The one limitation that is not theoretical here, unlike for the first three modules: **every
native module call is synchronous** (§2) — a slow remote server blocks the calling event-loop
thread for the whole request, bounded by a fixed 15s timeout so a hung server cannot pin it
forever, but still a real cost under load that `hash`/`csv`/`pdf` never had to pay (all three
finish in microseconds regardless). A genuinely non-blocking `http.*` needs the same kind of
worker-pool-plus-`await` plumbing `DbDriver` already has, generalized to native modules — real,
substantial future work (§6), not something to bolt on quietly inside what was asked for as "an
http module."

*A verification note specific to this module:* it could not be built through the normal CMake
path in the environment this was developed in (no `libcurl4-openssl-dev`, no root to install
it) — the module still gracefully reports itself missing (confirmed: `import http` on such a
build gives the same "not compiled into this binary" error any other absent module gives, and
`tests/run_http.sh`'s ctest entry skips with `SKIP_RETURN_CODE 77`, verified by actually forcing
that path, not assumed). The module's actual HTTP behavior was still verified for real, not
skipped: real curl 8.5.0 headers (matching the system's installed runtime `.so`) were fetched
straight from curl's own source repository, `http.cpp` was compiled and linked against
them and the system's `libcurl.so.4` directly (bypassing only the missing `-dev` symlink, not
curl itself), and the resulting real binary was run against a local echo server
(`tests/http_echo_server.py`) — the exact compiled code this file describes, not a
reimplementation of it, exercising every verb, header round-tripping, automatic JSON body
serialization versus raw-string passthrough, status-code passthrough, and a genuine connection-
refused error. On a machine with `libcurl4-openssl-dev` actually installed, `ctest -R http` (or
`tests/run_http.sh`) is the same suite via the normal path — this workaround will not be needed
there.

## 5. How to add a module — a worked walkthrough

Say the next module is `qrcode` (`qrcode.generate(text) -> string`, no third-party dependency —
follow §5.1 as written; it is deliberately parallel to how `hash` was actually added). If it
instead needs an external library (like `pdf`'s cairo dependency), §5.1 covers where that
changes things, and §5.2 spells the difference out with a real example (`pdf`) instead of a
hypothetical one.

### 5.1 Steps

1. **Write the module.** A new file, `src/lux_script/modules/qrcode.cpp` (a *user's* module
   would instead go in `src/lux_script/modules/` itself, one level up — see
   `src/lux_script/modules/README.md`; this walkthrough is adding an official one, so it goes
   in `base_modules/`), following `hash.cpp`'s shape: free functions matching `NativeFn`'s
   signature (`Value fn_qrcode_generate(NativeCtx&, std::vector<Value>& args, std::string&
   error)`) and a `LUX_MODULE(qrcode, {...})` table with each function's signature as the
   file's last lines (see `src/lux_script/modules/README.md` for the signature letters). `NativeCtx&` can be ignored if the module needs no
   request/response/session access, the way `hash`'s and `csv`'s functions do — accept it, do
   not use it. If the module needs to carry state across calls (`csv`'s tables, `pdf`'s
   documents), see §5.2 for the pattern that answers that — it is not a core-mechanism change,
   just a convention inside the module's own file.
2. **That's it for the build.** No second step here on purpose: `CMakeLists.txt` globs
   `src/lux_script/modules/base_modules/*.cpp` (and `src/lux_script/modules/*.cpp` for a user's
   own) and compiles whatever it finds, and `LUX_REGISTER_MODULE` is what makes the resulting
   object file register itself with `BuiltinModuleRegistry` at startup with no
   `builtin_module.cpp` edit needed — see §3.2 for the mechanism behind both halves of that.
   Reconfigure (`cmake -S . -B build`) if the new file does not seem to get picked up — a plain
   incremental `cmake --build` does not always notice a brand new source on its own, but the
   glob is `CONFIGURE_DEPENDS`, so the next full configure always will.
3. **Write the corpus test.** `tests/cases/modules.lux` (or a new file with its own `run_*.sh`,
   if the module carries an optional external dependency the way `pdf`'s does — see §7) plus a
   `check` block in `tests/run_tests.sh`.
4. **Rebuild and check the error paths, not just the happy path**, before trusting it:
   - `import qrcode` missing → `"missing 'import qrcode' in order to use 'qrcode.generate'"`.
   - Wrong arity → `"'qrcode.generate()' takes at most N argument(s)"`.
   - `await qrcode.generate(...)` → `"is not asynchronous"`.
   - `lux app.lux --native --check` → reports the route `-> bytecode`, not a compile error
     and not a crash. This is the one step it is easy to skip — and, if the module carries a
     third-party dependency, the one step that actually caught a real bug: see §5.2.
5. **A module that needs a third-party library** — `pdf`'s cairo is the real, worked example,
   not a hypothetical one — still needs a few lines by hand in `CMakeLists.txt`, because CMake
   cannot know a `pkg-config`/`find_library` check is needed just from a `.cpp` file existing:
   add a cmake `option()` the way `LUX_SQLITE`/`LUX_PDF` do, locate the library
   (`pkg_check_modules`/`find_path`+`find_library`), add the module's path to
   `LUX_MODULE_EXCLUDE`'s built-in list right above the module glob (so the *unconditional*
   automatic build does not try to compile it — mirroring `pdf.cpp`/`http.cpp` there) and
   `target_sources(lux_script PRIVATE ...)` it back in only inside the `if()` branch where the
   dependency was actually found. `LUX_REGISTER_MODULE` inside the file itself needs no
   `#ifdef` — it only ever runs if the file was compiled at all, which is exactly what that
   `if()` branch now controls. Read §5.2 before calling it done regardless, because linking the
   library into `lux_script` is not the only place it is needed.

### 5.2 What was actually harder: state, and a third-party dependency

**State — answered by `csv` and `pdf`, and simpler than expected.** `hash` is stateless: every
call is independent, nothing survives between requests. `csv` needs a table to survive from
`parse()` to `rows()`; `pdf` needs a document to survive from `create()` through several
`text()`/`rect()` calls to `save()`. Neither needed a change to Lux Script's type system (no
new `Type::Kind` per module — §3.4 already commits every module call to `Json`, and a per-module
native type would mean touching the core type system for every module, defeating the point of a
*general* mechanism). The pattern that worked, in both: an **opaque handle** — a plain `int` the
module hands back from its "create" function and expects as the first argument to every later
call — with the real C++ object kept in a mutex-protected table *inside that module's own file*
(`csv.cpp`'s `HandleTable`, `std::unordered_map<int, CsvTable>`; `pdf.cpp`'s,
`std::unordered_map<int, std::unique_ptr<PdfDoc>>`). The mutex matters and is not optional: a
module's state outlives any single request and every event-loop thread can reach it (GUIDE.md
§22, "N threads: event loop + its own VM") — `SharedState` (`state.*`, `natives.hpp`) already
faces the identical requirement and is the precedent this follows. No change to `BuiltinModule`,
`BuiltinModuleRegistry`, or anything compiler-side was needed for either module — this really is
just a convention an individual module's `.cpp` can adopt on its own, not a mechanism to build.

What is still genuinely unsolved: **handle lifetime.** Lux Script values carry no destructor a
module could hook into — nothing runs automatically when a handle's last reference in the script
goes out of scope. `csv` and `pdf` both require an *explicit* `close(handle)`; forgetting it
leaks the C++ object for the life of the process, exactly like forgetting to close a file handle
in a language without RAII. `pdf.save()`/`pdf.to_base64()` do NOT auto-close, on purpose — either
one might legitimately be called, then the other, on the same document. Neither module attempts
request-scoped auto-cleanup (freeing every handle a request opened when that request ends) —
plausible, not yet built, flagged here rather than assumed to be fine.

**A third-party dependency — the thing `hash` and `csv` could not have caught, because neither
carries one.** `liblux_script.a` is linked as a whole archive into every `--native`-generated
`.so` (`native_build.cpp`) — not just into routes that use a particular module. The moment
`pdf.cpp`'s object file (referencing cairo) became part of that archive, loading *any*
`--native`-compiled `.so` — including one for a route with nothing to do with `pdf` — started
failing with `undefined symbol: cairo_pdf_surface_create_for_stream`. Caught immediately by the
existing `native_route_shadow` test suite, not discovered later: adding `LUX_PDF` regressed a
test that has nothing to do with PDFs, which is exactly what a good test suite is for. The fix
is one more compile-time string, threaded through the same way `LUX_NATIVE_SCRIPT_LIB` already
is: `CMakeLists.txt` bakes `LUX_NATIVE_CAIRO_LIBS` (cairo's own link flags) into `lux_script`
whenever `LUX_PDF` is enabled, and `native_build.cpp` appends it to every `.so` build,
`#ifdef`-guarded, right after `LUX_NATIVE_SCRIPT_LIB`. **Any future module with an external
dependency needs this same step** — it is not `pdf`-specific, and skipping it does not fail
loudly at build time, only later, when `--native` tries to load a `.so` that happens to pull in
the archive's now-unresolved symbols.

## 6. What this deliberately does not solve

- **No dynamic loading.** Every module is compiled into `lux` at build time. A `pip install`-
  style story (a `.so` dropped in without rebuilding the compiler) is option (2) from §1 — a
  real, much larger project (stable ABI, a way to describe a module's types before it loads,
  versioning) that this document's design does not block, but also does not attempt.
- **No per-argument static type checking.** Arity only, matching the existing convention for
  every other call shape that is not a `BuiltinMethodCall` on a value of statically-known type
  (§3.3) — not a gap unique to native modules.
- ~~No async modules.~~ **Fixed.** `BuiltinModuleFn::is_async` (builtin_module.hpp) is the
  opt-in: `os.run()`/`read_file()`/`write_file()` and every `http.*` are marked, which makes
  `await <module>.<fn>(...)` mandatory for them (checked in `Emitter::check_call`, mirroring
  `DbModuleCall`) and routes the call through `Op::CallAsyncModule` instead of
  `Op::CallBuiltinModule` — the driver (`run_builtin_module_async`, project.cpp) runs the actual
  call on `lux::blocking_pool()`, the same shared pool a synchronous no-`await` route already
  uses, and resumes the handler on its own event loop thread when it finishes. A slow/hung
  remote server or child process still ties up a pool worker for the duration (bounded by each
  module's own timeout), but no longer the event-loop thread serving every OTHER connection on
  that core. A failure is raised at the `await` (`VM::resume_error`), exactly like a
  synchronous function's — one error model, caught by `try` like any runtime error; the database
  modules raise the same way. With `--native`, a module call compiles too: the generated code
  calls the same `BuiltinModuleFn::call()` with the request's `NativeCtx` (a route's own; for a
  function, the one its caller set, `current_native_ctx()`), an awaited one on the I/O pool,
  and converts the result to the type the signature declares.
- **Handles are released by idle time, not per request.** `csv`/`pdf`/`proc` handles are
  random and dropped after 10 minutes (an hour for `proc`) unused — a leftover handle does not
  live for good, but it is not freed the moment its request ends either.

## 7. How this is validated

`hash` and `csv` are dependency-free, so they live in the always-runs suite:
`tests/cases/modules.lux` + the `"== native modules =="` block in `tests/run_tests.sh` — real
HTTP requests against a real running `lux` binary, `hash` checked byte-for-byte against
Python's `hashlib`/`hmac`, `csv` checked through `read()`/`write()`, List methods and RFC 4180
quoted-field parsing, plus (in `"== compile errors =="`) the missing-
`import` case. All of it is part of the `regression` ctest suite.

`pdf` carries an optional dependency (cairo, `LUX_PDF`), so — like `sqlite`/`postgres`/`mysql`
— it gets its own suite that skips (`SKIP_RETURN_CODE 77`) rather than fails on a binary built
without it: `tests/cases/pdf.lux` + `tests/run_pdf.sh`, its own `pdf` ctest entry. It checks the
happy path, the three error paths (unknown handle, drawing after the document is finished,
double `close()`), and — the one that actually matters — decodes the route's real base64 output
and confirms the bytes start with `%PDF-`, not just that *a* response came back.

`http` follows the same optional-dependency pattern (`LUX_HTTP`, libcurl): its own
`tests/cases/http.lux` + `tests/run_http.sh` + ctest entry, calling a local echo server
(`tests/http_echo_server.py`) instead of the real internet so the suite is deterministic and
network-flake-free while still exercising a real TCP connection and a real HTTP/1.1 round trip.
Covers every verb, header round-tripping, the JSON-vs-raw-string body distinction, status-code
passthrough, and both real error paths (a malformed URL, a refused connection). Both the skip
path and the happy path were verified by actually forcing them, not assumed — see §4's
verification note on `http` for how the happy path was confirmed on a machine that could not
run the normal build (no `libcurl4-openssl-dev`, no root to install it).

`ctest` after building runs everything above along with the rest of the suite.
