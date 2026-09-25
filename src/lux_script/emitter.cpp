#include <lux_script/emitter.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <lux_script/template.hpp>
#include <lux_script/natives.hpp>
#include <lux_script/builtin_module.hpp>

#include <algorithm>

namespace lux_script {

void Emitter::error(SourceLoc loc, std::string msg) {
    diags_.error(loc, std::move(msg));
    failed_ = true;
}

int Emitter::declare_local(const std::string& name, SourceLoc loc, Type type) {
    for (auto it = locals_.rbegin(); it != locals_.rend(); ++it) {
        if (it->depth < scope_depth_) break;
        if (it->name == name) {
            error(loc, "'" + name + "' is already declared in this scope");
            return static_cast<int>(std::distance(locals_.begin(), it.base()) - 1);
        }
    }
    locals_.push_back({name, scope_depth_, std::move(type)});
    int slot = static_cast<int>(locals_.size()) - 1;
    if (slot + 1 > chunk_->num_locals) chunk_->num_locals = slot + 1;
    chunk_->local_names.push_back(name);
    return slot;
}

const Type& Emitter::local_type(const std::string& name) const {
    static const Type kNone = Type::unknown();
    int i = resolve_local(name);
    return i < 0 ? kNone : locals_[static_cast<size_t>(i)].type;
}

// Fresh per-body state: every entry point compiles one standalone chunk.
void Emitter::reset(Chunk& out, std::string method) {
    chunk_        = &out;
    route_method_ = std::move(method);
    locals_.clear();
    loops_.clear();
    scope_depth_  = 0;
}

int Emitter::resolve_local(const std::string& name) const {
    for (int i = static_cast<int>(locals_.size()) - 1; i >= 0; --i)
        if (locals_[static_cast<size_t>(i)].name == name) return i;
    return -1;
}

void Emitter::begin_scope() { ++scope_depth_; }

void Emitter::end_scope() {
    --scope_depth_;
    // Slots are not recycled: the cost is one more entry in the locals
    // vector, and in exchange the indices are stable, which simplifies the VM.
    while (!locals_.empty() && locals_.back().depth > scope_depth_)
        locals_.pop_back();
}

// The 6 real entry points (emit_route/emit_function/emit_method/
// emit_ctor/emit_condition/emit_error_handler) delegate to their
// corresponding check_* to build the IR -- with the real diags_, not a
// separate shadow, so check_* becomes the ONLY source of diagnostics -- and
// only if that succeeded do they call emit_block/emit_expr (the emitter
// that consumes the IR) to produce the bytecode. The version of emit_expr/
// emit_stmt/emit_call/emit_block that walked Expr/Stmt checking and
// emitting at the same time, and check_field/check_builtin_method,
// no longer exist: they were removed in a separate commit once it was
// confirmed -- with this cutover made and tested end to end -- that
// nothing was calling them anymore. type_of() IS kept (check_expr/
// check_call/check_field still use it): it stays in terms of Expr because
// it's the AST, not the IR, that the checker looks at when it needs a
// receiver's static type. The final `return !failed_` (not `return true`)
// matters: compiled render emission (the only spot that can still fail
// during emission, not only during checking -- see its comment) sets
// failed_ to true if the template itself doesn't compile, and that's only
// known AFTER emit_block.
bool Emitter::emit_route(const RouteDecl& route, Chunk& out) {
    IrBlock body;
    if (!check_route(route, out, diags_, &body)) { failed_ = true; return false; }
    failed_ = false;

    emit_block(body);

    // A handler that falls through the end returns nothing: the engine will
    // respond with whatever a builtin wrote, or 204 if nothing was written.
    out.emit(Op::ReturnNull, route.loc);
    return !failed_;
}

bool Emitter::emit_function(const FnDecl& fn, Chunk& out) {
    IrBlock body;
    if (!check_function(fn, out, diags_, &body)) { failed_ = true; return false; }
    failed_ = false;

    emit_block(body);
    out.emit(Op::ReturnNull, fn.loc);
    return !failed_;
}

bool Emitter::emit_method(const std::string& cls, const FnDecl& m, Chunk& out) {
    IrBlock body;
    if (!check_method(cls, m, out, diags_, &body)) { failed_ = true; return false; }
    failed_ = false;

    emit_block(body);
    out.emit(Op::ReturnNull, m.loc);
    return !failed_;
}

bool Emitter::emit_ctor(const std::string& cls, const std::vector<std::string>& fields,
                        const CtorDecl& ct, Chunk& out) {
    IrBlock body;
    if (!check_ctor(cls, fields, ct, out, diags_, &body)) { failed_ = true; return false; }
    failed_ = false;

    // check_ctor already declared the parameters and `this` (in that order,
    // before any begin_scope/end_scope that could make them disappear from
    // locals_), so they're still there to resolve by name.
    int self = resolve_local("this");

    // The instance starts with every declared field set to null, so that
    // accessing one the constructor doesn't touch is null and doesn't fail.
    for (const auto& f : fields) {
        chunk_->emit(Op::Const, ct.loc, chunk_->add_constant(Value::str(f)));
        chunk_->emit(Op::Const, ct.loc, chunk_->add_constant(Value::null()));
    }
    chunk_->emit(Op::MakeDict, ct.loc, static_cast<uint32_t>(fields.size()));
    chunk_->emit(Op::StoreLocal, ct.loc, static_cast<uint32_t>(self));

    if (ct.has_body) {
        emit_block(body);
    } else {
        // No body: each parameter goes to the field of the same name.
        // check_ctor already rejected any parameter that isn't a field, so
        // if we get here, they all are.
        for (const auto& p : ct.params) {
            chunk_->emit(Op::LoadLocal, ct.loc, static_cast<uint32_t>(self));
            int slot = resolve_local(p.name);
            chunk_->emit(Op::LoadLocal, ct.loc, static_cast<uint32_t>(slot));
            chunk_->emit(Op::SetMember, ct.loc, chunk_->add_constant(Value::str(p.name)));
            chunk_->emit(Op::Pop, ct.loc);
        }
    }

    chunk_->emit(Op::LoadLocal, ct.loc, static_cast<uint32_t>(self));
    chunk_->emit(Op::Return, ct.loc);
    return !failed_;
}

bool Emitter::emit_condition(const Expr& e, const std::vector<TypedName>& names,
                             Chunk& out) {
    IrExprPtr ir = check_condition(e, names, out, diags_);
    if (!ir) { failed_ = true; return false; }
    failed_ = false;

    emit_expr(*ir);
    out.emit(Op::Return, e.loc);
    return !failed_;
}

IrExprPtr Emitter::check_condition(const Expr& e, const std::vector<TypedName>& names,
                                   Chunk& out, DiagnosticBag& shadow) {
    reset(out, {});

    for (const auto& n : names) declare_local(n.name, e.loc, Type::from_legacy_name(n.type));

    return check_expr(e, shadow);
}

// check_route/check_function/check_method/check_ctor/check_error_handler:
// counterpart of emit_route/emit_function/emit_method/emit_ctor/
// emit_error_handler for check_stmt, with the same state reset and the same
// parameter/`this` declarations. They return whether this specific call
// added no error to `shadow` (not whether `shadow` is entirely empty:
// callers may reuse the same DiagnosticBag across several cases).
bool Emitter::check_route(const RouteDecl& route, Chunk& out, DiagnosticBag& shadow,
                          IrBlock* out_body) {
    size_t before   = shadow.size();
    reset(out, route.method);

    for (const auto& p : route.params) declare_local(p.name, p.loc, Type::from_declared(p.type));

    // Guards are "if not X, return Y" -- the same IrStmtKind::Require that
    // `require` already uses, and they run before the body (from outside
    // in), so they go first in the IrBlock that's returned.
    IrBlock guards;
    for (const auto& g : route.guards) {
        if (!g.condition || !g.otherwise) continue;
        if (IrStmtPtr r = check_require_like(g.loc, *g.condition, *g.otherwise, shadow))
            guards.push_back(std::move(r));
    }

    IrBlock body = check_block(route.body, shadow);
    if (out_body) {
        guards.insert(guards.end(), std::make_move_iterator(body.begin()),
                      std::make_move_iterator(body.end()));
        *out_body = std::move(guards);
    }
    return shadow.size() == before;
}

bool Emitter::check_function(const FnDecl& fn, Chunk& out, DiagnosticBag& shadow,
                             IrBlock* out_body) {
    size_t before = shadow.size();
    reset(out, "FN");

    for (const auto& p : fn.params) declare_local(p.name, p.loc, Type::from_declared(p.type));
    IrBlock body = check_block(fn.body, shadow);
    if (out_body) *out_body = std::move(body);
    return shadow.size() == before;
}

bool Emitter::check_method(const std::string& cls, const FnDecl& m, Chunk& out,
                           DiagnosticBag& shadow, IrBlock* out_body) {
    size_t before = shadow.size();
    reset(out, "FN");

    declare_local("this", m.loc, Type::class_ref(cls));
    for (const auto& p : m.params) declare_local(p.name, p.loc, Type::from_declared(p.type));
    IrBlock body = check_block(m.body, shadow);
    if (out_body) *out_body = std::move(body);
    return shadow.size() == before;
}

bool Emitter::check_ctor(const std::string& cls, const std::vector<std::string>& fields,
                         const CtorDecl& ct, Chunk& out, DiagnosticBag& shadow,
                         IrBlock* out_body) {
    size_t before = shadow.size();
    reset(out, "FN");

    for (const auto& p : ct.params) declare_local(p.name, p.loc, Type::from_declared(p.type));
    declare_local("this", ct.loc, Type::class_ref(cls));

    if (ct.has_body) {
        IrBlock body = check_block(ct.body, shadow);
        if (out_body) *out_body = std::move(body);
    } else {
        for (const auto& p : ct.params) {
            if (std::find(fields.begin(), fields.end(), p.name) == fields.end())
                shadow.error(p.loc, "'" + p.name + "' is not a field of '" + cls + "'");
        }
    }
    return shadow.size() == before;
}

bool Emitter::check_error_handler(const ErrorDecl& decl, Chunk& out, DiagnosticBag& shadow,
                                  IrBlock* out_body) {
    size_t before = shadow.size();
    reset(out, "ERROR");

    IrBlock body = check_block(decl.body, shadow);
    if (out_body) *out_body = std::move(body);
    return shadow.size() == before;
}

bool Emitter::emit_error_handler(const ErrorDecl& decl, Chunk& out) {
    IrBlock body;
    if (!check_error_handler(decl, out, diags_, &body)) { failed_ = true; return false; }
    failed_ = false;

    emit_block(body);
    out.emit(Op::ReturnNull, decl.loc);
    return !failed_;
}

// Translates a StmtKind into the corresponding IrStmtKind -- see the
// comment on ir_kind_of: an explicit switch, not a static_cast, so that a
// new StmtKind left unupdated here triggers a compile warning.
static IrStmtKind ir_stmt_kind_of(StmtKind k) {
    switch (k) {
        case StmtKind::Return:   return IrStmtKind::Return;
        case StmtKind::ExprStmt: return IrStmtKind::ExprStmt;
        case StmtKind::VarDecl:  return IrStmtKind::VarDecl;
        case StmtKind::Assign:   return IrStmtKind::Assign;
        case StmtKind::If:       return IrStmtKind::If;
        case StmtKind::While:    return IrStmtKind::While;
        case StmtKind::For:      return IrStmtKind::For;
        case StmtKind::Require:  return IrStmtKind::Require;
        case StmtKind::Try:      return IrStmtKind::Try;
        case StmtKind::Break:    return IrStmtKind::Break;
        case StmtKind::Continue: return IrStmtKind::Continue;
    }
    return IrStmtKind::Return; // unreachable if the switch above is exhaustive
}

// Shared by check_stmt (Require) and check_route (group guards): both are
// "if not `cond`, return `otherwise`", the same IrStmtKind::Require.
IrStmtPtr Emitter::check_require_like(SourceLoc loc, const Expr& cond, const Expr& otherwise,
                                      DiagnosticBag& shadow) const {
    IrExprPtr c = check_expr(cond, shadow);
    IrExprPtr e = check_expr(otherwise, shadow);
    if (!c || !e) return nullptr;
    auto r = std::make_unique<IrStmt>();
    r->kind   = IrStmtKind::Require;
    r->loc    = loc;
    r->value  = std::move(c);
    r->target = std::move(e);
    return r;
}

// Shadow of emit_block/emit_stmt (phase 1, parallel checker -- see
// emitter.hpp). Same check order, same text, without touching chunk_.
// Unlike check_expr, it DOES call declare_local/begin_scope/end_scope:
// VarDecl, the desugared `for`, and a `catch`'s name are real name
// accounting (not a codegen temporary), and it's necessary to reproduce it
// so that a later statement's Ident resolves to the correct slot.
//
// check_block ALWAYS visits every statement it has, even if one fails:
// that's what allows reporting all of a block's errors in a single pass,
// just like emit_stmt does today. A statement that failed (nullptr) is
// omitted from the resulting IrBlock -- silently, it's not a new error, it
// was already reported at its spot -- so an IrBlock never carries a null
// gap. An If/While/For whose condition or iterable failed doesn't
// propagate either (it returns nullptr itself), even though its body was
// checked in full so as not to lose any errors inside it; a Try never fails
// on its own account, because it has no expression of its own to check, it
// only delegates to its blocks.
IrBlock Emitter::check_block(const Block& body, DiagnosticBag& shadow) {
    begin_scope();
    IrBlock out;
    for (const auto& s : body) {
        IrStmtPtr st = check_stmt(*s, shadow);
        if (st) out.push_back(std::move(st));
    }
    end_scope();
    return out;
}

IrStmtPtr Emitter::check_stmt(const Stmt& s, DiagnosticBag& shadow) {
    auto node = [&]() {
        auto r = std::make_unique<IrStmt>();
        r->kind = ir_stmt_kind_of(s.kind);
        r->loc  = s.loc;
        return r;
    };

    switch (s.kind) {
        case StmtKind::Return: {
            auto r = node();
            if (s.value) {
                IrExprPtr v = check_expr(*s.value, shadow);
                if (!v) return nullptr;
                r->value = std::move(v);
            }
            return r;
        }

        case StmtKind::ExprStmt: {
            IrExprPtr v = check_expr(*s.value, shadow);
            if (!v) return nullptr;
            auto r = node();
            r->value = std::move(v);
            return r;
        }

        case StmtKind::VarDecl: {
            IrExprPtr v;
            if (s.value) {
                v = check_expr(*s.value, shadow);
                if (!v) return nullptr;
            }
            int slot = declare_local(s.name, s.loc, Type::from_declared(s.type));
            auto r = node();
            r->value     = std::move(v);
            r->name      = s.name;
            r->decl_type = Type::from_declared(s.type);
            r->slot      = slot;
            return r;
        }

        case StmtKind::Assign: {
            // `session.x = v` -> __session_set("x", v).
            if (s.target->kind == ExprKind::Member &&
                s.target->object->kind == ExprKind::Ident &&
                s.target->object->text == "session" &&
                resolve_local("session") < 0) {
                IrExprPtr v = check_expr(*s.value, shadow);
                if (!v) return nullptr;
                auto r = node();
                r->assign_target = IrAssignTarget::Session;
                r->assign_field  = s.target->text;
                r->value = std::move(v);
                return r;
            }

            // `xs[0] = v` and `d["k"] = v`.
            if (s.target->kind == ExprKind::Index) {
                IrExprPtr obj = check_expr(*s.target->object, shadow);
                IrExprPtr idx = check_expr(*s.target->lhs, shadow);
                IrExprPtr val = check_expr(*s.value, shadow);
                if (!obj || !idx || !val) return nullptr;
                auto r = node();
                r->assign_target = IrAssignTarget::Index;
                r->assign_object = std::move(obj);
                r->assign_index  = std::move(idx);
                r->value = std::move(val);
                return r;
            }

            // `this.field = v` and `object.field = v`.
            if (s.target->kind == ExprKind::Member) {
                if (!check_field(*s.target->object, s.target->text, s.loc, shadow)) return nullptr;
                IrExprPtr obj = check_expr(*s.target->object, shadow);
                IrExprPtr val = check_expr(*s.value, shadow);
                if (!obj || !val) return nullptr;
                auto r = node();
                r->assign_target = IrAssignTarget::Member;
                r->assign_object = std::move(obj);
                r->assign_field  = s.target->text;
                r->value = std::move(val);
                return r;
            }

            if (s.target->kind != ExprKind::Ident) {
                shadow.error(s.loc, "can only assign to a variable or a field");
                return nullptr;
            }
            int slot = resolve_local(s.target->text);
            if (slot < 0) {
                shadow.error(s.target->loc, "'" + s.target->text + "' is not declared");
                return nullptr;
            }
            IrExprPtr val = check_expr(*s.value, shadow);
            if (!val) return nullptr;
            auto r = node();
            r->assign_target = IrAssignTarget::Local;
            r->assign_slot   = slot;
            r->value = std::move(val);
            return r;
        }

        case StmtKind::If: {
            IrExprPtr cond = check_expr(*s.value, shadow);
            IrBlock   then = check_block(s.body, shadow);
            IrBlock   orelse;
            if (!s.orelse.empty()) orelse = check_block(s.orelse, shadow);
            if (!cond) return nullptr;
            auto r = node();
            r->value  = std::move(cond);
            r->body   = std::move(then);
            r->orelse = std::move(orelse);
            return r;
        }

        case StmtKind::While: {
            IrExprPtr cond = check_expr(*s.value, shadow);
            loops_.push_back({});
            IrBlock body = check_block(s.body, shadow);
            loops_.pop_back();
            if (!cond) return nullptr;
            auto r = node();
            r->value = std::move(cond);
            r->body  = std::move(body);
            return r;
        }

        case StmtKind::Require:
            return check_require_like(s.loc, *s.value, *s.target, shadow);

        case StmtKind::Break:
            if (loops_.empty()) { shadow.error(s.loc, "'break' outside a loop"); return nullptr; }
            return node();

        case StmtKind::Continue:
            if (loops_.empty()) { shadow.error(s.loc, "'continue' outside a loop"); return nullptr; }
            return node();

        case StmtKind::For: {
            begin_scope();
            IrExprPtr iterable = check_expr(*s.target, shadow);
            declare_local(" items", s.loc);
            declare_local(" count", s.loc);
            declare_local(" index", s.loc);
            int var = declare_local(s.name, s.loc, Type::from_declared(s.type));

            loops_.push_back({});
            IrBlock body = check_block(s.body, shadow);
            loops_.pop_back();
            end_scope();

            if (!iterable) return nullptr;
            auto r = node();
            r->target    = std::move(iterable);
            r->name      = s.name;
            r->decl_type = Type::from_declared(s.type);
            r->slot      = var;
            r->body      = std::move(body);
            return r;
        }

        case StmtKind::Try: {
            IrBlock body = check_block(s.body, shadow);
            begin_scope();
            int slot = -1;
            if (!s.name.empty()) slot = declare_local(s.name, s.loc);
            IrBlock orelse;
            for (const auto& st : s.orelse) {
                IrStmtPtr ir = check_stmt(*st, shadow);
                if (ir) orelse.push_back(std::move(ir));
            }
            end_scope();

            auto r = node();
            r->body   = std::move(body);
            r->name   = s.name;
            r->slot   = slot;
            r->orelse = std::move(orelse);
            return r;
        }
    }
    return nullptr; // unreachable if the switch above is exhaustive
}

// Only the obvious: a literal, or a variable with a declared type. There's
// no inference, so when in doubt it returns Type::unknown() and nothing
// gets checked.
// A module function's declared return; a List or Dict holds Json (what a
// module hands back is dynamic inside).
Type module_return_type(const BuiltinModuleFn& fn) {
    if (fn.returns == "List") return Type::list_of(Type::json());
    if (fn.returns == "Dict") return Type::dict_of(Type::json());
    return fn.returns.empty() ? Type::json() : Type::from_legacy_name(fn.returns);
}

Type Emitter::type_of(const Expr& e) const {
    switch (e.kind) {
        case ExprKind::StringLit: return Type::primitive(Type::Kind::String);
        case ExprKind::IntLit:    return Type::primitive(Type::Kind::Int);
        case ExprKind::FloatLit:  return Type::primitive(Type::Kind::Float);
        case ExprKind::BoolLit:   return Type::primitive(Type::Kind::Bool);
        case ExprKind::Ident:     return local_type(e.text);
        case ExprKind::This:      return local_type("this");
        // A call's own type, so chaining straight off it (`f().field`,
        // `f().method()`) resolves exactly like chaining off a variable
        // that already holds the same value does -- see FnSig::return_type's
        // comment (emitter.hpp) for the bug this closes (an untyped call
        // result silently fell back to whatever a receiver of unknown type
        // dispatches as at runtime, which for a Value::Dict is "only has
        // Dict's own methods", not the user class it actually was).
        case ExprKind::Call: {
            if (!e.object) return Type::unknown();

            // A bare name: either a standalone function (`make_point(3, 4)`,
            // typed by its OWN declared return type) or a constructor
            // (`Item("a")` -- any successful call to one always produces
            // exactly that class, regardless of which overload matched).
            if (e.object->kind == ExprKind::Ident) {
                const std::string& name = e.object->text;
                if (functions_) {
                    auto it = functions_->find(name);
                    if (it != functions_->end()) return it->second.return_type;
                }
                if (classes_ && classes_->count(name)) return Type::class_ref(name);
                return Type::unknown();
            }

            if (e.object->kind != ExprKind::Member) return Type::unknown();

            // `hash.sha256(s)`: an imported module's function, typed by its
            // signature's return.
            if (e.object->object->kind == ExprKind::Ident && imports_ &&
                imports_->count(e.object->object->text) && resolve_local(e.object->object->text) < 0) {
                const BuiltinModuleFn* fn =
                    BuiltinModuleRegistry::instance().find(e.object->object->text, e.object->text);
                return fn && !fn->returns.empty() ? module_return_type(*fn) : Type::unknown();
            }
            const Type recv = type_of(*e.object->object);

            // A method on a user-defined class (`p.square()`), typed by
            // that method's OWN declared return type -- checked before the
            // builtin-method table below, since a class is never in it.
            if (classes_) {
                auto cls = classes_->find(recv.base_name());
                if (cls != classes_->end()) {
                    auto m = cls->second.methods.find(e.object->text);
                    if (m != cls->second.methods.end()) return m->second.return_type;
                }
            }

            // A builtin method on a receiver of known type: the chain continues.
            const auto* list = methods_of(recv.base_name());
            if (!list) return Type::unknown();
            for (const auto& m : *list)
                if (e.object->text == m.name)
                    return m.return_type ? Type::from_legacy_name(m.return_type) : recv;
            return Type::unknown();
        }
        default:                  return Type::unknown();
    }
}

// Shadow of check_field: same logic letter for letter, error goes to
// `shadow` instead of diags_ (phase 1, parallel checker -- see emitter.hpp).
bool Emitter::check_field(const Expr& object, const std::string& field, SourceLoc loc,
                          DiagnosticBag& shadow) const {
    const Type tr = type_of(object);
    if (tr.is_unknown()) return true;
    const std::string base = tr.base_name();

    if (classes_) {
        auto it = classes_->find(base);
        if (it != classes_->end()) {
            const auto& f = it->second.fields;
            if (std::find(f.begin(), f.end(), field) != f.end()) return true;
            if (it->second.methods.count(field)) {
                shadow.error(loc, "'" + base + "." + field + "' is a method: "
                           "it must be called with ()");
            } else {
                std::string has;
                for (const auto& n : f) has += (has.empty() ? "" : ", ") + n;
                shadow.error(loc, "'" + base + "' has no field '" + field + "'" +
                           (has.empty() ? "" : "; it has " + has));
            }
            return false;
        }
    }
    if (methods_of(base) && base != "Dict") {
        shadow.error(loc, "'" + field + "' on " + base + ", which has no fields");
        return false;
    }
    return true;
}

// Shadow of check_builtin_method: same idea.
bool Emitter::check_builtin_method(const Expr& e, DiagnosticBag& shadow) const {
    const Type recv = type_of(*e.object->object);
    const auto* list = methods_of(recv.base_name());
    if (!list) return true;

    const std::string& method = e.object->text;
    const BuiltinMethod* def = nullptr;
    for (const auto& m : *list)
        if (method == m.name) { def = &m; break; }

    if (!def) {
        std::string has;
        for (const auto& m : *list) has += (has.empty() ? "" : ", ") + std::string(m.name);
        shadow.error(e.object->loc, "values of type " + recv.base_name() +
                             " have no method '" + method + "'; it has " + has);
        return false;
    }

    size_t argc = 0, named = 0;
    for (const auto& a : e.args) (a.name.empty() ? argc : named)++;
    if (named > 0) ++argc;

    if (static_cast<int>(argc) < def->min_args ||
        static_cast<int>(argc) > def->max_args) {
        std::string expected = std::to_string(def->min_args);
        if (def->max_args != def->min_args) expected += "-" + std::to_string(def->max_args);
        shadow.error(e.loc, "'" + method + "()' expects " + expected +
                     " argument(s), but receives " + std::to_string(argc));
        return false;
    }
    return true;
}

// Translates an ExprKind into the corresponding IrExprKind. It's a
// function, not a static_cast: the two enums happen to have the same order
// today on purpose, but a cast would silently rely on that. With an
// explicit switch, if someone adds an ExprKind without updating this
// function, the compiler warns (non-exhaustive switch) instead of letting
// a made-up IrExprKind slip through.
static IrExprKind ir_kind_of(ExprKind k) {
    switch (k) {
        case ExprKind::StringLit: return IrExprKind::StringLit;
        case ExprKind::IntLit:    return IrExprKind::IntLit;
        case ExprKind::FloatLit:  return IrExprKind::FloatLit;
        case ExprKind::BoolLit:   return IrExprKind::BoolLit;
        case ExprKind::NullLit:   return IrExprKind::NullLit;
        case ExprKind::Ident:     return IrExprKind::Ident;
        case ExprKind::This:      return IrExprKind::This;
        case ExprKind::Member:    return IrExprKind::Member;
        case ExprKind::Index:     return IrExprKind::Index;
        case ExprKind::Call:      return IrExprKind::Call;
        case ExprKind::Unary:     return IrExprKind::Unary;
        case ExprKind::Binary:    return IrExprKind::Binary;
        case ExprKind::Ternary:   return IrExprKind::Ternary;
        case ExprKind::Await:     return IrExprKind::Await;
        case ExprKind::PreStep:   return IrExprKind::PreStep;
        case ExprKind::PostStep:  return IrExprKind::PostStep;
        case ExprKind::ListLit:   return IrExprKind::ListLit;
        case ExprKind::DictLit:   return IrExprKind::DictLit;
    }
    return IrExprKind::NullLit; // unreachable if the switch above is exhaustive
}

// Shadow of emit_expr (phase 1, parallel checker -- see emitter.hpp): same
// shape, same check order and same error text, but without touching chunk_
// and without declaring a single slot (emit_expr's declare_local() calls
// are codegen temporaries that a check-only pass doesn't need).
//
// Besides checking, it builds the IrExpr for this expression. Each node's
// type is ALWAYS type_of(e), never something more precise invented here
// (that would be new functionality, not a reproduction of what exists
// today) -- that's why `node()` sets it just once and each case only fills
// in its own structure. nullptr means "shadow.error was already called at
// the exact spot": no half-built node propagates upward.
IrExprPtr Emitter::check_expr(const Expr& e, DiagnosticBag& shadow) const {
    auto node = [&]() {
        auto r = std::make_unique<IrExpr>();
        r->kind = ir_kind_of(e.kind);
        r->loc  = e.loc;
        r->type = type_of(e);
        return r;
    };

    switch (e.kind) {
        case ExprKind::StringLit: { auto r = node(); r->text = e.text; return r; }
        case ExprKind::IntLit:    { auto r = node(); r->int_value = e.int_value; return r; }
        case ExprKind::FloatLit:  { auto r = node(); r->float_value = e.float_value; return r; }
        case ExprKind::BoolLit:   { auto r = node(); r->bool_value = e.bool_value; return r; }
        case ExprKind::NullLit:   return node();

        case ExprKind::Ident: {
            int slot = resolve_local(e.text);
            if (slot < 0) {
                if (native_id(e.text) >= 0) {
                    shadow.error(e.loc, "'" + e.text + "' is a builtin: it must be called, "
                                 "not to use it as a value");
                    return nullptr;
                }
                // Not a local, not a builtin -- a bare reference to a
                // user-defined `fn` used as a VALUE (e.g. `list.map(my_func)`),
                // not a call (`my_func()` never reaches here: ExprKind::Call
                // does, via check_call/UserFunctionCall instead). No captured
                // environment, no closure -- see Value::Type::Func's comment
                // (value.hpp) for why that is a deliberate, smaller feature.
                auto it = functions_ ? functions_->find(e.text) : FunctionSigs::const_iterator();
                if (functions_ && it != functions_->end()) {
                    auto r = node();
                    r->kind = IrExprKind::FuncRef;
                    r->text = e.text;
                    r->call_index = static_cast<int>(it->second.index);
                    r->type = Type::from_legacy_name("Func");
                    return r;
                }
                shadow.error(e.loc, "'" + e.text + "' is not declared");
                return nullptr;
            }
            auto r = node();
            r->text = e.text;
            r->slot = slot;
            return r;
        }

        case ExprKind::Unary: {
            IrExprPtr operand = check_expr(*e.lhs, shadow);
            if (!operand) return nullptr;
            auto r = node();
            r->text = e.text;
            r->lhs  = std::move(operand);
            return r;
        }

        case ExprKind::Binary: {
            if (e.text == "and" || e.text == "or") {
                IrExprPtr l = check_expr(*e.lhs, shadow);
                IrExprPtr r2 = check_expr(*e.rhs, shadow);
                if (!l || !r2) return nullptr;
                auto r = node();
                r->text = e.text;
                r->lhs = std::move(l);
                r->rhs = std::move(r2);
                return r;
            }
            IrExprPtr l = check_expr(*e.lhs, shadow);
            IrExprPtr r2 = check_expr(*e.rhs, shadow);
            static const std::set<std::string> ops = {
                "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">="};
            if (!ops.count(e.text)) {
                shadow.error(e.loc, "unsupported operator: " + e.text);
                return nullptr;
            }
            if (!l || !r2) return nullptr;
            auto r = node();
            r->text = e.text;
            r->lhs = std::move(l);
            r->rhs = std::move(r2);
            return r;
        }

        case ExprKind::Ternary: {
            IrExprPtr cond      = check_expr(*e.object, shadow);
            IrExprPtr then_expr = check_expr(*e.lhs, shadow);
            IrExprPtr else_expr = check_expr(*e.rhs, shadow);
            if (!cond || !then_expr || !else_expr) return nullptr;
            auto r = node();
            r->object = std::move(cond);
            r->lhs    = std::move(then_expr);
            r->rhs    = std::move(else_expr);
            return r;
        }

        case ExprKind::ListLit: {
            auto r = node();
            for (const auto& item : e.items) {
                IrExprPtr it = check_expr(*item, shadow);
                if (!it) return nullptr;
                r->items.push_back(std::move(it));
            }
            return r;
        }

        case ExprKind::DictLit: {
            auto r = node();
            for (const auto& entry : e.entries) {
                IrExprPtr k = check_expr(*entry.key, shadow);
                IrExprPtr v = check_expr(*entry.value, shadow);
                if (!k || !v) return nullptr;
                r->entries.push_back({std::move(k), std::move(v)});
            }
            return r;
        }

        case ExprKind::Index: {
            IrExprPtr obj = check_expr(*e.object, shadow);
            IrExprPtr idx = check_expr(*e.lhs, shadow);
            if (!obj || !idx) return nullptr;
            auto r = node();
            r->object = std::move(obj);
            r->lhs    = std::move(idx);
            return r;
        }

        case ExprKind::Member: {
            // `session.x` accepts any name: it translates to
            // __session_get("x"). `text` already carries "x"; nothing more is needed.
            if (e.object->kind == ExprKind::Ident &&
                e.object->text == "session" &&
                resolve_local("session") < 0 &&
                member_native_id("session", e.text) < 0) {
                auto r = node();
                r->text = e.text;
                return r;
            }

            if (e.object->kind == ExprKind::Ident &&
                resolve_local(e.object->text) < 0 &&
                is_reserved_object(e.object->text)) {

                int id = member_native_id(e.object->text, e.text);
                if (id < 0) {
                    shadow.error(e.loc, "'" + e.object->text + "' has no member '" +
                                 e.text + "'");
                    return nullptr;
                }
                if (native_at(id).min_args > 0) {
                    shadow.error(e.loc, "'" + e.object->text + "." + e.text +
                                 "' is an operation: it must be called with ()");
                    return nullptr;
                }
                if ((e.object->text == "sse" && route_method_ != "SSE") ||
                    (e.object->text == "ws"  && route_method_ != "WS")) {
                    shadow.error(e.loc, "'" + e.object->text + "' only exists inside "
                                 "a route " + e.object->text);
                    return nullptr;
                }
                if (e.object->text == "error" && route_method_ != "ERROR") {
                    shadow.error(e.loc, "'error' only exists inside an 'on error'");
                    return nullptr;
                }
                // A 0-argument member of a reserved object (`sse.open`):
                // reuses call_name/call_index, see the comment in ir.hpp.
                auto r = node();
                r->text = e.text;
                r->call_name  = e.object->text;
                r->call_index = id;
                return r;
            }

            // `Color.RED`: an enum member, resolved here the same way a
            // reserved object's member is above -- by NAME, before ever
            // treating `e.object` as a value to evaluate -- because an enum
            // name is not a value in scope, it is a compile-time-only
            // namespace, exactly like `hash`/`math`/... are for a module.
            // Compiles straight to a StringLit IrExpr (the member's own
            // name as a string, see EnumDecl's comment) -- no new IrExprKind
            // needed, this rides Op::Const the same way a function reference
            // does (check_expr's Ident case).
            if (e.object->kind == ExprKind::Ident &&
                resolve_local(e.object->text) < 0 &&
                enums_ && enums_->count(e.object->text)) {
                const auto& members = enums_->at(e.object->text);
                if (!members.count(e.text)) {
                    std::string list;
                    for (const auto& m : members) { if (!list.empty()) list += ", "; list += m; }
                    shadow.error(e.loc, "enum '" + e.object->text + "' has no member '" +
                                 e.text + "'; it has " + list);
                    return nullptr;
                }
                auto r = node();
                r->kind = IrExprKind::StringLit;
                r->text = e.text;
                r->type = Type::from_legacy_name("string");
                return r;
            }

            if (!check_field(*e.object, e.text, e.loc, shadow)) return nullptr;
            IrExprPtr obj = check_expr(*e.object, shadow);
            if (!obj) return nullptr;
            auto r = node();
            r->object = std::move(obj);
            r->text   = e.text;
            return r;
        }

        case ExprKind::Call:
            return check_call(e, /*awaited=*/false, shadow);

        case ExprKind::PreStep:
        case ExprKind::PostStep: {
            const Expr& tgt = *e.lhs;

            if (tgt.kind == ExprKind::Ident) {
                int slot = resolve_local(tgt.text);
                if (slot < 0) {
                    shadow.error(tgt.loc, "'" + tgt.text + "' is not declared");
                    return nullptr;
                }
                auto target = std::make_unique<IrExpr>();
                target->kind = IrExprKind::Ident;
                target->loc  = tgt.loc;
                target->text = tgt.text;
                target->slot = slot;
                target->type = local_type(tgt.text);
                auto r = node();
                r->text = e.text;
                r->lhs  = std::move(target);
                return r;
            }
            if (tgt.kind == ExprKind::Member) {
                // Same as emit_expr: it does NOT check that the field
                // exists (check_field isn't called here today either).
                // Reproducing that gap, not fixing it, is what this phase
                // calls for.
                IrExprPtr obj = check_expr(*tgt.object, shadow);
                if (!obj) return nullptr;
                auto target = std::make_unique<IrExpr>();
                target->kind   = IrExprKind::Member;
                target->loc    = tgt.loc;
                target->text   = tgt.text;
                target->object = std::move(obj);
                auto r = node();
                r->text = e.text;
                r->lhs  = std::move(target);
                return r;
            }
            if (tgt.kind == ExprKind::Index) {
                IrExprPtr obj = check_expr(*tgt.object, shadow);
                IrExprPtr idx = check_expr(*tgt.lhs, shadow);
                if (!obj || !idx) return nullptr;
                auto target = std::make_unique<IrExpr>();
                target->kind   = IrExprKind::Index;
                target->loc    = tgt.loc;
                target->object = std::move(obj);
                target->lhs    = std::move(idx);
                auto r = node();
                r->text = e.text;
                r->lhs  = std::move(target);
                return r;
            }
            shadow.error(e.loc, "'++' and '--' only apply to a variable, a field "
                         "or an indexed element");
            return nullptr;
        }

        case ExprKind::Await: {
            if (!e.lhs || e.lhs->kind != ExprKind::Call) {
                shadow.error(e.loc, "'await' only applies to an asynchronous call "
                             "(sleep, a database module, or an is_async native module call)");
                return nullptr;
            }
            IrExprPtr call = check_call(*e.lhs, /*awaited=*/true, shadow);
            if (!call) return nullptr;
            auto r = node();
            r->lhs = std::move(call);
            return r;
        }

        case ExprKind::This: {
            int slot = resolve_local("this");
            if (slot < 0) {
                shadow.error(e.loc, "'this' only exists inside a method or a constructor");
                return nullptr;
            }
            auto r = node();
            r->slot = slot;
            return r;
        }
    }
    return nullptr; // unreachable if the switch above is exhaustive
}

// Shadow of emit_call: same shape, same order, same text -- see check_expr's
// comment. Builds an IrExpr(kind=Call) with call_shape already resolved to
// one of the IrCallShape shapes (ir.hpp).
// Collects the operands of a '+' chain in evaluation order, on the raw AST.
// It only walks down the left: the right-hand side enters as is, even if it
// is another '+' in parentheses, so as not to reassociate what the parser
// already associated. Used by check_call, before check_expr has built any
// IR yet, to look for request data glued directly into SQL.
void Emitter::flatten_concat(const Expr& e, std::vector<const Expr*>& out) {
    if (e.kind == ExprKind::Binary && e.text == "+" && e.lhs && e.rhs) {
        flatten_concat(*e.lhs, out);
        out.push_back(e.rhs.get());
        return;
    }
    out.push_back(&e);
}

// True if `e` IS, or is built by concatenating in, a direct call to
// query()/header() or a read of request.body — the ones that hand back
// exactly what the client sent, unvalidated. No dataflow tracking: assigning
// the result to a local first (`string next = query("next")`) is not
// caught, on purpose — that shape at least gives the developer a place to
// put a check before the value reaches redirect()/SQL. This only catches
// the literal, unguarded splice, which is also the only shape with no
// legitimate reading.
bool Emitter::looks_like_direct_request_data(const Expr& e) {
    if (e.kind == ExprKind::Call && e.object &&
        e.object->kind == ExprKind::Ident &&
        (e.object->text == "query" || e.object->text == "header")) {
        return true;
    }
    // request.body: the raw request body, added for webhook signature
    // verification (GUIDE.md) -- exactly as attacker-controlled as
    // query()/header(), so glueing it into SQL text needs the same warning.
    if (e.kind == ExprKind::Member && e.text == "body" && e.object &&
        e.object->kind == ExprKind::Ident && e.object->text == "request") {
        return true;
    }
    if (e.kind == ExprKind::Binary && e.text == "+" && e.lhs && e.rhs) {
        return looks_like_direct_request_data(*e.lhs) ||
               looks_like_direct_request_data(*e.rhs);
    }
    return false;
}

// Checks every argument of a call that only takes positional ones, appending
// them to `out`; a named one is an error with `named_msg`.
bool Emitter::check_positional(const Expr& e, std::vector<IrArg>& out, DiagnosticBag& shadow,
                               const std::string& named_msg) const {
    for (const auto& a : e.args) {
        if (!a.name.empty()) { shadow.error(a.loc, named_msg); return false; }
        IrExprPtr v = check_expr(*a.value, shadow);
        if (!v) return false;
        out.push_back({{}, std::move(v), a.loc});
    }
    return true;
}

IrExprPtr Emitter::check_call(const Expr& e, bool awaited, DiagnosticBag& shadow) const {
    if (!e.object) { shadow.error(e.loc, "call without a target"); return nullptr; }

    auto make_call = [&](IrCallShape shape) {
        auto r = std::make_unique<IrExpr>();
        r->kind = IrExprKind::Call;
        r->loc  = e.loc;
        r->type = type_of(e);
        r->call_shape = shape;
        r->awaited    = awaited;
        return r;
    };

    std::string  name;
    int          id = -1;
    IrCallShape  shape = IrCallShape::Invalid;

    // hash.sha256(...) — a native module function (NATIVE-MODULES.md).
    // Checked BEFORE the reserved-object branch below, and completely
    // separately from it: a native module's functions live in their own
    // flat id-space (BuiltinModuleRegistry), never in kMembers/kNatives, so
    // this cannot fall through into member_native_id() below and must
    // return on every path once it starts.
    if (e.object->kind == ExprKind::Member &&
        e.object->object->kind == ExprKind::Ident &&
        resolve_local(e.object->object->text) < 0 &&
        BuiltinModuleRegistry::instance().has(e.object->object->text)) {

        const std::string& obj    = e.object->object->text;
        const std::string& member = e.object->text;

        if (!imports_ || !imports_->count(obj)) {
            shadow.error(e.object->loc, "missing 'import " + obj + "' in order to use '" +
                                 obj + "." + member + "'");
            return nullptr;
        }
        const BuiltinModuleFn* fn = BuiltinModuleRegistry::instance().find(obj, member);
        if (!fn) {
            shadow.error(e.object->loc, "'" + obj + "' has no member '" + member + "'");
            return nullptr;
        }
        // Same enforcement DbModuleCall does below, mirrored exactly: an
        // is_async function (BuiltinModuleFn::is_async -- os.run()/
        // read_file()/write_file(), every http.*) MUST be awaited, so its
        // caller cannot forget the checked-at-runtime error convention that
        // comes with it (run_builtin_module_async, project.cpp, returns
        // failure as `{"error": ...}` data, never a hard fail()); a plain
        // function must NOT be, so the two calling conventions are never
        // ambiguous from the callsite alone.
        if (fn->is_async && !awaited) {
            shadow.error(e.loc, "'" + obj + "." + member + "()' is asynchronous: "
                         "you must write 'await " + obj + "." + member + "(...)'");
            return nullptr;
        }
        if (!fn->is_async && awaited) {
            shadow.error(e.loc, "'" + obj + "." + member + "()' is not asynchronous: "
                         "the 'await' is unnecessary");
            return nullptr;
        }

        IrExprPtr r = make_call(IrCallShape::BuiltinModuleCall);
        r->call_name  = obj;
        r->call_index = BuiltinModuleRegistry::instance().id_of(obj, member);

        if (!check_positional(e, r->args, shadow,
                              "'" + obj + "." + member + "()' does not accept named arguments"))
            return nullptr;
        size_t argc = e.args.size();
        if (argc < static_cast<size_t>(fn->min_args)) {
            shadow.error(e.loc, "'" + obj + "." + member + "()' expects at least " +
                         std::to_string(fn->min_args) + " argument(s)");
            return nullptr;
        }
        if (fn->max_args >= 0 && argc > static_cast<size_t>(fn->max_args)) {
            shadow.error(e.loc, "'" + obj + "." + member + "()' takes at most " +
                         std::to_string(fn->max_args) + " argument(s)");
            return nullptr;
        }
        // Every module function returns a plain, concrete value today (no
        // module has shipped one returning Json/List<Json> yet) -- Json is
        // still the honest, conservative type to hand the checker: it means
        // "this must be proven as JSON-constructible" (Comprobador::
        // es_valor_json) rather than claiming a specific type this call
        // cannot back up. See NATIVE-MODULES.md on why native codegen for
        // these calls is not attempted yet: with type Json, native_gen.cpp's
        // tipo_provable() simply never proves this shape, and the route
        // falls back to bytecode like any other unsupported construct.
        // The declared return type when there is one (a signature's '>'),
        // so a chained method is checked like on a variable; Json otherwise.
        r->type = module_return_type(*fn);
        return r;
    }

    // sse.send(...) — a member of a reserved object.
    if (e.object->kind == ExprKind::Member &&
        e.object->object->kind == ExprKind::Ident &&
        resolve_local(e.object->object->text) < 0 &&
        is_reserved_object(e.object->object->text)) {

        name = e.object->object->text + "." + e.object->text;
        id   = member_native_id(e.object->object->text, e.object->text);
        if (id < 0) {
            shadow.error(e.object->loc, "'" + e.object->object->text +
                                 "' has no member '" + e.object->text + "'");
            return nullptr;
        }
        const std::string& obj = e.object->object->text;

        bool is_module = is_db_module(obj);
        if (is_module && (!imports_ || !imports_->count(obj))) {
            shadow.error(e.object->loc, "missing 'import " + obj + "' in order to use '" +
                                 obj + "." + e.object->text + "'");
            return nullptr;
        }
        if (is_module) {
            const NativeDef& mdef = native_at(id);
            if (!awaited) {
                shadow.error(e.loc, "'" + obj + "." + e.object->text + "()' is asynchronous: "
                             "you must write 'await " + obj + "." + e.object->text +
                             "(...)'");
                return nullptr;
            }
            IrExprPtr r = make_call(IrCallShape::DbModuleCall);
            r->call_name  = obj;
            r->call_index = id;

            // Gluing a value into the SQL string is the one way left to
            // reintroduce injection in an API that is parameterised by
            // default: query()/exec() take the values as extra arguments
            // and bind them through the driver, and a '+' with a
            // non-literal operand walks straight past all of it. This
            // check runs here, in the shared checker both bytecode and
            // --native compile from, so a native-compiled route gets the
            // same guarantee as a bytecode one.
            if ((e.object->text == "query" || e.object->text == "exec") &&
                !e.args.empty() && e.args[0].value &&
                e.args[0].value->kind == ExprKind::Binary &&
                e.args[0].value->text == "+") {

                // Unambiguous case: query()/header() spliced straight into
                // the SQL text with nothing in between. There is no
                // reading of this that isn't the injection.
                if (looks_like_direct_request_data(*e.args[0].value)) {
                    shadow.error(e.args[0].value->loc,
                        "request data glued directly into SQL text in '" + obj + "." +
                        e.object->text + "()' — this is SQL injection. Pass the "
                        "value as an extra argument and write '?' in the query "
                        "instead: it goes through the driver's bind, not the "
                        "string.");
                    return nullptr;
                }

                // Everything else that concatenates in a non-literal is
                // only a warning: '... in (' + marks + ')' with a
                // placeholder list built at runtime is legitimate and has
                // no other spelling, so refusing to compile it would be
                // wrong. Concatenating only string literals is left alone
                // too — that is just a long query split over several lines.
                std::vector<const Expr*> parts;
                flatten_concat(*e.args[0].value, parts);
                bool interpolated = false;
                for (const Expr* p : parts)
                    if (p->kind != ExprKind::StringLit) { interpolated = true; break; }
                if (interpolated) {
                    const SourceLoc& l = e.args[0].value->loc;
                    std::cerr << "lux: warning: " << (l.file ? *l.file : "?")
                              << ":" << l.line << ":" << l.col
                              << ": SQL built by concatenation in '" << obj << '.'
                              << e.object->text << "()'; pass the value as an argument "
                                 "and write '?' in the query, or it goes in unescaped\n";
                }
            }

            if (!check_positional(e, r->args, shadow, "queries do not accept named arguments")) return nullptr;
            size_t argc = 1 + e.args.size();
            if (argc < static_cast<size_t>(mdef.min_args)) {
                shadow.error(e.loc, "'" + obj + "." + e.object->text +
                             "()' expects at least the SQL query");
                return nullptr;
            }
            if (mdef.max_args >= 0 && argc > static_cast<size_t>(mdef.max_args)) {
                shadow.error(e.loc, "'" + obj + "." + e.object->text +
                             "()' takes no arguments");
                return nullptr;
            }
            if (argc > 255) { shadow.error(e.loc, "too many arguments"); return nullptr; }
            return r;
        }

        if ((obj == "sse" && route_method_ != "SSE") ||
            (obj == "ws"  && route_method_ != "WS")) {
            shadow.error(e.object->loc, "'" + obj + "' only exists inside a route " + obj);
            return nullptr;
        }
        if (obj == "error" && route_method_ != "ERROR") {
            shadow.error(e.object->loc, "'error' only exists inside an 'on error'");
            return nullptr;
        }
        shape = IrCallShape::ReservedMemberCall;
    }
    else if (e.object->kind == ExprKind::Ident) {
        name = e.object->text;
        id   = native_id(name);

        if (id < 0) {
            auto it = functions_ ? functions_->find(name) : FunctionSigs::const_iterator();
            if (functions_ && it != functions_->end()) {
                const FnSig& sig = it->second;
                IrExprPtr r = make_call(IrCallShape::UserFunctionCall);
                r->call_index = static_cast<int>(sig.index);

                if (!check_positional(e, r->args, shadow, "a user function does not accept named arguments")) return nullptr;
                size_t given = e.args.size();

                if (given < sig.required || given > sig.defaults.size()) {
                    std::string expected = std::to_string(sig.required);
                    if (sig.defaults.size() != sig.required)
                        expected += " to " + std::to_string(sig.defaults.size());
                    shadow.error(e.loc, "'" + name + "()' expects " + expected +
                                 " argument(s), but receives " + std::to_string(given));
                    return nullptr;
                }
                // The missing ones are filled in with their default value:
                // the function always receives the full list (same as
                // emit_call does today, see IrArg's comment).
                for (size_t i = given; i < sig.defaults.size(); ++i) {
                    IrExprPtr v = check_expr(*sig.defaults[i], shadow);
                    if (!v) return nullptr;
                    r->args.push_back({{}, std::move(v), e.loc});
                }
                return r;
            }
            auto ct = classes_ ? classes_->find(name) : ClassSigs::const_iterator();
            if (classes_ && ct != classes_->end()) {
                IrExprPtr r = make_call(IrCallShape::ConstructorCall);

                if (!check_positional(e, r->args, shadow, "a constructor does not accept named arguments")) return nullptr;
                size_t argc = e.args.size();
                auto found = ct->second.ctors.find(argc);
                if (found == ct->second.ctors.end()) {
                    std::string options;
                    for (const auto& [n, _] : ct->second.ctors)
                        options += (options.empty() ? "" : ", ") + std::to_string(n);
                    shadow.error(e.loc, "'" + name + "' has no constructor taking " +
                                 std::to_string(argc) + " parameter(s)" +
                                 (options.empty() ? "" : "; there are ones taking " + options));
                    return nullptr;
                }
                r->call_index = static_cast<int>(found->second);
                return r;
            }

            shadow.error(e.object->loc, "unknown function: '" + name + "'");
            return nullptr;
        }
        shape = IrCallShape::BuiltinGlobalCall;
    }
    else if (e.object->kind == ExprKind::Member && classes_) {
        // type_of(), not a hand-rolled Ident/This-only check: the receiver
        // can be any expression whose type is known, including a call to
        // a function/constructor/method (`make_point(3, 4).square()`,
        // chaining straight off the result) -- see type_of()'s own Call
        // case (added alongside FnSig::return_type) for why that now resolves
        // to something other than unknown.
        Type recv_type = type_of(*e.object->object);
        const std::string recv_name = recv_type.base_name();

        auto cls = recv_type.is_unknown() ? classes_->end() : classes_->find(recv_name);
        if (cls != classes_->end()) {
            auto m = cls->second.methods.find(e.object->text);
            if (m == cls->second.methods.end()) {
                if (!cls->second.fields.empty() &&
                    std::find(cls->second.fields.begin(), cls->second.fields.end(),
                              e.object->text) == cls->second.fields.end()) {
                    shadow.error(e.object->loc, "'" + recv_name + "' has no method '" +
                                         e.object->text + "'");
                    return nullptr;
                }
                // A field with a generic method on top: continues further below.
            } else {
                const FnSig& sig = m->second;
                IrExprPtr receiver = check_expr(*e.object->object, shadow);
                if (!receiver) return nullptr;

                IrExprPtr r = make_call(IrCallShape::ClassMethodCall);
                r->object     = std::move(receiver);
                r->call_index = static_cast<int>(sig.index);

                if (!check_positional(e, r->args, shadow, "a method does not accept named arguments")) return nullptr;
                size_t given = e.args.size();
                if (given < sig.required || given > sig.defaults.size()) {
                    shadow.error(e.loc, "'" + recv_name + "." + e.object->text +
                                 "()' expects " + std::to_string(sig.required) +
                                 " argument(s), but receives " + std::to_string(given));
                    return nullptr;
                }
                for (size_t i = given; i < sig.defaults.size(); ++i) {
                    IrExprPtr v = check_expr(*sig.defaults[i], shadow);
                    if (!v) return nullptr;
                    r->args.push_back({{}, std::move(v), e.loc});
                }
                return r;
            }
        }
        if (!check_builtin_method(e, shadow)) return nullptr;
        IrExprPtr receiver = check_expr(*e.object->object, shadow);
        if (!receiver) return nullptr;
        IrExprPtr r = make_call(IrCallShape::BuiltinMethodCall);
        r->object    = std::move(receiver);
        r->call_name = e.object->text;
        for (const auto& a : e.args) {
            IrExprPtr v = check_expr(*a.value, shadow);
            if (!v) return nullptr;
            // Unlike the other shapes, a builtin method DOES accept named
            // arguments (check_builtin_method counts them as just
            // another slot, it doesn't reject them): a.name travels along,
            // so emit_method_call_dynamic can group them into the Dict of
            // the last positional slot, the same way it does today with the
            // original Expr.
            r->args.push_back({a.name, std::move(v), a.loc});
        }
        return r;
    }
    else if (e.object->kind == ExprKind::Member) {
        if (!check_builtin_method(e, shadow)) return nullptr;
        IrExprPtr receiver = check_expr(*e.object->object, shadow);
        if (!receiver) return nullptr;
        IrExprPtr r = make_call(IrCallShape::BuiltinMethodCall);
        r->object    = std::move(receiver);
        r->call_name = e.object->text;
        for (const auto& a : e.args) {
            IrExprPtr v = check_expr(*a.value, shadow);
            if (!v) return nullptr;
            // Unlike the other shapes, a builtin method DOES accept named
            // arguments (check_builtin_method counts them as just
            // another slot, it doesn't reject them): a.name travels along,
            // so emit_method_call_dynamic can group them into the Dict of
            // the last positional slot, the same way it does today with the
            // original Expr.
            r->args.push_back({a.name, std::move(v), a.loc});
        }
        return r;
    }
    else {
        shadow.error(e.loc, "for now only builtins or methods can be called");
        return nullptr;
    }

    // Tail shared by shapes 2 (ReservedMemberCall) and 6
    // (BuiltinGlobalCall): both resolve to a valid native_id and share
    // exactly the same await/named-argument/arity rules.
    const NativeDef& def = native_at(id);

    if (def.is_async && !awaited) {
        shadow.error(e.loc, "'" + name + "()' is asynchronous: you must write "
                     "'await " + name + "(...)'");
        return nullptr;
    }
    if (!def.is_async && awaited) {
        shadow.error(e.loc, "'" + name + "()' is not asynchronous: the 'await' is unnecessary");
        return nullptr;
    }

    size_t positional = 0, named = 0;
    for (const auto& a : e.args) (a.name.empty() ? positional : named)++;

    if (named > 0 && name != "render") {
        shadow.error(e.loc, "'" + name + "()' does not accept named arguments");
        return nullptr;
    }

    IrExprPtr r = make_call(shape);
    r->call_name  = name;
    r->call_index = id;

    // redirect(query("next")) / redirect(header("referer") + "/x") is the
    // textbook open redirect: whatever the client sends becomes the
    // Location header verbatim. There is no legitimate reason to splice
    // query()/header() straight into a redirect target — a route that
    // genuinely needs to bounce somewhere request-dependent validates it
    // first (an allowlist, an if/else over a fixed set of literals) and
    // hands redirect() the already-checked local variable, which this does
    // not flag. redirect(path, code) — a hardcoded/config-derived target,
    // by far the common case — is untouched. Checked here, in the shared
    // checker, so a --native route gets the same guarantee as bytecode.
    if (name == "redirect" && !e.args.empty() && e.args[0].value &&
        looks_like_direct_request_data(*e.args[0].value)) {
        shadow.error(e.loc, "redirect() target comes straight from query()/header(): "
                     "an attacker controls it and can point it anywhere "
                     "('open redirect'). Validate it first (compare against an "
                     "allowlist or a fixed set of literals) and pass that "
                     "checked value instead.");
        return nullptr;
    }

    // render() compiles the template itself in emit_compiled_render, which
    // is template surface (phase 3), not expression surface -- it isn't
    // reproduced here; only what was already validated above plus the
    // subexpressions gets validated.
    if (name == "render") {
        if (e.args.empty() || !e.args[0].name.empty() ||
            e.args[0].value->kind != ExprKind::StringLit) {
            shadow.error(e.loc, "render() needs the template name written out, not a "
                         "variable. To choose between several, use an if with literal "
                         "literals: every branch is checked at compile time");
            return nullptr;
        }
        if (!templates_ || !templates_->table) {
            shadow.error(e.loc, "render() cannot be used here");
            return nullptr;
        }
        for (const auto& a : e.args) {
            IrExprPtr v = check_expr(*a.value, shadow);
            if (!v) return nullptr;
            r->args.push_back({a.name, std::move(v), a.loc});
        }
        return r;
    }

    for (const auto& a : e.args) {
        if (!a.name.empty()) continue;
        IrExprPtr v = check_expr(*a.value, shadow);
        if (!v) return nullptr;
        r->args.push_back({{}, std::move(v), a.loc});
    }

    size_t argc = positional;
    if (named > 0) {
        for (const auto& a : e.args) {
            if (a.name.empty()) continue;
            IrExprPtr v = check_expr(*a.value, shadow);
            if (!v) return nullptr;
            r->args.push_back({a.name, std::move(v), a.loc});
        }
        ++argc;
    }

    if (static_cast<int>(argc) < def.min_args ||
        (def.max_args >= 0 && static_cast<int>(argc) > def.max_args)) {
        std::string expected = std::to_string(def.min_args);
        if (def.max_args != def.min_args)
            expected += def.max_args < 0 ? " or more"
                                         : "-" + std::to_string(def.max_args);
        shadow.error(e.loc, "'" + name + "()' expects " + expected +
                     " argument(s), but receives " + std::to_string(argc));
        return nullptr;
    }
    if (argc > 255) { shadow.error(e.loc, "too many arguments"); return nullptr; }

    return r;
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 1 of --native: the real emitter, which consumes the IR already
// built and validated by check_expr/check_stmt (called with the real diags_
// from the 6 entry points -- see emit_route/emit_function/etc. above). It
// checks nothing -- not a call to error(), not a name lookup -- because by
// the time any of these functions is called, the checker has already given
// its approval: it reads already-resolved fields (slot, call_shape,
// call_index, call_name, type) instead of resolving them. The version over
// Expr/Stmt that this emitter replaces (same opcodes, same order; this is
// its MECHANICAL counterpart, node for node) no longer exists: it was
// removed once it was confirmed, with the real cutover made and tested,
// that nothing was calling it anymore.
// ═══════════════════════════════════════════════════════════════════════════

// Same idea as flatten_concat, but walking the already-built IrExpr chain
// instead of the raw AST — needed here because by the time emit_expr runs,
// check_expr has already turned the '+' chain into nested IrExpr::Binary
// nodes and the original Expr tree is gone.
void Emitter::flatten_concat_ir(const IrExpr& e, std::vector<const IrExpr*>& out) {
    if (e.kind == IrExprKind::Binary && e.text == "+" && e.lhs && e.rhs) {
        flatten_concat_ir(*e.lhs, out);
        out.push_back(e.rhs.get());
        return;
    }
    out.push_back(&e);
}

bool Emitter::is_int_expr_ir(const IrExpr& e) const {
    switch (e.kind) {
        case IrExprKind::IntLit: return true;
        case IrExprKind::Ident:  return e.type.base_name() == "int";
        case IrExprKind::Binary:
            if (e.text == "+" || e.text == "-" || e.text == "*")
                return e.lhs && e.rhs && is_int_expr_ir(*e.lhs) && is_int_expr_ir(*e.rhs);
            return false;
        default: return false;
    }
}

void Emitter::emit_expr(const IrExpr& e) {
    switch (e.kind) {
        case IrExprKind::StringLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::str(e.text)));
            break;
        case IrExprKind::IntLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::integer(e.int_value)));
            break;
        case IrExprKind::FloatLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::real(e.float_value)));
            break;
        case IrExprKind::FuncRef:
            // A function reference is a compile-time-known index, exactly
            // like a string/int literal is a compile-time-known value -- so
            // it rides the SAME Const/constant-pool mechanism instead of
            // needing a new opcode: Value::func(index) just sits in the
            // pool like any other Value.
            chunk_->emit(Op::Const, e.loc,
                        chunk_->add_constant(Value::func(e.call_index)));
            break;
        case IrExprKind::BoolLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::boolean(e.bool_value)));
            break;
        case IrExprKind::NullLit:
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::null()));
            break;

        case IrExprKind::Ident:
        case IrExprKind::This:
            chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(e.slot));
            break;

        case IrExprKind::Unary:
            emit_expr(*e.lhs);
            chunk_->emit(e.text == "not" ? Op::Not : Op::Neg, e.loc);
            break;

        case IrExprKind::Binary: {
            if (e.text == "and" || e.text == "or") {
                emit_expr(*e.lhs);
                size_t j = chunk_->emit(e.text == "and" ? Op::JumpIfFalsePeek
                                                        : Op::JumpIfTruePeek, e.loc);
                emit_expr(*e.rhs);
                chunk_->patch(j, chunk_->here());
                break;
            }

            // A chain of '+' collapses into a single ConcatN instead of N-1
            // pairwise Adds: each Add on strings currently allocates a new
            // one, so "a" + b + c + ... walked pairwise is O(n) allocations
            // for one logical concatenation. Int chains skip this — they
            // already compile to the cheap AddInt below, nothing to batch.
            if (e.text == "+" && !is_int_expr_ir(e)) {
                std::vector<const IrExpr*> parts;
                flatten_concat_ir(e, parts);
                if (parts.size() >= 3) {
                    for (const IrExpr* p : parts) emit_expr(*p);
                    chunk_->emit(Op::ConcatN, e.loc, static_cast<uint32_t>(parts.size()));
                    break;
                }
            }

            emit_expr(*e.lhs);
            emit_expr(*e.rhs);
            Op op = Op::Add;
            if      (e.text == "+")  op = Op::Add;
            else if (e.text == "-")  op = Op::Sub;
            else if (e.text == "*")  op = Op::Mul;
            else if (e.text == "/")  op = Op::Div;
            else if (e.text == "%")  op = Op::Mod;
            else if (e.text == "==") op = Op::Eq;
            else if (e.text == "!=") op = Op::Ne;
            else if (e.text == "<")  op = Op::Lt;
            else if (e.text == "<=") op = Op::Le;
            else if (e.text == ">")  op = Op::Gt;
            else if (e.text == ">=") op = Op::Ge;
            // check_expr already rejected any operator that isn't one of
            // these eleven -- an IrExpr::Binary with any other text
            // shouldn't be able to exist.

            if (is_int_expr_ir(*e.lhs) && is_int_expr_ir(*e.rhs)) {
                switch (op) {
                    case Op::Add: op = Op::AddInt; break;
                    case Op::Sub: op = Op::SubInt; break;
                    case Op::Mul: op = Op::MulInt; break;
                    case Op::Lt:  op = Op::LtInt;  break;
                    case Op::Le:  op = Op::LeInt;  break;
                    case Op::Gt:  op = Op::GtInt;  break;
                    case Op::Ge:  op = Op::GeInt;  break;
                    default: break;
                }
            }
            chunk_->emit(op, e.loc);
            break;
        }

        case IrExprKind::Ternary: {
            emit_expr(*e.object);
            size_t to_else = chunk_->emit(Op::JumpIfFalse, e.loc);
            emit_expr(*e.lhs);
            size_t to_end = chunk_->emit(Op::Jump, e.loc);
            chunk_->patch(to_else, chunk_->here());
            emit_expr(*e.rhs);
            chunk_->patch(to_end, chunk_->here());
            break;
        }

        case IrExprKind::ListLit:
            for (const auto& item : e.items) emit_expr(*item);
            chunk_->emit(Op::MakeList, e.loc, static_cast<uint32_t>(e.items.size()));
            break;

        case IrExprKind::DictLit:
            for (const auto& entry : e.entries) {
                emit_expr(*entry.key);
                emit_expr(*entry.value);
            }
            chunk_->emit(Op::MakeDict, e.loc, static_cast<uint32_t>(e.entries.size()));
            break;

        case IrExprKind::Index:
            emit_expr(*e.object);
            emit_expr(*e.lhs);
            chunk_->emit(Op::GetIndex, e.loc);
            break;

        case IrExprKind::Member: {
            // No receiver: either session.x (__session_get) or a
            // 0-argument member of a reserved object (sse.open),
            // distinguished by whether call_name comes filled in -- see
            // ir.hpp's comment on this reuse.
            if (!e.object) {
                if (e.call_name.empty()) {
                    chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::str(e.text)));
                    chunk_->emit(Op::CallNative, e.loc,
                                 (static_cast<uint32_t>(native_id("__session_get")) << 8) | 1u);
                } else {
                    chunk_->emit(Op::CallNative, e.loc, static_cast<uint32_t>(e.call_index) << 8);
                }
                break;
            }
            emit_expr(*e.object);
            chunk_->emit(Op::GetMember, e.loc, chunk_->add_constant(Value::str(e.text)));
            break;
        }

        case IrExprKind::Call:
            emit_call(e);
            break;

        case IrExprKind::PreStep:
        case IrExprKind::PostStep: {
            bool post = (e.kind == IrExprKind::PostStep);
            Op   op   = (e.text == "+") ? Op::Add : Op::Sub;
            const IrExpr& tgt = *e.lhs;

            auto one = [&] {
                chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::integer(1)));
            };

            if (tgt.kind == IrExprKind::Ident) {
                uint32_t u = static_cast<uint32_t>(tgt.slot);

                if (post) chunk_->emit(Op::LoadLocal, e.loc, u);
                chunk_->emit(Op::LoadLocal, e.loc, u);
                one();
                chunk_->emit(op, e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, u);
                if (!post) chunk_->emit(Op::LoadLocal, e.loc, u);
                break;
            }

            if (tgt.kind == IrExprKind::Member) {
                begin_scope();
                uint32_t name_k = chunk_->add_constant(Value::str(tgt.text));

                emit_expr(*tgt.object);
                int obj = declare_local(" receiver", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(obj));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(obj));
                chunk_->emit(Op::GetMember, e.loc, name_k);
                int prev = declare_local(" previous", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(prev));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(obj));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                one();
                chunk_->emit(op, e.loc);
                chunk_->emit(Op::SetMember, e.loc, name_k);

                if (post) {
                    chunk_->emit(Op::Pop, e.loc);
                    chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                }
                end_scope();
                break;
            }

            if (tgt.kind == IrExprKind::Index) {
                begin_scope();
                emit_expr(*tgt.object);
                int cont = declare_local(" container", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(cont));

                emit_expr(*tgt.lhs);
                int idx = declare_local(" index", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(idx));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(cont));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(idx));
                chunk_->emit(Op::GetIndex, e.loc);
                int prev = declare_local(" previous", e.loc);
                chunk_->emit(Op::StoreLocal, e.loc, static_cast<uint32_t>(prev));

                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(cont));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(idx));
                chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                one();
                chunk_->emit(op, e.loc);
                chunk_->emit(Op::SetIndex, e.loc);

                if (post) {
                    chunk_->emit(Op::Pop, e.loc);
                    chunk_->emit(Op::LoadLocal, e.loc, static_cast<uint32_t>(prev));
                }
                end_scope();
                break;
            }
            // check_expr already rejected any other target of ++/--.
            break;
        }

        case IrExprKind::Await:
            emit_call(*e.lhs); // e.lhs is always Call, with awaited already true
            break;
    }
}

// Counterpart of emit_method_call_dynamic over the IR: same order
// (receiver, positional arguments, named ones grouped into a Dict), same opcode.
void Emitter::emit_method_call_dynamic(const IrExpr& e) {
    emit_expr(*e.object);
    uint32_t argc = 0, named = 0;
    for (const auto& a : e.args) {
        if (!a.name.empty()) { ++named; continue; }
        emit_expr(*a.value);
        ++argc;
    }
    if (named > 0) {
        for (const auto& a : e.args) {
            if (a.name.empty()) continue;
            chunk_->emit(Op::Const, a.loc, chunk_->add_constant(Value::str(a.name)));
            emit_expr(*a.value);
        }
        chunk_->emit(Op::MakeDict, e.loc, named);
        ++argc;
    }
    // check_builtin_method already rejected more than 255 arguments.
    chunk_->emit(Op::CallMethod, e.loc,
                 (chunk_->add_constant(Value::str(e.call_name)) << 8) | argc);
}

// Tail shared by ReservedMemberCall and BuiltinGlobalCall (except
// render()): both resolve to a valid native_id with the same named-argument
// rules emit_call has today.
void Emitter::emit_native_call(const IrExpr& e) {
    uint32_t argc = 0, named = 0;
    for (const auto& a : e.args) {
        if (a.name.empty()) { emit_expr(*a.value); ++argc; }
        else ++named;
    }
    if (named > 0) {
        for (const auto& a : e.args) {
            if (a.name.empty()) continue;
            chunk_->emit(Op::Const, a.loc, chunk_->add_constant(Value::str(a.name)));
            emit_expr(*a.value);
        }
        chunk_->emit(Op::MakeDict, e.loc, named);
        ++argc;
    }
    const NativeDef& def = native_at(e.call_index);
    if (def.is_async) chunk_->has_await = true;
    chunk_->emit(def.is_async ? Op::CallAsync : Op::CallNative, e.loc,
                 (static_cast<uint32_t>(e.call_index) << 8) | argc);
}

void Emitter::emit_call(const IrExpr& e) {
    switch (e.call_shape) {
        case IrCallShape::BuiltinModuleCall: {
            // No module-name constant to push, unlike DbModuleCall: the
            // (module, function) pair is already resolved to a single flat
            // id at check time (BuiltinModuleRegistry::id_of()), so the VM
            // never has to look either up by name.
            //
            // e.awaited is exactly fn->is_async here -- check_call rejected
            // every OTHER combination already (an is_async function without
            // `await`, or `await` on a plain one), so it is safe to branch
            // on it alone to pick the opcode, with no need to re-look-up
            // BuiltinModuleFn here just to read is_async again.
            for (const auto& a : e.args) emit_expr(*a.value);
            if (e.awaited) chunk_->has_await = true;
            chunk_->emit(e.awaited ? Op::CallAsyncModule : Op::CallBuiltinModule, e.loc,
                         (static_cast<uint32_t>(e.call_index) << 8) |
                         static_cast<uint32_t>(e.args.size()));
            break;
        }

        case IrCallShape::DbModuleCall: {
            chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::str(e.call_name)));
            for (const auto& a : e.args) emit_expr(*a.value);
            chunk_->has_await = true;
            chunk_->emit(Op::CallAsync, e.loc,
                         (static_cast<uint32_t>(e.call_index) << 8) |
                         static_cast<uint32_t>(e.args.size() + 1));
            break;
        }

        case IrCallShape::UserFunctionCall:
        case IrCallShape::ConstructorCall: {
            for (const auto& a : e.args) emit_expr(*a.value);
            chunk_->emit(Op::CallFunction, e.loc,
                         (static_cast<uint32_t>(e.call_index) << 8) |
                         static_cast<uint32_t>(e.args.size()));
            break;
        }

        case IrCallShape::ClassMethodCall: {
            emit_expr(*e.object); // `this`
            for (const auto& a : e.args) emit_expr(*a.value);
            chunk_->emit(Op::CallFunction, e.loc,
                         (static_cast<uint32_t>(e.call_index) << 8) |
                         static_cast<uint32_t>(e.args.size() + 1));
            break;
        }

        case IrCallShape::ReservedMemberCall:
            emit_native_call(e);
            break;

        case IrCallShape::BuiltinGlobalCall:
            if (e.call_name == "render") emit_compiled_render(e);
            else                         emit_native_call(e);
            break;

        case IrCallShape::BuiltinMethodCall:
            emit_method_call_dynamic(e);
            break;

        case IrCallShape::Invalid:
            break; // unreachable: check_call already rejected it
    }
}

// Counterpart of emit_compiled_render over the IR. It remains the one real
// exception to "check_* is the only source of diagnostics": the template
// itself (its file, its own {{ }}) cannot be validated without reading and
// compiling it, and that's template surface (phase 3), not something
// check_call can anticipate over an expression. error()/diags_ are still
// correct here on purpose.
void Emitter::emit_compiled_render(const IrExpr& e) {
    const std::string& name = e.args[0].value->text;

    if (name.find("..") != std::string::npos ||
        std::filesystem::path(name).is_absolute()) {
        error(e.args[0].loc, "invalid template name: '" + name + "'");
        return;
    }

    std::vector<TypedName> keys;
    for (const auto& a : e.args) {
        if (a.name.empty()) continue;
        keys.push_back({a.name, a.value->type.base_name()});
    }

    auto source = read_whole_file(std::filesystem::path(templates_->dir) / name);
    if (!source) {
        error(e.args[0].loc, "template not found: '" + name + "' in " +
                             templates_->dir);
        return;
    }

    Template tpl;
    if (!compile_template(*source, name, templates_->dir, keys, diags_, tpl)) {
        failed_ = true;
        return;
    }
    const uint32_t idx = static_cast<uint32_t>(templates_->table->size());
    templates_->table->push_back(std::move(tpl));
    if (templates_->by_key) {
        std::string key = name + "|";
        for (const auto& k : keys) key += k.name + ":" + k.type + ",";
        templates_->by_key->emplace(std::move(key), idx);
    }

    chunk_->emit(Op::Const, e.loc, chunk_->add_constant(Value::integer(idx)));
    for (const auto& a : e.args) {
        if (a.name.empty()) continue;
        chunk_->emit(Op::Const, a.loc, chunk_->add_constant(Value::str(a.name)));
        emit_expr(*a.value);
    }
    chunk_->emit(Op::MakeDict, e.loc, static_cast<uint32_t>(keys.size()));

    const int id = native_id("__render_tpl");
    chunk_->emit(Op::CallNative, e.loc, (static_cast<uint32_t>(id) << 8) | 2u);
}

void Emitter::emit_block(const IrBlock& body) {
    begin_scope();
    for (const auto& s : body) emit_stmt(*s);
    end_scope();
}

void Emitter::emit_stmt(const IrStmt& s) {
    switch (s.kind) {
        case IrStmtKind::Return:
            if (s.value) { emit_expr(*s.value); chunk_->emit(Op::Return, s.loc); }
            else         { chunk_->emit(Op::ReturnNull, s.loc); }
            break;

        case IrStmtKind::ExprStmt:
            emit_expr(*s.value);
            chunk_->emit(Op::Pop, s.loc);
            break;

        case IrStmtKind::VarDecl:
            if (s.value) {
                emit_expr(*s.value);
                // See CoerceInt/CoerceFloat's own comment (bytecode.hpp):
                // only int/float are ambiguous at runtime (division), so
                // only those two declared types get a coercion here.
                if (s.decl_type.kind() == Type::Kind::Int)
                    chunk_->emit(Op::CoerceInt, s.loc);
                else if (s.decl_type.kind() == Type::Kind::Float)
                    chunk_->emit(Op::CoerceFloat, s.loc);
            }
            else         chunk_->emit(Op::Const, s.loc, chunk_->add_constant(Value::null()));
            // declare_local() has to be called here too, even though s.slot
            // already carries the real slot: it's what keeps locals_ (and
            // therefore the next slot a later declare_local computes -- the
            // desugared `for`, a `catch`, a ++/-- temporary) synchronized
            // with what check_stmt already computed. Without this, the
            // body's first VarDecl leaves locals_ shorter than it should be
            // and everything declared afterward lands in the wrong slot --
            // exactly the bug tests/emit_ir_shadow.cpp found before this fix.
            declare_local(s.name, s.loc, s.decl_type);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(s.slot));
            break;

        case IrStmtKind::Assign: {
            switch (s.assign_target) {
                case IrAssignTarget::Session:
                    chunk_->emit(Op::Const, s.loc,
                                 chunk_->add_constant(Value::str(s.assign_field)));
                    emit_expr(*s.value);
                    chunk_->emit(Op::CallNative, s.loc,
                                 (static_cast<uint32_t>(native_id("__session_set")) << 8) | 2u);
                    chunk_->emit(Op::Pop, s.loc);
                    break;

                case IrAssignTarget::Index:
                    emit_expr(*s.assign_object);
                    emit_expr(*s.assign_index);
                    emit_expr(*s.value);
                    chunk_->emit(Op::SetIndex, s.loc);
                    chunk_->emit(Op::Pop, s.loc);
                    break;

                case IrAssignTarget::Member:
                    emit_expr(*s.assign_object);
                    emit_expr(*s.value);
                    chunk_->emit(Op::SetMember, s.loc,
                                 chunk_->add_constant(Value::str(s.assign_field)));
                    chunk_->emit(Op::Pop, s.loc);
                    break;

                case IrAssignTarget::Local:
                    emit_expr(*s.value);
                    chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(s.assign_slot));
                    break;
            }
            break;
        }

        case IrStmtKind::If: {
            emit_expr(*s.value);
            size_t to_else = chunk_->emit(Op::JumpIfFalse, s.loc);
            emit_block(s.body);

            if (!s.orelse.empty()) {
                size_t to_end = chunk_->emit(Op::Jump, s.loc);
                chunk_->patch(to_else, chunk_->here());
                emit_block(s.orelse);
                chunk_->patch(to_end, chunk_->here());
            } else {
                chunk_->patch(to_else, chunk_->here());
            }
            break;
        }

        case IrStmtKind::While: {
            size_t start = chunk_->here();
            emit_expr(*s.value);
            size_t to_end = chunk_->emit(Op::JumpIfFalse, s.loc);

            loops_.push_back({});
            emit_block(s.body);
            for (size_t j : loops_.back().continues) chunk_->patch(j, start);
            chunk_->emit(Op::Jump, s.loc, static_cast<uint32_t>(start));
            chunk_->patch(to_end, chunk_->here());

            for (size_t j : loops_.back().breaks) chunk_->patch(j, chunk_->here());
            loops_.pop_back();
            break;
        }

        case IrStmtKind::Require: {
            emit_expr(*s.value);
            size_t to_ok = chunk_->emit(Op::JumpIfFalse, s.loc);
            size_t skip  = chunk_->emit(Op::Jump, s.loc);
            chunk_->patch(to_ok, chunk_->here());
            emit_expr(*s.target);
            chunk_->emit(Op::Return, s.loc);
            chunk_->patch(skip, chunk_->here());
            break;
        }

        case IrStmtKind::Break:
            loops_.back().breaks.push_back(chunk_->emit(Op::Jump, s.loc));
            break;

        case IrStmtKind::Continue:
            loops_.back().continues.push_back(chunk_->emit(Op::Jump, s.loc));
            break;

        case IrStmtKind::For: {
            begin_scope();

            emit_expr(*s.target);
            chunk_->emit(Op::IterList, s.loc);
            int items = declare_local(" items", s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(items));

            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(items));
            chunk_->emit(Op::CallNative, s.loc,
                         (static_cast<uint32_t>(native_id("len")) << 8) | 1u);
            int count = declare_local(" count", s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(count));

            chunk_->emit(Op::Const, s.loc, chunk_->add_constant(Value::integer(0)));
            int index = declare_local(" index", s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(index));

            // `s.slot` is already the loop variable's slot -- but
            // declare_local still has to be called, in the same order, so
            // that items/count/index land in the slots check_stmt already
            // computed (see IrStmt::slot's comment). The return value is
            // discarded because it's already known.
            declare_local(s.name, s.loc, s.decl_type);
            uint32_t var = static_cast<uint32_t>(s.slot);

            size_t start = chunk_->here();
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(count));
            chunk_->emit(Op::Lt, s.loc);
            size_t to_end = chunk_->emit(Op::JumpIfFalse, s.loc);

            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(items));
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::GetIndex, s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, var);

            loops_.push_back({});
            emit_block(s.body);

            size_t step = chunk_->here();
            for (size_t j : loops_.back().continues) chunk_->patch(j, step);
            chunk_->emit(Op::LoadLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::Const, s.loc, chunk_->add_constant(Value::integer(1)));
            chunk_->emit(Op::Add, s.loc);
            chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(index));
            chunk_->emit(Op::Jump, s.loc, static_cast<uint32_t>(start));

            chunk_->patch(to_end, chunk_->here());
            for (size_t j : loops_.back().breaks) chunk_->patch(j, chunk_->here());
            loops_.pop_back();

            end_scope();
            break;
        }

        case IrStmtKind::Try: {
            TryRange range;
            range.begin = chunk_->here();
            emit_block(s.body);
            range.end = chunk_->here();

            size_t to_end = chunk_->emit(Op::Jump, s.loc);
            range.catch_pc = chunk_->here();
            chunk_->try_ranges.push_back(range);

            begin_scope();
            if (!s.name.empty()) {
                // Same reason as in For: declare_local has to be called so
                // that the real slot matches the one check_stmt already
                // computed (s.slot); it isn't resolved by name here.
                declare_local(s.name, s.loc);
                chunk_->emit(Op::StoreLocal, s.loc, static_cast<uint32_t>(s.slot));
            } else {
                chunk_->emit(Op::Pop, s.loc);
            }
            for (const auto& st : s.orelse) emit_stmt(*st);
            end_scope();

            chunk_->patch(to_end, chunk_->here());
            break;
        }
    }
}

} // namespace lux_script
