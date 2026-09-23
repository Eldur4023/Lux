// Verification of Emitter::check_stmt/check_block (phase 1, NATIVE-
// COMPILATION.md, step 3 of section 1.1): compares, statement by statement,
// the output of check_route/check_function/check_method/check_ctor against
// the real compilation (emit_route/emit_function/emit_method/emit_ctor) over
// full real functions -- not standalone expressions, which is already
// verified by tests/check_expr_shadow.cpp.
#include <lux_script/diagnostic.hpp>
#include <lux_script/emitter.hpp>
#include <lux_script/lexer.hpp>
#include <lux_script/parser.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace lux_script;

static int failures = 0;

static bool parse_program(const std::string& src, SourceFile& file, DiagnosticBag& diags,
                          Program& out) {
    file.path = "<test>";
    file.text = src;
    Lexer  lexer(file, diags);
    Parser parser(lexer.tokenize(), diags);
    parser.parse_into(out);
    return diags.empty();
}

static std::vector<std::string> texts(const DiagnosticBag& d) {
    std::vector<std::string> v;
    for (const auto& it : d.items()) v.push_back(it.message);
    return v;
}

// Compiles the first `fn` of `src` as a user function, once through the real
// path and once through the parallel checker, and compares both lists.
// `inspect`, if given, receives the built IrBlock (only if the happy path
// applied, i.e. `expected_substr` empty) to check its shape.
static void case_fn(const char* name, const std::string& src,
                    const std::string& expected_substr,
                    const std::function<bool(const IrBlock&, std::string&)>& inspect = {}) {
    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    if (!parse_program(src, file, diag_parse, prog) || prog.functions.empty()) {
        ++failures;
        std::printf("  FAIL %s (did not parse: %s)\n", name,
                    diag_parse.items().empty() ? "no fn" : diag_parse.items().front().message.c_str());
        return;
    }

    DiagnosticBag diags_real, diags_shadow;
    Emitter       em(diags_real, nullptr, nullptr, nullptr);
    Chunk         chunk;
    em.emit_function(prog.functions[0], chunk);
    IrBlock body;
    em.check_function(prog.functions[0], chunk, diags_shadow, &body);

    std::vector<std::string> real = texts(diags_real), shadow = texts(diags_shadow);

    if (expected_substr.empty()) {
        if (!real.empty()) {
            ++failures;
            std::printf("  FAIL %s (expected a clean compile, got %zu error(s))\n",
                        name, real.size());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
    } else {
        bool ok = false;
        for (const auto& m : real) if (m.find(expected_substr) != std::string::npos) ok = true;
        if (!ok) {
            ++failures;
            std::printf("  FAIL %s (the real compilation did not give \"%s\"; gave %zu error(s))\n",
                        name, expected_substr.c_str(), real.size());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
    }

    if (real != shadow) {
        ++failures;
        std::printf("  FAIL %s (check_stmt does not reproduce the same thing)\n", name);
        std::printf("           real   (%zu): ", real.size());
        for (const auto& m : real) std::printf("[%s] ", m.c_str());
        std::printf("\n           shadow (%zu): ", shadow.size());
        for (const auto& m : shadow) std::printf("[%s] ", m.c_str());
        std::printf("\n");
        return;
    }

    if (expected_substr.empty() && inspect) {
        std::string reason;
        if (!inspect(body, reason)) {
            ++failures;
            std::printf("  FAIL %s (IrBlock shape: %s)\n", name, reason.c_str());
            return;
        }
    }

    std::printf("  ok    %s\n", name);
}

// Same as case_fn, but for a whole class: compiles the first method and the
// first constructor (if any) of the first class.
static void case_class(const char* name, const std::string& src,
                       const std::string& expected_substr,
                       const std::function<bool(const IrBlock&, std::string&)>& inspect = {}) {
    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    if (!parse_program(src, file, diag_parse, prog) || prog.classes.empty()) {
        ++failures;
        std::printf("  FAIL %s (did not parse: %s)\n", name,
                    diag_parse.items().empty() ? "no class" : diag_parse.items().front().message.c_str());
        return;
    }
    const ClassDecl& cls = prog.classes[0];
    std::vector<std::string> fields;
    for (const auto& f : cls.fields) fields.push_back(f.name);

    // check_field only looks at classes_ if it comes with the class already
    // registered: without this, "P" has no table and the field is assumed
    // valid.
    ClassSigs classes;
    classes[cls.name].fields = fields;
    for (const auto& m : cls.methods) classes[cls.name].methods[m.name] = {};

    DiagnosticBag diags_real, diags_shadow;
    Emitter       em(diags_real, nullptr, &classes, nullptr);
    IrBlock       body;

    if (!cls.methods.empty()) {
        Chunk chunk;
        em.emit_method(cls.name, cls.methods[0], chunk);
        em.check_method(cls.name, cls.methods[0], chunk, diags_shadow, &body);
    }
    if (!cls.ctors.empty()) {
        Chunk chunk;
        em.emit_ctor(cls.name, fields, cls.ctors[0], chunk);
        em.check_ctor(cls.name, fields, cls.ctors[0], chunk, diags_shadow, &body);
    }

    std::vector<std::string> real = texts(diags_real), shadow = texts(diags_shadow);

    if (expected_substr.empty()) {
        if (!real.empty()) {
            ++failures;
            std::printf("  FAIL %s (expected a clean compile, got %zu error(s))\n",
                        name, real.size());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
    } else {
        bool ok = false;
        for (const auto& m : real) if (m.find(expected_substr) != std::string::npos) ok = true;
        if (!ok) {
            ++failures;
            std::printf("  FAIL %s (the real compilation did not give \"%s\")\n", name,
                        expected_substr.c_str());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
    }

    if (real != shadow) {
        ++failures;
        std::printf("  FAIL %s (check_stmt does not reproduce the same thing)\n", name);
        std::printf("           real   (%zu): ", real.size());
        for (const auto& m : real) std::printf("[%s] ", m.c_str());
        std::printf("\n           shadow (%zu): ", shadow.size());
        for (const auto& m : shadow) std::printf("[%s] ", m.c_str());
        std::printf("\n");
        return;
    }

    if (expected_substr.empty() && inspect) {
        std::string reason;
        if (!inspect(body, reason)) {
            ++failures;
            std::printf("  FAIL %s (IrBlock shape: %s)\n", name, reason.c_str());
            return;
        }
    }

    std::printf("  ok    %s\n", name);
}

int main() {
    // ── Happy path: VarDecl, if/else, while, for with break/continue,
    //    try/catch, indices, ++/--, already-desugared compound assignments.
    case_fn("basic arithmetic and control flow",
        "fn int suma(int a, int b):\n"
        "    int c = a + b\n"
        "    if c > 10:\n"
        "        return c\n"
        "    else:\n"
        "        return 0\n",
        "",
        [](const IrBlock& body, std::string& why) {
            // a: slot 0, b: slot 1 (parameters) -> c: slot 2 (VarDecl).
            if (body.size() != 2) { why = "not 2 statements (VarDecl + If)"; return false; }
            const IrStmt& decl = *body[0];
            if (decl.kind != IrStmtKind::VarDecl) { why = "the 1st is not a VarDecl"; return false; }
            if (decl.name != "c" || decl.slot != 2) { why = "'c' did not end up in slot 2"; return false; }
            if (decl.decl_type != Type::primitive(Type::Kind::Int)) {
                why = "'c's decl_type is not int"; return false;
            }
            const IrStmt& si = *body[1];
            if (si.kind != IrStmtKind::If) { why = "the 2nd is not an If"; return false; }
            if (si.body.size() != 1 || si.orelse.size() != 1) {
                why = "If does not carry a Return in each branch"; return false;
            }
            return true;
        });

    case_fn("while with assignment",
        "fn int cuenta(int n):\n"
        "    int i = 0\n"
        "    int total = 0\n"
        "    while i < n:\n"
        "        total = total + i\n"
        "        i = i + 1\n"
        "    return total\n",
        "",
        [](const IrBlock& body, std::string& why) {
            // n: slot 0, i: slot 1, total: slot 2.
            const IrStmt& loop = *body[2];
            if (loop.kind != IrStmtKind::While) { why = "the 3rd is not a While"; return false; }
            const IrStmt& assign_total = *loop.body[0];
            if (assign_total.kind != IrStmtKind::Assign ||
                assign_total.assign_target != IrAssignTarget::Local ||
                assign_total.assign_slot != 2) {
                why = "'total = ...' did not end up as an Assign Local to slot 2"; return false;
            }
            return true;
        });

    case_fn("for with break and continue",
        "fn int recorre(List<int> xs):\n"
        "    int total = 0\n"
        "    for int x in xs:\n"
        "        if x == 0:\n"
        "            continue\n"
        "        if x < 0:\n"
        "            break\n"
        "        total = total + x\n"
        "    return total\n",
        "",
        [](const IrBlock& body, std::string& why) {
            // xs: slot 0, total: slot 1; inside the For: items/count/index
            // (2, 3, 4) and x in slot 5.
            const IrStmt& loop = *body[1];
            if (loop.kind != IrStmtKind::For) { why = "the 2nd is not a For"; return false; }
            if (loop.name != "x" || loop.slot != 5) {
                why = "'x' did not end up in slot 5 after the 3 helper slots"; return false;
            }
            if (loop.decl_type != Type::primitive(Type::Kind::Int)) {
                why = "'x's decl_type is not int"; return false;
            }
            if (!loop.target || loop.target->kind != IrExprKind::Ident ||
                loop.target->slot != 0) {
                why = "the iterable is not the Ident 'xs' with its resolved slot"; return false;
            }
            return true;
        });

    case_fn("try/catch",
        "fn int intenta():\n"
        "    try:\n"
        "        return 1\n"
        "    catch e:\n"
        "        return 0\n",
        "",
        [](const IrBlock& body, std::string& why) {
            const IrStmt& attempt = *body[0];
            if (attempt.kind != IrStmtKind::Try) { why = "not a Try"; return false; }
            if (attempt.name != "e" || attempt.slot != 0) {
                why = "'e' did not end up with resolved name/slot"; return false;
            }
            if (attempt.body.size() != 1 || attempt.orelse.size() != 1) {
                why = "Try does not carry a Return in the body and in the catch"; return false;
            }
            return true;
        });

    case_fn("indices and ++/--",
        "fn int mezcla():\n"
        "    List<int> l = [1, 2, 3]\n"
        "    l[0] = 9\n"
        "    int i = 0\n"
        "    i++\n"
        "    return l[0] + i\n",
        "",
        [](const IrBlock& body, std::string& why) {
            const IrStmt& assign = *body[1];
            if (assign.kind != IrStmtKind::Assign ||
                assign.assign_target != IrAssignTarget::Index) {
                why = "'l[0] = 9' did not end up as an Assign Index"; return false;
            }
            if (!assign.assign_object || assign.assign_object->slot != 0) {
                why = "the indexed object is not 'l' with its resolved slot"; return false;
            }
            return true;
        });

    // ── Errors: one per new branch touched by check_stmt ──────────────────
    case_fn("assigning to an undeclared variable",
        "fn int malo():\n"
        "    equis = 1\n"
        "    return equis\n",
        "is not declared");

    case_fn("break outside a loop",
        "fn int malo2():\n"
        "    break\n"
        "    return 0\n",
        "'break' outside a loop");

    case_fn("continue outside a loop",
        "fn int malo3():\n"
        "    continue\n"
        "    return 0\n",
        "'continue' outside a loop");

    case_fn("require with a condition using an undeclared variable",
        "fn int malo4():\n"
        "    require equis else 0\n"
        "    return 1\n",
        "is not declared");

    // ── Classes: valid/invalid field in an Assign to a Member, and a
    //    bodyless constructor with a parameter that is not a field ─────────
    case_class("assignment to a valid field in a method",
        "class P:\n"
        "    int x\n"
        "\n"
        "    fn void pon(int v):\n"
        "        this.x = v\n",
        "",
        [](const IrBlock& body, std::string& why) {
            if (body.empty()) { why = "empty body"; return false; }
            const IrStmt& assign = *body[0];
            if (assign.kind != IrStmtKind::Assign ||
                assign.assign_target != IrAssignTarget::Member) {
                why = "'this.x = v' did not end up as an Assign Member"; return false;
            }
            if (assign.assign_field != "x") { why = "assign_field is not 'x'"; return false; }
            // this: slot 0.
            if (!assign.assign_object || assign.assign_object->kind != IrExprKind::This ||
                assign.assign_object->slot != 0) {
                why = "the receiver is not the 'this' with its resolved slot"; return false;
            }
            return true;
        });

    case_class("assignment to a nonexistent field in a method",
        "class P:\n"
        "    int x\n"
        "\n"
        "    fn void pon(int v):\n"
        "        this.noexiste = v\n",
        "has no field");

    case_class("implicit constructor with a parameter that is not a field",
        "class P:\n"
        "    int x\n"
        "\n"
        "    P(int x, int sobra)\n",
        "is not a field");

    if (failures == 0) {
        std::printf("check_stmt_shadow: everything reproduced, %d failures\n", failures);
        return 0;
    }
    std::printf("check_stmt_shadow: %d failure(s)\n", failures);
    return 1;
}
