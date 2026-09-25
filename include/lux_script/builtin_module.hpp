#pragma once
#include <map>
#include <memory>
#include <chrono>
#include <mutex>
#include <random>
#include <unordered_map>
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
    // Argument types, checked by call() before fn runs, so a function body
    // can trust them: s string, i int, n int or float, b bool, l List,
    // d Dict, f function, x anything; uppercase also accepts null; after
    // '|' the rest are optional; a trailing '*' takes any number more.
    std::string sig;
    std::string full_name;    // "hash.sha256", filled in by the registry

    BuiltinModuleFn(std::string n, int min, int max, NativeFn f, bool async = false)
        : name(std::move(n)), min_args(min), max_args(max), fn(f), is_async(async) {}
    // min/max come from the signature: {"hmac_sha256", "ss", fn}
    BuiltinModuleFn(std::string n, const char* signature, NativeFn f, bool async = false);

    Value call(NativeCtx& ctx, std::vector<Value>& args, std::string& error) const;
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

// ─── Drop-in registration ────────────────────────────────────────────────────
//
// A module used to need THREE edits spread across the project: write the
// .cpp, declare its factory function and add it to BuiltinModuleRegistry's
// constructor in builtin_module.cpp, and list the .cpp in CMakeLists.txt.
// The middle one is what LUX_REGISTER_MODULE below removes -- combined with
// CMakeLists.txt globbing src/lux_script/modules/ and
// src/lux_script/modules/base_modules/ (see NATIVE-MODULES.md), adding a
// module with no third-party dependency is now just "drop a .cpp in that
// folder": nothing else in the project changes.
//
// Mechanism: each module .cpp ends with one line, `LUX_REGISTER_MODULE(X)`,
// which defines a namespace-scope object whose constructor pushes a factory
// for X into module_factories() below. BuiltinModuleRegistry's constructor
// (builtin_module.cpp) then just calls every registered factory and reads
// each one's own name() -- the module's class is the single source of
// truth for the name `import` uses, never a second string that could drift
// from it.
//
// Why a Meyer's singleton (a function-local `static`) and not a plain
// namespace-scope `std::vector`: relying on the order two DIFFERENT
// translation units' namespace-scope objects get constructed in is
// undefined behavior in C++ ("static initialization order fiasco") --  a
// module's LUX_REGISTER_MODULE object and a plain global vector it tries to
// push into are exactly two such objects, with no guarantee the vector
// exists yet when a module registers into it. A function-local static
// sidesteps that: it is constructed lazily, on its FIRST call, no matter
// who makes that call or when -- including a call made from inside another
// namespace-scope object's own constructor, which is exactly what
// LUX_REGISTER_MODULE's registrar does below.
namespace detail {

using ModuleFactory = std::unique_ptr<BuiltinModule> (*)();

// Every LUX_REGISTER_MODULE in the binary appends itself here before
// main() runs. BuiltinModuleRegistry's constructor drains this list.
std::vector<ModuleFactory>& module_factories();

struct ModuleRegistrar {
    explicit ModuleRegistrar(ModuleFactory f) { module_factories().push_back(f); }
};

} // namespace detail

// A module .cpp with NOTHING ELSE referenced from outside its own
// translation unit (true of every module: the class lives in an anonymous
// namespace, and the only thing calling its constructor is this macro,
// right here, in the same file) can end up as a member of liblux_script.a
// that the final linker discards entirely -- an .a is linked member by
// member, and the linker only keeps one if something ALREADY pulled in
// already references one of its symbols. CMakeLists.txt's
// `lux_link_script()` function compensates with `--whole-archive` on every
// target that links lux_script, specifically so this stays true: dropping
// a module .cpp in the folder and doing nothing else is enough for it to
// actually run, not just to compile.
#define LUX_REGISTER_MODULE(ClassName)                                        \
    namespace {                                                               \
    ::lux_script::detail::ModuleRegistrar lux_module_registrar_##ClassName(   \
        +[]() -> std::unique_ptr<::lux_script::BuiltinModule> {               \
            return std::make_unique<ClassName>();                             \
        });                                                                   \
    }

// The common case, a module that is just a name and a function table:
//
//     LUX_MODULE(hash, {
//         {"sha256", 1, 1, fn_hash_sha256},
//     })
//
// A module that needs configure() still writes its class and uses
// LUX_REGISTER_MODULE.
namespace detail {
class TableModule final : public BuiltinModule {
public:
    TableModule(const char* name, std::vector<BuiltinModuleFn> fns) : name_(name), fns_(std::move(fns)) {}
    const char*                         name() const override { return name_; }
    const std::vector<BuiltinModuleFn>& functions() const override { return fns_; }
private:
    const char*                  name_;
    std::vector<BuiltinModuleFn> fns_;
};
} // namespace detail

#define LUX_MODULE(Name, ...)                                                 \
    namespace {                                                               \
    ::lux_script::detail::ModuleRegistrar lux_module_registrar_##Name(        \
        +[]() -> std::unique_ptr<::lux_script::BuiltinModule> {               \
            return std::make_unique<::lux_script::detail::TableModule>(       \
                #Name, std::vector<::lux_script::BuiltinModuleFn> __VA_ARGS__); \
        });                                                                   \
    }

// Integer handles for module objects that outlive a request (csv tables,
// pdf documents). Mutex-protected: every event-loop thread can reach them.
//
// An id is random (53 bits: exact in JSON, and no route can guess another
// user's document by counting), and an entry nobody touched for `idle` is
// dropped, so a handler that forgot close() does not leak it for good.
template <class T>
class HandleTable {
public:
    using Clock = std::chrono::steady_clock;
    explicit HandleTable(Clock::duration idle = std::chrono::minutes(10)) : idle_(idle) {}

    long long put(std::unique_ptr<T> v) {
        std::lock_guard<std::mutex> lock(mutex_);
        sweep_locked();
        long long id;
        do id = random_id(); while (items_.count(id));
        items_.emplace(id, Entry{std::shared_ptr<T>(std::move(v)), Clock::now()});
        return id;
    }
    long long put(T v) { return put(std::make_unique<T>(std::move(v))); }

    // Kept alive by the caller's shared_ptr even if close() runs meanwhile.
    std::shared_ptr<T> get(long long id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = items_.find(id);
        if (it == items_.end()) return nullptr;
        it->second.used = Clock::now();
        return it->second.value;
    }
    bool close(long long id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.erase(id) > 0;
    }

private:
    struct Entry { std::shared_ptr<T> value; Clock::time_point used; };

    static long long random_id() {
        thread_local std::mt19937_64 rng{std::random_device{}()};
        return static_cast<long long>(rng() & ((1ULL << 53) - 1)) | 1;
    }
    void sweep_locked() {
        const auto now = Clock::now();
        std::erase_if(items_, [&](const auto& kv) { return now - kv.second.used > idle_; });
    }

    Clock::duration                         idle_;
    std::mutex                              mutex_;
    std::unordered_map<long long, Entry>    items_;
};

} // namespace lux_script
