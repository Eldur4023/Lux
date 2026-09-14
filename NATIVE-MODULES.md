# Native modules — extending Lumen Script beyond the language core

> How `import <name>` grows beyond the database drivers (`sqlite`/`postgres`/`mysql`) into a
> general mechanism for adding capability to Lumen Script — a hash module today, a PDF module
> or anything else tomorrow — and a step-by-step guide for building one. Companion to
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

## 4. The proof: `hash`

`src/lumen_script/module_hash.cpp` is deliberately the *smallest* module that could exercise
every part of this mechanism: zero external dependencies (three thin wrappers over
`crypto.hpp`, which already existed for session/JWT signing), a fixed small function count, no
configuration. It exists to prove the mechanism works before anything bigger — a PDF library,
say — gets built on top of it. Do not mistake it for the interesting part of this document; the
interesting part is that adding it required no change to `kNatives`, no change to `DbDriver`,
and no change to anything `--native`-specific beyond "gracefully do not support it yet."

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

## 5. How to add a module — a worked walkthrough

Say the next module is `csv` (`csv.parse(text) -> List<Json>`, no third-party dependency —
follow §5.1 as written). If it instead needs an external library (a PDF writer, say), the only
difference is §5.1 step 1 and the cmake option in step 6 — everything else is identical, and
§5.2 spells that difference out.

### 5.1 Steps

1. **Write the module.** A new file, `src/lumen_script/module_csv.cpp`, following
   `module_hash.cpp`'s shape: free functions matching `NativeFn`'s signature
   (`Value fn_csv_parse(NativeCtx&, std::vector<Value>& args, std::string& error)`), a class
   implementing `BuiltinModule`, a factory function (`make_csv_module()`) the registry calls.
   `NativeCtx&` can be ignored if the module needs no request/response/session access, the way
   `hash`'s functions do — accept it, do not use it.
2. **Register it.** `builtin_module.cpp`'s `BuiltinModuleRegistry::BuiltinModuleRegistry()`:
   declare the factory (`std::unique_ptr<BuiltinModule> make_csv_module();`) and add
   `{ Slot s; s.module = make_csv_module(); slots_["csv"] = std::move(s); }` — one line, same
   pattern as `hash`'s.
3. **Add it to the build.** `CMakeLists.txt`, `lumen_script`'s `add_library` sources:
   `src/lumen_script/module_csv.cpp`.
4. **Write the corpus test.** `tests/cases/modules.lum` (or a new file, if the module is
   substantial enough to deserve its own) plus a `check` block in `tests/run_tests.sh` — see
   §7 for what already exists to extend.
5. **Rebuild and check the error paths, not just the happy path**, before trusting it:
   - `import csv` missing → `"missing 'import csv' in order to use 'csv.parse'"`.
   - Wrong arity → `"'csv.parse()' takes at most N argument(s)"`.
   - `await csv.parse(...)` → `"is not asynchronous"`.
   - `lumen app.lum --native --check` → reports the route `-> bytecode`, not a compile error
     and not a crash. This is the one step it is easy to skip and easy to get catastrophically
     wrong (§3.4) — verify it, do not assume it.
6. **That's it for a dependency-free module.** For one that needs a third-party library, add a
   cmake `option()` the way `LUMEN_SQLITE`/`LUMEN_POSTGRES` do (`CMakeLists.txt`, near
   `option(LUMEN_SQLITE ...)`), `find_path`/`find_library` it, `#ifdef` the factory declaration
   and the registration line in `builtin_module.cpp` on that option (mirroring exactly how
   `db.cpp`'s `DbRegistry` constructor gates `make_postgres_driver()`), and link the library
   only into that one `.cpp`'s compilation, not the whole `lumen_script` target.

### 5.2 What is genuinely harder for a module with real state or a third-party dependency

`hash` is stateless: every call is independent, nothing survives between requests. A module
wrapping a stateful library (a PDF *document* being built up over several calls: `create()`,
then `add_page()`, then `save()`) needs a way to carry that state across calls without changing
Lumen Script's type system — no new `Type::Kind` is added per module (§3.4 already commits to
`Json`-typed calls; adding a real native type per module would mean touching the core type
system for every module, defeating the point of a *general* mechanism). The intended pattern,
not yet built or proven: an **opaque handle** — a plain `int` a module hands back from
`create()` and expects as the first argument to every later call, with the actual C++ object
kept in the module's own internal table (`std::unordered_map<int, std::unique_ptr<PdfDoc>>`),
freed either explicitly (`.save()` doubling as the release point) or left for a future
request-scoped cleanup hook. This is a real, open design question — flagged here honestly
rather than solved speculatively, exactly the "no generar en base a una suposición sin
verificar" rule COMPILACION-NATIVA.md holds itself to (§14): the right shape for a stateful
module should come from building one, not from guessing what it will need in the abstract.

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

## 7. How this is validated

`tests/cases/modules.lum` + the `"== native modules =="` suite in `tests/run_tests.sh`: real
HTTP requests against a real running `lumen` binary, checked byte-for-byte against Python's
`hashlib`/`hmac` for `sha256`/`hmac_sha256`, and the exact hex length for `random_hex`. Plus,
in `"== compile errors =="`, the missing-`import` case. All of it is part of the `regression`
ctest suite — `ctest` after building runs it along with everything else.
