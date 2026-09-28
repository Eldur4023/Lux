# To add

Real compiler/language limitations found in passing while working on something
else — they don't block whatever was being done at the time (there's always a
reasonable workaround), but they're worth fixing later. Each entry says what
fails, why, the current workaround, and where to look to fix it properly.

---

## `type_of()` doesn't know the return type of a call to a user function — FIXED

**Found:** 2026-09-16, while fixing the fact that a standalone `fn` couldn't use
class constructors (see the commit "Standalone functions can now use classes").

**Fixed:** same day, commit "Resolve chained method calls on function/constructor
results". `FnSig` gained a `devuelve` field (the declared return type), populated by
`make_sig()`, and `type_of()`/`check_call` consult it in the `ExprKind::Call` case
(both for a standalone function and for a class method) instead of returning
`Type::unknown()`. `check_call`'s own resolution of a `.method()` receiver (previously
a hand-written check only for `Ident`/`this`) now also goes through `type_of()`, which
is what actually resolves the example below. Verified with 60 consecutive requests
against a clean process (nothing else listening on the port) — see the note at the
end about the false-positive non-determinism that showed up while verifying it.

**Note on a non-determinism that turned out to be false:** during verification, the
same repro returned 204 instead of `{"r":25}` on a fraction of requests, seemingly at
random, even under concurrency and with ThreadSanitizer staying quiet. It turned out
to be an artifact of the test environment, not a bug: `SO_REUSEPORT` lets more than
one process listen on the same port at once, and a VSCode extension process from
another repo (`.../Github/Lux/build/lux .`) happened to have been listening for a
while on the same port (8098) used for this test repro in `/tmp`. The kernel spread
new connections between the two processes (sticky per connection, hence it looked
"sometimes yes, sometimes no" but always consistent within the same connection) — the
unrelated process doesn't have that route and just returned 204. Confirmed by killing
both processes and repeating the test on a port verified to be free: 60/60 correct
requests. Lesson for next time something looks non-deterministic on a port shared
between tests: run `ss -tlnp | grep <port>` first, don't assume only your own process
is listening there.

**What fails:**

```lux
class Punto:
    int x
    int y

    fn int cuadrado():
        return this.x * this.x + this.y * this.y

fn Punto hacer_punto(int x, int y):
    return Punto(x, y)

get endpoint("/x"):
    return { "r": hacer_punto(3, 4).cuadrado() }
```

Compiles (`lux --check` doesn't complain), but fails at runtime:

```
{"error":"Dicts have no method 'cuadrado'"}
```

**Why:** `Emitter::type_of()` (`src/lux_script/emitter.cpp`) only knows the type of a
`Call` expression when it's a METHOD call on a receiver whose type is already known
(`case ExprKind::Call: { if (!e.object || e.object->kind != ExprKind::Member) return
Type::unknown(); ... }`). For a call to a standalone user function
(`hacer_punto(3, 4)`), there's no branch that consults `FunctionSigs`/the function's
declared return type — it falls straight through to `Type::unknown()`. Without
knowing the result is a `Punto`, the checker can't resolve `.cuadrado()` on it, and
at runtime the value (a plain `Value::Dict`) is dispatched through the generic
dynamic path, which only knows `Dict`'s methods (`has`/`keys`), hence the error message.

Confirmed to be **independent** of whether the call happens inside a route or inside
another `fn` — it fails the same way in both places, so it's unrelated to the
`build_function_signatures()`/`emit_function_bodies()` fix that was actually made.

**Current workaround (works perfectly):** assign the result to a local variable with
the declared type before chaining the method:

```lux
fn Punto hacer_punto(int x, int y):
    return Punto(x, y)

get endpoint("/x"):
    Punto p = hacer_punto(3, 4)
    return { "r": p.cuadrado() }
```

This works because `type_of(Ident)` does return the variable's declared type
(`local_type(e.text)`), unlike `type_of(Call)`.

**Where to look to fix it:** `Emitter::type_of()`, `ExprKind::Call` case
(`src/lux_script/emitter.cpp`, near line 538 in the commit where this was found).
It would need a new branch: if `e.object->kind == ExprKind::Ident` and that name
resolves in `functions_` (the `FunctionSigs` the `Emitter` already receives), return
that function's declared return type — analogous to how a method's return is already
resolved (`m.devuelve ? Type::from_legacy_name(m.devuelve) : recv`), but looking at
`FnSig`/whatever holds the return type of a standalone function instead of a method.
Also check whether `check_call`/`emit_call` have the same kind of gap for other
chained forms (`fn_returning_list()[0].field`, etc.) once this is touched, not just
the method case.

---

## `:param.ext` in a route pattern doesn't bind the parameter — and `{param}.ext` "compiles" but binds the wrong value

**Found:** 2026-09-17, reported from Homeflix while trying `get endpoint("/hls/:n.ts", int n)`
to serve HLS segments with the extension embedded in the URL.

**What fails — two layers, not just one:**

1. `:n.ts` gives a confusing compile error:
   ```
   error: the pattern declares ':n.ts' but no parameter binds it
   ```
   This comes from `pattern_params()` (`src/lux_script/project.cpp`), which for the
   `:name` syntax looks for the next `/` as the closing delimiter (`char close = (pattern[i] == '{') ? '}' : '/';`)
   — for `:n.ts`, the "name" it extracts is literally `n.ts` (everything up to the slash
   or the end), not `n`. The checker then requires a parameter named `n.ts`, which no
   one ever declares.

2. Switching to `{n}.ts` (delimiter `}`, different in `pattern_params()`) does make the
   checker correctly extract `n` and compile clean — but **the value that arrives at
   runtime is wrong**, not an error: `GET /segment/42.ts` against `get
   endpoint("/segment/{n}.ts", int n)` returns `{"n":0}`, not `{"n":42}`. This is a
   **different and deeper** bug, in the C++ router (`src/router.cpp`), not in the Lux
   Script checker:
   - `Router::normalize_pattern()` converts `{n}.ts` to `:n.ts` BEFORE registering the
     route (replaces `{` with `:` and drops the `}`, leaving everything else as-is) — so
     at runtime `{n}.ts` and `:n.ts` are exactly the same internal pattern.
   - `Router::add_internal()` (line `name = seg.substr(1);`) treats the whole segment
     `n.ts` (everything after `:`) as the parameter's NAME — it binds
     `params["n.ts"] = "42.ts"` (the RAW value of the full segment), never `params["n"]`.
   - Lux Script's binding looks up `req.params["n"]` (because the checker, via the
     `{n}` syntax, believes the parameter is named `n`) — doesn't find it, and `int n`
     falls back to its default value (`0`), with no error or warning.

   Conclusion: **the router doesn't support a parameter combined with literal text in
   the same segment at all** (`:id.json`, `file-:id`, `{id}.ts`, whatever) — it's not
   just a checker gap, it's a real limitation in segment matching. A pattern segment
   can only be entirely static, entirely a parameter, or `*`.

**Current workaround (works perfectly):** use a clean route segment for the
parameter, without embedding the extension (`/segment/:n` instead of `/segment/:n.ts`)
— HLS doesn't require the URL to literally end in `.ts`, the response's `Content-Type`
is enough.

**Where to look to fix it:**
- `pattern_params()` (`src/lux_script/project.cpp`, near line 373): it would need to
  know how to parse a MIXED segment (`:name` followed by literal text before the `/`),
  not just `:name-up-to-the-slash` or `{name}`.
- `Router::normalize_pattern()`/`Router::add_internal()`/`Router::match_recursive()`
  (`src/router.cpp`): the `PARAM`-type `Node` would need to be able to carry a
  literal suffix/prefix, and `match_recursive()` would need to check that suffix
  against the actual segment BEFORE accepting the match and extracting only the
  variable part as the value — today it assumes a `PARAM` segment consumes the whole
  segment, no more.
- The two files need to stay consistent with each other (the name the Lux Script
  checker believes the parameter has must be EXACTLY the key the real router uses
  when binding `params[...]`) — the `{n}.ts` bug above is precisely that inconsistency.

---

## `/*` doesn't cover the empty root route `/`

**Found:** 2026-09-17, reported from Homeflix while setting up an SPA fallback with
`any endpoint("/*")`.

**What happens:** `get endpoint("/*")` (or `any`) matches `/something`, `/a/b/c`, etc.,
but NOT `/` on its own — at least one character after the slash is required. It's not
confirmed whether this is intentional (a wildcard normally implies "one or more
segments," not "zero or more"), but it's surprising if it's not documented anywhere,
so it needs to be clearly written down even if the behavior is kept as-is.

**Current workaround (works perfectly):** also declare an explicit `get endpoint("/")`
alongside the `/*` wildcard.

**Where to look:** `Router::match_recursive()`/`split_path()` (`src/router.cpp`) —
`/` produces an EMPTY list of segments (`split_path` discards empty segments), so the
`WILDCARD` node never gets tried for it (the `for (const auto& seg : segments)` loop
in `match_recursive` doesn't iterate at all, and the terminal case at
`index == segments.size()` only looks at `node->handlers`, not the root node's
wildcard children). If support is added, it would go in that terminal case: also
check `node->find_wildcard_child()` when `index == segments.size()` before giving up.
If it's decided NOT to support it (reasonable: it's consistent with a wildcard always
capturing "the rest of the path," never "nothing"), a note in GUIDE.md next to the
routes section is enough.

---

## A JSON body is parsed to a generic tree before it is bound to its class — FIXED

**Found:** 2026-09-27, benchmarking `POST /orders` (a body bound to a class with a
`List<Item>` field and `validate:` rules) against Actix, Axum and Gin.

**What happens:** it works and answers the same bytes as bytecode, but it is still ~3%
behind Actix in req/s (445k vs 460k; Lux's p99 is better). `Value::parse_json()` builds a
generic tree first — a `Dict` per object, every key a `std::string` — and
`bind_body()`/`value_matches()` (`src/lux_script/project.cpp`) then walk it to check the
types and build the instance. The parse is now the biggest piece of that route (~7% of
CPU; the kernel's `write` is ~54%). serde, what Actix uses, parses straight into the
struct.

**Already done:** a Dict class's `validate:` rules run as C++ on `--native`
(`construir_clases()`/`generate_native_route()` in `src/lux_script/native_gen.cpp`,
`prepare_native_args(..., rules=false)`), not a VM per rule, and `value_matches()` moves
values out of the parsed tree instead of copying them (×0.91 → ×0.97 vs Actix).

**Fixed:** 2026-09-27. The body is parsed straight into the class's shape, with no
generic tree in between:

- `include/lux_script/json_bind.hpp` (new) + `src/lux_script/json_parse.cpp`: the same
  strict scanner, driven by the class's fields (`bind_json_class()`/`bind_json_flat()`).
  Each key is matched against the declared fields as it arrives; declared scalars are
  read straight into `Value`s; unknown keys are validated and skipped without building
  anything; nested classes and `List<Class>` recurse through the same path. The binder
  never words a message — it reports, per declared field, `Ok`/`Missing`/`BadType`, plus
  `InvalidJson` (400) and `NotAnObject` (422) apart, so every caller keeps its exact
  responses. Same grammar, same nesting cap, same verdicts as the tree walk: `5.0` in an
  int field is `expected int`, `float` fields normalize to `Value::real`, duplicate keys
  are last-wins, trailing garbage is 400.
- `bind_body()` (project.cpp) binds through a `ClassShapeTable` (ClassInfo → shapes,
  interned per request — never cached, so a hot reload can't hand a recycled ClassInfo's
  shape to a different class). `value_matches()` is gone; the message walk and the
  validate:-rules path are untouched. This also covers, via `prepare_native_args()`,
  every `--native` route whose body class is `dinamica` (List/class fields), which is
  exactly the case `codigo_bind_cuerpo()` never handled.
- `codigo_bind_cuerpo()` (native_gen.cpp) emits a static flat `JsonFieldSpec` array and
  one `bind_json_flat()` call instead of `Value::parse_json()` + per-field `find()`s;
  the 400/422 blocks, the messages and the `L<Clase>` construction are unchanged.

`tests/run_tests.sh` and the `native_route_shadow` suite already checked those messages:
335/335 and ctest 16/16, byte for byte. Two bugs caught on the way, both by the suites:
the shape interning claimed its index before recursing into nested classes (a nested
class took the claimed slot, and every key of the outer class went unmatched), and the
nested-object path only judged the keys the body DID carry, so a nested element missing
a required field slipped through as a valid instance instead of `expected List`.

**Measured** (quiet machine, pristine dev binary vs this one, back to back; k6
closed-loop 50 VUs, 45s): the win is real but small — binding was never the big share
of these routes' latency (the kernel's `write` is). Pure-bind routes (no DB):
+1.8% req/s bytecode, p90 −1.1..−1.7%, p99 better on every route; p50 −0.3..−0.7%.
Writes with SQLite underneath: bytecode p50 −0.6..−0.7%, p90 −1.3..−1.6% consistently
across all four routes; `--native` flat classes are a wash (±1%, within noise — a
3-int body was already cheap to parse). The win grows with payload: unknown keys are
skipped without building anything and `List<Class>` elements never materialize a
per-element Dict-with-string-keys tree, so the improvement shows up as bodies get
bigger or noisier, not on `{a:1}`.

---

## The JSON binder still pays ~6 allocations per request that it doesn't need

**Found:** 2026-09-27, reviewing the fresh binder itself (review only, nothing
changed). The tree is gone, but the scaffolding around the parse still allocates
from plain `new` — while the OLD path's tree allocations mostly went through the
thread-local recycler (`detail::SmallAlloc`, `Value::new_box()`), which is why the
`--native` p50 barely moved. Ordered by what each one is worth:

1. **`ClassShapeTable` (project.cpp) is built per request, and it is pure
   scaffolding.** `intern()` pays an `unordered_map` (bucket array + node: 2
   allocs), the outer `shapes_` vector and the inner `fields` vector (2 more),
   plus 2 virtual dispatches per body key (`JsonShapeTable::field_count/field`).
   None of this is necessary at request time: the shapes are a pure function of
   `ClassInfo`, which is immutable after `build_classes()`. Build them THERE
   instead — one `JsonShape { const JsonFieldSpec* fields; size_t n; }` hanging
   off each `ClassInfo`, with `JsonFieldSpec::nested` becoming a direct
   `const JsonShape*` (stable: it lives inside the `ClassInfo` that
   `nested_class`'s shared_ptr keeps alive through the call). `bind_body` then
   passes `ci.json_shape`, the `JsonShapeTable` interface, the map and both
   vectors disappear, and hot reload stays safe for free (a new ClassTable
   builds new shapes; nothing is cached across builds). Do NOT cache shapes on
   the side keyed by `ClassInfo*`: a recycled address after a reload would bind
   a body against another class's fields.

2. **`JsonBound`'s two vectors allocate on every bound body** (`bind_document`'s
   `status.assign` + `values.assign`) — both binaries, every route with a class
   parameter. `Value` is 16 bytes and `JsonBindStatus` is one: an inline buffer
   for `n <= 16` (heap fallback above) removes both; the `--native` generated
   code can go further and emit fixed stack arrays.

3. **`class_object` allocates `std::vector<char> seen` per NESTED object** —
   an 8-element `List<Item>` is 8 vector constructions per request. A `uint64_t`
   bitmask covers classes with ≤64 fields (they are all far below that); vector
   fallback only past it.

4. **`typed_value`'s `Value::List result` grows unreserved** — 2→4→8 reallocs
   for the typical 3/8-element lists. `reserve(4)` erases them.

5. **Micros, free of risk:** `bind_document`'s instance loop uses
   `Dict::set()`, which runs a `find` per insertion over keys that are unique
   by construction there (each declared field exactly once) — `append()` skips
   the scan; in `class_object` the bitmask from (3) tells first-sight from
   duplicate, so `append`-then-assign works too. And the fresh
   `std::string s` per string value in `typed_value` can be one scratch member
   of `Parser` (`clear()` keeps capacity) — the tree path spent the same, so
   this is a gain, not a regression fix.

1+3+4 are the same low-risk pattern; 2 changes a public-ish signature
(json_bind.hpp) and the emitted native code, so measure after it separately.
None of the five touches an observable verdict: `Ok`/`Missing`/`BadType`,
400/422 and the messages stay byte for byte, and the suites that caught the two
binder bugs above are exactly the ones that hold the line here.

**Reviewed and found already fine** (so nobody re-audits blindly):
`to_json_text()` reserves 256 up front; `Value` boxes and `Dict` pairs go
through the thread-local recycler; `Dict::find/set` are linear only below 16
keys and instances are that small; the scratch key string in `bind_document`
already keeps its capacity across keys; `thread_local VM rule_vm`,
`prepare_args`'s lazy `form_data` and `last_validation_messages()` were already
optimized before this.
