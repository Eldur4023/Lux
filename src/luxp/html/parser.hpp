#pragma once
// HTML tree builder -- the M0 subset described in docs/ARCHITECTURE.md §5.
//
// Simplified WHATWG construction: implied html/head/body, void elements,
// raw-text children for <script>/<style>, a small table of implied end tags,
// and recover-with-diagnostic error handling. Full foster parenting / adoption
// agency / table scopes are deliberately out (see the doc).

#include "dom/node.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace luxium::html {

struct ParseResult {
    std::unique_ptr<dom::Node> root;  // #document, never null
    std::vector<std::string>   errors;  // "line N: message" -- recovered, page still usable
};

ParseResult parse(std::string_view source);

} // namespace luxium::html
