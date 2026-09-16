# Lux

![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat&logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?style=flat&logo=cmake&logoColor=white)
![Linux only](https://img.shields.io/badge/platform-Linux-FCC624?style=flat&logo=linux&logoColor=black)
![Binary size](https://img.shields.io/badge/binary-1.5_MB-informational?style=flat)
![Tests](https://img.shields.io/badge/tests-246_passing-brightgreen?style=flat)

Lux is a web framework focused on giving both performance and a simple and easy developer experience. Its core is written in C++20, but the programming itself is done in LuxScript, a language made exclusively for the Lux framework.


# So... Do I have to learn a new language?

### No! ...Kind of.

The language is really easy to read. It's basically Python with a sprinkle of C++: easy as the former, but with static typing to avoid stupid nonsensical operations (Javascript...), and borrowing classes, generics (`List<T>`, `Dict<K,V>`) and good old `++`/`--` from the latter.


```lux
app:
    name      "My blog"
    port      8080
    templates "./templates"

    static "/static" -> "./public"
    docs

    session:
        secret env("SESSION_SECRET")


class Article:
    int     id
    string  title
    string  body
    string? tags

    validate:
        title != ""       "title: required"
        len(body) >= 20    "body: minimum 20 characters"


get endpoint("/"):
    return render("index.html")

get endpoint("/articles/:id", int id):
    return { "id": id, "title": "Hello world" }

post endpoint("/articles", Article a):
    # If the body fails to parse or fails validation, the response is 422
    # and this line never runs.
    return { "created": a.title }.status(201)


group("/admin"):
    require session.role == "admin" else redirect("/login")

    get endpoint("/panel"):
        return render("panel.html", user=session.user)
```

```
$ lux ./my-blog
lux: 3 file(s), 5 route(s) — 2 declarative, 3 with logic
Lux running on http://0.0.0.0:8080 (threads=16, press CTRL+C to quit)
```

Just save the file after changing something and Lux reloads immediately. No recompiling, no restarting, no CMake, no nothing. And the binary is around 2.5MB too!

---

## Architecture

```
lux ./app          →  lex → parse → check → emit
                       ↓
                    route table + bytecode  (built ONCE, not per request)
                       ↓
                    N threads, each with its own event loop + VM, SO_REUSEPORT
```

One event loop per core doing the actual work, `SO_REUSEPORT` splitting connections between
them, no GIL and no global GC getting in the way. HTTP parsing is llhttp, JSON and static files (`sendfile(2)`) get handled directly.

If a route can be fully resolved at compile time — `return render("index.html")` — it
becomes a native action and doesn't run a single byte of bytecode. The binary tells you exactly how many routes got that treatment on startup.

Every request gets its own VM, living inside the handler's own coroutine frame. Nothing to lock, nothing to sync between cores. If you actually need state shared across requests, you ask for it on purpose, and it comes back atomic — no surprise race conditions because two handlers happened to touch the same thing.

---

## Language

Python syntax with a sprinkle of C++ here and there. Meant for web development, but somewhat capable of everything else. I tried to keep the experience as close to Flask and FastAPI as possible, and managed to simplify it even more in the end.


### Parameters

```lux
get endpoint("/users/:id", int id, int page = 1, string q):
    return { "id": id, "page": page, "searching": q }
```

`:id` grabs the path segment, `page` and `q` come from the query string, with a default if you give it one. And the compiler checks both directions: every `:name` in the pattern needs a parameter for it, and every parameter not in the 
pattern gets read from the query string — something Flask can't do, since its route types live inside a plain string.

Type a parameter as a class and it binds straight to the request **body**, validation and
the 422 included for free. `File` or `List<File>` grabs multipart parts the same way.

### Responses

| | |
|---|---|
| `return { "a": 1 }` | 200, JSON |
| `return render("x.html", k=v)` | HTML through LuxScript templates |
| `return text("hi")` / `html(...)` | plain text / HTML |
| `return send_file(path)` | file, `sendfile(2)` |
| `return redirect("/other")` | 302 |
| `return status(204)` | status code, no body |

No mutable `response` object to carry around and mutate — you just `return` the thing.

### Guards

```lux
group("/api/v1"):
    require jwt.valid else status(401)

    get endpoint("/me"):
        return { "sub": jwt.claims["sub"] }
```

CORS, compression and rate limiting are the proxy's job, so the only "middleware" left is
route protection — and that's just `if not X: return Y` wearing a nicer outfit, not some
separate concept you have to go learn. Groups nest, and guards stack right along with them.

### Reserved objects

| Object | Where | Gives |
|---|---|---|
| `request` | any handler | `path`, `method`, `ip` |
| `session` | any handler | signed cookie, any field |
| `jwt` | any handler | `valid`, `claims` |
| `state` | any handler | store shared across threads |
| `log` | everywhere | `info`, `warn`, `error` |
| `sse` | `sse` routes | `send`, `ping`, `open` |
| `ws` | `ws` routes | `send`, `recv`, `open`, `close` |
| `error` | `on error` blocks | `code`, `message` |

Try to use `sse` outside an `sse` route, or `error` outside `on error`, and it just won't
compile. Same deal if you typo a method or field on something with a known type —
`name.uppercase()` on a `string`, `p.missing` on a class — and that check follows you all the
way into templates too.

---

## Design decisions

| Topic | Decision |
|---|---|
| Auth | Vendored HMAC-SHA256, HS256. No OpenSSL. RS256 is not available |
| Real time | SSE and WebSockets |
| Transport | Plain HTTP/1.1, TLS and HTTP/2 belong to the reverse proxy |
| Execution | Bytecode on a custom VM, one VM per event-loop thread |
| Compilation | Built into the binary. No external toolchain, no transpilation to C++ |
| Persistence | `sqlite`, `postgres`, and `mysql` modules over a thread pool and `await`. `?` placeholder in all three — the postgres driver translates it to `$1` |
| Templates | Custom engine, like Jinja2 but with LuxScript expressions inside |
| Types | `class` for known, validated shape; `Json` for dynamic data, containers for homogeneous data, no `Any` (javascript...) |
| Generics | Native containers only, erased at compile time. No user-defined generic classes |
| Config | LuxScript, `app:` block, once per project. No YAML or TOML — one language to learn |
| Layout | `lux ./my-app` reads the tree recursively. Order does not matter; compilation happens in two passes |

### Templates

I tried an off-the-shelf engine first. It worked, and it also dragged in Boost, fmt, rapidjson, and a `build/_deps` directory pushing 800 MB just to render a string. I wrote my own instead: same shape as Jinja2 — `{{ }}`, `{% if %}`, `{% for %}`, `{% extends %}`,`|safe` — except what's inside the braces is LuxScript, checked by the same compiler as everything else. A template typo is a `lux --check` error with a file and a line.

I gave up `{{ super() }}` and Jinja2's filters (`|upper`, `|join`...) — those are just LuxScript methods now.

---

## Examples

### Login with session and role

```lux
class Login:
    string username
    string password

    validate:
        username != ""  "username: required"
        password != ""  "password: required"

post endpoint("/login", Login data):
    if data.password != "hunter2":
        return status(401)

    session.user = data.username
    session.role = data.username == "alice" ? "admin" : "user"
    return redirect("/")

post endpoint("/logout"):
    session.clear()
    return redirect("/")

group("/admin"):
    require session.role == "admin" else status(403)

    get endpoint("/panel"):
        return render("panel.html", of=session.user)
```

`session` is a cookie signed with HMAC-SHA256, no server-side state, always `HttpOnly`. An
invalid signature clears it — never leaves it half-populated.

### Real time

```lux
sse endpoint("/metrics"):
    int tick = 0
    while sse.open:
        await sleep(2000)
        tick = tick + 1
        sse.send("delta", "{\"tick\":" + str(tick) + "}", str(tick))

ws endpoint("/chat") origins("https://myapp.com"):
    ws.send("welcome")
    while ws.open:
        string msg = await ws.recv()
        if msg == null:
            break
        ws.send("echo: " + msg)
```

`await` suspends the handler without blocking the event loop — eight concurrent 500 ms
requests take 500 ms, not four seconds.

`origins(...)` is required on a `ws` route. Skip it and it won't compile.

### File uploads

```lux
post endpoint("/avatar", File image):
    require image.content_type.starts_with("image/") else status(415)
    require image.size <= 5 * 1024 * 1024              else status(413)
    return { "saved": image.save("./uploads") }

post endpoint("/gallery", List<File> photos):
    List<string> names = []
    for File f in photos:
        names.add(f.save("./uploads"))
    return { "names": names }
```

`save()` keeps only the file name — a `filename` carrying `..` or an absolute path can't
escape the target directory.

### Shared state

```lux
get endpoint("/visits"):
    return { "n": state.incr("visits") }
```

`state` is the one shared-state path across the N event loops, and it exposes operations
instead of properties on purpose: `state.x = state.x + 1` would race the read against the
write.

### Custom error pages

```lux
on error 404:
    return render("404.html", path=request.path)

on error:
    log.error(error.message)
    return render("500.html")
```

---

## Errors

```
./app.lux:12:19: error: pattern declares ':id' but no parameter binds it
  12 | get endpoint("/users/:id"):
     |              ^
```

File, line, column, cursor. Everything you (sometimes) love about g++ and clang++.

---

## Features

| | |
|---|---|
| **Routing** | Radix tree, `:param`, `{param}`, `*`, nested groups |
| **Input** | Path, query with defaults, typed JSON body, multipart, headers, cookies, forms |
| **Validation** | Per-class `validate:` block → automatic 422 with every message |
| **Output** | JSON, HTML, text, custom templates with `{% extends %}`, files, redirects, status codes |
| **Async** | `await sleep(ms)`, `await ws.recv()`, cancellation on disconnect |
| **Real time** | SSE with `id:` for reconnection, RFC 6455 WebSockets |
| **Auth** | `session` in a signed cookie, JWT HS256 with `alg`, `exp`, and `iss` verification |
| **State** | Shared store with atomic operations |
| **Persistence** | `sqlite`, `postgres`, and `mysql` modules: connection pool, transactions, parameterized queries |
| **Files** | MIME, ETag, 304, `sendfile(2)`, SPA support, dotfile and path-traversal blocking |
| **Language** | Classes, `for`, `while`, `try/catch`, ternary, lists, dicts, methods |
| **Docs** | `/openapi.json` and `/docs` generated from the AST |
| **Observability** | Logger with rotation, `/health`, `/metrics` in Prometheus format |
| **Reload** | File watching and atomic module swap |

**Not included:** TLS, CORS, compression, rate limiting, and security headers,
that's the reverse proxy's job. Also no user-defined generic classes: `List<T>` and
`Dict<K,V>` exist, `class Box<T>` doesn't (yet (maybe)). I might consider implementing HTTP/2 in the future.

---

## Build

Requires **Linux** since I use epoll, `sendfile(2)` and `SO_REUSEPORT`. **CMake 3.20+** and **C++20**
(GCC 11+ or Clang 13+).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The only vendored dependency is **llhttp**, just a few KB of dependency-free C. No `node_modules`, no npm, no downloading 40GB or god knows what just for a Hello World.

The only system dependencies are the database clients you actually want — each module
builds only if its client is present: `libsqlite3-dev`, `libpq-dev`, `libmysqlclient-dev`.

jemalloc is optional but worth it: with several event loops and a database pool, glibc's
`malloc` serializes on its arenas and becomes a bottleneck pretty fast. `cmake` links it automatically
if it finds it; otherwise the build proceeds and warns you.

```bash
sudo apt install libjemalloc-dev     # optional
```

```bash
lux ./my-app          # every .lux file in the directory, recursively
lux app.lux            # just that file
lux a.lux b.lux        # just those
lux ./app --check      # compile and exit
lux ./app --no-watch   # no hot reload
lux ./app --verbose    # one log line per request
lux ./app --autotest   # walks the endpoints after startup and after each reload
```

---

## Modules

Need something and don't want to write it yourself? `import` it. Besides the three DB
drivers above, there's a handful of native ones; zero `app:` configuration needed, just `import` and go.

```lux
import hash
import regex

get endpoint("/hash/:s", string s):
    return { "sha256": hash.sha256(s) }

get endpoint("/valid_email/:s", string s):
    return { "ok": regex.test("^[^@]+@[^@]+\\.[^@]+$", s) }
```

| Module | Gives |
|---|---|
| `hash` | `sha256`, `hmac_sha256`, `random_hex` |
| `csv` | An in-memory table: parse, filter, group, aggregate — no pandas required |
| `os` | Env vars, paths, async file I/O, and running subprocesses (`os.run`, no shell involved) |
| `math` | `abs`, `min`, `max`, `round`, `sqrt`, `pow`, `random`, the usual suspects |
| `time` | Unix time plus ISO 8601 formatting and parsing |
| `regex` | `test`, `find`, `find_all`, `groups`, `replace`, `split` |
| `rooms` | Cross-connection WebSocket broadcast: join a room, leave it, send to everyone in it |
| `pdf` | Generates PDFs, needs cairo at build time |
| `http` | Outbound HTTP calls, needs libcurl at build time |

Forget the `import` and use the module anyway and it's a compile error, not a 3 a.m. crash the
first time that code actually runs. And if the library a module needs (cairo, libcurl...)
wasn't there when Lux itself was built, the module just isn't compiled in — `import pdf`
fails at compile time too, same as any other module, instead of hiding behind a runtime check
somebody has to remember to write.

### Creating your own

Writing a module is just C++, nothing else.

It's a drop-in. Write the file and that's it.

1. Write the functions in a new `.cpp` inside `src/lux_script/modules/` — a sibling of
   `base_modules/`, where the officially shipped ones live. Name the file after the module — `qrcode.cpp` for `import qrcode`.
2. End it with one line, `LUX_REGISTER_MODULE(YourClassName)`. That's the registration —
   no other file changes, nothing to add to a list anywhere.
3. Reconfigure (`cmake -S . -B build`) and rebuild. The file is picked up automatically.
4. Write a test, and actually check the compiler rejects what it should: a missing
   `import`, the wrong number of arguments, an `await` where there shouldn't be one.

The module gets compiled straight into the `lux` binary, same as `sqlite`/`postgres`/`mysql` are. 
Need state that survives between calls, like `csv`'s tables? Hand back a plain `int` handle and keep the
real object in a table inside your module's own file — a convention, not something the
compiler needs to know about.

The full walkthrough — including what changes if the module needs a third-party library,
which is the one case that still needs a few lines of `CMakeLists.txt` by hand — is in
[NATIVE-MODULES.md](NATIVE-MODULES.md) and [src/lux_script/modules/README.md](src/lux_script/modules/README.md).

---

## Benchmark

I didn't just want to say Lux is fast, I wanted to actually put it against the frameworks
people already reach for, so I wrote the exact same backend — same routes, same JSON shapes,
same SQL, byte-for-byte — in nine of them: Gin (Go), Fastify and Express (Node), Flask and
FastAPI (Python), Actix, Axum and genhttp-ioxide (Rust), and Lux itself, both as plain
bytecode and compiled with `--native`. Every implementation hits the same SQLite dataset
(5000 users, 2000 products, 8000 orders) so nobody gets an easier dataset to work with.

The load comes from k6, with a realistic mix of weighted endpoints: point reads, a list read,
a search, a write inside a transaction, two CPU-bound endpoints (recursive fibonacci and a
naive prime count) and one that just `await sleep()`s to simulate a slow upstream call. 50
virtual users, ramping 10s up, 30s steady, 5s down (45s total), closed loop — each VU fires
requests as fast as the backend under test lets it, so the number that comes out is that
backend's own ceiling, not an artificial rate I picked. One process per backend, no cluster,
no extra workers, and the load generator runs on the same machine as the server, same as
everyone else in the table.

| Backend | req/s | p50 | p90 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| **Lux (bytecode)** | 5794 | 0.18ms | 5.47ms | 35.13ms | 171.09ms |
| **Lux (`--native`)** | 7154 | 0.10ms | 2.62ms | 13.05ms | 163.05ms |
| Gin | 4410 | 1.36ms | 20.83ms | 42.75ms | 163.79ms |
| Fastify | 4265 | 3.43ms | 13.13ms | 25.81ms | 170.81ms |
| Express | 3974 | 4.34ms | 14.05ms | 26.29ms | 168.88ms |
| Actix | 4979 | 0.59ms | 14.42ms | 41.81ms | 163.70ms |
| Axum | 5073 | 0.51ms | 14.04ms | 41.35ms | 164.49ms |
| genhttp-ioxide | 2073 | 13.56ms | 26.64ms | 36.06ms | 172.11ms |
| Flask | 388 | 101.44ms | 211.94ms | 253.81ms | 335.06ms |
| FastAPI | 1835 | 11.38ms | 58.77ms | 83.60ms | 179.45ms |

`req/s` counted over the full 45s run, ramps included, same divisor for all ten so the
comparison holds even if the absolute number isn't a pure steady-state figure.

A couple of things worth calling out instead of just letting the table speak:

- **Lux already is the fastest of the ten on plain reads, before `--native` even enters the picture** 
  sub-millisecond p50 on `/users/:id`, `/orders/:id`, `/health`, ahead of Actix and
  Axum. Those routes take the declarative/native-action path and barely touch
  the bytecode at all.
- **`--native` closes the gap** Plain bytecode Lux is, expectedly,
  much slower than a compiled language on the recursive fibonacci and prime-counting
  endpoints, but when compiled those same routes land right next to Actix/Axum/Gin —
  because at that point they *are* compiled C++, not bytecode.
- **genhttp-ioxide's number isn't really comparable** its run had a 23% error rate, which
  points at a broken implementation on my end. I left it in the table anyway, but I doubt anything can be done that would make take it to 7k+ req/s.
- **Flask and FastAPI trail as expected** — sync WSGI with the GIL, and async-with-a-threadpool
  respectively, both fundamentally more contended under 50 concurrent users than an event loop
  per core.

**Just so nobody has to ask:** one process per framework, not how any of these would actually be deployed in production. SQLite as BD, so write contention says as much about SQLite as it does about any framework. The load generator shares the machine with the server under test. And it's one 45-second sample, not an average of several runs, so the numbers may vary. The point is that Lux is well ahead.

---

# A Manifesto

I wish programming went back to when it was simpler. Just a small, optimized (and not RAM-hungry) binary that gets the job done, not a whole stack of software built upon software and software that nobody understands but chooses to use anyway because 'it's what everyone else is using'.

In case I haven't made it clear during this whole file, I hate JavaScript. I really hate it. Not just the fact that it allows bullshit like 1 + "1" as if it were a normal Tuesday, but also that people have just internalized it as 'just a small quirk of the language' instead of what it is, bullshit. Someone really looked at ```const x = ({ a: { b } = {} }) => b;``` and thought this was fine. Someone really looked at this piece of bullshit glorified DOM scripting language and thought it had to go to the backend and make everyone else miserable.

I'm sick of installing npm and downloading thousands of files of God knows what and tens of Gigabytes of bullshit dependencies that I don't need, especially when you could just have an LLM write that small module you need and can't be bothered to write yourself. And let's not forget that npm has had malware snuck into it a few times at least. And the fact that someone could just remove that one bullshit dependency that EVERYTHING depends on for some reason and half the internet would go down.

I thought Web development was complicated and weird. Turns out it's just JavaScript that is complicated and weird. I loved Flask the first time I used it. It was easy, it was simple and it made sense. Then I wanted more performance, but, of course, I'm NOT touching Node.js, so I decided to make my own framework. I made it in C++, because C = Fast and Compiled = Good, but then I wanted it to be simpler to use so I made my own interpreted language around the framework so I didn't have to write C, but then I wanted it to be fast again so I ended up compiling the LuxScript code to C++.

It **might** have a bug or two, I'm sure it can be optimized even more and I'm sure I've made poor decisions along the way, but it's good enough as it is now and I'm sure it will get better with time. I know writing a whole language so I don't have to write C++ just to end up compiling it back to C++ sounds weird, but it works **and it's not a 10GB shitbomb of malware**.

And no, TypeScript doesn't cut it.


Fuck JavaScript and Fuck Node.js.
\- Eldur
