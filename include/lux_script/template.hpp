#pragma once
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "bytecode.hpp"
#include "diagnostic.hpp"
#include "emitter.hpp"
#include "natives.hpp"
#include "value.hpp"

namespace lux_script {

// Template engine of Lux Script.
//
// The shape is Jinja2's —`{{ }}`, `{% if %}`, `{% for %}`— because that is the
// one people write and the one LLMs generate.  What goes INSIDE are Lux
// Script expressions, not a separate language: `{{ a.title }}` is the same
// expression you would write in the .lux, parsed by the same parser and run by
// the same VM.
//
// That gives what no external engine can: a typo in a template is a COMPILE
// error, with file, line and cursor, not a failure in production.
//
//
// The template is compiled once at startup, like the routes.  Rendering is
// walking a list of very simple instructions: paste a chunk of text, or
// evaluate an expression and paste its escaped result.
struct Template {
    enum class Op : uint8_t {
        Text,          // pastes texts[a]
        Write,       // evaluates exprs[a] and pastes the result, escaped
        WriteRaw,  // same, unescaped (marked with |safe)
        JumpIfFalse,   // evaluates exprs[a]; if false, skip a b
        Jump,          // skip a b
        LoopStart,     // exprs[a] gives the list; if empty it jumps to b
        LoopNext,      // next pass: if any is left it jumps to b, otherwise it carries on
    };

    struct Instr {
        Op        op;
        uint32_t  a    = 0;
        uint32_t  b    = 0;
        uint32_t  slot = 0;   // item slot, in loops
        uint32_t  slot_loop = 0;
        SourceLoc loc;
    };

    std::vector<std::string> texts;
    std::vector<Chunk>       exprs;
    std::vector<Instr>       code;

    // Slot names, in order.  The first ones are the data the route passes to
    // render(); the next ones, the loop variables.
    //
    // They carry the type along because the expressions inside are compiled
    // against them: that is what makes `{{ who.uppercase() }}` on a string a
    // compile error and not a broken page.
    std::vector<TypedName> names;
};

// Reads a whole file in binary mode, or nullopt if it can't be opened --
// the one piece shared by every "load a template off disk" call site
// (a top-level render(), {% include %}, {% extends %}).
inline std::optional<std::string> read_whole_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Compiles the source of a template.
//
// `data` are the names —with their type, if known— the route will pass to
// render(), and they take the first slots.  `dir` is the templates folder, to
// resolve {% include %}.  Returns false if there were errors; they go in `diags`.
bool compile_template(const std::string& source, const std::string& file,
                        const std::string& dir,
                        const std::vector<TypedName>& data,
                        DiagnosticBag& diags, Template& out);

// Renders.  `values` arrives in the same order as the `data` it was compiled
// with.  A runtime error —dividing by zero inside a {{ }}— comes out through
// `error` and leaves `out` half-written.
bool render_template(const Template& p, std::vector<Value> values,
                      NativeCtx& ctx, const FunctionTable* fns,
                      std::string& out, std::string& error);

// Escapes for HTML.  Public because the |safe marker also uses it when
// deciding what NOT to escape.
void escape_html(const std::string& in, std::string& out);

} // namespace lux_script
