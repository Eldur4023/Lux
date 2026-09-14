# Native modules — extending Lumen Script beyond the language core

> How `import <name>` grows beyond the database drivers (`sqlite`/`postgres`/`mysql`) into a
> general mechanism for adding capability to Lumen Script — `hash`, `csv` and `pdf` today, more
> tomorrow — and a step-by-step guide for building one. Companion to
> [COMPILACION-NATIVA.md](COMPILACION-NATIVA.md) (native *compilation*, a different thing) and
> [GUIDE.md](GUIDE.md) (using an application once it is built).

---

## 1. What this is, and what it deliberately is not

Python has `hashlib`, `csv`, `smtplib` — a large standard library, installable extensions on
top of it (`pip install`), and a stable C-extension ABI that lets either ship as compiled code.
Lumen has none of that yet. This document is the first step: a way to add a capability to
Lumen Script that is more than a language keyword and less than a full package ecosystem.

Three shapes were on the table:

1. **Generalize the existing compiled-in pattern.** A module broader than `DbDriver`'s
   query/exec shape, but still compiled *into* the `lumen` binary at CMake time, exactly like
   `sqlite`/`postgres`/`mysql` are today. Reuses the type-checked, `--native`-compatible
   infrastructure that already exists; using a new module still means rebuilding `lumen`.
2. **True dynamic loading**, `.so` files discovered at runtime with no rebuild — the actually
   Python-like answer. Needs a stable ABI between modules and the compiler, a way to describe a
   module's types *before* it is loaded, versioning, probably a package manager. Bigger than
   `--native` was.
3. **A couple of hardcoded builtins**, added straight to the compiler the way `crypto`/`auth`
   already are — no new mechanism, but nothing a Lumen *user* could do without patching the
   compiler either.

**This document builds (1).** It is the pragmatic middle: real extensibility, without taking on
a second, much larger project (dynamic loading) before the shape of a "module" is even proven.
Nothing here forecloses (2) later — see §6.

---

## 2. Why not just reuse `DbDriver`?

`DbDriver` (`db.hpp`) already proves that "an `import`ed, compiled-in capability" works —
`DbRegistry` gates it on `import`, `#ifdef LUMEN_SQLITE`-style options gate it on cmake. It was
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

So a native module (`BuiltinModule`, `include/lumen_script/builtin_module.hpp`) is a sibling of
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
};

class BuiltinModule {
public:
    virtual const char*                         name() const = 0;
    virtual const std::vector<BuiltinModuleFn>&  functions() const = 0;
    virtual bool configure(const std::map<std::string, std::string>& options,
                           std::string& error) { return true; }  // most modules need nothing
};
```

Reusing `NativeFn`'s calling convention is what lets the compiler's existing checking/emission
machinery stay almost untouched — a module function is dispatched exactly like `len()` or
`state.incr()` are, just resolved through a different table (§3.3).

### 3.2 The registry

`BuiltinModuleRegistry` (`builtin_module.hpp`/`.cpp`) mirrors `DbRegistry` deliberately:

- Its constructor lists every compiled-in module with a plain factory call
  (`make_hash_module()`), `#ifdef`-gated for any module that carries an external dependency —
  exactly `DbRegistry::DbRegistry()`'s pattern. `hash` carries none, so it is unconditional; a
  future module with one would add a cmake `option()` the same way `LUMEN_SQLITE` etc. do
  (`CMakeLists.txt`).
- `has(name)` / `available()` — same shape as `DbRegistry`, used for the same "not compiled
  into this binary; available: ..." error (`project.cpp`).
- No `DbPool`, no per-connection anything: `activate()` just runs `configure()` once (with the
  `app:` block if there is one, an empty map if there is none — unlike a DB module, a native
  module's `app:` block is **optional**, since most need no configuration at all).

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
$ lumen app.lum --native --check
lumen: --native: 0 function(s), 0 route(s) compiled to native code
lumen:   GET /hash/:s -> bytecode
```

Giving a module native support later is additive, exactly like every `--native` phase in
COMPILACION-NATIVA.md has been: teach `tipo_provable()` a case for `NativeModuleCall` (probably
keyed by `call_index`, the way `db_query_id()`/`db_exec_id()` already are for DB calls) and
`Generador::expr()` how to emit a direct C++ call instead of going through `Value`. Nothing
about the mechanism in this document needs to change for that to happen — it is deferred
because proving the *mechanism* first, with a module too small to need it, was more important
than optimizing before there was anything to measure.

## 4. Three modules, three points proven

**`hash`** (`src/lumen_script/module_hash.cpp`) is deliberately the *smallest* module that could
exercise the mechanism: zero external dependencies (three thin wrappers over `crypto.hpp`,
which already existed for session/JWT signing), stateless, no configuration. Adding it required
no change to `kNatives`, no change to `DbDriver`, and no change to anything `--native`-specific
beyond "gracefully do not support it yet."

```lum
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

**`csv`** (`src/lumen_script/module_csv.cpp`) proved the harder case §5.2 originally left open:
*state*. It parses CSV text into an internal table and hands back an opaque `int` handle instead
of the data itself — `filter_eq`/`filter_gt`/`sort_by`/`select`/`slice` each take a handle and
return a *new* one, `rows`/`get`/`sum`/`mean`/`group_sum`/`to_csv` read one without consuming
it, `close` frees it. Twenty-two functions, still zero external dependencies, still no core
mechanism change — see §5.2 for the pattern that made that true, filled in once there was a
real module to draw it from rather than guess at it.

```lum
import csv

get endpoint("/report"):
    int h = csv.parse(request_body_or_file_contents)
    int in_madrid = csv.filter_eq(h, "city", "madrid")
    return { "avg_age": csv.mean(in_madrid, "age"), "by_city": csv.group_sum(h, "city", "age") }
```

Pandas-*shaped*, deliberately not pandas-*equivalent*: Lumen Script has no function values, so
there is no `df[df.age > 18]` — `filter_gt(h, "age", 18)` is the honest equivalent a language
without closures can actually offer (§5.2 argues this is a fair trade, not a shortfall).

**`pdf`** (`src/lumen_script/module_pdf.cpp`) proved the third case: a module with a real
external dependency (cairo's PDF surface — already liberally licensed and already installed
almost everywhere that does graphics work, so no new library had to be vetted). It surfaced a
genuine bug in the `--native` build path that neither `hash` nor `csv` could have (§5.2).

```lum
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

## 5. How to add a module — a worked walkthrough

Say the next module is `qrcode` (`qrcode.generate(text) -> string`, no third-party dependency —
follow §5.1 as written; it is deliberately parallel to how `hash` was actually added). If it
instead needs an external library (like `pdf`'s cairo dependency), the only difference is §5.1
step 1 and the cmake option in step 6 — everything else is identical, and §5.2 spells that
difference out, now with a real example (`pdf`) instead of a hypothetical one.

### 5.1 Steps

1. **Write the module.** A new file, `src/lumen_script/module_qrcode.cpp`, following
   `module_hash.cpp`'s shape: free functions matching `NativeFn`'s signature
   (`Value fn_qrcode_generate(NativeCtx&, std::vector<Value>& args, std::string& error)`), a
   class implementing `BuiltinModule`, a factory function (`make_qrcode_module()`) the registry
   calls. `NativeCtx&` can be ignored if the module needs no request/response/session access,
   the way `hash`'s and `csv`'s functions do — accept it, do not use it. If the module needs to
   carry state across calls (`csv`'s tables, `pdf`'s documents), see §5.2 for the pattern that
   answers that — it is not a core-mechanism change, just a convention inside the module's own
   file.
2. **Register it.** `builtin_module.cpp`'s `BuiltinModuleRegistry::BuiltinModuleRegistry()`:
   declare the factory (`std::unique_ptr<BuiltinModule> make_qrcode_module();`) and add
   `{ Slot s; s.module = make_qrcode_module(); slots_["qrcode"] = std::move(s); }` — one line,
   same pattern as `hash`'s and `csv`'s.
3. **Add it to the build.** `CMakeLists.txt`, `lumen_script`'s `add_library` sources:
   `src/lumen_script/module_qrcode.cpp`.
4. **Write the corpus test.** `tests/cases/modules.lum` (or a new file with its own `run_*.sh`,
   if the module carries an optional external dependency the way `pdf`'s does — see §7) plus a
   `check` block in `tests/run_tests.sh`.
5. **Rebuild and check the error paths, not just the happy path**, before trusting it:
   - `import qrcode` missing → `"missing 'import qrcode' in order to use 'qrcode.generate'"`.
   - Wrong arity → `"'qrcode.generate()' takes at most N argument(s)"`.
   - `await qrcode.generate(...)` → `"is not asynchronous"`.
   - `lumen app.lum --native --check` → reports the route `-> bytecode`, not a compile error
     and not a crash. This is the one step it is easy to skip — and, if the module carries a
     third-party dependency, the one step that actually caught a real bug: see §5.2.
6. **That's it for a dependency-free module.** For one that needs a third-party library —
   `pdf`'s cairo is the real, worked example, not a hypothetical one — add a cmake `option()`
   the way `LUMEN_SQLITE`/`LUMEN_PDF` do (`CMakeLists.txt`), locate the library
   (`pkg_check_modules`/`find_path`+`find_library`), `#ifdef` the factory declaration and the
   registration line in `builtin_module.cpp` on that option (mirroring exactly how `db.cpp`'s
   `DbRegistry` constructor gates `make_postgres_driver()`) — and then read §5.2 before calling
   it done, because linking the library into `lumen_script` is not the only place it is needed.

### 5.2 What was actually harder: state, and a third-party dependency

**State — answered by `csv` and `pdf`, and simpler than expected.** `hash` is stateless: every
call is independent, nothing survives between requests. `csv` needs a table to survive from
`parse()` to `rows()`; `pdf` needs a document to survive from `create()` through several
`text()`/`rect()` calls to `save()`. Neither needed a change to Lumen Script's type system (no
new `Type::Kind` per module — §3.4 already commits every module call to `Json`, and a per-module
native type would mean touching the core type system for every module, defeating the point of a
*general* mechanism). The pattern that worked, in both: an **opaque handle** — a plain `int` the
module hands back from its "create" function and expects as the first argument to every later
call — with the real C++ object kept in a mutex-protected table *inside that module's own file*
(`module_csv.cpp`'s `HandleTable`, `std::unordered_map<int, CsvTable>`; `module_pdf.cpp`'s,
`std::unordered_map<int, std::unique_ptr<PdfDoc>>`). The mutex matters and is not optional: a
module's state outlives any single request and every event-loop thread can reach it (GUIDE.md
§22, "N threads: event loop + its own VM") — `SharedState` (`state.*`, `natives.hpp`) already
faces the identical requirement and is the precedent this follows. No change to `BuiltinModule`,
`BuiltinModuleRegistry`, or anything compiler-side was needed for either module — this really is
just a convention an individual module's `.cpp` can adopt on its own, not a mechanism to build.

What is still genuinely unsolved: **handle lifetime.** Lumen Script values carry no destructor a
module could hook into — nothing runs automatically when a handle's last reference in the script
goes out of scope. `csv` and `pdf` both require an *explicit* `close(handle)`; forgetting it
leaks the C++ object for the life of the process, exactly like forgetting to close a file handle
in a language without RAII. `pdf.save()`/`pdf.to_base64()` do NOT auto-close, on purpose — either
one might legitimately be called, then the other, on the same document. Neither module attempts
request-scoped auto-cleanup (freeing every handle a request opened when that request ends) —
plausible, not yet built, flagged here rather than assumed to be fine.

**A third-party dependency — the thing `hash` and `csv` could not have caught, because neither
carries one.** `liblumen_script.a` is linked as a whole archive into every `--native`-generated
`.so` (`native_build.cpp`) — not just into routes that use a particular module. The moment
`module_pdf.cpp`'s object file (referencing cairo) became part of that archive, loading *any*
`--native`-compiled `.so` — including one for a route with nothing to do with `pdf` — started
failing with `undefined symbol: cairo_pdf_surface_create_for_stream`. Caught immediately by the
existing `native_route_shadow` test suite, not discovered later: adding `LUMEN_PDF` regressed a
test that has nothing to do with PDFs, which is exactly what a good test suite is for. The fix
is one more compile-time string, threaded through the same way `LUMEN_NATIVE_SCRIPT_LIB` already
is: `CMakeLists.txt` bakes `LUMEN_NATIVE_CAIRO_LIBS` (cairo's own link flags) into `lumen_script`
whenever `LUMEN_PDF` is enabled, and `native_build.cpp` appends it to every `.so` build,
`#ifdef`-guarded, right after `LUMEN_NATIVE_SCRIPT_LIB`. **Any future module with an external
dependency needs this same step** — it is not `pdf`-specific, and skipping it does not fail
loudly at build time, only later, when `--native` tries to load a `.so` that happens to pull in
the archive's now-unresolved symbols.

## 6. What this deliberately does not solve

- **No dynamic loading.** Every module is compiled into `lumen` at build time. A `pip install`-
  style story (a `.so` dropped in without rebuilding the compiler) is option (2) from §1 — a
  real, much larger project (stable ABI, a way to describe a module's types before it loads,
  versioning) that this document's design does not block, but also does not attempt.
- **No per-argument static type checking.** Arity only, matching the existing convention for
  every other call shape that is not a `BuiltinMethodCall` on a value of statically-known type
  (§3.3) — not a gap unique to native modules.
- **No async modules.** Every function is synchronous by construction (§2). A module that
  genuinely needs to suspend (a network call, say) would need its own worker-pool story, closer
  to `DbDriver`'s — not designed here, and not needed by anything built so far.
- **No native (`--native`) codegen for any module function yet.** Deliberate and safe (§3.4),
  not an oversight — falls back to bytecode per route, cleanly, with the fallback verified
  against the real binary rather than assumed.
- **No request-scoped handle cleanup.** `csv`/`pdf` handles live until explicitly `close()`d or
  the process exits (§5.2) — nothing frees a request's leftover handles when it ends.

## 7. How this is validated

`hash` and `csv` are dependency-free, so they live in the always-runs suite:
`tests/cases/modules.lum` + the `"== native modules =="` block in `tests/run_tests.sh` — real
HTTP requests against a real running `lumen` binary, `hash` checked byte-for-byte against
Python's `hashlib`/`hmac`, `csv` checked against hand-computed filter/sum/mean/group_sum
results and RFC 4180 quoted-field parsing, plus (in `"== compile errors =="`) the missing-
`import` case. All of it is part of the `regression` ctest suite.

`pdf` carries an optional dependency (cairo, `LUMEN_PDF`), so — like `sqlite`/`postgres`/`mysql`
— it gets its own suite that skips (`SKIP_RETURN_CODE 77`) rather than fails on a binary built
without it: `tests/cases/pdf.lum` + `tests/run_pdf.sh`, its own `pdf` ctest entry. It checks the
happy path, the three error paths (unknown handle, drawing after the document is finished,
double `close()`), and — the one that actually matters — decodes the route's real base64 output
and confirms the bytes start with `%PDF-`, not just that *a* response came back.

`ctest` after building runs everything above along with the rest of the suite.
