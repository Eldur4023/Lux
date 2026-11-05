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

#include <cctype>
#include <string>
#include <vector>

namespace luxium::style {

// Shared by selector.cpp and stylesheet.cpp (C locale, ASCII whitespace/case).
inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

enum class Pseudo { None, Before, After, Placeholder, FirstLetter, FirstLine };

struct Selector;   // complex selector

struct SimpleSel {
    enum class Kind {
        Id, Class, Attr, Not, Is, Nth, Root, Empty, Link, Hover, Active, Focus,
        Checked, Disabled, Enabled, FirstChild, LastChild, OnlyChild,
        FirstOfType, LastOfType, OnlyOfType, Has, FocusWithin, FocusVisible, PlaceholderShown, Open,
    } kind = Kind::Class;
    std::string name;            // id / class / attribute name
    std::string value;           // attribute value
    char op = 0;                 // 0 exists, '=', '~', '|', '^', '$', '*'
    bool icase = false;
    bool has_child = false;      // :has(> x): direct children only
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
