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
    std::string container; // @container conditions ("[name] (query)"), joined by '\n' when nested; "" = none
    std::string layer;     // @layer name (nested: "a.b"); "" = unlayered.  Rank = first appearance, see Resolver
    int order = 0;         // source order across the whole sheet set
    // @font-face: no selectors, media = "@font-face"; decls are the descriptors (font-family, src, font-weight...)
    bool font_face() const { return media == "@font-face"; }
    // @keyframes: no selectors, media = "@keyframes|<name>|<offset 0..1>" (survives the compiled-page codec)
    bool keyframe(std::string* name = nullptr, float* offset = nullptr) const {
        if (!media.starts_with("@keyframes|")) return false;
        const size_t bar = media.rfind('|');
        if (name) *name = media.substr(11, bar - 11);
        if (offset) *offset = std::stof(media.substr(bar + 1));
        return true;
    }
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

// @supports: a rule inside carries its condition after a \x02 in `media` (media_matches evaluates both).  A
// declaration test asks this hook (set by the engine that knows its properties); without one every
// declaration counts as supported.
using SupportsHook = bool (*)(const std::string& property, const std::string& value);
void set_supports_hook(SupportsHook hook);

// One @container prelude: "[name] <condition>" -> name ("" = any container) and condition.
void split_container_prelude(const std::string& prelude, std::string& name, std::string& cond);
// The condition against a container's content box (h is NaN for an inline-size container: height tests fail).
bool container_query_matches(const std::string& cond, float w, float h);

} // namespace luxium::style
