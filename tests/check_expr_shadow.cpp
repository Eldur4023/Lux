// Verification of Emitter::check_expr/check_call (--native, phase 1,
// step 2): checks that they reproduce, letter by letter, the diagnostics
// already produced by the real compilation (emit_expr/emit_call seen through
// emit_condition) for a standalone expression. It does not replace emit_expr
// anywhere real yet -- this is just the comparison the plan calls for before
// taking that step.
#include <lux_script/diagnostic.hpp>
#include <lux_script/emitter.hpp>
#include <lux_script/lexer.hpp>
#include <lux_script/parser.hpp>

#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace lux_script;

static int failures = 0;

static ExprPtr parse(const std::string& src, SourceFile& file, DiagnosticBag& diags) {
    file.path = "<test>";
    file.text = src;
    Lexer lexer(file, diags);
    Parser parser(lexer.tokenize(), diags);
    return parser.parse_single_expression();
}

// Compiles `src` as a standalone expression with `names` declared, once
// through the real path (emit_condition) and once through the parallel
// checker (check_condition), and compares both message lists. It also
// checks the other half of what check_expr/check_call does since they
// return an IrExprPtr: that the built node is non-null exactly when the
// real compilation succeeded, and null exactly when it failed -- a
// half-built IrExpr should never survive an error in any case.
// `inspect`, if given, receives the built IrExpr (only called if it is not
// null) to check its shape (call_shape, slot, etc.).
static void case_(const char* name, const std::string& src,
                 const std::vector<TypedName>& names,
                 const std::string& expected_substr,
                 const FunctionSigs* fns = nullptr, const ClassSigs* classes = nullptr,
                 const std::set<std::string>* imports = nullptr,
                 const std::function<bool(const IrExpr&, std::string&)>& inspect = {}) {
    SourceFile   file;
    DiagnosticBag diag_parse;
    ExprPtr       e = parse(src, file, diag_parse);
    if (!e) {
        ++failures;
        std::printf("  FAIL %s (did not parse: %s)\n", name,
                    diag_parse.items().empty() ? "?" : diag_parse.items().front().message.c_str());
        return;
    }

    DiagnosticBag diags_real, diags_shadow;
    Emitter       em(diags_real, fns, classes, imports);
    Chunk         chunk;
    em.emit_condition(*e, names, chunk);
    IrExprPtr ir = em.check_condition(*e, names, chunk, diags_shadow);

    auto texts = [](const DiagnosticBag& d) {
        std::vector<std::string> v;
        for (const auto& it : d.items()) v.push_back(it.message);
        return v;
    };
    std::vector<std::string> real = texts(diags_real), shadow = texts(diags_shadow);

    // Empty string = happy path: the real compilation is expected to give
    // NO diagnostics at all (and therefore neither does the parallel checker).
    if (expected_substr.empty()) {
        if (!real.empty()) {
            ++failures;
            std::printf("  FAIL %s (expected a clean compile, got %zu error(s))\n",
                        name, real.size());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
        if (!ir) {
            ++failures;
            std::printf("  FAIL %s (compiled clean but check_expr/check_call returned "
                        "nullptr)\n", name);
            return;
        }
    } else {
        bool real_has_expected = false;
        for (const auto& m : real)
            if (m.find(expected_substr) != std::string::npos) real_has_expected = true;

        if (!real_has_expected) {
            ++failures;
            std::printf("  FAIL %s (the real compilation did not give \"%s\"; gave %zu error(s))\n",
                        name, expected_substr.c_str(), real.size());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
        if (ir) {
            ++failures;
            std::printf("  FAIL %s (compilation failed but check_expr/check_call "
                        "returned a non-null IrExpr)\n", name);
            return;
        }
    }

    if (real != shadow) {
        ++failures;
        std::printf("  FAIL %s (check_expr/check_call does not reproduce the same thing)\n", name);
        std::printf("           real   (%zu): ", real.size());
        for (const auto& m : real) std::printf("[%s] ", m.c_str());
        std::printf("\n           shadow (%zu): ", shadow.size());
        for (const auto& m : shadow) std::printf("[%s] ", m.c_str());
        std::printf("\n");
        return;
    }

    if (ir && inspect) {
        std::string reason;
        if (!inspect(*ir, reason)) {
            ++failures;
            std::printf("  FAIL %s (IrExpr shape: %s)\n", name, reason.c_str());
            return;
        }
    }

    std::printf("  ok    %s\n", name);
}

int main() {
    // ── The 6 branches of emit_expr/emit_call already covered by the real
    //    corpus (tests/cases/bad/*.lux) ─────────────────────────────────────
    case_("async builtin without await (bad/await.lux)", "sleep(10)", {}, "is asynchronous");

    case_("reserved object out of place (bad/sse.lux)", "sse.send(\"x\")", {},
         "only exists inside a route sse");

    {
        ClassSigs classes;
        classes["P"].fields = {"x"};
        case_("nonexistent field on a class (bad/field_type.lux)", "p.noexiste",
             {{"p", "P"}}, "has no field", nullptr, &classes);
    }

    {
        ClassSigs classes;
        classes["P"].fields = {"x"};
        case_("nonexistent method on a class (bad/method.lux)", "p.noexiste()",
             {{"p", "P"}}, "has no method", nullptr, &classes);
    }

    case_("nonexistent builtin method on a string (bad/method_type.lux)",
         "quien.mayusculas()", {{"quien", "string"}}, "have no method");

    case_("db module without import (bad/import.lux)",
         "await sqlite.query(\"select 1\")", {}, "missing 'import sqlite'");

    // ── Branches of the other call shapes (IrCallShape, ir.hpp),
    //    with no dedicated corpus but built against the real compilation ────
    {
        ClassSigs classes;
        classes["Usuario"].ctors[1] = 0;
        case_("constructor arity", "Usuario(1, 2, 3)", {}, "has no constructor taking 3",
             nullptr, &classes);
    }

    {
        FunctionSigs fns;
        fns["saluda"].required = 1;
        fns["saluda"].defaults.resize(1);
        case_("user function arity", "saluda()", {}, "expects 1 argument", &fns);
    }

    case_("unknown function", "no_existe_esta_funcion()", {}, "unknown function");

    case_("undeclared identifier", "equis", {}, "is not declared");

    case_("builtin used as a value, not called", "len", {}, "is a builtin");

    case_("await on something that is not a call", "await 5", {},
         "only applies to an asynchronous call");

    case_("call on something not callable (shape 8: invalid)", "(a + b)()",
         {{"a", "int"}, {"b", "int"}}, "for now only builtins or methods can be called");

    case_("ws outside a ws route", "ws.send(\"x\")", {}, "only exists inside a route ws");

    // ── Happy path: neither path should complain, and the shape of the
    //    built IrExpr is also inspected against the IrCallShape (ir.hpp)
    //    shapes reachable from a standalone expression (check_condition
    //    pins route_method_ to "", so ReservedMemberCall can only be
    //    exercised in its error branch, already covered above) ─────────────
    case_("valid builtin method on a string", "quien.upper()", {{"quien", "string"}}, "",
         nullptr, nullptr, nullptr,
         [](const IrExpr& ir, std::string& why) {
             if (ir.kind != IrExprKind::Call) { why = "not a Call"; return false; }
             if (ir.call_shape != IrCallShape::BuiltinMethodCall) {
                 why = "call_shape is not BuiltinMethodCall"; return false;
             }
             if (ir.call_name != "upper") { why = "call_name != 'upper'"; return false; }
             if (!ir.object || ir.object->kind != IrExprKind::Ident) {
                 why = "the receiver is not the expected Ident"; return false;
             }
             return true;
         });

    {
        ClassSigs classes;
        classes["P"].fields = {"x"};
        case_("valid field on a class", "p.x", {{"p", "P"}}, "", nullptr, &classes);
    }

    {
        FunctionSigs fns;
        fns["saluda"].required = 1;
        fns["saluda"].defaults.resize(1);
        case_("valid call to a user function", "saluda(1)", {}, "", &fns,
             nullptr, nullptr,
             [](const IrExpr& ir, std::string& why) {
                 if (ir.call_shape != IrCallShape::UserFunctionCall) {
                     why = "call_shape is not UserFunctionCall"; return false;
                 }
                 if (ir.call_index != 0) { why = "call_index is not the expected index"; return false; }
                 if (ir.args.size() != 1) { why = "does not carry the single argument given"; return false; }
                 return true;
             });
    }

    {
        ClassSigs classes;
        classes["Usuario"].ctors[2] = 3;
        case_("valid call to a constructor", "Usuario(1, 2)", {}, "", nullptr, &classes,
             nullptr,
             [](const IrExpr& ir, std::string& why) {
                 if (ir.call_shape != IrCallShape::ConstructorCall) {
                     why = "call_shape is not ConstructorCall"; return false;
                 }
                 if (ir.call_index != 3) { why = "call_index is not the ctor's index"; return false; }
                 if (ir.args.size() != 2) { why = "does not carry the 2 arguments given"; return false; }
                 return true;
             });
    }

    {
        ClassSigs classes;
        classes["Punto"].fields = {"x", "y"};
        classes["Punto"].methods["cuadrado"] = {5, 0, {}};
        case_("valid call to a class method", "p.cuadrado()", {{"p", "Punto"}}, "",
             nullptr, &classes, nullptr,
             [](const IrExpr& ir, std::string& why) {
                 if (ir.call_shape != IrCallShape::ClassMethodCall) {
                     why = "call_shape is not ClassMethodCall"; return false;
                 }
                 if (ir.call_index != 5) { why = "call_index is not the method's index"; return false; }
                 if (!ir.object || ir.object->kind != IrExprKind::Ident || ir.object->slot != 0) {
                     why = "the receiver does not carry 'p's resolved slot"; return false;
                 }
                 return true;
             });
    }

    {
        std::set<std::string> imports = {"sqlite"};
        case_("valid call to an async builtin with await",
             "await sqlite.query(\"select 1\")", {}, "", nullptr, nullptr, &imports,
             [](const IrExpr& ir, std::string& why) {
                 if (ir.kind != IrExprKind::Await) { why = "not an Await"; return false; }
                 if (!ir.lhs || ir.lhs->call_shape != IrCallShape::DbModuleCall) {
                     why = "the wrapped call is not a DbModuleCall"; return false;
                 }
                 if (ir.lhs->call_name != "sqlite") { why = "call_name is not 'sqlite'"; return false; }
                 if (!ir.lhs->awaited) { why = "awaited did not end up true"; return false; }
                 if (ir.lhs->args.size() != 1) { why = "does not carry the query argument"; return false; }
                 return true;
             });
    }

    case_("identifier with a resolved slot", "equis", {{"equis", "int"}}, "",
         nullptr, nullptr, nullptr,
         [](const IrExpr& ir, std::string& why) {
             if (ir.kind != IrExprKind::Ident) { why = "not an Ident"; return false; }
             if (ir.slot != 0) { why = "slot is not 0"; return false; }
             if (ir.type != Type::primitive(Type::Kind::Int)) { why = "type is not int"; return false; }
             return true;
         });

    if (failures == 0) {
        std::printf("check_expr_shadow: everything reproduced, %d failures\n", failures);
        return 0;
    }
    std::printf("check_expr_shadow: %d failure(s)\n", failures);
    return 1;
}
