#pragma once
// CSS selectors (Selectors L3 + a few L4 conveniences): parse once, match
// right-to-left against the DOM.  Unknown pseudo-classes make a selector
// invalid (the rule is dropped, as in a real browser).
//
//   type, *, #id, .class, [a], [a=v], ~= |= ^= $= *= (with the i flag)
//   :first-child :last-child :only-child :nth-child() :nth-last-child()
//   :first-of-type :last-of-type :only-of-type :nth-of-type() :nth-last-of-type()
//   :not() :is() :where() :root :empty :link :hover :active :focus
//   :checked :disabled :enabled
//   combinators: descendant, >, +, ~
//   pseudo-elements: ::before ::after (a single colon works too)

#include <dom/node.hpp>

#include <string>
#include <vector>

namespace luxium::style {

enum class Pseudo { None, Before, After };

struct Selector;   // complex selector

struct SimpleSel {
    enum class Kind {
        Id, Class, Attr, Not, Is, Nth, Root, Empty, Link, Hover, Active, Focus,
        Checked, Disabled, Enabled, FirstChild, LastChild, OnlyChild,
        FirstOfType, LastOfType, OnlyOfType,
    } kind = Kind::Class;
    std::string name;            // id / class / attribute name
    std::string value;           // attribute value
    char op = 0;                 // 0 exists, '=', '~', '|', '^', '$', '*'
    bool icase = false;
    // :nth-*(an+b)
    int a = 0, b = 0;
    bool from_end = false, of_type = false;
    std::vector<Selector> args;  // :not / :is / :where
};

struct Compound {
    std::string tag;             // "" or "*" = any
    std::vector<SimpleSel> simples;
};

struct Selector {
    // parts[0] is the leftmost compound; combinator[i] joins parts[i-1] and
    // parts[i] (' ' descendant, '>' child, '+' next sibling, '~' later sibling)
    std::vector<Compound> parts;
    std::vector<char> combinator;
    Pseudo pseudo = Pseudo::None;
    int specificity = 0;         // a*65536 + b*256 + c
    bool valid = true;
};

// A comma-separated selector list.  Invalid members are dropped.
std::vector<Selector> parse_selector_list(const std::string& text);

bool matches(const Selector& sel, const dom::Node& el);

} // namespace luxium::style
