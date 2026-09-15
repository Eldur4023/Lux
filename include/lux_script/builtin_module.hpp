#pragma once
#include <map>
#include <string>
#include <vector>

#include "natives.hpp"

namespace lux_script {

// A native module -- Lux's equivalent of a Python C-extension: a chunk of
// C++ exposed to Lux Script under `import <name>`, compiled into the
// `lux` binary (not loaded dynamically -- see NATIVE-MODULES.md for why).
//
// Most functions are SYNCHRONOUS (no `await`, no worker pool) and share the
// exact calling convention of any other builtin (NativeFn, natives.hpp) -- a
// module function IS a builtin, just namespaced under an `import`ed name
// instead of always being present. That is what lets it reuse
// Op::CallBuiltinModule's dispatch (vm.cpp) as a near copy of Op::CallNative's,
// instead of inventing a second calling convention.
//
// `is_async` is the opt-in escape hatch for the functions whose cost is NOT
// microseconds-regardless (NATIVE-MODULES.md §6 named this as "the clearest
// concrete next phase" after that document shipped): os.run()/read_file()/
// write_file() do real, unbounded disk or child-process I/O, and every
// http.* call waits on a remote server. Marking one `is_async = true` makes
// `await <module>.<fn>(...)` mandatory (checked in Emitter::check_call,
// mirroring DbModuleCall) and routes it through Op::CallAsyncModule instead
// of Op::CallBuiltinModule: the driver (project.cpp's run_builtin_module_async)
// runs the call on lux::blocking_pool() -- the SAME shared pool a
// synchronous route with no `await` already uses -- instead of inline on the
// event loop thread, so a slow disk write or a hung remote server no longer
// pins the whole core for its duration. Same error convention `await
// <db-module>.*` already established: a failure comes back as
// `{"error": message}`, never a hard failure of the handler -- see
// run_builtin_module_async's comment for why that, and not `fail()`, is the
// right choice here.
struct BuiltinModuleFn {
    std::string name;         // "sha256" (used as `hash.sha256(...)`)
    int         min_args;
    int         max_args;     // -1 = no limit
    NativeFn    fn;           // same signature as any other builtin
    bool        is_async = false;
};

// What a module needs to describe itself.  `configure()` is optional --
// most modules (hash, and most stdlib-shaped modules generally) need no
// `<name>: { ... }` block in `app:` at all; a future module that DOES need
// one (an API key, a directory, a limit) overrides it exactly like
// DbDriver::configure().
class BuiltinModule {
public:
    virtual ~BuiltinModule() = default;

    virtual const char*                        name() const = 0;
    virtual const std::vector<BuiltinModuleFn>&  functions() const = 0;

    // Called once, right after `import`, with whatever `<name>: { ... }`
    // block exists in `app:` (empty if there is none).  The default accepts
    // silently -- a module with nothing to configure does not have to
    // override this just to say so.
    virtual bool configure(const std::map<std::string, std::string>& options,
                           std::string& error) {
        (void)options; (void)error;
        return true;
    }
};

// ─── Registry ──────────────────────────────────────────────────────────────
//
// Mirrors DbRegistry (db.hpp) on purpose: which modules exist depends on
// cmake options (see NATIVE-MODULES.md, "adding a module" -- most modules
// need none, since they carry no external dependency), so an `import` of one
// not compiled in has to say so plainly, the same way a missing DB driver
// already does.
//
// Unlike DbRegistry, there is no PER-CONNECTION pool and no `activate()`
// step tied to a thread count: an `is_async` function runs on the process's
// one shared lux::blocking_pool() (any free worker, no pinning -- unlike a
// DB connection, an os.run()/http.get() call has no per-call state that
// needs to stick to the same worker across calls), not a pool this registry
// owns or starts. A plain (non-async) function still runs inline, on
// whatever thread calls them (the event loop thread, same as any other
// builtin) -- there is nothing to start.
class BuiltinModuleRegistry {
public:
    static BuiltinModuleRegistry& instance();

    std::vector<std::string> available() const;
    bool                     has(const std::string& name) const;

    // Marks a module imported: runs its configure() with the `app:` block
    // (or an empty map if there is none) and remembers it as active so a
    // stray function call before `import` is still caught.
    bool activate(const std::string& name,
                  const std::map<std::string, std::string>& options,
                  std::string& error);
    bool is_active(const std::string& name) const;

    // Resolves `<module>.<function>` to its BuiltinModuleFn, or nullptr.
    // Used at COMPILE time (Emitter::check_call) to validate the call and
    // assign it a flat, stable id -- see builtin_module_function_at() below,
    // which is what the id from THIS lookup indexes into.
    const BuiltinModuleFn* find(const std::string& module, const std::string& function) const;

    // The same lookup, but returning the flat id CallBuiltinModule's operand
    // carries (module functions across every registered module share one
    // id-space, exactly like native_id() does for the core builtins) --
    // -1 if the pair does not exist.
    int id_of(const std::string& module, const std::string& function) const;

    // The flat-id counterpart of find(): what CallBuiltinModule's handler
    // (vm.cpp) actually calls at runtime. Out of range is a logic error, not
    // guarded here -- see builtin_module_function_at() below.
    const BuiltinModuleFn& function_at(int id) const { return flat_[static_cast<size_t>(id)].fn; }

private:
    BuiltinModuleRegistry();

    struct Slot {
        std::unique_ptr<BuiltinModule> module;
        bool                          activated = false;
    };
    std::map<std::string, Slot> slots_;

    // Flat table every id_of()/builtin_module_function_at() indexes into,
    // built once at construction from every registered module's
    // functions(), in registration order.
    struct FlatEntry {
        std::string      module;
        BuiltinModuleFn    fn;
    };
    std::vector<FlatEntry> flat_;

    void build_flat_table();
};

// Stable id -> BuiltinModuleFn, the CallBuiltinModule counterpart of
// native_at() (natives.hpp).  Out-of-range is a logic error (the emitter
// only ever hands out ids id_of() returned), so it is not defensive here,
// exactly like native_at() is not.
const BuiltinModuleFn& builtin_module_function_at(int id);

} // namespace lux_script
