#pragma once
// Precompiled page programs (.luxbc, inside a .luxp).
//
// A page from a .luxp never compiles source: it runs the bytecode --pack
// produced.  That bytecode is UNTRUSTED (anyone can hand-write a .luxp), and
// the VM trusts its input -- it does not bounds-check constant indexes,
// local slots, native ids or the stack.  So there is exactly one way in,
// load_bytecode(), and it verifies before it returns anything:
//
//   - every operand in range: constants, locals, functions, jump targets,
//     try ranges; names used by GetMember/SetMember/CallMethod are strings
//   - natives only from the browser whitelist, with a legal argc; module
//     calls and await opcodes are rejected outright
//   - a call never passes more arguments than the callee has locals
//   - the stack height is the same on every path into an instruction and
//     never drops below what that instruction pops
//
// Types are NOT proven here: the compiler checked them at pack time, and the
// VM re-checks every operation at run time, so a forged program can at most
// raise a script error -- never step outside the VM.

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <script/page_compile.hpp>

namespace luxium::script {

std::string serialize(const Compiled& c);

// serialize() in pieces, so a program that differs between requests only in some string constants is
// serialized once and reassembled with those strings replaced (their table indexes, sorted).
struct Layout {
    std::string              head;      // magic, versions, native-name table
    std::vector<std::string> strings;   // the string-constant table
    std::string              body;      // functions; refers to the strings by index
};
Layout serialize_layout(const Compiled& c);
std::string assemble(const Layout& l, const std::vector<std::pair<size_t, std::string>>& replaced);

// Parses AND verifies.  nullptr (err filled) on any problem.
std::unique_ptr<Compiled> load_bytecode(const std::string& bytes, std::string* err);

// What a verified program can possibly do: every native and every method
// name it can call.  Exact upper bound -- LuxScript has no eval and no
// dynamic native lookup, so nothing outside these sets is reachable.
struct Capabilities {
    std::set<std::string> natives;   // "page_after", "document_get", ...
    std::set<std::string> methods;   // "set_text", "remove", "upper", ...
};
Capabilities capabilities(const Compiled& c);

} // namespace luxium::script
