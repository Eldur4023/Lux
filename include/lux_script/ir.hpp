#pragma once
#include <memory>
#include <string>
#include <vector>

#include "ast.hpp"
#include "type.hpp"

namespace lux_script {

// Typed IR for expressions (--native, phase 1).
//
// NOT YET CONNECTED to Emitter: this file is purely additive, the same kind
// of safe step that type.hpp was before it got connected. Nothing in
// Emitter constructs or consumes IrExpr yet.
//
// The goal of this IR is for `check_expr` (still to be written) and
// `emit_expr` (already existing) to stop having to agree on resolved names
// by convention: the checker walks the AST's Expr ONCE, performs exactly
// the same checks and declare_local/resolve_local calls that emit_expr
// performs today, and the result is an IrExpr that already carries the
// resolved type and slot. An emitter that only consumes IrExpr never
// resolves a name again nor calls error() -- that is exactly what avoids
// the diagnostic duplication described in phase 1.
//
// The shape of this struct deliberately mirrors that of Expr (ast.hpp): the
// same literal fields, the same use of `object`/`lhs`/`rhs` depending on the
// kind (see emit_expr for the reasoning behind each reuse: Ternary uses
// object as the condition and lhs/rhs as the two branches; Index uses
// object as the receiver and lhs as the index). Mirroring the shape is
// intentional -- building the IrExpr from the Expr is a node-by-node
// translation, not a redesign.

struct IrExpr;
using IrExprPtr = std::unique_ptr<IrExpr>;

enum class IrExprKind {
    StringLit, IntLit, FloatLit, BoolLit, NullLit,
    Ident, This, Member, Index, Call,
    Unary, Binary, Ternary, Await,
    PreStep, PostStep,
    ListLit, DictLit,
    // A bare reference to a user `fn` used as a value -- `my_func` where an
    // expression is expected, not `my_func(...)` (that stays a Call). NOT a
    // closure: no captured environment, just which function -- see
    // Value::Type::Func (value.hpp). `call_index` (below) carries which one,
    // resolved the same way UserFunctionCall already resolves it.
    FuncRef,
};

// The 9 call shapes that emit_call distinguishes today. This is not "one
// generic call with arguments": each shape has its own arity/keyword-arg
// rule, its own need for await, and its own destination opcode/backend, so
// the checker has to decide WHICH one it is before the emitter (bytecode or
// native) can act.
enum class IrCallShape {
    DbModuleCall,        // 1. sqlite.query(...)                    -> CallAsync
    ReservedMemberCall,  // 2. sse.send(...) / ws.send(...) / error.foo(...)
    UserFunctionCall,    // 3. user-defined fn, resolved against FunctionSigs
    ConstructorCall,     // 4. Class(...), resolved by arity against ctors
    ClassMethodCall,     // 5. method with a receiver of known static type
    BuiltinGlobalCall,   // 6. len(...)/sleep(...)/render(...)
    BuiltinMethodCall,   // 7. s.upper()/xs.add(v)... static or dynamic
    BuiltinModuleCall,    // 8. hash.sha256(...) (NATIVE-MODULES.md) -> CallBuiltinModule.
                          //    Deliberately synchronous -- see the why in that document --
                          //    and that's why it does NOT share a shape with DbModuleCall even
                          //    though both come from an `import`: each needs its own opcode.
    Invalid,             // 9. none of the above: a compile error
};

// An already-resolved argument: the checker has already verified that named
// arguments are valid wherever they appear (only render(), shape 6, accepts
// them today) and has already filled in the missing ones with their default
// value (FnSig::defaults) in the shapes that have them. `name` travels empty
// for a positional argument or for a default value filled in by the
// checker; it only carries content for a genuine named argument
// (`render(x, k=v)`), which is the only piece of information in an IrArg
// that isn't already in its `value` -- it's needed so that whoever consumes
// the IR can reconstruct the variables Dict (see emit_call).
struct IrArg {
    std::string name;
    IrExprPtr   value;
    SourceLoc   loc;
};

struct IrDictEntry {
    IrExprPtr key;
    IrExprPtr value;
};

struct IrExpr {
    IrExprKind kind;
    SourceLoc  loc;

    // Type already resolved by the checker. Type::unknown() is a legitimate
    // result (the same thing type_of() returns today for what can't be
    // known at compile time), not a marker for "not yet filled in".
    Type type = Type::unknown();

    // Literals -- same fields as Expr, same meaning.
    std::string text;
    long long   int_value   = 0;
    double      float_value = 0;
    bool        bool_value  = false;

    // Ident: the slot already resolved by resolve_local/declare_local. An
    // Ident that the checker couldn't resolve has already triggered error()
    // and never reaches building an IrExpr -- that's why there's no need
    // here for an "unresolved" state, just the real index.
    int slot = -1;

    // General structure. Mirrors the actual reuse in Expr (ast.hpp),
    // verified against emit_expr case by case -- it's not symmetric on
    // purpose, because Expr isn't either:
    //   Member/Index/Call -> object is the receiver
    //   Binary            -> lhs, rhs are the operands
    //   Ternary           -> object is the condition, lhs/rhs are the branches
    //   Unary             -> lhs is the operand
    //   Await             -> lhs is the call (always kind == Call)
    //   PreStep/PostStep  -> lhs is the target, rebuilt as a synthetic
    //                        Ident/Member/Index IrExpr (see check_expr):
    //                        Member/Index there do NOT check the
    //                        field/object with the same rigor as a
    //                        standalone Member/Index, because emit_expr
    //                        doesn't either today -- see check_expr's
    //                        comment about that gap.
    IrExprPtr object;
    IrExprPtr lhs, rhs;

    // Call
    IrCallShape        call_shape = IrCallShape::Invalid;
    std::vector<IrArg> args;
    // Depending on call_shape: the index already resolved in the
    // corresponding table (FunctionSigs::index in
    // UserFunctionCall/ClassMethodCall, the parameter count already used to
    // index ClassSig::ctors in ConstructorCall). -1 if that shape doesn't
    // resolve by index (BuiltinMethodCall resolves by name;
    // ReservedMemberCall and BuiltinGlobalCall DO carry call_index -- both
    // end up resolving to today's same native_id, only call_shape changes
    // depending on where the call came from).
    int call_index = -1;
    // Name already resolved that the final opcode needs: the module
    // injected in DbModuleCall, the builtin method name in
    // BuiltinMethodCall, or the already-formed full name ("obj.member" /
    // the builtin) in ReservedMemberCall/BuiltinGlobalCall. Empty if
    // call_shape doesn't need it (UserFunctionCall/ClassMethodCall/
    // ConstructorCall resolve by call_index, not by name).
    std::string call_name;
    bool        awaited = false;

    // Kind == Member representing a 0-argument member of a reserved object
    // (`sse.open`, without parentheses: emit_expr resolves it with
    // CallNative just like a call, see check_expr) reuses call_name (the
    // reserved object, e.g. "sse") and call_index (the already-resolved
    // native_id) instead of adding new fields just for this case -- `text`
    // still carries the member's name, as in any other Member.

    // ListLit / DictLit
    std::vector<IrExprPtr>   items;
    std::vector<IrDictEntry> entries;
};

// Statement IR (--native, phase 1: check_stmt already exists and is
// verified -- see Emitter::check_stmt -- this is the IR it should produce
// instead of writing to a DiagnosticBag). Additive, not yet connected: as
// with IrExpr, the shape deliberately mirrors Stmt (ast.hpp), and building
// it from a real Stmt is a node-by-node translation.

struct IrStmt;
using IrStmtPtr = std::unique_ptr<IrStmt>;
using IrBlock   = std::vector<IrStmtPtr>;

enum class IrStmtKind {
    Return, ExprStmt, VarDecl, Assign,
    If, While, For, Require, Try, Break, Continue,
};

// The 4 target shapes that emit_stmt/check_stmt distinguish today in the
// StmtKind::Assign case (looking at Stmt::target->kind): each is a distinct
// runtime operation, just as well differentiated as the 8 shapes of IrCall.
enum class IrAssignTarget {
    Session,  // session.x = v -> __session_set("x", v)
    Index,    // xs[i] = v     -> SetIndex
    Member,   // o.f = v       -> SetMember, after checking that the field exists
    Local,    // x = v         -> StoreLocal in the already-resolved slot
};

struct IrStmt {
    IrStmtKind kind;
    SourceLoc  loc;

    // Return / ExprStmt / VarDecl.init: the expression (null in a Return
    // with no value). If / While / Require: the condition. Assign: the
    // value being assigned, in all 4 shapes.
    IrExprPtr value;

    // VarDecl / For: the declared type and the variable's name, and the
    // slot that declare_local assigned to it -- the consumer of this IR
    // doesn't call declare_local again, just StoreLocal into `slot`. Try:
    // `name` is the `catch` clause's name (empty if it doesn't capture
    // anything) and `slot` its slot.
    Type        decl_type = Type::unknown();
    std::string name;
    int         slot = -1;

    // For: the iterable (the original `Stmt::target`). Require: the `else`
    // expression (what's returned if the condition is false).
    IrExprPtr target;

    // Assign: which target shape this is, and the pieces each one needs.
    // Never filled in beyond what corresponds to `assign_target`.
    IrAssignTarget assign_target = IrAssignTarget::Local;
    IrExprPtr      assign_object;    // Index/Member: the receiver
    IrExprPtr      assign_index;     // Index: the index expression
    std::string    assign_field;     // Session/Member: the field's name
    int            assign_slot = -1; // Local: the already-resolved slot

    IrBlock body;    // If-then / While / For / Try
    IrBlock orelse;  // If-else / Try-catch
};

} // namespace lux_script
