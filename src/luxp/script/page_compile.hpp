#pragma once
// Compiling a page's scripts: every <script> body of a page, in document
// order, becomes ONE LuxScript program.  The bodies are plain statements and
// fns, so they are wrapped in a synthetic `fn void __luxium_main__():` (the
// parser only takes declarations at the top level); fn/class/enum
// declarations are lifted out verbatim, and every diagnostic is remapped to
// document lines.
//
// Always compiled against the BROWSER's API (lux_script::BrowserProfile):
// the same function serves Luxium loading a page and Lux translating one to
// .luxp, so both produce identical bytecode.

#include <lux_script/emitter.hpp>
#include <lux_script/natives.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luxium::script {

using lux_script::Value;

struct Compiled {
    lux_script::FunctionTable functions;        // slot 0 = __luxium_main__
    lux_script::FunctionSigs  fn_sigs;
    lux_script::ClassSigs     class_sigs;
    lux_script::EnumSigs      enum_sigs;
    std::vector<int>          line_map;         // wrapped line -> document line (runtime errors)
    std::vector<std::string>  errors;           // "line N: message", document coordinates
    bool ok() const { return errors.empty(); }
};

// Compiles one script body.  `base_line` is the document line of the first
// source line, so diagnostics remap to where the script sits in the HTML.
std::unique_ptr<Compiled> compile_script(std::string_view source, int base_line);

// The real entry point: every <script> body of a page, in document order,
// compiled as ONE program (shared scope, one __luxium_main__ that calls each
// block's wrapper in order).  `blocks` = (body, document line of the body's
// first line); diagnostics remap to document coordinates.
std::unique_ptr<Compiled> compile_blocks(
    const std::vector<std::pair<std::string, int>>& blocks,
    const std::string& source_name);

} // namespace luxium::script
