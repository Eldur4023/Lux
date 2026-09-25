#pragma once
// Shared by the shadow tests: lex + parse `src` into `out`. `file` must
// outlive `out` (tokens and diagnostics point into it).
#include <lux_script/diagnostic.hpp>
#include <lux_script/lexer.hpp>
#include <lux_script/parser.hpp>

#include <string>

inline bool parse_program(const std::string& src, lux_script::SourceFile& file,
                          lux_script::DiagnosticBag& diags, lux_script::Program& out) {
    file.path = "<test>";
    file.text = src;
    lux_script::Lexer  lexer(file, diags);
    lux_script::Parser parser(lexer.tokenize(), diags);
    parser.parse_into(out);
    return diags.empty();
}
