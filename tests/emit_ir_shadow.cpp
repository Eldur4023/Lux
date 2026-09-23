// Verification of EXECUTION equivalence (--native, phase 1):
// compares, actually running in the VM, the bytecode emitted by the old
// emitter (emit_function over Expr/Stmt) against the one emitted by the new
// emitter that consumes the IR (check_function builds IrExpr/IrStmt with the
// real diags_, emit_block/emit_stmt/emit_expr consume them without checking
// anything again).
//
// tests/check_expr_shadow.cpp and tests/check_stmt_shadow.cpp already prove
// that the DIAGNOSTICS match. This test is the other half: that the
// resulting bytecode actually BEHAVES the same when really executed, not
// just that it compiles clean -- necessary before considering wiring the new
// emitter into any real entry point.
#include <lux_script/diagnostic.hpp>
#include <lux_script/emitter.hpp>
#include <lux_script/lexer.hpp>
#include <lux_script/natives.hpp>
#include <lux_script/parser.hpp>
#include <lux_script/vm.hpp>

#include <lux/request.hpp>
#include <lux/response.hpp>

#include <cstdio>
#include <memory>
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

static std::string run(const Chunk& chunk, std::vector<Value> args,
                            const FunctionTable* fns, std::string& summary) {
    lux::Request  req;
    lux::Response res;
    NativeCtx       ctx{req, res};
    VM              vm;
    VM::Result      r = vm.start(chunk, std::move(args), ctx, fns);
    switch (r.status) {
        case VM::Status::Done:
            summary = "Done";
            return r.value.to_json_text();
        case VM::Status::Error:
            summary = "Error";
            return r.error;
        case VM::Status::Suspended:
            summary = "Suspended";
            return "(await_id=" + std::to_string(r.await_id) + ")";
    }
    return {};
}

// Compiles ALL the functions of `src` (so recursion/calls between them
// resolve) with both the old and new paths, runs `target_fn` with `args` in
// both, and compares result and status word for word.
static void case_(const char* name, const std::string& src, const std::string& target_fn,
                 const std::vector<Value>& args) {
    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    if (!parse_program(src, file, diag_parse, prog)) {
        ++failures;
        std::printf("  FAIL %s (did not parse: %s)\n", name,
                    diag_parse.items().empty() ? "?" : diag_parse.items().front().message.c_str());
        return;
    }

    FunctionSigs sigs;
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        const FnDecl& f = prog.functions[i];
        FnSig sig;
        sig.index = i;
        sig.required = 0;
        for (const auto& p : f.params) {
            sig.defaults.push_back(p.default_value.get());
            if (!p.default_value) ++sig.required;
        }
        sigs[f.name] = std::move(sig);
    }

    auto idx = sigs.find(target_fn);
    if (idx == sigs.end()) {
        ++failures;
        std::printf("  FAIL %s (function '%s' does not exist)\n", name, target_fn.c_str());
        return;
    }
    size_t target = idx->second.index;

    // Old path: real emit_function, just as project.cpp does today.
    FunctionTable table_old(prog.functions.size());
    DiagnosticBag diags_old;
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter em(diags_old, &sigs, nullptr, &prog.imports);
        em.emit_function(prog.functions[i], *chunk);
        table_old[i] = chunk;
    }
    if (!diags_old.empty()) {
        ++failures;
        std::printf("  FAIL %s (the old path did not compile: %s)\n", name,
                    diags_old.items().front().message.c_str());
        return;
    }

    // New path: check_function builds the IR with the REAL diags_ (not
    // shadow: here a real error has to stop the test, not be compared
    // against itself) and emit_block consumes it without checking anything
    // again.
    FunctionTable table_new(prog.functions.size());
    DiagnosticBag diags_new;
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter em(diags_new, &sigs, nullptr, &prog.imports);
        IrBlock body;
        bool ok = em.check_function(prog.functions[i], *chunk, diags_new, &body);
        if (ok) {
            em.emit_block(body);
            chunk->emit(Op::ReturnNull, prog.functions[i].loc);
        }
        table_new[i] = chunk;
    }
    if (!diags_new.empty()) {
        ++failures;
        std::printf("  FAIL %s (the new path did not compile: %s)\n", name,
                    diags_new.items().front().message.c_str());
        return;
    }

    std::string summary_old, summary_new;
    std::string value_old = run(*table_old[target], args, &table_old, summary_old);
    std::string value_new = run(*table_new[target], args, &table_new, summary_new);

    if (summary_old != summary_new || value_old != value_new) {
        ++failures;
        std::printf("  FAIL %s (different execution)\n", name);
        std::printf("           old: %s %s\n", summary_old.c_str(), value_old.c_str());
        std::printf("           new: %s %s\n", summary_new.c_str(), value_new.c_str());
        return;
    }

    std::printf("  ok    %s (%s: %s)\n", name, summary_old.c_str(), value_old.c_str());
}

int main() {
    case_("arithmetic and recursion (factorial)",
        "fn int factorial(int n):\n"
        "    if n <= 1:\n"
        "        return 1\n"
        "    return n * factorial(n - 1)\n",
        "factorial", {Value::integer(6)});

    case_("for with break/continue over a List",
        "fn int suma_pares(List<int> xs):\n"
        "    int total = 0\n"
        "    for int x in xs:\n"
        "        if x < 0:\n"
        "            break\n"
        "        if x % 2 != 0:\n"
        "            continue\n"
        "        total = total + x\n"
        "    return total\n",
        "suma_pares",
        {Value::list({Value::integer(1), Value::integer(2), Value::integer(3),
                     Value::integer(4), Value::integer(-1), Value::integer(8)})});

    case_("while with compound assignment and ++/--",
        "fn int cuenta(int n):\n"
        "    int i = 0\n"
        "    int total = 0\n"
        "    while i < n:\n"
        "        total = total + i\n"
        "        i++\n"
        "    return total\n",
        "cuenta", {Value::integer(10)});

    case_("indices, lists and builtin methods",
        "fn List<int> duplica(List<int> xs):\n"
        "    List<int> out = []\n"
        "    for int x in xs:\n"
        "        out.add(x * 2)\n"
        "    return out\n",
        "duplica",
        {Value::list({Value::integer(1), Value::integer(2), Value::integer(3)})});

    case_("require (same desugaring as a group guard)",
        "fn int limite(int n):\n"
        "    require n >= 0 else 0 - 1\n"
        "    return n * 2\n",
        "limite", {Value::integer(5)});

    case_("require that triggers the else",
        "fn int limite2(int n):\n"
        "    require n >= 0 else 0 - 1\n"
        "    return n * 2\n",
        "limite2", {Value::integer(-3)});

    case_("string methods",
        "fn string grita(string s):\n"
        "    return s.upper() + \"!\"\n",
        "grita", {Value::str("hola")});

    case_("try/catch with division by zero",
        "fn int seguro(int a, int b):\n"
        "    try:\n"
        "        return a / b\n"
        "    catch e:\n"
        "        return -1\n",
        "seguro", {Value::integer(10), Value::integer(0)});

    case_("try/catch without triggering the error",
        "fn int seguro2(int a, int b):\n"
        "    try:\n"
        "        return a / b\n"
        "    catch e:\n"
        "        return -1\n",
        "seguro2", {Value::integer(10), Value::integer(2)});

    case_("dict and ternary",
        "fn Json etiqueta(int edad):\n"
        "    return { \"rol\": edad >= 18 ? \"adulto\" : \"menor\" }\n",
        "etiqueta", {Value::integer(20)});

    case_("mutual call between functions",
        "fn bool es_par(int n):\n"
        "    if n == 0:\n"
        "        return true\n"
        "    return es_impar(n - 1)\n"
        "\n"
        "fn bool es_impar(int n):\n"
        "    if n == 0:\n"
        "        return false\n"
        "    return es_par(n - 1)\n",
        "es_par", {Value::integer(11)});

    if (failures == 0) {
        std::printf("emit_ir_shadow: everything equivalent, %d failures\n", failures);
        return 0;
    }
    std::printf("emit_ir_shadow: %d failure(s)\n", failures);
    return 1;
}
