#pragma once
#include <vector>
#include "ast.hpp"
#include "diagnostic.hpp"
#include "token.hpp"

namespace lux_script {

// Recursive descent parser over the lexer's token stream.
//
// On an error it does not abort: it records it and synchronizes to the next
// declaration start, so it can report several faults in a single pass.
class Parser {
public:
    Parser(std::vector<Token> tokens, DiagnosticBag& diags)
        : toks_(std::move(tokens)), diags_(diags) {}

    // Parses into `out`.  Several calls with different files accumulate onto
    // the same Program: that is what allows splitting the app across .lux files.
    void parse_into(Program& out);

    // Parses ONE expression and nothing else.  The templates use it: what goes
    // inside {{ }} or {% if %} is a Lux Script expression, not a separate
    // language.  Returns null if there is no expression or trailing input.
    ExprPtr parse_single_expression();

private:
    std::vector<Token> toks_;
    DiagnosticBag&     diags_;
    size_t             i_ = 0;
    // Every switch needs a name for the temp local holding its
    // once-evaluated subject; counting up keeps two switches in the same
    // function (or nested ones) from colliding on the same synthetic name.
    size_t             switch_count_ = 0;

    // ── Navigation ───────────────────────────────────────────────────────────
    const Token& peek(size_t ahead = 0) const;
    const Token& prev() const;
    bool  check(Tok k) const { return peek().is(k); }
    bool  match(Tok k);
    const Token& advance();
    bool  expect(Tok k, const char* context);
    void  error_here(std::string msg);
    void  synchronize();
    void  skip_newlines();

    // ── Declarations ─────────────────────────────────────────────────────────
    void parse_declaration(Program& out);
    void parse_route(Program& out, const Token& method_tok,
                     const std::string& prefix, const std::vector<Guard>& guards);
    void parse_group(Program& out, const std::string& prefix,
                     const std::vector<Guard>& guards);
    void parse_app(Program& out);
    // Resolves a config value: string_value, number, boolean, or env("VAR").
    // from_env (optional): true if the value came from env("VAR") -- what
    // ends up there is decided by the deployment environment, not this
    // file, so a validation on the value's CONTENT (e.g. a secret's
    // minimum length) must not apply when it comes from there, whatever
    // that content happens to be at compile time.
    bool config_value(std::string& text, long long& number, bool& flag, int& kind,
                      bool* from_env = nullptr);
    void parse_class(Program& out);
    void parse_enum(Program& out);
    void parse_error(Program& out);
    void parse_fn(Program& out);

    // ── Statements ───────────────────────────────────────────────────────────
    Block   parse_block();
    StmtPtr parse_statement();
    StmtPtr parse_if();
    StmtPtr parse_if_from_elif();
    StmtPtr parse_while();
    StmtPtr parse_for();
    StmtPtr parse_return();
    static bool assignable(const Expr& e);
    ExprPtr clone_target(const Expr& e);
    StmtPtr parse_require();
    StmtPtr parse_try();
    // `switch` is not a real AST node: it desugars straight to an if/elif
    // chain (see the .cpp comment) and pushes its result(s) directly into
    // the enclosing block, which is why this takes the Block by reference
    // instead of returning a single StmtPtr like every other parse_*
    // statement function does.
    void parse_switch_into(Block& out);

    // ── Types and parameters ─────────────────────────────────────────────────
    bool    looks_like_type() const;
    TypeRef parse_type();
    Param   parse_param();

    // ── Expressions, from lowest to highest precedence ───────────────────────
    ExprPtr parse_expr();
    ExprPtr parse_ternary();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_not();
    ExprPtr parse_equality();
    ExprPtr parse_comparison();
    ExprPtr parse_sum();
    ExprPtr parse_product();
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr parse_primary();

    ExprPtr make(ExprKind k, SourceLoc loc);
};

} // namespace lux_script
