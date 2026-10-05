#pragma once
// Stylesheet parsing: rules (selector list + declarations), !important,
// @media (evaluated lazily against the viewport), @import (through a loader),
// @supports/@layer (transparent), everything else skipped.  Never fails:
// broken rules are dropped, the way a browser drops them.

#include <style/selector.hpp>

#include <functional>
#include <string>
#include <vector>

namespace luxium::style {

struct Decl {
    std::string name;      // lowercase (custom properties keep their case)
    std::string value;
    bool important = false;
};

struct Rule {
    std::vector<Selector> selectors;
    std::vector<Decl> decls;
    std::string media;     // "" = always
    int order = 0;         // source order across the whole sheet set
};

// Resolves an @import URL to the stylesheet text ("" if unavailable).  URLs
// arrive relative to the sheet given to parse_sheet(); a nested import is
// rewritten relative to that one (url "b/c.css" imported from "a/x.css"
// reaches the loader as "a/b/c.css").
using Loader = std::function<std::string(const std::string& url)>;

// Appends the rules of `text` to `out` (order continues from out.size()).
void parse_sheet(const std::string& text, std::vector<Rule>& out,
                 const Loader& loader = nullptr, int depth = 0);

// style="" attribute / rule body.
std::vector<Decl> parse_decls(const std::string& text);

// Media query list ("screen and (min-width: 800px), print") against a viewport.
bool media_matches(const std::string& media, float vw, float vh);

} // namespace luxium::style
