# Security audit — Lux

Manual review of the framework (C++20 core + LuxScript) focused on the surfaces
exposed to untrusted input: the HTTP parser, the connection layer, static files,
templates, sessions/JWT, the SQL layer, the VM, and the native modules (`regex`, `http`, `os`,
`csv`, multipart, WebSocket).

**Commit audited:** `d10f274`
**Date:** 2026-09-16
**Remediation date:** 2026-09-16 (branch `security-fixes`)

Findings marked **[verified]** were reproduced with a separately compiled test
program; the rest are the result of reading the code.

Findings 1-8, 11-13 and 17 were fixed and verified (clean rebuild, the existing
test suite with no new regressions, and manual testing against the real binary
for the first three). The detail of each fix is at the end of the document, under
"Remediation status." Findings 9, 10, 14, 15 and 16 are design or product
decisions that require a maintainer's choice, not a bug with a mechanical
fix; each one explains why it was left untouched.

---

## Summary

| # | Finding | Severity | Status |
|---|---|---|---|
| 1 | `regex.*` overflows the stack and kills the entire process with ~30 KB of input | **Critical** | ✅ Fixed |
| 2 | `--native` vs bytecode divergence in `int` param coercion | **High** | ✅ Fixed |
| 3 | `std::mismatch` over `fs::path` reads out of range → SEGV in static files | **High** | ✅ Fixed |
| 4 | `verify_jwt` does not reject a token without `iss` when an issuer is configured | Medium | ✅ Fixed |
| 5 | No size limit on `http.*` client responses (OOM) | Medium | ✅ Fixed |
| 6 | `http.*` outgoing headers without CRLF sanitization | Medium | ✅ Fixed |
| 7 | WebSocket `read_buf` grows without limit after a parse failure | Medium | ✅ Fixed |
| 8 | `float` params accept `nan`, `inf`, `0x10` | Medium | ✅ Fixed |
| 9 | SSRF: `http.*` with no allowlist or internal-network blocking | Medium | ⚠️ Partial — see note |
| 10 | Static mounts skip the entire middleware chain | Medium | ⏭ Not touched — design choice |
| 11 | `Content-Length` overridable by the handler | Low | ✅ Fixed |
| 12 | Formula injection in `csv.*` | Low | ✅ Fixed |
| 13 | `random_bytes` silently returns empty on failure | Low | ✅ Fixed |
| 14 | No rate limiting or brute-force protection | Low (design) | ⏭ Not touched — new feature |
| 15 | `/metrics` and health with no authentication by default | Informational | ⏭ Not touched — deployment decision |
| 16 | The VM does not check stack or locals bounds | Informational | ⏭ Not touched — not remotely exploitable |
| 17 | False comment about `fd_ = -1` after `close()` | Informational | ✅ Fixed |

---

## 1. `regex.*` overflows the stack and kills the entire process — **Critical** [verified]

**Where:** [module_regex.cpp:34](src/lux_script/module_regex.cpp#L34) and the six
functions registered at [module_regex.cpp:124-129](src/lux_script/module_regex.cpp#L124-L129).

libstdc++'s `std::regex` executes the search recursively, one stack level per
character of the subject. This is not a pathological-pattern problem: it reproduces
with the most trivial possible pattern.

Measured on this machine (8 MB stack, `ulimit -s 8192`), with `std::regex_search`:

| Pattern | Subject length | Result |
|---|---|---|
| `[a-z]+` | 25 000 | ok |
| `[a-z]+` | 30 000 | **SIGSEGV** |
| `^[\w.+-]+@[\w-]+\.[\w.]+$` (textbook email validation) | 50 000 | **SIGSEGV** |
| `(\s*\w+)*` | 50 000 | **SIGSEGV** |
| `^(a\|aa)+$` | 1 000 | hang >15 s (backtracking) |

The threshold sits around ~30 KB. The framework's body limit is 16 MB
([http_parser.hpp:30](src/http/http_parser.hpp#L30)), so a single `POST` with a
30 KB field reaches it comfortably.

Two consequences, and the second is the serious one:

- **Hang:** no `regex` function is marked `is_async`, so all of them run
  on the event-loop thread. Catastrophic backtracking doesn't block one request: it
  blocks the entire core and every connection that thread was serving.
- **Full crash:** Lux is *a single process* with N threads sharing connections via
  `SO_REUSEPORT`, and it only installs handlers for `SIGINT`/`SIGTERM`/`SIGPIPE`
  ([app.cpp:369,485-486](src/app.cpp#L369)). A SIGSEGV on any thread brings down the
  whole process: every core, every connection, with no restart.

In other words: **any app that passes request data to `regex.*` — which is exactly
what the module exists for — has an unauthenticated, single-request, remote DoS
that causes a total service outage.**

The module's header comment documents the cost of recompiling the pattern on every
call as the known limitation; the real problem is a different one and isn't mentioned.

---

## 2. `--native` vs bytecode divergence in `int` param coercion — **High** [verified]

**Where:** [project.cpp:641-651](src/lux_script/project.cpp#L641-L651) (bytecode) versus
[native_gen.cpp:2890-2900](src/lux_script/native_gen.cpp#L2890-L2900) (native).

The interpreter requires `std::stoll` to consume the entire text:

```cpp
size_t pos = 0;
long long v = std::stoll(text, &pos);
if (pos != text.size()) return false;
```

The code generated for `--native` does not:

```cpp
inline bool lux_route_coerce_int(const std::string& t, int64_t& out) {
    try { out = std::stoll(t); return true; } catch (...) { return false; }
}
```

The comment preceding that function claims the omission is deliberate, "replicate
the interpreter's exact behavior bit for bit, so `--native` never accepts (or rejects)
a value that bytecode would have treated differently." That claim is no longer true: the
interpreter was hardened and the native backend wasn't updated. Both sites entered the
tree in the same commit (`052f7cd`, the Lumen→Lux rename), so the divergence predates
that rewrite.

Measured behavior for `get endpoint("/users/:id", int id)`:

| URL | bytecode | `--native` |
|---|---|---|
| `/users/12abc` | 400 | **`id = 12`** |
| `/users/1e999` | 400 | **`id = 1`** |
| `/users/0x1p4` | 400 | **`id = 0`** |
| `/users/0x10` | 400 | **`id = 0`** |

Impact:

- The same app changes behavior when compiled with `--native`, which is exactly what the
  comment promises can't happen. A test suite run in bytecode mode catches nothing.
- **Parser differential:** anything that acts on the raw path — an authorization
  middleware, a cache key, a WAF in front, audit logs — sees `12abc`
  while the handler operates on record `12`. `/users/12abc`, `/users/12%20`,
  `/users/12x` and `/users/12` are all the same resource for the handler and four
  distinct strings for everything else.

---

## 3. `std::mismatch` over `fs::path` reads out of range — **High** [verified]

**Where:** [app.cpp:140-141](src/app.cpp#L140-L141), and again at
[app.cpp:164-166](src/app.cpp#L164-L166) and [app.cpp:184-185](src/app.cpp#L184-L185).

```cpp
auto [ri, fi] = std::mismatch(canonical_root.begin(), canonical_root.end(),
                              preliminary.begin());
```

The three-iterator overload walks the entire range of `canonical_root` without knowing
where `preliminary` ends. If the resolved path has **fewer components** than the root,
`mismatch` dereferences `preliminary`'s `end()` iterator.

Verified with `-fsanitize=address` against root `/srv/www/public` and resolved `/srv`:

```
AddressSanitizer: SEGV on unknown address 0x000000000028
  #0 std::filesystem::path::compare(...) const
  #5 std::mismatch<path::iterator, path::iterator>(...)
```

This isn't theoretical UB: it's a hard SEGV, and for the same reason as finding 1 it
takes down the entire process.

The dotfile guard at [app.cpp:114-120](src/app.cpp#L114-L120) rejects any `/.`
before reaching here, so `..` isn't the path in. The path in is a **symlink inside
the served root that points to a shallower directory** — `public/assets -> /opt/assets`
with the root at `/home/user/app/public`, for example. `weakly_canonical` resolves it
to something shorter than the root and the check blows up.

The irony is that this is precisely the branch that exists to *return 403* on a
symlink escaping the root: in the case that matters most, instead of blocking, it crashes.

---

## 4. `verify_jwt` accepts tokens without `iss` — Medium

**Where:** [auth.cpp:100-105](src/lux_script/auth.cpp#L100-L105).

```cpp
if (!issuer.empty()) {
    auto it = payload.as_dict().find("iss");
    if (it != payload.as_dict().end() &&
        (!it->second.is_str() || it->second.as_str() != issuer))
        return false;
}
```

The validation only applies **if the claim is present**. A token without `iss` passes
the check even when an issuer is configured. The correct behavior is for the claim's
absence to be a rejection when the issuer is mandatory.

Exploitable when the same HMAC secret is shared across services (a common pattern): a
token issued by another service in the same trust domain, or one minted without `iss`,
is accepted by this route.

In the same function, `exp` is only checked if it exists and is numeric
([auth.cpp:95-99](src/lux_script/auth.cpp#L95-L99)) — a token without `exp` never
expires — and `nbf` isn't checked at all.

A separate design note: the session cookie is **signed but not encrypted**
([auth.cpp:14-22](src/lux_script/auth.cpp#L14-L22) — base64url of plaintext JSON). This is
Flask's model and is defensible, but the docs should say so explicitly so nobody puts
sensitive data into `session`.

---

## 5. No size limit on `http.*` responses — Medium

**Where:** [module_http.cpp:52-55](src/lux_script/module_http.cpp#L52-L55).

```cpp
size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}
```

No `CURLOPT_MAXFILESIZE` and no cutoff in the callback. A malicious or compromised
remote server returns an unbounded body and exhausts the process's RAM. `write_header`
([module_http.cpp:59](src/lux_script/module_http.cpp#L59)) has the same problem on the
headers `Dict`.

The 15 s timeout bounds time but not bytes: at normal bandwidth several GB fit within
that window.

---

## 6. `http.*` outgoing headers without CRLF sanitization — Medium

**Where:** [module_http.cpp:100-102](src/lux_script/module_http.cpp#L100-L102).

```cpp
std::string line = key + ": " + val.as_str();
header_list = curl_slist_append(header_list, line.c_str());
```

Neither the key nor the value is filtered. A `\r\n` in either one injects arbitrary
headers into the outgoing request (request splitting against the destination
service), and an embedded NUL truncates the header via `c_str()`.

This is asymmetric with the rest of the framework: `Response::header`
([response.hpp:82-91](include/lux/response.hpp#L82-L91)) and `build_set_cookie`
([cookies.hpp:44-63](include/lux/cookies.hpp#L44-L63)) do carefully filter CR/LF. The
outgoing path was left without that treatment.

---

## 7. WebSocket `read_buf` grows without limit after a parse failure — Medium

**Where:** [websocket.hpp:314](include/lux/websocket.hpp#L314) together with
[websocket.hpp:235](include/lux/websocket.hpp#L235) and
[websocket.hpp:243](include/lux/websocket.hpp#L243).

```cpp
void feed(const uint8_t* data, size_t len) {
    read_buf.append(reinterpret_cast<const char*>(data), len);
    try_parse();
    resume_waiter_if_ready();
}
```

`feed()` doesn't check `closed` before accumulating. Two paths in `try_parse()` do
`closed = true; return;` without sending a close frame and without anyone closing the socket:

- a frame larger than `kMaxFramePayload` ([websocket.hpp:235](include/lux/websocket.hpp#L235))
- `pending.size() >= kMaxPendingFrames` ([websocket.hpp:243](include/lux/websocket.hpp#L243))

`closed` is just a flag. The TCP connection stays alive until the *handler* finishes its
coroutine, and `_ws_on_data` is only unhooked in `HttpConnection::close()`
([http_connection.cpp:637-640](src/http/http_connection.cpp#L637-L640)). If the handler is
suspended in a long `await` — a slow query, a `sleep` — the client can keep pumping
bytes indefinitely: each one gets appended to `read_buf`, `try_parse()` bails on the
first check, and nothing frees memory.

The normal path is capped at ~16 MB per connection; these two branches have no cap at all.

Minor, in the same area: the length doesn't enforce minimal encoding (a `len7 == 126`
with a 3-byte payload is accepted), which allows disagreements with intermediaries.

---

## 8. `float` params accept `nan`, `inf` and hexadecimal — Medium [verified]

**Where:** [project.cpp:653-658](src/lux_script/project.cpp#L653-L658).

`std::stod` consumes `"nan"`, `"inf"` and `"-inf"` in full, so the
`pos != text.size()` check treats them as valid. It also accepts hexadecimal floats
(`"0x10"` → 16, `"0x1p4"` → 16) and leading whitespace (`"  42"`, reachable via `%20`).
Affects **both backends**.

A `float precio` field that receives `nan` makes every ordering comparison false: a guard
like `require precio > 0 else ...` doesn't fire, and `precio != precio` is true. It's a
silent validation bypass.

Output serialization is protected: `write_double`
([value.cpp:247-256](src/lux_script/value.cpp#L247-L256)) converts non-finite values to `null`,
so the response JSON doesn't get corrupted. The problem is the logic, not the format.

---

## 9. SSRF: `http.*` with no allowlist or internal-network blocking — Medium

**Where:** [module_http.cpp:85-88](src/lux_script/module_http.cpp#L85-L88) and
[module_http.cpp:134-135](src/lux_script/module_http.cpp#L134-L135).

The only URL validation is that it starts with `http://` or `https://`. There's no
blocking of loopback, RFC1918, or link-local addresses — `http.get("http://169.254.169.254/...")`
(cloud metadata) or `http://127.0.0.1:5000/admin` both work.

`CURLOPT_FOLLOWLOCATION` is also active, with 5 hops and **no explicit
`CURLOPT_REDIR_PROTOCOLS`**. Even if the initial destination is validated at the
application level, a 302 redirect takes the request wherever the remote wants. The
allowed protocols should be pinned explicitly instead of relying on the default of
whatever libcurl version is linked.

There's no way to disable redirect-following from LuxScript.

---

## 10. Static mounts skip the entire middleware chain — Medium

**Where:** [app.cpp:286-290](src/app.cpp#L286-L290).

```cpp
// Static file mounts bypass the middleware chain.
if (req.method == "GET" || req.method == "HEAD") {
    if (try_serve_static(static_mounts_, req, res)) co_return;
}
```

It's intentional and commented, but the consequence deserves to be written down: a
`group("/admin"): require session.role == "admin"` **does not protect** a `static` mount
under that prefix. The guard never even runs.

Related: the `path` middlewares see is the **raw** one
([http_connection.cpp:213](src/http/http_connection.cpp#L213) assigns it undecoded),
while the file server decodes with `url_decode_path`
([app.cpp:72-93](src/app.cpp#L72-L93)). For normal routes, a middleware that compares
`req.path` against a prefix sees a different string from the one resolved against disk.

Adjacent design note: `File.save()` ([natives.cpp:909-974](src/lux_script/natives.cpp#L909-L974))
doesn't restrict extensions, and `safe_name` ([natives.cpp:539-544](src/lux_script/natives.cpp#L539-L544))
only strips directory separators and rejects `.`/`..`. Uploading `evil.html` or `evil.svg` to a
directory that's also mounted as `static` yields stored XSS served with its real
`Content-Type`. The dotfile guard covers `.env`, but not this.

---

## 11. `Content-Length` overridable by the handler — Low

**Where:** [response.hpp:292-310](include/lux/response.hpp#L292-L310).

`build()` emits `Content-Length` unconditionally and *then* `emit_headers()` dumps the
header map as-is. A handler that does `res.header("Content-Length", ...)` or
`res.header("Transfer-Encoding", "chunked")` produces a response with
duplicate/contradictory headers — the basis for request smuggling if there's a proxy
in front that resolves the conflict differently than the client does.

The CR/LF filtering in `header()` prevents classic response splitting, but doesn't
prevent overwriting framing headers. There should be a list of reserved headers that
handlers can't emit.

Minor: 204 and 304 responses also carry `Content-Length`.

---

## 12. Formula injection in `csv.*` — Low

**Where:** [module_csv.cpp:107-108](src/lux_script/module_csv.cpp#L107-L108).

```cpp
bool needs_quotes = s.find_first_of(",\"\n\r") != std::string::npos;
```

The escaping is correct per RFC 4180, but a field starting with `=`, `+`, `-`, `@`,
TAB or CR is interpreted as a formula when the file is opened in Excel, LibreOffice or
Sheets. Exporting user data to CSV is exactly the module's use case, so the mitigation
(prefixing a `'`) fits here.

---

## 13. `random_bytes` fails silently — Low

**Where:** [crypto.cpp:223-232](src/lux_script/crypto.cpp#L223-L232).

```cpp
std::FILE* f = std::fopen("/dev/urandom", "rb");
if (!f) return {};
...
if (got != n) return {};
```

Returns an **empty** string on failure, indistinguishable from a valid result to the
caller. If `/dev/urandom` fails to open — descriptor exhaustion under load, a
misconfigured chroot — consumers get zero bytes of entropy without knowing it.

Current consumers: `hash.random_bytes` ([module_hash.cpp:47](src/lux_script/module_hash.cpp#L47)),
exposed to LuxScript and a natural candidate for generating tokens; and the
anti-collision suffix in `File.save()` ([natives.cpp:947-949](src/lux_script/natives.cpp#L947-L949)),
where an empty result makes all 5 retries produce the same name.

This should be a hard failure, and it's worth migrating to `getrandom(2)`, which
doesn't depend on having a free descriptor.

---

## 14. No rate limiting — Low (design)

There is no rate limiting anywhere in the framework. The only control is
`max_connections` (10,000 by default, [app.hpp:320](include/lux/app.hpp#L320)), which caps
concurrent connections but not requests per IP or per unit of time.

For a framework that ships sessions, JWT and password hashing out of the box, offering
nothing against brute force on a login is a notable gap. The Slowloris timeouts
([http_connection.cpp:78-90](src/http/http_connection.cpp#L78-L90)) and the pipelining
limit are well handled; this is what's missing next to them.

---

## 15. `/metrics` and health with no authentication — Informational

**Where:** [app.hpp:300-316](include/lux/app.hpp#L300-L316).

`enable_metrics()` registers the Prometheus endpoint with no guard at all. They go
through the router, so a global middleware can protect them, but the default leaves
them open and they expose traffic volume and status-code distribution.

---

## 16. The VM does not check stack or locals bounds — Informational

**Where:** [vm.hpp:92-93](include/lux_script/vm.hpp#L92-L93).

```cpp
void  push(Value v) { stack_.push_back(std::move(v)); }
Value pop()         { Value v = std::move(stack_.back()); stack_.pop_back(); return v; }
```

`pop()` doesn't check that the stack isn't empty, and accesses to
`locals_[frame.locals_base + in.operand]` ([vm.cpp:221,225](src/lux_script/vm.cpp#L221))
don't verify range. Safety depends entirely on the emitter generating balanced bytecode.

It isn't remotely exploitable — the bytecode is compiled from the author's own `.lux`
file, it doesn't come from the attacker — but it means any emitter bug turns into
memory corruption instead of a language error. An `assert` in debug builds, or a
bytecode verifier at load time, would contain the entire class of bugs.

Related: `kStepLimit` (50 M) resets **on every suspension**
([vm.hpp:65](include/lux_script/vm.hpp#L65)), so a loop that does `await` inside never
exhausts it. This is deliberate (long-lived SSE), but it renders the cutoff moot
precisely in the loops that do I/O.

---

## 17. False comment about `fd_` after `close()` — Informational

**Where:** [http_connection.cpp:233-236](src/http/http_connection.cpp#L233-L236).

> "After close(), fd_ is -1, so calls degrade to write(-1) which returns EBADF cleanly."

`fd_` is **never** set to -1: `close()` ([http_connection.cpp:655](src/http/http_connection.cpp#L655))
does `::close(fd_)` and leaves the value untouched. Only `file_fd_`, `header_tfd_` and
`timeout_tfd_` are reset.

Today it isn't exploitable because the `_raw_write` lambda checks `closed_` before
writing, and that flag is set before the `::close()`. But the comment documents a
safety net that doesn't exist: if someone adds a write path that trusts the comment
instead of checking `closed_`, they will write to a descriptor the kernel may have
already reassigned to another connection.

---

## What holds up well

It's worth recording what survived the review, because it's substantial:

- **SQL.** All three drivers genuinely parameterize: `sqlite3_bind_*` with `SQLITE_TRANSIENT`
  ([db_sqlite.cpp:214-227](src/lux_script/db_sqlite.cpp#L214-L227)) and `PQexecParams`
  ([db_postgres.cpp:302](src/lux_script/db_postgres.cpp#L302)). Postgres's `?`→`$n`
  rewriter correctly skips string literals, quoted identifiers, line comments, nested
  block comments and **dollar-quoting** (`$tag$...$tag$`) — the case almost everyone
  forgets. `sqlite3_prepare_v2` with a `nullptr` tail also prevents stacked queries.
- **SSTI.** `render()` requires the template name to be a compile-time literal
  ([emitter.cpp:1435-1444](src/lux_script/emitter.cpp#L1435-L1444)), which eliminates
  the entire class of attacker-controlled dynamic templates at the root.
- **JSON parser.** A nesting cap of 200, rejection of unescaped control characters,
  full validation of surrogate pairs, and rejection of trailing garbage after the document.
- **WebSocket.** RSV bits, reserved opcodes, mandatory client→server masking, the high
  bit of the 64-bit length, interleaved fragmentation and control-frame size: all the
  RFC 6455 rules that tend to get skipped are checked. And `origins(...)` is
  **mandatory** on ws routes, with a compile error if missing
  ([project.cpp:1371-1373](src/lux_script/project.cpp#L1371-L1373)) — stricter than
  most frameworks, which leave it optional.
- **`os.run()`** uses `posix_spawnp` with an argv vector, never a shell: metacharacter
  injection is impossible by construction.
- **Static-file traversal.** Double canonicalization (before and after resolving
  symlinks), **component-wise** prefix comparison (not string-based, so
  `/srv/www-evil` doesn't pass for `/srv/www`), dotfile blocking on the already-decoded
  path, and rejection of `%00`. Finding 3 is an implementation bug within an otherwise
  correct design.
- **HTTP pipelining.** The parser's pause/resume serialization and the `in_parser_`
  bracket are carefully reasoned and resolve a real and subtle reentrancy.
- **HMAC/session.** RFC 2104 done correctly, constant-time comparison, and the comment
  explaining why it's fine to branch on `exp` *after* verifying the MAC is accurate.

---

## Suggested order

1. **Finding 1** — the only one with a remote, unauthenticated, full-outage impact, and
   it affects any app that uses the module for what it exists for. Options include
   capping the subject size before calling `std::regex`, moving the module to `is_async`
   with a time budget, or switching to a non-backtracking engine (RE2).
2. **Findings 2 and 3** — a cross-backend differential and a reachable SEGV.
3. **Finding 4** — a small, contained fix in JWT validation.
4. The rest, in table order.

---

## Remediation status

Everything below lives on the `security-fixes` branch, builds cleanly with `./compile.sh`
and passes the existing suite (`ctest`) with no new regressions — the two remaining
failures (`placeholders`/`missing arguments` and `http`/`invalid url`+`connection refused`)
were already failing on `d10f274` before anything was touched, confirmed by rebuilding
that same revision and rerunning them.

### Fixed

- **#1 — `regex.*`.** [module_regex.cpp](src/lux_script/module_regex.cpp) gains a
  4096-byte cap on the SUBJECT text (never the pattern) across all six functions,
  returned as a normal `error` instead of letting `std::regex` recurse until it blows
  the stack. The value comes from measuring the actual crash point with ASan at
  different stack sizes (see the comment next to `kMaxSubjectLength`), with margin for
  thread stacks smaller than glibc's default 8 MB. Verified end-to-end: a 30 KB body
  that used to take down the whole process now gets a 500 with a clear message and the
  server keeps serving every other connection. **Not resolved**: the catastrophic-backtracking
  hang on an already-complex pattern within the limit (`^(a|aa)+$`) — that would require
  replacing `std::regex` with a non-backtracking engine (RE2), out of scope for this patch.
- **#2 — `--native`/bytecode divergence.** [native_gen.cpp](src/lux_script/native_gen.cpp)
  rewrote the generated `lux_route_coerce_int`/`lux_route_coerce_float` to require
  consuming the entire string (as `project.cpp` already did) and reject non-decimal
  forms. Verified with a cross-checked test table between both paths (`12abc`, `0x10`,
  `nan`, `inf`, `1e999`, …): they match on all 13 cases.
- **#3 — SEGV in static files.** [app.cpp](src/app.cpp) replaces the three uses of
  `std::mismatch(root.begin(), root.end(), candidate.begin())` with `path_is_within()`,
  which advances both iterators in parallel and never dereferences past
  `candidate.end()`. Verified with ASan against the exact case that crashed (`root`
  deeper than `candidate`) and with the real binary via a symlink that collapses to a
  directory shorter than the served root: it now returns 403 and the process stays alive.
- **#4 — `verify_jwt` without `iss`.** [auth.cpp](src/lux_script/auth.cpp): with an
  issuer configured, a token missing the `iss` claim is now rejected the same way as
  one with the wrong issuer, instead of slipping through by omission.
- **#5 — `http.*` with no response limit.** [module_http.cpp](src/lux_script/module_http.cpp)
  caps the response body (16 MB, matching the input limit) and headers (64 KB);
  exceeding it aborts the transfer through libcurl's standard mechanism (returning
  fewer bytes than received in the callback) instead of accumulating without bound.
- **#6 — CRLF in `http.*` outgoing headers.** Same file: each header's key and value
  go through a CR/LF/NUL strip before `curl_slist_append`, matching what
  `Response::header()` and `build_set_cookie()` already did on the inbound side. While
  at it, `CURLOPT_PROTOCOLS`/`CURLOPT_REDIR_PROTOCOLS` are now pinned to `http,https`
  (bitmask form, not the `_STR` variant from curl 7.85+, to avoid breaking the build
  with an older libcurl-dev) — a 30x can no longer redirect the request to `file://` or
  another scheme.
- **#7 — unbounded WebSocket `read_buf`.** [websocket.hpp](include/lux/websocket.hpp):
  `feed()` stops accumulating bytes as soon as `closed` is `true`, and the two paths
  that set it without notifying (frame over 16 MB, full message queue) now send a real
  Close (1009/1008) just like every other protocol violation, and free `read_buf`
  immediately instead of waiting for the object to be destroyed.
- **#8 — `float` accepts `nan`/`inf`/hex.** [project.cpp](src/lux_script/project.cpp) and
  [native_gen.cpp](src/lux_script/native_gen.cpp): before calling `std::stod`, any text
  containing `x`/`X`/`n`/`N`/`i`/`I` is rejected — no finite decimal float can contain
  those characters, so the filter has no false positives. The same change was
  replicated on both paths to avoid reopening finding 2.
- **#11 — overridable `Content-Length`/`Transfer-Encoding`.**
  [response.hpp](include/lux/response.hpp): `Response::header()` ignores (with a stderr
  warning) any attempt to set those two headers from a handler — the framework computes
  them and there's never a legitimate reason for a handler to touch them, since Lux
  never emits `chunked`.
- **#12 — formula injection in `csv.*`.** [module_csv.cpp](src/lux_script/module_csv.cpp):
  a field starting with `=`, `+`, `-`, `@`, TAB or CR is prefixed with `'` (the
  standard OWASP mitigation) before RFC 4180 escaping.
- **#13 — `random_bytes` fails silently.** [crypto.cpp](src/lux_script/crypto.cpp) was
  switched from opening `/dev/urandom` to `getrandom(2)` (doesn't depend on a free
  descriptor); it still returns `""` on failure, but now the one consumer that didn't
  check for that case — the anti-collision suffix in `File.save()`,
  [natives.cpp](src/lux_script/natives.cpp) — does too, failing loudly instead of
  retrying with a name that has no real entropy.
- **#17 — false comment about `fd_`.** [http_connection.cpp](src/http/http_connection.cpp):
  the comment now states what actually prevents writes after `close()` (the `closed_`
  check, not `fd_` being -1, which it never is).

### Partial

- **#9 — SSRF.** Protocol restriction on redirects was added (see #6), which closes
  the cheapest way to escape the initial `http://`/`https://`. **Not** added: blocking
  of private/loopback/link-local ranges — doing that properly requires resolving DNS
  and checking the resulting IP (not just looking at the URL string, which an attacker
  works around with a domain that resolves to `127.0.0.1`), and deciding whether it
  should be opt-in or opt-out affects any deployment that legitimately calls an
  internal service from a route. This is a product decision, not a bug with an obvious
  fix — left for the maintainer to decide the policy before enforcing it.

### Not touched (design decision, not a bug)

- **#10 — static files skip middleware.** This is behavior explicitly documented in
  the code itself (`app.cpp`). Changing it alters the framework's contract for every
  existing app that relies on that ordering; not something for me to decide.
- **#14 — no rate limiting.** This is missing functionality, not a defect in existing
  code. Adding one fits better as a separate proposal with its own design (per-IP,
  per-session, fixed window vs. token bucket, etc.) than as part of a batch of fixes.
- **#15 — `/metrics` with no authentication.** The same data Prometheus exposes by
  convention in the vast majority of deployments (protected by network or by the
  scraper itself); forcing auth by default would break any existing `prometheus.yml`
  without an opt-out flag that would need to be designed first.
- **#16 — the VM doesn't check stack/locals bounds.** Only reachable if Lux's own
  emitter generates malformed bytecode, not from an HTTP request. Adding checks
  (`assert`, which isn't used anywhere else in the codebase) at the cost of the
  interpreter's hottest path is a change worth measuring in `bench/` before applying
  it blindly, not something to slip in without data.
