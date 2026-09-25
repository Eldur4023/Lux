#pragma once
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "bytecode.hpp"
#include "value.hpp"

namespace lux {
class Request; class Response; class SSEWriter; class WSConnection;
struct MultipartPart;
}

namespace lux_script {

// A runtime error (division by zero, index out of range, a type mismatch
// the checker could not rule out statically) is answered as a 500 whose
// body used to always include the raw error message AND the exact
// file:line:col it happened at (project.cpp's bytecode routes,
// native_gen.cpp's generated ones) -- useful while developing, but a real
// deployment leaking its own source layout and internal error text to
// whoever sent the request that triggered it is the kind of thing this
// project's own SECURITY-AUDIT.md exists to catch. Both places check this
// ONE function instead of duplicating the decision, so bytecode and
// --native can never disagree about which mode a deployment is in --
// exactly the divergence this codebase's own comments repeatedly call out
// as the class of bug to design away, not just fix once found.
//
// LUX_ENV=production opts in explicitly, the same convention NODE_ENV/
// RAILS_ENV/etc. use -- checked, not assumed, once per call (cheap: one
// getenv() and a string compare) so a test harness that sets/unsets it
// between runs never sees a stale cached answer. Full detail stays in the
// server's own log either way (lux::log().error(...) already runs before
// either branch decides what the CLIENT sees); this only ever narrows what
// leaves the process, never what an operator can see.
inline bool is_production_mode() {
    const char* env = std::getenv("LUX_ENV");
    return env && std::string(env) == "production";
}

// Context the VM passes to the builtins: it is the layer-2 bridge, where the
// bytecode reaches the native engine.
// Store shared across ALL the event loop threads.
//
// It is the only shared-state path: every VM has its own stack and heap and
// shares nothing with the others.  That is why it exposes operations and not
// properties — `state.x = state.x + 1` would race the read against the write.
class SharedState {
public:
    static SharedState& instance();

    // ttl_ms > 0 makes the key expire. incr() sets it only when it creates
    // the key, so `state.incr("login:" + ip, 1, 60000)` counts in fixed
    // one-minute windows: a rate limiter in one line.
    long long incr(const std::string& key, long long by, long long ttl_ms = 0);
    Value     get(const std::string& key);
    void      set(const std::string& key, Value v, long long ttl_ms = 0);
    bool      remove(const std::string& key);

private:
    struct Entry { Value v; long long expires_ms = 0; };
    using Map = std::map<std::string, Entry>;

    Map::iterator live(const std::string& key, long long now);   // end() if absent or expired
    void          sweep(long long now);

    std::mutex mutex_;
    Map        data_;
    unsigned   writes_ = 0;
};

// Session state during a request.
struct SessionState {
    Value::Dict data;
    bool        loaded = false;   // the cookie has already been read once
    bool        dirty  = false;   // the handler modified it: it must be rewritten
    std::string secret;           // empty = sessions not configured
};

struct Template;

struct NativeCtx {
    lux::Request&  req;
    lux::Response& res;

    // Compiled templates of the module.  They are compiled at startup, so
    // rendering is walking them, not parsing them.
    const std::vector<Template>* templates = nullptr;

    // User function table of the module.  The expressions of a template need it
    // to be able to call a function from the .lux.
    const FunctionTable* functions = nullptr;

    // Only on sse routes: the stream writer, created by the driver before
    // starting the VM.  Null on the rest, and the sse builtins check it.
    lux::SSEWriter* sse = nullptr;

    // Transaction in progress per module: name -> pinned worker.  While it
    // exists, every query of that module goes through the same connection.
    std::map<std::string, int> pinned_workers;

    // Worker that ran the last exec of each module.  `last_id()` has to go
    // through that SAME connection: the generated identifier does not exist on
    // the others.
    std::map<std::string, int> last_exec_workers;

    // Modules whose CURRENT transaction already had a statement fail. See
    // the comment on `poisoned` in db.hpp's await_db() -- commit() on a
    // poisoned module rolls back instead of committing, and every other
    // call refuses outright until rollback()/commit() closes it.
    std::set<std::string> poisoned_db;

    // Multipart parts already parsed.  A File in the language keeps the index
    // of its part here, not the bytes: copying a File is copying an int.
    const std::vector<lux::MultipartPart>* parts   = nullptr;
    bool                                      uploads = false;

    // Only on ws routes: the connection already established.
    lux::WSConnection* ws = nullptr;

    // Session: signed cookie, no server-side state.  It is loaded lazily on
    // first access and only rewritten if the handler modifies it.
    SessionState* session = nullptr;

    // Only in an `on error` handler: the code, the reason, and —when the error
    // comes from validating a body— the complete list of messages.
    int                             error_code    = 0;
    std::string                     error_message;
    const std::vector<std::string>* error_messages = nullptr;

    // Claims of the Authorization: Bearer JWT, already verified by the driver.
    const Value* jwt_claims = nullptr;
    bool         jwt_ok     = false;

    // Set by any builtin that writes the response (text, render, redirect...).
    // If it is still false when the handler ends, the returned value is
    // serialized as JSON.
    bool response_written = false;
};

// A builtin returns a Value and, if it fails, writes the reason into `error`.
using NativeFn = Value (*)(NativeCtx& ctx, std::vector<Value>& args,
                           std::string& error);

struct NativeDef {
    const char* name;
    int         min_args;
    int         max_args;    // -1 = no limit
    NativeFn    fn;          // null if it is asynchronous
    bool        is_async = false;
};

// Builtins that suspend.  The VM does not run them: it hands them back to the
// driver, which is the one that can do a real co_await on the engine.  The
// identifier is the table index itself, so it is looked up by name only once.

// Stable index of the builtin in the table, or -1 if it does not exist.  The
// emitter stores it in the instruction, so the VM never looks it up at runtime.
int              native_id(const std::string& name);

// Resolves `object.member` of a reserved object (sse.send, sse.open...) to the
// builtin implementing it, or -1 if that combination does not exist.  The
// mapping lives next to the table so adding a member is touching one place.
int              member_native_id(const std::string& object, const std::string& member);

// Method on a value: "hi".starts_with(...), list.add(...), file.save(...).
// Unlike the reserved objects, the receiver is known at runtime, so the
// dispatch is by type inside the VM.
Value call_method(NativeCtx& ctx, Value& receiver, const std::string& name,
                  std::vector<Value>& args, std::string& error);

// One of the methods call_method() recognizes, with the arguments it takes and
// what it returns.
struct BuiltinMethod {
    const char* name;
    int         min_args;
    int         max_args;
    // Type of the result, or nullptr if it returns the receiver itself —which
    // is what the chainable ones do: .status(), .add()...—.  Knowing it allows
    // checking to continue past the dot: s.upper().trim() also fails at
    // compile time.
    const char* return_type;
};

// The methods that exist for a Lux Script type: "string", "int", "List", ...
//
// Returns nullptr when the type has no closed list —a user class, or a value
// whose type is only known at run time— and then there is nothing to check at
// compile time.
//
// The list lives glued to call_method() on purpose: they are two sides of the
// same thing, and adding a method has to be touching one place.
const std::vector<BuiltinMethod>* methods_of(const std::string& type);
bool             is_reserved_object(const std::string& name);
bool             is_db_module(const std::string& name);

// Messages of the last failed validation on this thread.  They are filled when
// building the 422 and read by the `on error` handler of the same request;
// there is no suspension between the two moments, so requests cannot cross.
std::vector<std::string>& last_validation_messages();

// The real message of the last runtime error (division by zero, index out
// of range, ...) on this thread -- same idea and same one-request lifetime
// as last_validation_messages() above, filled right before the route's own
// 500 body is written (project.cpp's bytecode routes, native_gen.cpp's
// generated ones) and read by app.on_error() (main.cpp) to fill
// `error.message` in a user-declared `on error 500`/`on error` handler.
// Before this existed, `error.message` there was hardcoded to the string
// "internal error" UNCONDITIONALLY -- not gated by is_production_mode() at
// all -- so GUIDE.md's own example (`log.error(error.message)`) never
// logged anything a developer could use to find the actual bug, even
// outside production. Still gated by is_production_mode() at the one
// place that reads it (main.cpp), same as the raw 500 body already is:
// this function always holds the real message, but production mode never
// looks at it.
std::string& last_internal_error();
const NativeDef& native_at(int id);

} // namespace lux_script
