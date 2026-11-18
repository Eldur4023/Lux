#pragma once
// A per-request memory budget for the VM.
//
// The VM already caps steps (kStepLimit) and call depth, but a handler can still allocate without
// bound with a handful of steps: s = s + s forty times, a list of a million rows, a 2 GB repeat.
// One such request takes the whole process down (the OOM killer), and every other connection with it.
//
// The budget counts the bytes a handler CREATES (concatenation growth, list/dict growth, what a
// builtin or a module returns), not what it keeps alive: precise liveness would need a GC's
// bookkeeping. Created >= alive, so the cap is an upper bound on what one request can hold.
// `max_memory` in app: sets it (default 512MB, "0"-style unlimited is not offered on purpose).
// ponytail: --native code does not go through the VM and is not counted; add charge calls to
// native_gen's runtime if native routes need it.
#include <cstddef>
#include <stdexcept>

namespace lux_script {

struct MemBudget {
    size_t limit = 0;   // 0 = not armed
    size_t used  = 0;
};

inline MemBudget*& current_budget() {
    static thread_local MemBudget* b = nullptr;
    return b;
}

// Throws std::runtime_error when the running handler is over its budget; the VM turns that into a
// language error (a `try` can catch it, otherwise the request fails with a 500).
inline void charge_memory(size_t n) {
    MemBudget* b = current_budget();
    if (!b || !b->limit) return;
    b->used += n;
    if (b->used > b->limit)
        throw std::runtime_error("request memory limit exceeded (" + std::to_string(b->limit >> 20) +
                                 " MB: max_memory in app:)");
}

} // namespace lux_script
