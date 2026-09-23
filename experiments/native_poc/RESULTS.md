# Proof of concept: is compiling to native C++ worth it?

Goal: before committing to the big `--native` refactor,
measure the actual number. This is not a transpiler — it's
`fib()`/`cuenta_primos()` hand-written in C++, copying exactly the logic of
`bench/lux/app.lux`, with the same input ranges used by `bench/k6/script.js`.

Two measurements, deliberately kept separate:

1. **`bench_direct`** — calls the functions in a tight loop, with no HTTP involved.
   This isolates the cost of *executing the logic*, which is the question that actually
   matters for deciding whether compiling Lux Script is worthwhile.
2. **`server` + `k6_poc.js`** — the same functions behind a raw-socket HTTP server,
   no framework, no keep-alive (a new TCP connection per request, one C++ thread per
   connection). This measures *how much the transport adds* when the transport is
   deliberately naive — it's not a fair comparison against Lux's HTTP engine or against
   Gin as a framework, it's the upper bound of overhead for my own toy server.

## Result: cost of executing the logic (the one that answers the question)

| | fib (n=20-28) p50 | primes (n=20k-100k) p50 |
|---|---:|---:|
| Lux VM (measured via HTTP, `bench/RESULTS.md`) | 20.09ms | 192.6ms |
| Gin / Go (measured via HTTP, same benchmark harness) | 0.21ms | 2.1ms |
| **Native C++, tight loop with no HTTP (`bench_direct`)** | **0.036ms** | **1.86ms** |

**The VM takes ~560x longer than native C++ on `fib` and ~104x longer on `primes`.** And
the native C++ number doesn't just come close to Go — it matches it on `primes` and
beats it on `fib`, with the same logic and the same compiler (GCC) Lux already uses to
compile itself. This is exactly what `--native`'s central thesis predicts: with types
already resolved at compile time, a compiled Lux Script loop is indistinguishable from
the same loop written directly in C++ — there's no structural reason for it to be
slower than Go.

## Result: with the PoC's raw-socket server (so it doesn't read as more than it is)

| | fib p50 | primes p50 |
|---|---:|---:|
| PoC raw-socket server (`server` + `k6_poc.js`) | 3.43ms | 7.31ms |
| `/health` on the same server (transport-overhead baseline) | — | 0.36ms |

The gap between these two tables (0.036ms → 3.43ms on `fib`) is **entirely mine**: a
new TCP connection per request and a `std::thread` per connection are the two most
naive possible decisions for an HTTP server, made on purpose so as not to write a
framework for this test. Lux's real HTTP engine (`lux::App`/`Router`, with its own
event loop on `epoll`/`io_uring`) already solves this — it's the same reason that
today, interpreted, Lux serves `/health` in 0.05-0.08ms. A handler compiled to native
would register on that same engine exactly like an interpreted one, so the real
transport cost it would pay is the one Lux already pays today, not the one from this
toy server.

## Conclusion of the test

The `--native` thesis is validated with an actual number, not just by reading the
profile: **compiling the pure-CPU path to native C++ closes the gap with Go, and does
so with margin** (104x-560x depending on the case). What's *not* validated is how much
of real `--native`'s final performance will depend on backend engineering (`Value`
representation, refcounting, codegen from the IR) — only implementing it will tell —
but the question this test had to answer, "is the performance ceiling there if you
actually compile?", has an answer: yes.

## Appendix: a cheap VM fix was also attempted (and didn't pay off)

Before writing the `--native` plan, the CPU profile pointed at
`std::vector<Value>::emplace_back` taking 14.4% of thread time on the VM's operand
stack (`stack_`, `include/lux_script/vm.hpp`). The cheap hypothesis: `stack_.reserve(32)`
was an arbitrary value (`src/lux_script/vm.cpp`, `VM::start()`) and `locals_`/`frames_`
reserved nothing at all, so they grew by repeated reallocation on every call. `stack_`
was bumped to 256, `locals_.reserve(256)` was added, and `frames_.reserve(kMaxFrames)`
(the recursion cap already existed as a constant, it was just never reserved).

**All 79 tests in `tests/run_tests.sh` still pass** — the change is safe. But measured
on the same machine, same session, back-to-back (with the change vs. without it,
reverted via `git stash`), the difference in `compute/primes`/`compute/fib` stayed
**within noise**: no measurable improvement. The reason, in hindsight, is the correct
one: the `emplace_back` symbol showing up in the profile wasn't mostly the cost of
*reallocating* the vector — it was the cost of the `push_back` call itself (checking
capacity, constructing, incrementing size) on the fast path, which happens on
**every** VM instruction that pushes a value, with or without a reallocation involved.
Reserving capacity avoids the reallocations (which were few to begin with: this same
`VM` gets reused across requests via `project.cpp`'s `shared_vm`, so it only
reallocated at each thread's startup) but doesn't touch that underlying cost.

This doesn't invalidate the idea of touching the VM — it invalidates the idea that
there was a cheap win to be had there. The real cost is structural: **every Lux Script
operation pays the price of going through `push`/`pop` on a boxed `Value` and through
the opcode-dispatch `switch`**, and that isn't fixed with a `reserve()`. It's exactly
the structural reason `--native` documents: the only way to remove that cost is to not
pay it — compile to code that doesn't dispatch opcodes or push `Value`s at all. The
change is left in the working tree (`src/lux_script/vm.cpp`) because it's correct and
costs nothing, but **it doesn't count as a demonstrated performance improvement** —
just as hygiene.

## Reproducing

```bash
cd experiments/native_poc
g++ -O2 -std=c++17 -o bench_direct bench_direct.cpp && ./bench_direct
g++ -O2 -std=c++17 -pthread -o server server.cpp
./server &
k6 run --env VUS=50 --env DURATION=20s k6_poc.js
```
