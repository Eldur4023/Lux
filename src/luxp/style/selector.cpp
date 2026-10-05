#include "style/selector.hpp"

#include <algorithm>
#include <cctype>

namespace luxium::style {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

// Splits at top-level commas (not inside () or []).
std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    char quote = 0;
    std::string cur;
    for (char c : s) {
        if (quote) { cur += c; if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '(' || c == '[') ++depth;
        if (c == ')' || c == ']') --depth;
        if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    out.push_back(cur);
    return out;
}

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;

    explicit Parser(const std::string& text) : s(text) {}

    bool eof() const { return i >= s.size(); }
    char peek() const { return i < s.size() ? s[i] : '\0'; }

    std::string ident() {
        size_t st = i;
        while (i < s.size() && (ident_char(s[i]) || s[i] == '\\')) ++i;
        return s.substr(st, i - st);
    }

    // Parses "an+b" / odd / even / n / -n+3 / 5
    bool nth(const std::string& raw, int& a, int& b) {
        std::string t;
        for (char c : lower(raw)) if (!std::isspace(static_cast<unsigned char>(c))) t += c;
        if (t == "odd") { a = 2; b = 1; return true; }
        if (t == "even") { a = 2; b = 0; return true; }
        const size_t n = t.find('n');
        try {
            if (n == std::string::npos) { a = 0; b = std::stoi(t); return true; }
            const std::string as = t.substr(0, n);
            a = as.empty() || as == "+" ? 1 : (as == "-" ? -1 : std::stoi(as));
            const std::string bs = t.substr(n + 1);
            b = bs.empty() ? 0 : std::stoi(bs);
            return true;
        } catch (...) { return false; }
    }

    // A parenthesised argument: returns the raw text between the parens.
    std::string paren_arg() {
        if (peek() != '(') { ok = false; return {}; }
        int depth = 0;
        size_t st = i + 1;
        for (; i < s.size(); ++i) {
            if (s[i] == '(') ++depth;
            if (s[i] == ')' && --depth == 0) break;
        }
        if (i >= s.size()) { ok = false; return {}; }
        std::string arg = s.substr(st, i - st);
        ++i;
        return arg;
    }

    Compound compound(Pseudo& pseudo, int& a, int& b, int& c) {
        Compound cp;
        if (peek() == '*') { cp.tag = "*"; ++i; }
        else if (ident_char(peek())) { cp.tag = lower(ident()); ++c; }
        if (peek() == '|' && i + 1 < s.size() && s[i + 1] != '=') {   // ns|tag: ignore ns
            ++i;
            if (peek() == '*') { cp.tag = "*"; ++i; }
            else cp.tag = lower(ident());
        }
        while (!eof()) {
            const char ch = peek();
            SimpleSel ss;
            if (ch == '#') {
                ++i; ss.kind = SimpleSel::Kind::Id; ss.name = ident(); ++a;
                if (ss.name.empty()) ok = false;
            } else if (ch == '.') {
                ++i; ss.kind = SimpleSel::Kind::Class; ss.name = ident(); ++b;
                if (ss.name.empty()) ok = false;
            } else if (ch == '[') {
                ++i; ++b;
                ss.kind = SimpleSel::Kind::Attr;
                while (std::isspace(static_cast<unsigned char>(peek()))) ++i;
                ss.name = lower(ident());
                while (std::isspace(static_cast<unsigned char>(peek()))) ++i;
                if (peek() != ']') {
                    const char o = peek();
                    if (o == '=') { ss.op = '='; ++i; }
                    else if (std::string("~|^$*").find(o) != std::string::npos &&
                             i + 1 < s.size() && s[i + 1] == '=') { ss.op = o; i += 2; }
                    else { ok = false; return cp; }
                    while (std::isspace(static_cast<unsigned char>(peek()))) ++i;
                    if (peek() == '"' || peek() == '\'') {
                        const char q = s[i++];
                        while (!eof() && peek() != q) ss.value += s[i++];
                        ++i;
                    } else ss.value = ident();
                    while (std::isspace(static_cast<unsigned char>(peek()))) ++i;
                    if (peek() == 'i' || peek() == 'I') { ss.icase = true; ++i; }
                    while (std::isspace(static_cast<unsigned char>(peek()))) ++i;
                }
                if (peek() != ']') { ok = false; return cp; }
                ++i;
            } else if (ch == ':') {
                ++i;
                bool dbl = false;
                if (peek() == ':') { ++i; dbl = true; }
                const std::string name = lower(ident());
                if (name == "before" || name == "after") {
                    pseudo = name == "before" ? Pseudo::Before : Pseudo::After;
                    ++c;
                    continue;
                }
                if (dbl) { ok = false; return cp; }   // ::marker, ::selection, ...: not supported
                if (name == "first-child") { ss.kind = SimpleSel::Kind::FirstChild; ++b; }
                else if (name == "last-child") { ss.kind = SimpleSel::Kind::LastChild; ++b; }
                else if (name == "only-child") { ss.kind = SimpleSel::Kind::OnlyChild; ++b; }
                else if (name == "first-of-type") { ss.kind = SimpleSel::Kind::FirstOfType; ++b; }
                else if (name == "last-of-type") { ss.kind = SimpleSel::Kind::LastOfType; ++b; }
                else if (name == "only-of-type") { ss.kind = SimpleSel::Kind::OnlyOfType; ++b; }
                else if (name == "root") { ss.kind = SimpleSel::Kind::Root; ++b; }
                else if (name == "empty") { ss.kind = SimpleSel::Kind::Empty; ++b; }
                else if (name == "link" || name == "any-link") { ss.kind = SimpleSel::Kind::Link; ++b; }
                else if (name == "hover") { ss.kind = SimpleSel::Kind::Hover; ++b; }
                else if (name == "active") { ss.kind = SimpleSel::Kind::Active; ++b; }
                else if (name == "focus" || name == "focus-visible" || name == "focus-within") { ss.kind = SimpleSel::Kind::Focus; ++b; }
                else if (name == "checked") { ss.kind = SimpleSel::Kind::Checked; ++b; }
                else if (name == "disabled") { ss.kind = SimpleSel::Kind::Disabled; ++b; }
                else if (name == "enabled") { ss.kind = SimpleSel::Kind::Enabled; ++b; }
                else if (name == "nth-child" || name == "nth-last-child" ||
                         name == "nth-of-type" || name == "nth-last-of-type") {
                    std::string arg = paren_arg();
                    const size_t of = lower(arg).find(" of ");
                    if (of != std::string::npos) arg = arg.substr(0, of);
                    ss.kind = SimpleSel::Kind::Nth;
                    ss.from_end = name.find("last") != std::string::npos;
                    ss.of_type = name.find("of-type") != std::string::npos;
                    if (!nth(arg, ss.a, ss.b)) ok = false;
                    ++b;
                } else if (name == "not" || name == "is" || name == "where" || name == "matches") {
                    const std::string arg = paren_arg();
                    ss.kind = name == "not" ? SimpleSel::Kind::Not : SimpleSel::Kind::Is;
                    ss.args = parse_selector_list(arg);
                    if (ss.args.empty()) { if (name == "not") ok = false; }
                    int best = 0;
                    for (const auto& sel : ss.args) best = std::max(best, sel.specificity);
                    if (name != "where") { a += best >> 16; b += (best >> 8) & 255; c += best & 255; }
                } else { ok = false; return cp; }
            } else break;
            cp.simples.push_back(std::move(ss));
            if (!ok) return cp;
        }
        return cp;
    }
};

Selector parse_one(const std::string& text) {
    Selector sel;
    Parser p(text);
    int a = 0, b = 0, c = 0;
    char pending = 0;
    while (true) {
        bool space = false;
        while (!p.eof() && std::isspace(static_cast<unsigned char>(p.peek()))) { ++p.i; space = true; }
        if (p.eof()) break;
        const char ch = p.peek();
        if (ch == '>' || ch == '+' || ch == '~') {
            if (sel.parts.empty() || pending) { sel.valid = false; return sel; }
            pending = ch;
            ++p.i;
            continue;
        }
        if (!sel.parts.empty() && !pending) {
            if (!space) { sel.valid = false; return sel; }
            pending = ' ';
        }
        if (!sel.parts.empty()) sel.combinator.push_back(pending);
        pending = 0;
        sel.parts.push_back(p.compound(sel.pseudo, a, b, c));
        if (!p.ok) { sel.valid = false; return sel; }
        if (sel.parts.back().tag.empty() && sel.parts.back().simples.empty() &&
            sel.pseudo == Pseudo::None) { sel.valid = false; return sel; }
    }
    if (sel.parts.empty() || pending) sel.valid = false;
    sel.specificity = (std::min(a, 255) << 16) | (std::min(b, 255) << 8) | std::min(c, 255);
    return sel;
}

// ── matching ────────────────────────────────────────────────────────────────

std::vector<const dom::Node*> element_children(const dom::Node& parent) {
    std::vector<const dom::Node*> out;
    for (const auto& ch : parent.children)
        if (ch->is_element()) out.push_back(ch.get());
    return out;
}

bool has_class(const dom::Node& el, const std::string& cls) {
    const std::string v = el.attr_or("class");
    size_t start = 0;
    while (start < v.size()) {
        while (start < v.size() && std::isspace(static_cast<unsigned char>(v[start]))) ++start;
        size_t end = start;
        while (end < v.size() && !std::isspace(static_cast<unsigned char>(v[end]))) ++end;
        if (end > start && v.compare(start, end - start, cls) == 0) return true;
        start = end;
    }
    return false;
}

bool attr_matches(const dom::Node& el, const SimpleSel& ss) {
    if (!el.has_attr(ss.name)) return false;
    if (ss.op == 0) return true;
    std::string v = el.attr_or(ss.name), want = ss.value;
    if (ss.icase) { v = lower(v); want = lower(want); }
    switch (ss.op) {
    case '=': return v == want;
    case '~': {
        if (want.empty() || want.find_first_of(" \t\n") != std::string::npos) return false;
        size_t st = 0;
        while (st < v.size()) {
            while (st < v.size() && std::isspace(static_cast<unsigned char>(v[st]))) ++st;
            size_t e = st;
            while (e < v.size() && !std::isspace(static_cast<unsigned char>(v[e]))) ++e;
            if (e > st && v.compare(st, e - st, want) == 0 && e - st == want.size()) return true;
            st = e;
        }
        return false;
    }
    case '|': return v == want || v.starts_with(want + "-");
    case '^': return !want.empty() && v.starts_with(want);
    case '$': return !want.empty() && v.ends_with(want);
    case '*': return !want.empty() && v.find(want) != std::string::npos;
    }
    return false;
}

bool simple_matches(const SimpleSel& ss, const dom::Node& el);

bool compound_matches(const Compound& cp, const dom::Node& el) {
    if (!cp.tag.empty() && cp.tag != "*" && el.name != cp.tag) return false;
    for (const auto& ss : cp.simples)
        if (!simple_matches(ss, el)) return false;
    return true;
}

bool simple_matches(const SimpleSel& ss, const dom::Node& el) {
    using K = SimpleSel::Kind;
    switch (ss.kind) {
    case K::Id: return el.attr_or("id") == ss.name;
    case K::Class: return has_class(el, ss.name);
    case K::Attr: return attr_matches(el, ss);
    case K::Not:
        for (const auto& s : ss.args) if (matches(s, el)) return false;
        return true;
    case K::Is:
        for (const auto& s : ss.args) if (matches(s, el)) return true;
        return false;
    case K::Root: return el.parent && el.parent->type == dom::NodeType::Document;
    case K::Empty:
        for (const auto& ch : el.children)
            if (ch->is_element() || (ch->type == dom::NodeType::Text && !ch->text.empty())) return false;
        return true;
    case K::Link: return el.name == "a" && el.has_attr("href");
    case K::Hover: return el.state & dom::Node::Hover;
    case K::Active: return el.state & dom::Node::Active;
    case K::Focus: return el.state & dom::Node::Focus;
    case K::Checked: return el.has_attr("checked") || el.has_attr("selected");
    case K::Disabled: return el.has_attr("disabled");
    case K::Enabled: return !el.has_attr("disabled") &&
                            (el.name == "button" || el.name == "input" || el.name == "select" || el.name == "textarea");
    case K::FirstChild: case K::LastChild: case K::OnlyChild:
    case K::FirstOfType: case K::LastOfType: case K::OnlyOfType: {
        if (!el.parent) return ss.kind == K::OnlyChild || ss.kind == K::FirstChild || ss.kind == K::LastChild;
        const bool of_type = ss.kind == K::FirstOfType || ss.kind == K::LastOfType || ss.kind == K::OnlyOfType;
        int before = 0, after = 0;
        bool seen = false;
        for (const auto* sib : element_children(*el.parent)) {
            if (sib == &el) { seen = true; continue; }
            if (of_type && sib->name != el.name) continue;
            (seen ? after : before)++;
        }
        switch (ss.kind) {
        case K::FirstChild: case K::FirstOfType: return before == 0;
        case K::LastChild: case K::LastOfType: return after == 0;
        default: return before == 0 && after == 0;
        }
    }
    case K::Nth: {
        if (!el.parent) return false;
        int before = 0, after = 0;
        bool seen = false;
        for (const auto* sib : element_children(*el.parent)) {
            if (sib == &el) { seen = true; continue; }
            if (ss.of_type && sib->name != el.name) continue;
            (seen ? after : before)++;
        }
        const int pos = (ss.from_end ? after : before) + 1;   // 1-based
        // exists n >= 0 with a*n + b == pos
        if (ss.a == 0) return pos == ss.b;
        const int diff = pos - ss.b;
        return diff % ss.a == 0 && diff / ss.a >= 0;
    }
    }
    return false;
}

const dom::Node* prev_element(const dom::Node& el) {
    if (!el.parent) return nullptr;
    const dom::Node* prev = nullptr;
    for (const auto& ch : el.parent->children) {
        if (ch.get() == &el) return prev;
        if (ch->is_element()) prev = ch.get();
    }
    return nullptr;
}

bool match_from(const Selector& sel, int idx, const dom::Node& el) {
    if (!compound_matches(sel.parts[idx], el)) return false;
    if (idx == 0) return true;
    const char comb = sel.combinator[idx - 1];
    switch (comb) {
    case '>':
        return el.parent && el.parent->is_element() && match_from(sel, idx - 1, *el.parent);
    case ' ':
        for (const dom::Node* p = el.parent; p && p->is_element(); p = p->parent)
            if (match_from(sel, idx - 1, *p)) return true;
        return false;
    case '+': {
        const dom::Node* p = prev_element(el);
        return p && match_from(sel, idx - 1, *p);
    }
    case '~': {
        for (const dom::Node* p = prev_element(el); p; p = prev_element(*p))
            if (match_from(sel, idx - 1, *p)) return true;
        return false;
    }
    }
    return false;
}

} // namespace

std::vector<Selector> parse_selector_list(const std::string& text) {
    std::vector<Selector> out;
    for (const auto& part : split_commas(text)) {
        const std::string t = trim(part);
        if (t.empty()) continue;
        Selector sel = parse_one(t);
        if (sel.valid) out.push_back(std::move(sel));
    }
    return out;
}

bool matches(const Selector& sel, const dom::Node& el) {
    if (!el.is_element() || sel.parts.empty()) return false;
    return match_from(sel, static_cast<int>(sel.parts.size()) - 1, el);
}

} // namespace luxium::style
