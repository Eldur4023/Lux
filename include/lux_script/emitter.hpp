#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.hpp"
#include "bytecode.hpp"
#include "diagnostic.hpp"
#include "ir.hpp"
#include "type.hpp"

namespace lux_script {

// Translates the body of a route into bytecode.
//
// It resolves names to local slots at compile time, so the VM never looks a
// variable up by name: LoadLocal is a direct index.
// What the emitter needs to know about a user function in order to call it:
// where it is and what arguments it takes.
struct FnSig {
    size_t index    = 0;
    size_t required = 0;                    // parameters without a default value
    std::vector<const Expr*> defaults;      // one per parameter; null if it has none
    // The function's/method's own declared return type -- what
    // Emitter::type_of() needs to know the type of a CALL to it (so that
    // chaining straight off the call, `f().campo`, resolves the same way
    // chaining off a variable already does). Unknown() for a constructor's
    // synthetic FnSig-less path (see build_class_signatures(): ctors are
    // looked up by arity, never through this struct at all, so there is
    // nothing to fill in here for them).
    Type return_type = Type::unknown();
};
using FunctionSigs = std::map<std::string, FnSig>;

// What the emitter needs to know about a class to build it and call its
// methods.  Both compile to functions with `this` as the first parameter.
struct ClassSig {
    std::map<std::string, FnSig> methods;
    std::map<size_t, size_t>     ctors;    // number of parameters -> index
    std::vector<std::string>     fields;
};
using ClassSigs = std::map<std::string, ClassSig>;

// What the emitter needs to know about an `enum`: its name, and the set of
// member names `Name.MEMBER` may resolve to -- resolving to the member's
// own name as a plain string constant (EnumDecl's comment, ast.hpp), not an
// index, so this is all the checker needs to validate access and nothing
// else has to carry a "the enum's runtime representation" concept at all.
using EnumSigs = std::map<std::string, std::set<std::string>>;

struct Template;

// A name visible inside a standalone expression, with its declared type.
//
// The type may be empty, and then nothing is checked about it: that is what
// happens with the variable of a {% for %}, whose type depends on what the
// list carries.
struct TypedName {
    std::string name;
    std::string type;

    // No type is the normal case, so only the name is written.  The literal
    // needs its own constructor: from const char* to TypedName there are two
    // conversions and the compiler only does one.
    TypedName(std::string n, std::string t = {})
        : name(std::move(n)), type(std::move(t)) {}
    TypedName(const char* n) : name(n) {}
};

// Where the templates are and where they are stored once compiled.
//
// When the emitter sees a render("x.html", k=v) with a literal name, it
// compiles the template RIGHT THERE against those keys.  That is why a typo
// inside a {{ }} shows up in `lux --check` and not when someone loads the page.
struct TemplateCtx {
    std::string             dir;
    std::vector<Template>* table = nullptr;
};

class Emitter {
public:
    // `functions` maps a user function name to its index in the module table.
    // It is resolved while emitting, so the VM never looks one up by name.
    explicit Emitter(DiagnosticBag& diags, const FunctionSigs* functions = nullptr,
                     const ClassSigs* classes = nullptr,
                     const std::set<std::string>* imports = nullptr,
                     TemplateCtx* templates = nullptr,
                     const EnumSigs* enums = nullptr)
        : diags_(diags), functions_(functions), classes_(classes), imports_(imports),
          templates_(templates), enums_(enums) {}

    // The route parameters take the first slots, in order.
    // Returns false if something in the body cannot be compiled yet.
    bool emit_route(const RouteDecl& route, Chunk& out);

    // Body of a user function.
    bool emit_function(const FnDecl& fn, Chunk& out);

    // Method and constructor: they compile as functions with `this` as the
    // first parameter, so they reuse the VM frame stack with nothing special.
    bool emit_method(const std::string& cls, const FnDecl& m, Chunk& out);
    bool emit_ctor(const std::string& cls, const std::vector<std::string>& fields,
                   const CtorDecl& ct, Chunk& out);

    // Compiles a standalone expression with `names` already declared as locals,
    // in that order.  Used by the `validate` rules —each one becomes a tiny
    // chunk that takes the fields and returns a boolean— and by the expressions
    // inside a {{ }} in a template.
    //
    // The types travel with the names so that `name.mayusculas()` can be
    // rejected here: without them the emitter does not know `name` is a string
    // and the typo is discovered in production.
    bool emit_condition(const Expr& e, const std::vector<TypedName>& names,
                        Chunk& out);

    // Body of an `on error`: no parameters, with the `error` object available.
    bool emit_error_handler(const ErrorDecl& decl, Chunk& out);

    // ── Phase 1 of --native: parallel checker ─────────────────────────────────
    //
    // Reproduces ALL the checks that emit_expr/emit_call perform (same error
    // text, same order), but without touching chunk_ or locals_: it emits no
    // bytecode and declares no slot (the only declare_local() calls today
    // are codegen temporaries -- PreStep/PostStep over a field or an index
    // -- that a check-only pass doesn't need). That's why it can run over
    // the REAL `this` of an in-progress compilation, in any order relative
    // to emit_expr/emit_call, without corrupting the slot numbering: it's a
    // reader of locals_, never a writer.
    //
    // Errors go to `shadow`, NEVER to diags_: it isn't yet the source of
    // diagnostics (that comes when the real cutover connects this to
    // Emitter), so an error from here must not duplicate the one
    // emit_expr/emit_call already produces on its own.
    //
    // Besides checking, it BUILDS and returns the corresponding IrExpr (null
    // if something failed to compile -- shadow.error was already called at
    // the exact spot). The type of each node is type_of(e), with no
    // exception: never a more precise type invented here, because that
    // would be new functionality and not a reproduction of what the
    // compiler already does. Public because the verification (comparing
    // shadow against the real diags_, and the shape of the returned IrExpr,
    // over the tests/cases corpus) lives in a separate test binary; nothing
    // in the real compiler calls this yet.
    IrExprPtr check_expr(const Expr& e, DiagnosticBag& shadow) const;
    IrExprPtr check_call(const Expr& e, bool awaited, DiagnosticBag& shadow) const;

    // Counterpart of check_expr for emit_condition: same reset of
    // locals_/route_method_/scope_depth_, same declarations of `names`, but
    // calling check_expr instead of emit_expr.
    //
    // It takes its own `Chunk& out` -- just like emit_condition -- even
    // though it doesn't emit a single opcode: declare_local() writes
    // chunk_->num_locals as it goes (the same accounting the VM needs to
    // size the stack, the real one, not a copy), so it needs a valid chunk_
    // from the very first moment and can't depend on someone having already
    // called emit_condition on the same Emitter to have set it up. Passing
    // the SAME chunk that was already passed to emit_condition (which is
    // what project.cpp's canary does today) is valid: declare_local()
    // re-registers the same names, but num_locals can no longer rise beyond
    // what it already rose to, so nothing observable changes.
    IrExprPtr check_condition(const Expr& e, const std::vector<TypedName>& names,
                              Chunk& out, DiagnosticBag& shadow);

    // check_stmt/check_block: the same idea as check_expr, but for
    // statements -- they reproduce emit_stmt/emit_block branch by branch and
    // build the corresponding IrStmt/IrBlock (null/empty on error, same
    // criterion as check_expr). Unlike check_expr, they DO declare slots
    // (VarDecl, the desugared `for`, a `catch`'s name): those ARE real name
    // accounting, not a codegen temporary, and the checker needs to carry it
    // so that a later statement's Ident resolves correctly. By construction
    // it cannot run over the `this` of a real emission in progress (it
    // would stomp on its slots) -- hence check_route/check_function/etc. as
    // their own entry points, each resetting state exactly like its emit_*
    // counterpart, so they can be called in sequence on the same Emitter
    // (first the real path, then the shadow) without interfering.
    IrStmtPtr check_stmt(const Stmt& s, DiagnosticBag& shadow);
    // Empty if some statement in the block failed (already reported at its
    // spot); same as today with `failed_`, a half-built block isn't used.
    IrBlock   check_block(const Block& body, DiagnosticBag& shadow);

    // Pure emitter that consumes the IrBlock returned by
    // check_block/check_route/etc.: it checks nothing, it trusts that the IR
    // already went through the checker. Public (unlike emit_stmt/emit_expr/
    // emit_call over IrExpr/IrStmt, which are private and internal to this
    // one) because tests/emit_ir_shadow.cpp calls it directly to test that
    // the bytecode it produces BEHAVES the same as the old emitter's,
    // actually run on the VM -- it's exactly what emit_function will do once
    // connected, so the test replicates it from outside.
    void emit_block(const IrBlock& body);

    // `out`: same reason as in check_condition (declare_local needs a valid
    // chunk_). `out_body`, if not null, receives the built IrBlock (the same
    // one that project.cpp already discards today: check_route/etc. use it
    // only to know whether `shadow` grew).
    bool check_route(const RouteDecl& route, Chunk& out, DiagnosticBag& shadow,
                     IrBlock* out_body = nullptr);
    bool check_function(const FnDecl& fn, Chunk& out, DiagnosticBag& shadow,
                        IrBlock* out_body = nullptr);
    bool check_method(const std::string& cls, const FnDecl& m, Chunk& out,
                      DiagnosticBag& shadow, IrBlock* out_body = nullptr);
    bool check_ctor(const std::string& cls, const std::vector<std::string>& fields,
                    const CtorDecl& ct, Chunk& out, DiagnosticBag& shadow,
                    IrBlock* out_body = nullptr);
    bool check_error_handler(const ErrorDecl& decl, Chunk& out, DiagnosticBag& shadow,
                             IrBlock* out_body = nullptr);

private:
    DiagnosticBag&                       diags_;
    const FunctionSigs*                  functions_ = nullptr;
    const ClassSigs*                     classes_   = nullptr;
    const std::set<std::string>*         imports_   = nullptr;
    TemplateCtx*                        templates_ = nullptr;
    const EnumSigs*                      enums_     = nullptr;
    Chunk*         chunk_ = nullptr;
    bool           failed_ = false;

    // Method of the route being compiled: lets `sse.*` be rejected outside an
    // sse route at compile time, instead of leaving it to runtime.
    std::string route_method_;

    // The declared type is kept so `u.method()` can be resolved at compile
    // time: at runtime an instance is a Dict and would be indistinguishable.
    struct Local { std::string name; int depth; Type type; };
    std::vector<Local> locals_;
    int                scope_depth_ = 0;

    // Pending jumps of the loop in progress.  Both are patched when it closes:
    // in a `for` the target of `continue` is the increment, which has not been
    // emitted yet when the `continue` appears inside the body.
    struct LoopCtx {
        std::vector<size_t> breaks;
        std::vector<size_t> continues;
    };
    std::vector<LoopCtx> loops_;

    void error(SourceLoc loc, std::string msg);

    int  declare_local(const std::string& name, SourceLoc loc,
                       Type type = Type::unknown());
    const Type& local_type(const std::string& name) const;

    // Collects the operands of a '+' chain, on the raw AST — used by
    // check_call to look for request data glued directly into SQL, before
    // any IR exists yet.
    static void flatten_concat(const Expr& e, std::vector<const Expr*>& out);
    // True if `e` is, or concatenates in, a direct query()/header() call —
    // see the implementation in emitter.cpp for what this does and does not
    // catch.
    static bool looks_like_direct_request_data(const Expr& e);
    // Same idea as flatten_concat, but walking the IrExpr chain check_expr
    // already built — used by the bytecode emitter to batch a '+' chain
    // into one ConcatN instead of N-1 pairwise Adds.
    static void flatten_concat_ir(const IrExpr& e, std::vector<const IrExpr*>& out);

    int  resolve_local(const std::string& name) const;
    void begin_scope();
    void end_scope();

    // Static type of an expression, or Type::unknown() if it cannot be known.
    // Only looks at the obvious, with no inference: a literal, or a declared
    // variable. Stays in terms of Expr (not IrExpr) because the AST is what
    // check_expr/check_call/check_field look at when they need the static
    // type of a receiver — no IR version is needed.
    Type type_of(const Expr& e) const;

    // ── The real emitter: consumes the IrExpr/IrStmt that check_expr/
    // check_stmt already built and validated (with the real diags_ — see
    // emit_route/emit_function/etc. above). It checks nothing — not a call
    // to error(), not a name lookup (resolve_local/native_id/
    // is_reserved_object...): it trusts that the IR it receives already
    // passed the checker, reading already-resolved fields (slot, call_shape,
    // call_index, call_name, type) instead of resolving them. emit_block is
    // public (see the comment next to check_block) because
    // tests/emit_ir_shadow.cpp calls it directly to test execution
    // equivalence against the emitter that existed before the cut — what
    // emit_function does today.
    void emit_stmt(const IrStmt& s);
    void emit_expr(const IrExpr& e);
    void emit_call(const IrExpr& e);
    // True only if the IrExpr CAN be proven to be of type int — same rule as
    // is_int_expr(), but reading IrExpr::type (already resolved by
    // check_expr) instead of local_type(): no IrExpr needs to resolve a name
    // again.
    bool is_int_expr_ir(const IrExpr& e) const;
    // The builtins that arrive here with an already-resolved native_id
    // (ReservedMemberCall and BuiltinGlobalCall except render(), compiled
    // separately): the same shared tail emit_call has today.
    void emit_native_call(const IrExpr& e);
    // Method whose receiver has no known type at compile time (BuiltinMethodCall).
    void emit_method_call_dynamic(const IrExpr& e);
    void emit_compiled_render(const IrExpr& e);

    // Checks a builtin field/method against the closed list for the
    // receiver's type, reporting the error to `shadow` (the real diags_ from
    // the entry points, a separate DiagnosticBag in tests). Used by
    // check_expr/check_call.
    bool check_field(const Expr& object, const std::string& field, SourceLoc loc,
                     DiagnosticBag& shadow) const;
    bool check_builtin_method(const Expr& e, DiagnosticBag& shadow) const;

    // The desugaring shared by `require X else Y` (StmtKind::Require) and a
    // group guard (Guard): both are "if not X, return Y", same bytecode
    // (see emit_stmt/emit_route). Sharing this function avoids building the
    // IrStmt::Require twice with slightly different logic.
    IrStmtPtr check_require_like(SourceLoc loc, const Expr& cond, const Expr& otherwise,
                                 DiagnosticBag& shadow) const;
};

} // namespace lux_script
