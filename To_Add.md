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
