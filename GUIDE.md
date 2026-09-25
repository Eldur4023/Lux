# Lux — Developer guide

> A practical reference for Lux Script, the language Lux runs. The formal grammar is in
> [LUX_SCRIPT-GRAMMAR.md](LUX_SCRIPT-GRAMMAR.md); the design decisions and their reasons,
> in [README.md](README.md#design-decisions).

## Contents

1. [Getting started](#1-getting-started)
2. [Project layout](#2-project-layout)
3. [The `app:` block](#3-the-app-block)
4. [Routes](#4-routes)
5. [Parameters](#5-parameters)
6. [Classes and validation](#6-classes-and-validation)
7. [Responses](#7-responses)
8. [Groups and guards](#8-groups-and-guards)
9. [Session](#9-session)
10. [JWT](#10-jwt)
11. [Async](#11-async)
12. [Databases](#12-databases)
13. [Server-Sent Events](#13-server-sent-events)
14. [WebSockets](#14-websockets)
15. [Shared state](#15-shared-state)
16. [File uploads](#16-file-uploads)
17. [Error handlers](#17-error-handlers)
18. [Functions](#18-functions)
19. [The language](#19-the-language)
20. [Builtin reference](#20-builtin-reference)
21. [Common errors](#21-common-errors)
22. [How it works inside](#22-how-it-works-inside)
23. [Native modules](#23-native-modules)

---

## 1. Getting started

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Requires Linux (epoll, `sendfile(2)`, `SO_REUSEPORT`), CMake 3.20+ and C++20. The first
`configure` needs no network: nothing is downloaded.

Optional but recommended: `sudo apt install libjemalloc-dev`. With several event loops and a
database pool, glibc's `malloc` serializes on its arenas; swapping it is worth more than any
code optimization on large JSON responses. If it is there, `cmake` links it automatically; if
not, it builds anyway and says so.

The argument decides what gets compiled, with no surprises:

| Invocation | What it compiles |
|---|---|
| `lux app.lux` | Just that file |
| `lux a.lux b.lux` | Just those two |
| `lux ./my-app` | Every `.lux` in the directory, recursively |

| Option | |
|---|---|
| `--check` | Compile and exit, without starting |
| `--port N` | Override the port from the `app:` block |
| `--no-watch` | Do not watch for changes |
| `--verbose` | Log every incoming request to the console. It costs ~25% of the throughput, so it is off by default |
| `--autotest` | Walk the endpoints on startup and on every reload |
| `--autotest=all` | Also include POST/PUT/PATCH/DELETE |

### Self-test

With `--autotest`, after startup and after **every successful reload**, Lux talks to itself
over HTTP and walks the module's routes:

```
changes detected: recompiling
reloaded: 11 route(s) — 4 declarative, 7 with logic
autotest: probing 11 route(s)
  ok        GET    /users/1                          200  0ms
  ERROR     GET    /broken                           500  0ms
  rejected  GET    /admin/panel                      403  0ms
  stream    GET    /events                           200  0ms
  skipped   WS     /chat   (needs a WebSocket handshake)
autotest: 8 ok, 2 rejected, 1 with an error, 1 skipped
```

It does not check business logic: it looks for **no handler breaking** after a change. That
is why a `5xx` is the only thing that counts as an error — a `4xx` can be the correct
behaviour of a guard or a validation.

Route parameters are filled in by type (an `:id` of type `int` → `1`), and with
`--autotest=all` the body is synthesized from the class the route expects.

**On side effects:** probing an endpoint *runs its handler*. A `DELETE` would really do its
work on every reload, so by default only `GET`, `HEAD` and `SSE` are walked. Including the
rest is a decision of whoever launches the binary, not of the binary.

The watcher watches the set that was compiled, plus every file under the templates
directory (`app: templates "..."`), so editing a `.html` reloads the same way editing a
`.lux` does. On save, it recompiles and replaces the module. **If the new file does not
compile, the previous one keeps serving** and the error is printed with file, line and
column.

---

### Lux's own tests

```bash
cd build && ctest --output-on-failure
# or directly:
tests/run_tests.sh ~/lux-build/lux
```

246 tests in six suites:

| suite | what it covers | |
|---|---|---|
| `regression` | the binary over the socket, with real `.lux` files | 79 |
| `templates` | compiling and rendering, in process | 48 |
| `sqlite` | types, limits and the statement cache | 37 |
| `postgres` | types, placeholders, transactions and concurrency with checked content | 34 |
| `mysql` | the same, plus the null byte inside a text | 29 |
| `placeholders` | the translation of `?` to `$1` | 19 |

The `mysql` and `postgres` ones need a server and **skip themselves** if there is none; the
instructions for setting one up are in each script's header. The `sqlite` one creates its own
schema and always runs. The suite links nothing from the project: it tests what gets
deployed, not an instrumented version of it.

---

## 2. Project layout

```
my-app/
  app.lux           configuration
  routes/
    public.lux
    admin.lux
  templates/         Lux Script templates
  public/            static files
```

The order between files does not matter: compilation runs in two passes, first the
declarations are collected and then the names are resolved. A class can be used before it is
declared, and can live in another file.

---

## 3. The `app:` block

It can be in any file, but **only once**.

```lux
app:
    name      "My application"
    version   "1.0.0"
    port      8080
    templates "./templates"

    static "/static" -> "./public"
    static "/"       -> "./dist" spa

    docs                      # /openapi.json and /docs
    health                    # /health
    metrics                   # /metrics, Prometheus format

    session:
        secret  env("SESSION_SECRET")
        max_age 86400
        secure  true

    jwt:
        secret env("JWT_SECRET")
        issuer "my-app"
```

`env("VAR")` is resolved **at compile time**. It is how a secret avoids ending up written in
the `.lux` — `port` accepts it too (`port env("PORT")`), which is how Railway, Render, Fly.io,
Heroku and most other platforms assign the listen port at deploy time, leaving the app no
choice in the number. `--port N` on the command line overrides whatever `port` resolves to,
literal or `env(...)`.

A relative path here (`templates`, a static mount's directory, a `sqlite:`/etc. module's
`file`) resolves against the directory of the `.lux` file that has the `app:` block, not
against wherever the `lux` process happens to be launched from — so `lux /srv/blog/app.lux`
from a cron job, a systemd unit with no `WorkingDirectory=`, or any other directory still
finds `/srv/blog/templates` and opens `/srv/blog/blog.db`, not a same-named one relative to
whatever directory that launcher happened to start in (which, for a database file, means
silently creating and using an empty one — no error, since a missing sqlite file is normally
just a fresh database). An absolute path, or one built from `env(...)`, is never touched.

`spa` on a static mount makes routes that are not found fall back to `index.html`. A directory
request (`/docs` or `/docs/`) serves that directory's own `index.html` if it has one, same as
any other static file server — not the mount's root page. `index.html` itself is always sent
with `Cache-Control: no-cache` (an ETag revalidation on every load — one cheap 304, not a full
re-download), so a deploy that publishes new hashed asset names is picked up immediately
instead of up to an hour later; every other file keeps the usual long cache once its name
contains a content hash.

Because `spa` matches EVERY path nothing else claims, it also swallows a typo'd or genuinely
missing endpoint under an API prefix — `fetch('/api/typo')` gets `index.html` back with a
`200`, not a `404`, which surfaces client-side as an unrelated JSON-parse error. Reserve the
prefix explicitly if you serve both an API and an SPA from the same app:

```lux
any endpoint("/api/*"):
    return status(404)
```

A real route always wins over BOTH the SPA fallback and this catch-all, since routes are
matched before falling back to static files at all — this only catches what neither matched.

---

## 4. Routes

```lux
get     endpoint("/path"):
post    endpoint("/path"):
put     endpoint("/path"):
patch   endpoint("/path"):
delete  endpoint("/path"):
options endpoint("/path"):                      # CORS preflight
any     endpoint("/path"):
sse     endpoint("/path"):                      # event stream
ws      endpoint("/path") origins("https://x"):  # WebSocket
```

Patterns: `/users/:id`, `/users/{id}`, `/files/*`.

There is no CORS middleware: Lux has no middleware layer at all by design (delegated to the
reverse proxy, see §17), so a cross-origin caller (a mobile app, another backend, a browser
extension -- not a separate JS frontend, which Lux's own templates make unnecessary) needs
the usual two things spelled out by hand. The actual
response needs the header on every request, not only during the preflight:

```lux
options endpoint("/api/*"):
    return status(204)
        .header("Access-Control-Allow-Origin", "https://example.com")
        .header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        .header("Access-Control-Allow-Headers", "Content-Type")

get endpoint("/api/hello"):
    return { "hi": true }.header("Access-Control-Allow-Origin", "https://example.com")
```

### The two route levels

A route whose body is resolved entirely at compile time —a single `return` of a constant
value or of a native call with literal arguments— becomes a **native action** and does not
run a single bytecode step:

```lux
get endpoint("/"):
    return render("index.html")        # declarative: zero bytecode
```

The rest run bytecode. On startup, the binary says how many take each path:

```
lux: 3 file(s), 12 route(s) — 5 declarative, 7 with logic
```

A route with group guards is **never** declarative: the native action would not run them.

---

### Scheduled tasks

`every` runs a block on a schedule, with everything a route has: databases, modules, `await`.

```lux
every "10m":
    await sqlite.exec("delete from sessions where expires < ?", time.now())

every "03:00":                      # daily, 03:00 UTC
    List<Json> rows = await sqlite.query("select email from users where digest = 1")
    for Json r in rows:
        await mail.send({ "to": r["email"], "subject": "Your digest", "text": "..." })
```

The schedule is an interval (`"30s"`, `"5m"`, `"2h"`, `"1d"`) or a daily UTC time (`"HH:MM"`);
anything else is a compile error. An interval's first run is one interval after startup. A run
that is still going when the next one is due makes that one skip, with a warning. A task that
fails is logged and runs again next time; it never takes the server down. A task has no
request, so `query()`, `header()`, `session` and the like are empty in it.

Tasks run in the process, on one event loop: with several instances of the app behind a load
balancer, each instance runs them. A hot reload changes what a task does; adding a task or
changing its schedule needs a restart.

## 5. Parameters

Everything the handler needs is declared in the signature.

```lux
get endpoint("/users/:id", int id, int page = 1, string q):
    return { "id": id, "page": page, "q": q }
```

| Form | Where it comes from |
|---|---|
| A name that appears in the pattern | Route segment |
| A name that does not appear | Query string |
| `= value` | Default value if missing from the query |
| A type that is a `class` | JSON body, with validation |
| `Json` | JSON body, unvalidated -- any shape, including a top-level array |
| `File` / `List<File>` | Multipart parts |

Scalar types: `int`, `long`, `float`, `double`, `bool`, `string`.

**The compiler checks both directions**: that every `:name` in the pattern has a parameter
that binds it, and that no route parameter is left over.

A value that does not fit its type is a **400**, not an exception:

```json
{"error":"invalid parameter","expected":"int","param":"id","received":"abc"}
```

A query parameter with no `= value` is required: missing it entirely is a **422**, the same
as a missing `File` or a missing field of a class body, not the type's zero value (`""`,
`false`, `0`) filled in silently:

```json
{"error":"Validation failed","messages":["q: required"]}
```

An `int`/`float`/`bool` query or form parameter that IS present but empty (`?page=`, an
`<input type="number">` left blank) falls back to `= value` instead of a 400: there is no
valid empty spelling of a number or a boolean to reject, and this is the ordinary way a
browser submits "nothing was entered" for one. A `bool` parameter also accepts `"on"`/`"off"`
alongside `"true"`/`"false"`/`"1"`/`"0"`, since `"on"` is the literal value an HTML
`<input type="checkbox">` sends when checked and given no explicit `value=`.

`request.body` holds the raw, unparsed request body as a `string`, whatever the
`Content-Type`. Use it instead of a `class` body parameter when the shape is not fixed
(`Json`, for a body you only need to pass through or index dynamically) or when you need the
exact bytes the client sent — a class body parameter re-serializes what it parsed, which is
not byte-identical to the original and breaks a webhook's HMAC signature check
(Stripe/GitHub/etc. sign the bytes on the wire, not your interpretation of them):

```lux
post endpoint("/webhooks/stripe"):
    string sig = header("Stripe-Signature")
    if not hash.hmac_sha256(webhook_secret, request.body) == sig:
        return status(400)
    Json event = json.parse(request.body)
    return { "ok": true }
```

---

## 6. Classes and validation

```lux
class User:
    int     id
    string  name
    int     age
    string? password            # ? = may be missing

    validate:
        name != ""      "name: required"
        age >= 0        "age: cannot be negative"
        age < 150       "age: hardly believable value"
```

Used as a parameter, it binds to the body:

```lux
post endpoint("/users", User u):
    # Here `u` is always valid.
    return { "created": u.name }
```

| Situation | Response |
|---|---|
| A body that is not JSON | `400 {"error":"invalid JSON"}` |
| A required field is missing | `422` with `"field: required"` |
| Wrong type | `422` with `"field: expected int"` |
| A `validate` rule fails | `422` with its message |

**Every message comes out at once**, not just the first. And the handler never runs.

There is no coercion: `"30"` in an `int` field is a 422, it is not parsed.

The `validate` rules **are compiled**, so a misspelled field in a rule is a compile error and
never reaches production:

```
./app.lux:9:9: error: 'namme' is not declared
```

### Constructors

```lux
class Point:
    int x
    int y

    Point(int x):              # with a body
        this.x = x
        this.y = 0

    Point(int x, int y)        # without a body: each parameter goes to its field
```

They are told apart by the **number** of parameters. With none declared, one with every
field in declaration order is offered. Fields the constructor does not touch are `null`.

### Methods

```lux
class Point:
    int x
    int y

    fn int squared():
        return this.x * this.x + this.y * this.y

    fn string label(string prefix = "P"):
        return prefix + "(" + str(this.x) + "," + str(this.y) + ")"

    fn Point moved(int dx, int dy):
        return Point(this.x + dx, this.y + dy)
```

```lux
Point p = Point(3, 4)
p.squared()           # 25
p.label("Q")          # "Q(3,4)"
```

Methods and constructors compile as functions with `this` as the first parameter, so they use
the same frame stack and support recursion and default values just like `fn`.

The call **is resolved at compile time** from the receiver's declared type, so a misspelled
method never reaches production:

```
./app.lux:8:20: error: 'P' has no method 'triple'
```

An instance built by hand and one bound from the request body are the same thing: methods
work the same on both.

The receiver does not have to be a variable — chaining a method straight off a function or
constructor call works too, with no intermediate variable needed:

```lux
fn Point make_point(int x, int y):
    return Point(x, y)

get endpoint("/squared"):
    return { "r": make_point(3, 4).squared() }
```

### Nesting

A field can also be another class, or a `List` of one — not just the four scalars:

```lux
class Episode:
    string title
    int    duration_s

class Season:
    string        name
    List<Episode> episodes
    Episode?      pilot        # a single nested instance is fine too, ? or not
```

Which class comes first in the file does not matter — `Season` naming `Episode` above is
exactly as valid as the other way around.

```lux
post endpoint("/seasons", Season s):
    int total = 0
    for Episode ep in s.episodes:
        total = total + ep.duration_s
    return { "name": s.name, "total_seconds": total }
```

Bound from a request body, the same rules apply one level deeper: a missing or wrong-typed
field *inside* one of the episodes is exactly as much a `422` as one at the top level — the
message just names the outer field (`"episodes: expected List"`), not which particular
episode or subfield was the problem, matching a plain wrong-typed field's message shape.

A field can be a scalar, another class, or a `List` of either — not a `List<List<...>>`, not a
`Dict` (nobody has needed one yet). `--native` does not compile a class with a field like this
today: a route using one of these classes stays on bytecode, silently and correctly, the same
way an `await`-less database call or any other not-yet-native-representable shape already does
— see `lux app.lux --native --check`'s own report of what did and did not compile.

---

## 7. Responses

Everything goes out through `return`. There is no `response` object to carry around.

```lux
return { "key": "value" }                # 200, JSON
return [1, 2, 3]                         # 200, JSON
return render("page.html", k=v)          # HTML with Lux Script templates
return text("hello")                     # text/plain
return html("<h1>hello</h1>")            # text/html
return send_file("/var/f.pdf")           # sendfile(2)
return redirect("/other")                # 302
return redirect("/other", 301)           # 301
return status(204)                       # status code, no body
```

A handler that returns nothing and writes no response produces **204**.

### Chaining

```lux
return { "id": 1 }.status(201)
return { "a": 1 }.header("X-Thing", "value")
return render("x.html").status(203)

return { "ok": true }.cookie("theme", "dark",
                             max_age=3600, http_only=false, same_site="strict")
```

`cookie` options: `max_age`, `path`, `domain`, `secure`, `http_only`, `same_site`
(`"lax"`, `"strict"`, `"none"`). Defaults: `path=/`, `HttpOnly`, `SameSite=Lax`.

### Serving files

`send_file(path)` and a `static "..." -> "..."` mount (§3) both end up going through the same
`sendfile(2)`-backed path, so everything here applies to either:

```lux
return send_file("/var/videos/episode.mp4")
```

The response always carries `Accept-Ranges: bytes`, and honours a `Range` request header
(RFC 7233) — the mechanism a video player's seek bar and a download manager's resume both rely
on: `Range: bytes=1000-1999` gets back a `206 Partial Content` with only those 1000 bytes and a
`Content-Range: bytes 1000-1999/<total>` header; `bytes=1000-` (open-ended) and `bytes=-500`
(the last 500 bytes) both work too. A range whose start is past the end of the file is a `416
Range Not Satisfiable`; one whose END is past the end is clamped to the last real byte instead
— that is a normal request, not an error. A `Range` header with several ranges
(`bytes=0-99,200-299`) or one this cannot parse at all is not an error either: the file is
served in full, exactly as if the header had never arrived.

### There is no `Response` type

Since "everything goes out through `return`" (above), a helper `fn` cannot build a response
and hand it back to the route that calls it — `send_file()`, `text()`, `html()`, `json()`,
`status()`, `redirect()` and `render()` all write straight to the real response as a side
effect and always return `null` to their own caller, chaining included. Declaring `Response`
or `Response?` as a return type (or a parameter type) is a compile error for exactly that
reason — it cannot ever tell a "no response yet" `null` apart from a "already served" one,
since both are the same `null`:

```lux
fn Response? try_serve(string path):     # error: 'Response' cannot be used as a return type
    if not os.is_file(path):
        return null
    return send_file(path)
```

Do the check inline in the route instead — this is the only shape that actually works:

```lux
get endpoint("/file/:name", string name):
    string path = "./files/" + name
    if not os.is_file(path):
        return status(404)
    return send_file(path).header("Content-Type", "text/plain")
```

If several routes share the same "does this exist, then serve it" logic, factor out the
*check* (a `fn bool`/`fn string?` that returns a plain value), not the *serving*:

```lux
fn string? resolve_path(string name):
    string path = "./files/" + name
    return os.is_file(path) ? path : null

get endpoint("/file/:name", string name):
    string? path = resolve_path(name)
    return path == null ? status(404) : send_file(path).header("Content-Type", "text/plain")
```

---

## 8. Groups and guards

```lux
group("/api/v1"):
    require jwt.valid else status(401)

    get endpoint("/me"):
        return { "sub": jwt.claims["sub"] }

    group("/admin"):
        require jwt.claims["role"] == "admin" else status(403)

        get endpoint("/stats"):
            return { "users": state.get("users", 0) }
```

The prefixes are concatenated and **the guards accumulate**: to reach `/api/v1/admin/stats`
you have to pass the parent group's and then its own.

`require X else Y` is sugar for `if not X: return Y`. It works anywhere, not only inside a
group. There is no middleware concept.

Guards go **before** the routes inside the block.

---

## 9. Session

A cookie signed with HMAC-SHA256, Flask style. No server-side state.

```lux
post endpoint("/login", Login data):
    session.user = data.name
    session.role = "admin"
    return redirect("/")

get endpoint("/who"):
    return { "user": session.user, "role": session.role }

post endpoint("/logout"):
    session.clear()
    return redirect("/")
```

`session.clear()` tells the BROWSER to drop the cookie (a `Set-Cookie` that expires it
immediately) — it does not, and structurally cannot, invalidate a COPY of the old cookie taken
before logout (saved by other software, replayed from a proxy log, lifted via a
vulnerability elsewhere in a page). There is no server-side session store to revoke an entry
in (that is the whole design, see above): the signature alone is what a request is checked
against, and a signature stays valid until its own `exp`, logout or not. Keep `max_age` only
as long as the session actually needs to live, and put anything that truly must be
revocable-on-demand (a password reset, a banned user) behind a check against real data —
`state.*` or a database row keyed by user id, consulted alongside the session, not instead of
it.

`session.<whatever>` accepts any name: it is a store, not an object with fixed fields. A
field that does not exist is `null`.

- It needs `session: secret ...` in the `app:` block. Without it, touching it is a runtime error.
- It is always **`HttpOnly`**; `Secure` depends on the configuration.
- The cookie is only rewritten if the handler modifies it.
- The content is **signed but not encrypted**: the user can read it, they just cannot forge
  it. Do not keep anything there they should not see.
- An invalid signature leaves the session empty, never half-filled.
- **Everything you put in `session` has to fit inside the cookie**, since there is no
  server-side store behind it — unlike a session id that looks up a row somewhere. Real
  browsers drop a cookie over ~4096 bytes silently: no error, the next request just comes back
  with an empty session. Past that size, a server log line (`WARN session cookie is N bytes,
  over the 4096-byte limit...`) says so — the one place this can be caught, since neither the
  request that grew it nor the one that lost it sees anything wrong on its own. A shopping
  cart or any other collection that grows with use is the usual way to hit it; store an id and
  look the data up server-side (a database row, `state.*`) once it does.

---

## 10. JWT

HS256, verified against the `Authorization: Bearer ...` header.

```lux
group("/api"):
    require jwt.valid else status(401)

    get endpoint("/me"):
        return { "sub": jwt.claims["sub"], "role": jwt.claims["role"] }
```

Signature, `exp` and `iss` (if an `issuer` was configured) are checked. **Any `alg` other
than HS256 is rejected, `none` included**: accepting the algorithm the token itself declares
is the classic JWT library vulnerability.

RS256 is not there: it would require asymmetric cryptography, and Lux does not link
OpenSSL.

---

## 11. Async

```lux
get endpoint("/slow/:ms", int ms):
    await sleep(ms)
    return { "waited": ms }
```

`await` suspends the handler and hands control back to the event loop. Eight concurrent
500 ms requests take 500 ms, not four seconds.

`sleep()` wakes early if the client disconnects, and in that case the handler does not carry
on.

Rules checked at compile time:

- An asynchronous builtin **requires** `await`: a bare `sleep(100)` is an error.
- A synchronous one **forbids** it: `await text("x")` is an error.
- `await` only applies to an asynchronous call: `await 5` is an error.

Available asynchronous builtins: `sleep(ms)` and `ws.recv()`.

A plain `fn` can `await` too, and calling it works exactly like calling any other function —
`await` at the call site if you want its result, plain `return` if the caller is itself
returning it straight through:

```lux
fn List<Json> episode_titles(int series_id):
    return await sqlite.query(
        "select title from episodes where series_id = ?", series_id)

get endpoint("/titles/:series_id", int series_id):
    List<Json> titles = await episode_titles(series_id)
    return { "titles": titles }
```

This is what lets shared logic that needs a database (or anything else async) live in one
function several routes call, instead of being copied into each one. It works through any
number of calls — a function that awaits something only via a chain of other functions is
exactly as awaitable from its own caller as one that awaits directly.

---

## 12. Databases

Three modules: `sqlite`, `postgres` and `mysql`. You import and configure them; they manage
the connection.

```lux
import postgres

app:
    postgres:
        host     "127.0.0.1"
        port     5432
        database "my_app"
        user     "lux_script"
        password env("PG_PASSWORD")
        pool     4
```

Each module is only compiled if its client was present when Lux was built. If not,
`import postgres` gives an error when compiling the `.lux`, not a strange failure in
production.

### Configuration

| Module | Keys |
|---|---|
| `sqlite` | `file` (required), `pool`, `timeout_ms` |
| `postgres` | `url`, or else `host` / `port` / `database` (required) / `user` / `password`; `pool` |
| `mysql` | `host` / `port` / `database` / `user` / `password`; `pool` |

`pool` is the number of connections, between 1 and 64. Defaults to 4. Use `env()` for
passwords: it is resolved at compile time and does not stay written in the `.lux`.

---

### Querying

```lux
get endpoint("/articles"):
    return await postgres.query("select id, title from articles order by id")

get endpoint("/articles/:id", int id):
    List<Json> rows = await postgres.query("select title from articles where id = ?", id)
    if len(rows) == 0:
        return status(404)
    return rows[0]
```

`query()` returns `List<Json>`: a list of dictionaries, with the engine's types converted to
Lux Script's —integer, decimal, boolean, string and `null`.

Binary columns —`BLOB` in sqlite and mysql— arrive in **base64**, not as a string. It is not
a preference: a blob is arbitrary bytes, and returning them as text left the response not
valid UTF-8, so the client receiving it failed rather than the request. In postgres a `bytea`
arrives in libpq's hex form (`\x68656c6c6f`).

```lux
post endpoint("/articles", Article a):
    int rows = await sqlite.exec(
        "insert into articles (title, views) values (?, ?)", a.title, a.views)
    int id = await sqlite.last_id()
    return { "id": id }.status(201)
```

`exec()` returns the number of affected rows. `last_id()` returns the last auto-generated id
**in sqlite and mysql**.

**Postgres does not have it**, and the module says so instead of making one up: there the id
is asked for in the query itself, which is more reliable anyway because it does not depend on
which connection served the insert.

```lux
post endpoint("/articles", Article a):
    List<Json> rows = await postgres.query(
        "insert into articles (title, views) values (?, ?) returning id",
        a.title, a.views)
    return rows[0].status(201)
```

**Parameters always travel separately, never concatenated.** Concatenating the query by hand
is the only way to open yourself to an injection, and the language does not make it easy.

The placeholder is **`?` in all three engines**. Postgres numbers its own —`$1`, `$2`…— but
its driver takes care of that, so the same query works on sqlite, mysql and postgres without
changing a letter:

```lux
await sqlite.query(  "select title from articles where id = ?", id)
await mysql.query(   "select title from articles where id = ?", id)
await postgres.query("select title from articles where id = ?", id)
```

Three details of the translation, which only matter in postgres:

- A query written with `$1` comes out untouched, so code written before this keeps working.
- A `?` inside a string, a quoted identifier, a comment or a `$$…$$` block is not a
  placeholder and is left alone.
- `?` is also postgres's JSONB operator —`data ? 'key'`. If the query carries **no
  parameters** nothing is translated and the operator works as is; if it does carry them,
  write `??` to say "this is the operator, not a placeholder".

Mixing `?` and `$1` in the same query is an error, because the numbering would clash.

### Transactions

```lux
post endpoint("/transfer"):
    await sqlite.begin()
    await sqlite.exec("update accounts set balance = balance - 30 where id = 1")
    await sqlite.exec("update accounts set balance = balance + 30 where id = 2")
    await sqlite.commit()
    return { "ok": true }
```

`begin()` pins the connection: everything that follows in that request goes through the same
one, and `commit()` or `rollback()` release it. If the handler ends —or blows up— with a
transaction open, Lux issues a `ROLLBACK` and warns on the console. Without that, the next
request to take that connection from the pool would inherit the state.

### Errors

A failed statement raises an error at its `await`, like any other runtime error: uncaught, the
request ends with `500 {"error": "no such table: does_not_exist", "at": "app.lux:2:16"}`; `try`
catches it.

```lux
post endpoint("/users"):
    try:
        await sqlite.exec("insert into users (email) values (?)", form("email"))
    catch e:
        return { "error": "that email is already registered" }.status(409)
    return { "ok": true }.status(201)
```

Inside a transaction, a failed statement also aborts the transaction: anything but `rollback()`
after it raises too, and `commit()` rolls back instead. A handler that ends with a transaction
still open has it rolled back.

### Why it does not block

The sqlite, libpq and libmysqlclient clients are synchronous. Each module keeps a thread pool
with **one connection per worker**; `await` queues the work, releases the event loop and
picks it back up when the thread finishes. No `query()` stops the event loop, which is what
would make the whole efficiency argument collapse.

---

## 13. Server-Sent Events

```lux
sse endpoint("/metrics/:every", int every):
    int tick = 0
    sse.send("snapshot", "{\"startup\":true}")

    while sse.open:
        await sleep(every)
        tick = tick + 1
        sse.send("delta", "{\"tick\":" + str(tick) + "}", str(tick))
        if tick % 10 == 0:
            sse.ping("keepalive")
```

| Call | Frame |
|---|---|
| `sse.send(data)` | `data: ...` |
| `sse.send(event, data)` | `event: ...` + `data: ...` |
| `sse.send(event, data, id)` | adds `id:`, for reconnection with `Last-Event-ID` |
| `sse.ping(text)` | a `: ...` comment, ignored by the browser |
| `sse.open` | false when the connection closes |

The stream is opened before the handler runs and closed when it ends. There is no final
response to return.

---

## 14. WebSockets

```lux
ws endpoint("/echo") origins("https://myapp.com", "http://localhost:5173"):
    int n = 0
    ws.send("welcome")

    while ws.open:
        string msg = await ws.recv()
        if msg == null:
            break

        n = n + 1
        if msg == "bye":
            ws.send("closing after " + str(n) + " messages")
            ws.close()
            break

        ws.send("echo " + str(n) + ": " + msg)
```

`await ws.recv()` returns the message text, or `null` when the connection closes.

**`origins(...)` is required**, and leaving it out is a compile error. Browsers do not apply
the same-origin policy to the WebSocket handshake: without an allowlist, any site can open
the connection from your user's browser and inherit their cookies. An origin outside the list
gets a `403`.

### Broadcasting to a room

`ws` above is per-connection: `ws.send()` only ever reaches the one client that sent the
message you are replying to. Reaching every OTHER client watching the same thing — a chat
room, a "watch together" session, live presence — needs `rooms`, a native module built for
exactly this:

```lux
import rooms

ws endpoint("/watch/:id", string id) origins("https://myapp.com"):
    rooms.join("movie-" + id)

    while ws.open:
        string msg = await ws.recv()
        if msg == null:
            break
        rooms.broadcast_others("movie-" + id, msg)

    rooms.leave("movie-" + id)

get endpoint("/watch/:id/viewers", string id):
    return { "n": rooms.count("movie-" + id) }
```

| | |
|---|---|
| `rooms.join(name)` | Adds the current connection to room `name`. `ws` route only. |
| `rooms.leave(name)` | Removes it. `ws` route only. |
| `rooms.leave_all()` | Removes it from every room it is in. `ws` route only. |
| `rooms.broadcast(name, message)` | Sends `message` to everyone currently in `name`, sender included. |
| `rooms.broadcast_others(name, message)` | Same, minus the calling connection — the usual "echo to everyone else" shape. |
| `rooms.count(name)` | How many connections are currently in `name`. |

A room is just a string you make up — there is nothing to declare or configure. `broadcast`/
`broadcast_others`/`count` work from any route, `ws` or not; `join`/`leave`/`leave_all` need an
actual connection to add or remove, so they only work inside a `ws` route.

A connection that disconnects without calling `leave()` (closing the tab, losing the network)
is not removed immediately — it is noticed and dropped the next time that room is joined,
broadcast to, or counted. `rooms.count()` right after a disconnect can be off by however many
clients dropped that way since the last such call; call it again and it corrects itself. This
does not affect `broadcast()`: reaching a connection that is actually gone by then is a normal,
silently-skipped case, not an error.

---

## 15. Shared state

```lux
get endpoint("/visits"):
    return { "n": state.incr("visits") }

get endpoint("/counter"):
    return { "n": state.get("visits", 0) }
```

| | |
|---|---|
| `state.incr(key)` / `state.incr(key, n)` / `state.incr(key, n, ttl_ms)` | Adds and returns the new value; a TTL starts when the key is created |
| `state.decr(key)` / `state.decr(key, n)` | Subtracts |
| `state.get(key)` / `state.get(key, default)` | Reads |
| `state.set(key, value)` / `state.set(key, value, ttl_ms)` | Writes; with a TTL the key expires |
| `state.remove(key)` | Deletes |

It is the **only** shared-state path between the event loops: each VM has its own stack and
heap and shares nothing. That is why it exposes operations and not properties —
`state.x = state.x + 1` would be a race between the read and the write.

`incr` with a TTL is a rate limiter in one line — the count resets when the window the first
hit opened is over:

```lux
post endpoint("/login"):
    if state.incr("login:" + request.ip, 1, 60000) > 5:
        return status(429)
```

It lives in process memory: it is lost on restart, and is not shared between machines.

---

## 16. File uploads

```lux
post endpoint("/avatar", File image):
    require image.content_type.starts_with("image/") else status(415)
    require image.size <= 5 * 1024 * 1024             else status(413)
    string name = image.save("./public/uploads")
    return { "url": "/static/uploads/" + name }

post endpoint("/gallery", List<File> photos):
    List<string> names = []
    for File f in photos:
        names.add(f.save("./public/uploads"))
    return { "names": names }
```

The saved-to directory and the returned URL have to agree with the same `static "..." -> "..."`
mount (§3) for the URL to actually resolve — `save("./public/uploads")` above pairs with the
`static "/static" -> "./public"` mount §3 shows, so `/static/uploads/<name>` is that same file.
Saving into a directory no mount covers gives back a URL that 404s.

A `File` has `name`, `filename`, `content_type` and `size`, plus the method
`save(directory)`, which returns the name it was saved under.

`save()` keeps only the file component of the name: a `filename` with `..` or an absolute one
cannot escape the target directory.

With `File` (not `List<File>`), a missing file is a `422`. With `List<File>`, an empty list.

---

## 17. Error handlers

```lux
on error 404:
    return render("404.html", path=request.path, method=request.method)

on error 403:
    return { "error": "you cannot come in here", "code": error.code }

on error:
    log.error(error.message)
    return render("500.html")
```

In an `on error 422`, `error.messages` carries the complete list of validation messages —
empty if the 422 did not come from validating a body:

```lux
on error 422:
    return { "details": error.messages }
```

Without a code, it is the global handler. **It only covers 400–599**: with a 2xx the route's
handler has already written the response, and replacing it would be a response filter — that
is, middleware, which Lux delegates to the proxy on purpose.

The status code is preserved. If the handler writes nothing, the default body is kept.

---

## 18. Functions

```lux
fn int double(int x):
    return x * 2

fn string greet(string name, string greeting = "hello"):
    return greeting + ", " + name

fn bool is_email(string s):
    return s.contains("@") and s.contains(".")
```

Declaration order does not matter: a function can call another declared further down, or in
another file. Recursion works, with a cap of 200 nested calls — going over gives a language
error, it does not exhaust the process memory.

A function can construct a class, call a method on one, or use an enum value exactly like a
route handler can — useful for logic shared across several routes (building a result several
endpoints return, say) that would otherwise have to be duplicated inside each one:

```lux
class Episode:
    string title

fn Episode make_episode(string title):
    return Episode(title)

fn List<Episode> scan():
    List<Episode> out = []
    out.add(make_episode("Pilot"))
    return out

get endpoint("/episodes"):
    return scan()
```

Parameters accept default values, and the missing ones are filled in at the call site. A
parameter without a default cannot come after one that has one.

A function without `return` returns `null`. An error inside it **can be caught by the
caller**:

```lux
fn int divide(int a, int b):
    return a / b

get endpoint("/x"):
    try:
        return { "r": divide(1, 0) }
    catch e:
        return { "failure": e.message }
```

They can also be used inside a `validate:` block:

```lux
class Signup:
    string email

    validate:
        is_email(email)   "email: invalid format"
```

Declaring a function with a builtin's name is a compile error.

### Function references

A bare function name, used where a value is expected instead of being called, is a
reference to that function:

```lux
fn int double_it(int x):
    return x * 2

get endpoint("/doubled"):
    List<int> l = [1, 2, 3, 4]
    return { "r": l.map(double_it) }
```

**Not a closure.** It carries no captured environment — just which function, the way a C
function pointer does. There is no lambda syntax (`fn(x) => x * 2`) and no way to reference a
function that reads an outer local variable it does not receive as a parameter; it can still
reach `state`/`log`/other reserved objects, since those are not "outer locals," they are always
in scope. This is deliberately the smaller of two possible features — see the comment on
`Value::Type::Func` (`value.hpp`) for why full closures were not built instead.

The only place a function reference is currently useful is `List.map`/`filter`/`reduce`/
`for_each` (§20). A function passed to one of them **must not use `await`** — it runs
synchronously, with no event loop to suspend onto, and a callback that tries gives a clear
error instead of hanging.

---

## 19. The language

### Types

```
int  long  float  double  bool  string        primitives, lowercase
Json  List<T>  Dict<K,V>  File  Func           native classes, uppercase
```

`T?` marks that the value may be missing. Generics are **erased**: the checker verifies them
and they disappear before the bytecode. There are no user-defined generic classes.

Declaring a local as `int` or `float` coerces the initializer to match, the one case where
that actually matters: `/` between two `int`s gives an `int` when the division is exact and
a `float` otherwise, decided at runtime, so `int pages = total / per` truncates toward zero
(2.5 becomes 2) instead of silently holding a `float` that only breaks something several
lines later. A `float` local similarly promotes an `int` initializer (`float f = 10` holds
`10.0`). This applies to the initializer only — reassigning an existing local (`x = "text"`
over an `int x`) still runs with the same fully dynamic semantics as everywhere else in the
language.

### Enums

```lux
enum Status:
    PENDING, ACTIVE, DONE

get endpoint("/orders/:id/status", int id):
    return { "status": Status.ACTIVE }
```

A member (`Status.ACTIVE`) is a plain `string` — its own name, `"ACTIVE"`, not an index —
so it needs no separate representation anywhere: it serializes straight into a JSON
response exactly like any other string would, and compares with `==` like one too.
One member per line or comma-separated on the same line, whichever reads better for how
many there are. A typo in the member name is a compile error, listing the real ones:

```
./app.lux:8:20: error: enum 'Status' has no member 'ACTVE'; it has ACTIVE, DONE, PENDING
```

### Multi-line strings

Three quotes, for SQL or HTML without fighting the line breaks:

```lux
get endpoint("/posts"):
    return await sqlite.query("""
        select id, title
        from posts
        order by date desc
        """)
```

The margin is not part of the string: it is the file's indentation, not the text's. A line
break right after the opening is removed, the closing line if it stands alone, and the
indentation **common** to the rest — which keeps the relative indentation between lines. The
above is exactly `select id, title\nfrom posts\norder by date desc`.

The escapes are the same as in a normal string: `\n`, `\t`, `\r`, `\0`, `\"`, `\\`.

### Truthiness

**False** are `null`, `false`, `0`, `0.0`, `""`, and the empty list and dictionary. It is
Python's rule.

It is not coercion: there is no type conversion inside the operators.

```lux
1 + "1"     # error
0 == "0"    # false
"n = " + str(n)     # this is how you concatenate a number
```

A trap inherited from Python: with an optional value, `if x:` does not tell "it is zero" from
"it did not arrive". For presence, use `x == null`.

### Statements

```lux
int n = 5
n = n + 1

if n > 3:
    ...
else if n > 1:
    ...
else:
    ...

while n > 0:
    n = n - 1

for int x in [1, 2, 3]:
    ...
for string k in myDictionary:      # walks the keys
    ...

require n > 0 else status(400)

try:
    int x = n / 0
catch e:
    log.warn(e.message)

break
continue
return value

switch n:
    case 1, 2:
        ...
    case 3:
        ...
    else:
        ...
```

`for` walks lists and a dictionary's keys, `range(...)` included: `for int i in range(5):`.
`try/catch` is resolved with a range table computed at compile time, so a `return` or a
`break` inside the `try` does not leave a handler dangling. The error reaches the `catch`
as a value with `message`.

`switch` is not a separate mechanism — it desugars, at parse time, into exactly the
if/elif chain writing it out by hand would be. The subject (`n` above) is evaluated
**once**, into a compiler-generated local, no matter how many `case`s there are — an
expression with a side effect in the subject position is not repeated per case. A `case`
takes one or more comma-separated values (`case 1, 2:` matches either); values are
ordinary expressions, not restricted to literals, compared with the subject using the
same `==` the rest of the language uses. `else` is optional — with no match and no
`else`, nothing in the switch runs, same as an `if` with no matching branch and no `else`.

### Expressions

Precedence, lowest to highest: `?:` · `or` · `and` · `not` · `==` `!=` · `<` `<=` `>` `>=` ·
`+` `-` · `*` `/` `%` · unary `-` · `.` `()` `[]`.

```lux
string role = age >= 18 ? "adult" : "minor"
```

---

## 20. Builtin reference

### Functions

| | |
|---|---|
| `text(v)` `html(v)` `json(v)` | Write the response |
| `render(template, k=v, ...)` | Renders a Lux Script template |
| `status(code)` `redirect(target[, code])` `send_file(path)` | |
| `len(v)` | Size of a string, List or Dict |
| `str(v)` `int(v)` `float(v)` | Explicit conversion |
| `range(n)` `range(start, end)` `range(start, end, step)` | A `List<int>`, Python's `range()` shape |
| `header(name[, default])` | Request header |
| `query(name[, default])` | Query parameter |
| `cookie(name[, default])` | Request cookie |
| `form(name[, default])` | Field of a `urlencoded` form |
| `await sleep(ms)` | Suspends |

### Reserved objects

| Object | Members | Where |
|---|---|---|
| `request` | `path` `method` `ip` `body` | Any handler |
| `session` | any field, `clear()` | Any handler |
| `jwt` | `valid` `claims` | Any handler |
| `state` | `incr` `decr` `get` `set` `remove` | Any handler |
| `log` | `info` `warn` `error` | Everywhere |
| `sse` | `send` `ping` `open` | `sse` routes |
| `ws` | `send` `recv` `open` `close` | `ws` routes |
| `error` | `code` `message` `messages` | `on error` blocks |
| `sqlite` `postgres` `mysql` | `query` `exec` `begin` `commit` `rollback`; `last_id` in sqlite and mysql only | With `import` and its block in `app:` |

Using one outside its context is a compile error. Every method of a database module is
asynchronous: they are called with `await`.

### Methods by type

| Receiver | Methods |
|---|---|
| Any | `status(code)` `header(k, v)` `cookie(k, v, ...)` |
| `string` | `starts_with` `ends_with` `contains` `upper` `lower` `trim` `index_of` `replace` `split` `slice` `repeat` |
| `List` | `add(v)` `insert(i, v)` `pop()` `remove_at(i)` `contains(v)` `index_of(v)` `first()` `last()` `sort()` `sort_by(fn)` `reverse()` `slice(start[, end])` `concat(other)` `join(sep)` `unique()` `chunk(n)` `map(fn)` `filter(fn)` `reduce(fn, initial)` `for_each(fn)` `find(fn)` `find_index(fn)` `any([fn])` `all([fn])` `count([fn])` `group_by(fn)` `sum()` `min()` `max()` |
| `Dict` | `has(key)` `keys()` `values()` `items()` `get(key[, default])` `remove(key)` `merge(other)` |
| `File` | `save(directory)` |

`index_of` and `slice` work in byte offsets, not Unicode codepoints — correct for ASCII and for
multi-byte UTF-8 as long as a slice does not land mid-sequence. `len(s)`, `upper()`, `lower()`,
`split(s, "")` and `for c in s` are codepoint-aware, not byte-aware: `len("ñandú")` is `5`, and
`"café".upper()` is `"CAFÉ"`. `upper`/`lower` cover ASCII, the Latin-1 Supplement, and Latin
Extended-A — Spanish, French, German, Portuguese and most other Latin-script languages — but
are not a complete Unicode case-conversion table; a codepoint outside that set passes through
unchanged rather than being silently dropped or corrupted. `split(s, "")` (an empty separator)
and `for c in s` both walk `s` one codepoint at a time — the only two ways to go
character-by-character over a string, since there is no index-based single-character access.
`slice` accepts negative indices (counted from the end, like Python) on both `string` and
`List`; out-of-range bounds are clamped, not an error. `List.sort()` is natural order only
(numbers ascending, strings lexicographic) — no custom-comparator form: `map`/`filter`/
`reduce`/`for_each` are, for now, the only methods that take a function reference (§18) as an
argument.

When the receiver's type is known at compile time —a declared parameter, a typed variable, a
literal— the name and the argument count are checked **there**, not at run time:

```
error: values of type string have no method 'uppercase';
       it has status, header, cookie, starts_with, ends_with, contains, upper, lower, trim
```

The check continues down the chain, because every method knows what it returns:
`s.upper().trimm()` also fails at compile time. The same goes for a class's fields:
`p.doesnotexist` says which fields `p` has instead of silently returning `null`.

This reaches **inside the templates** too, because `render()` passes its argument types to
the template compiler: `{{ who.uppercase() }}` is a `lux --check` error, with the
template's file and line.

Where the type is not known —the variable of a `{% for %}`, a field of a `Json`— nothing is
checked and dispatch stays at run time, as before.

Because of this, **every `render()` call for a given template has to pass every variable that
template references**, even one this particular call has no real use for — a `layout.html`
included by several pages, with `{{ user }}` in its header, means every page that
`{% include %}`s or `{% extends %}` it needs a `user=...` in ITS OWN `render()` call, not just
the pages that actually show it:

```lux
get endpoint("/a"):
    return render("page.html", title="A", user=current_user)   # fine

get endpoint("/b"):
    return render("page.html", title="B")                       # error: 'user' is not declared
```

Pass `null` for a page that genuinely has nothing to put there (`user=null`) rather than
leaving it out — each `render()` call is checked independently against what IT supplies, the
same as any other typed parameter, so there is no way for one call site to "inherit" a
variable another call site happens to pass for the same file.

---

## 21. Common errors

| Message | What is happening |
|---|---|
| `the pattern declares ':id' but no parameter binds it` | The parameter is missing from the signature |
| `'sleep()' is asynchronous: you must write 'await sleep(...)'` | The `await` is missing |
| `'text()' is not asynchronous: the 'await' is unnecessary` | The `await` is redundant |
| `'sse' only exists inside an sse route` | A reserved object out of context |
| `a ws route needs origins(...)` | The origin allowlist is missing |
| `cannot add int and string` | An operation between different types |
| `the session is not configured` | `session: secret ...` is missing from `app:` |
| `'X' is not declared` | An unknown name, inside `validate` too |

They all come out with file, line, column and a cursor under the exact position.

---

## 22. How it works inside

```
lux ./app  →  lex → parse → check → emit
                 ↓
              route table + bytecode   (once, not per request)
                 ↓
              N threads: event loop + its own VM, SO_REUSEPORT
```

**One compilation.** Lexer, parser, semantic analysis and emission happen at startup and once
per file change. Never per request.

**Two levels.** Declarative routes are entries in the radix tree with a native action: zero
interpreted steps. The rest run bytecode that only does glue — the real work (HTTP parsing,
routing, file I/O, templates, JSON) is always native C++.

**The VM does not know how to wait.** When it reaches an asynchronous call it gathers the
arguments and stops; the handler, which is already a coroutine, does the real `co_await` on
the engine and resumes it with the result. That is why the VM has its own stack and locals
instead of using C++'s: it is what allows stopping halfway.

**One VM per in-flight request**, held in the handler's coroutine frame. Chunks that cannot
suspend —and that is known at compile time— reuse one per thread and save the allocations.

**Reload.** The module has its own router; the engine only carries a wildcard entry that
delegates. Switching version is publishing a `shared_ptr`: no `dlopen`, no `.so`, no restart.
If the new version does not compile, it is not published.

**Step cap.** An infinite loop in a `.lux` is cut with an error instead of pinning an event
loop thread, which would take down every connection on that core. The counter resets on every
suspension, so a legitimate SSE loop can live for hours.

## 23. Native modules

Beyond `sqlite`/`postgres`/`mysql`, `import` reaches the compiled-in modules below. A call is
checked at compile time (the module exists, it is imported, the argument count fits) and its
argument types are checked when it runs, with one message format:

```
hash.sha256(): argument 1 must be a string, not int
```

Functions marked **await** do real I/O or CPU work and run on the I/O pool, never on the event
loop: `await` is mandatory. Everything else is synchronous and fast.

A module function that fails raises an error — awaited or not, the same as a database call. Left
alone it ends the handler with a `500 {"error": message, "at": "file:line:col"}`; `try` catches
it where there is something better to do:

```lux
try:
    Json r = await http.get(url, null, { "timeout_ms": 3000 })
    return r["body"]
catch e:
    log.warn(e.message)
    return { "cached": true, "items": state.get("last_items", []) }
```

### hash

| | |
|---|---|
| `sha256(s)` `hmac_sha256(key, msg)` | Hex digests |
| `equal(a, b)` | Constant-time comparison — use it for signatures and tokens, never `==` |
| `random_hex(n)` `token([n])` | Random: `n` bytes as hex; a URL-safe token (default 32 bytes) for API keys and one-time links |
| `uuid()` `uuid(7)` | UUID v4, or v7 (time-ordered — kinder to a database index) |
| **await** `password(pw)` `verify(pw, stored)` | PBKDF2-HMAC-SHA256, 600k iterations, in Django's `pbkdf2_sha256$...` format |
| `sign(value, key)` `unsign(token, key[, max_age_s])` | A tamper-proof token carrying any value; `unsign` is `null` if it was altered or is older than `max_age_s` |

```lux
post endpoint("/login"):
    List<Json> rows = await sqlite.query("select id, hash from users where email = ?", form("email"))
    if len(rows) == 0 or not await hash.verify(form("password"), rows[0]["hash"]):
        return status(401)
    session.user = rows[0]["id"]
    return redirect("/")

get endpoint("/reset/:token", string token):
    Json who = hash.unsign(token, os.getenv("SECRET"), 3600)   # a link valid for one hour
    if who == null:
        return status(410)
```

### encoding

`base64_encode(s[, url_safe])` `base64_decode(s)` (either alphabet) · `hex_encode(s)`
`hex_decode(s)` · `url_encode(s)` `url_decode(s)` (RFC 3986: a space is `%20`) ·
`query_encode(dict)` (`{"q": "a b", "tag": ["x", "y"]}` → `q=a%20b&tag=x&tag=y`) ·
`query_decode(s)` · `html_escape(s)` · `url_parse(url)` → `{scheme, host, port, path, query,
fragment}`.

`url_parse` reads a URL the way a browser does (`\` is `/`, leading spaces are dropped), so
it is the check for an open redirect:

```lux
string next = query("next", "/")
if encoding.url_parse(next)["host"] != "":
    next = "/"                # only paths on this site
return redirect(next)
```

### text

| | |
|---|---|
| `slug(s)` | `"¡Café con Leche!"` → `"cafe-con-leche"` (Latin letters transliterated) |
| `truncate(s, n[, suffix])` | At most `n` characters, `suffix` (default `…`) included |
| `format_number(x[, decimals, thousands, point])` | `format_number(1234.5, 2, ".", ",")` → `"1.234,50"` |
| `pad_left(s, n[, ch])` `pad_right(s, n[, ch])` | `pad_left("7", 4, "0")` → `"0007"` |
| `distance(a, b)` | Levenshtein distance, for "did you mean…?" |

### math

`abs` `sign` `min` `max` (two or more numbers, or one `List`) `clamp(x, lo, hi)` ·
`round(x[, digits])` `floor` `ceil` · `sqrt` `exp` `log(x[, base])` `log10` `pow` ·
`sin` `cos` `tan` `asin` `acos` `atan` `atan2` `hypot` · `pi()` `e()` ·
`random()` (`[0, 1)`) `random_int(lo, hi)` (inclusive) `choice(list)` `shuffle(list)`
`sample(list, k)`. A result stays an `int` when the input was one and the answer is whole;
`sqrt(-1)` and friends are a `math domain error`.

### time

A time is a plain `int`: milliseconds since the Unix epoch, UTC. So "in 5 minutes" is
`time.now() + 5 * 60 * 1000`. Where a function takes an offset it is a fixed number of minutes
(`60` for CET), never a server timezone.

| | |
|---|---|
| `now()` `now_seconds()` | |
| `format(ts, fmt[, offset])` `format_iso(ts)` | strftime formats |
| `parse(s, fmt)` `parse_iso(s)` | `null` when it does not match. `parse_iso` takes a bare date, `T` or a space, milliseconds and `Z`/`+02:00` |
| `parts(ts[, offset])` | `{year, month, day, hour, minute, second, weekday (1 = Monday), yearday}` |
| `start_of(ts, unit[, offset])` | Start of the `"day"`, `"week"`, `"month"` or `"year"` |
| `add_months(ts, n)` | Calendar months: Jan 31 + 1 is Feb 28/29 |
| `ago(ts[, from, lang])` | `"3 minutes ago"`, `"in 2 hours"`; `lang` `"es"` gives `"hace 3 minutos"` |

### regex

`test(pattern, text)` · `find` / `find_all` · `groups` (`[whole, group 1, ...]`, or `null`) ·
`replace(pattern, text, with)` (`$1`, `$&`) · `split` · `escape(s)` (user input inside a
pattern). Its own linear-time engine: no pattern can blow up on hostile input, and compiled
patterns are cached. A deliberate subset — no lookaround, no non-capturing groups, no
backreferences inside the pattern — and a subject is capped at 4096 bytes.

### json

`parse(s)` (a catchable error on invalid input) · `stringify(v[, indent])`.

### csv

`read(text[, header, delimiter])` → a `List` of `Dict`s with typed cells; `write(rows[,
columns, delimiter])` → text. A UTF-8 BOM is dropped, `;` works as a delimiter (what Excel
writes in many locales), and a cell that would run as a spreadsheet formula (`=`, `+`, `-`,
`@`) is written defused. Filter, sort and aggregate with the `List` methods:

```lux
fn bool paid(Json r):
    return r["status"] == "paid"

get endpoint("/export"):
    List<Json> rows = await sqlite.query("select * from orders")
    return text(csv.write(rows.filter(paid)))
```

The handle-based `parse`/`rows`/`columns`/`row_count`/`to_csv`/`close` still work.

### os

| | |
|---|---|
| `getenv(name[, default])` `cwd()` | |
| `path_join(...)` `path_basename` `path_dirname` `path_ext` `path_abs` | |
| `path_exists` `is_dir` `is_file` `file_size` `mtime_ms` | `-1` size/time for a missing path |
| `mime(path)` | `"image/jpeg"` for `"IMG_01.JPG"` |
| `list_dir(dir[, recursive])` `glob("uploads/*.jpg")` | Names (paths relative to `dir` when recursive), sorted glob matches |
| `make_dir` `remove_file` `remove_dir(dir[, recursive])` | `remove_dir` refuses a non-empty directory unless told |
| `temp_dir()` `temp_file([suffix])` | A new private (0600) temporary file |
| **await** `read_file` `write_file` `append_file` `copy_file(from, to[, overwrite])` `move(from, to)` | `move` works across filesystems |
| **await** `run(cmd[, args, options])` | `{status, stdout, stderr}`; options `input` (stdin), `cwd`, `env` (a `null` value unsets), `timeout_ms` (default 15 s, up to 120 s) |

`run` takes an argument `List`, never a shell string, so no argument is ever interpreted by a
shell. There is no path sandboxing: a path built from user input is as dangerous as in Python.

### proc

A process that outlives the request that starts it (a transcode, a long job):
`start(cmd[, args, options])` returns a handle at once; options `stdout` (`"pipe"` default /
`"null"` / a path), `stderr` (`"null"` / a path), `stdin` (`"pipe"`), `cwd`, `env`.
`write(h, text)` feeds stdin without ever blocking (it returns how much went in; `null` closes
stdin). **await** `read(h, max_bytes, timeout_ms)` (`""` if nothing arrived, `null` at EOF) and
**await** `wait(h, timeout_ms)` (the exit code, or `null` if still running). `alive(h)`,
`kill(h[, signal])` (signals only), `close(h)` (never kills).

### rooms

Cross-connection WebSocket broadcast — see [§14](#14-websockets). `join(name)` `leave(name)`
`leave_all()` (from a `ws` route) · `broadcast(name, msg)` `broadcast_others(name, msg)` (a
`Dict`/`List` goes out as JSON) · `count(name)`.

### net

`ip_in(ip, "10.0.0.0/8")` (or a `List` of ranges) · `is_private(ip)` (loopback, private,
link-local, CGNAT, IPv6 unique-local) · `ip_version(ip)` (`4`, `6` or `null`).

```lux
get endpoint("/admin"):
    if not net.ip_in(request.ip, ["10.8.0.0/24", "127.0.0.1"]):
        return status(403)
```

### zip

**await** `create(dest, files)` writes an archive of the files (paths, or `[path,
name_in_zip]` pairs) and returns how many went in. Entries are stored, not compressed — fine
for what usually ends up in one (photos, PDFs, video), and no compression library needed; up
to 4 GB.

### pdf

`create(w, h)` `add_page(h, w, h)` · `text(h, x, y, s, size)` `text_width(h, s, size)` ·
`set_font(h, family[, bold, italic])` `set_color(h, r, g, b)` `set_line_width(h, w)` ·
`rect(h, x, y, w, h[, filled])` `line(h, x1, y1, x2, y2)` `image(h, png, x, y[, w, h])` ·
`send(h[, filename, download])` (the PDF as this request's response) `save(h, path)`
`to_base64(h)` · `close(h)`. Sizes in points (A4 is 595 × 842). Needs cairo at build time
(`libcairo2-dev`).

### http

**await** `get(url[, headers, options])` `delete(url[, headers, options])`
`post(url, body[, headers, options])` `put` `patch` → `{status, headers, body}`, with `body`
parsed when it is JSON. A `Dict` body is sent as JSON. `options`: `timeout_ms` (default 15 s,
up to 120 s) and `public_only` — refuse any private, loopback or link-local address (checked
on the address actually connected to, redirects included), for a URL that comes from a user:

```lux
Json r = await http.get(webhook_url, null, { "public_only": true, "timeout_ms": 5000 })
```

HTTPS through libcurl (`libcurl4-openssl-dev` at build time), certificates always verified,
redirects only to http/https. Connections are kept and reused across calls. `url_encode(s)` is
`encoding.url_encode`.

### mail

```lux
import mail

app:
    mail:
        host     "smtp.example.com"
        port     587                     # the default; 465 with tls "tls"
        user     "apikey"
        password env("SMTP_PASSWORD")
        from     "My App <noreply@example.com>"
        tls      "starttls"              # "starttls" (default), "tls" or "none"

post endpoint("/contact"):
    Json r = await mail.send({ "to": "me@example.com", "reply_to": form("email"),
                               "subject": "Contact: " + form("subject"), "text": form("message") })
```

**await** `send({to, cc, bcc, reply_to, from, subject, text, html})` → `true` (a failure raises). `to`/`cc`/`bcc` take one address or a `List`; with both `text` and `html`
the mail carries both. Every header value is stripped of line breaks, so a form field cannot
add a header, and `bcc` never appears in the message. Built with the `http` module (libcurl).

---

Using a module that is not imported, or a function it does not have, is a compile error:

```
./app.lux:2:23: error: missing 'import hash' in order to use 'hash.sha256'
```

How modules work inside and how to write one: [NATIVE-MODULES.md](NATIVE-MODULES.md) and
[src/lux_script/modules/README.md](src/lux_script/modules/README.md).
