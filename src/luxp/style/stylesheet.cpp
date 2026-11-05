#include "style/stylesheet.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace luxium::style {

namespace {

std::string strip_comments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    char quote = 0;
    for (size_t i = 0; i < s.size();) {
        if (quote) {
            out += s[i];
            if (s[i] == '\\' && i + 1 < s.size()) out += s[++i];
            else if (s[i] == quote) quote = 0;
            ++i;
            continue;
        }
        if (s[i] == '"' || s[i] == '\'') { quote = s[i]; out += s[i++]; continue; }
        if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
            const size_t e = s.find("*/", i + 2);
            if (e == std::string::npos) break;
            i = e + 2;
            out += ' ';
            continue;
        }
        out += s[i++];
    }
    return out;
}

// Index of the first top-level `stop` char at or after `i` (quotes/parens
// aware), or npos.
size_t find_top(const std::string& s, size_t i, const char* stops) {
    int paren = 0;
    char quote = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(') ++paren;
        else if (c == ')') paren = std::max(0, paren - 1);
        else if (paren == 0 && std::string(stops).find(c) != std::string::npos) return i;
    }
    return std::string::npos;
}

// `{` at s[open]: index just past the matching `}`.
size_t block_end(const std::string& s, size_t open) {
    int depth = 0;
    char quote = 0;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return i + 1;
    }
    return s.size();
}

std::string import_url(const std::string& prelude, std::string* media) {
    std::string p = trim(prelude);
    std::string url, rest;
    if (lower(p).starts_with("url(")) {
        const size_t e = p.find(')');
        url = trim(p.substr(4, e == std::string::npos ? std::string::npos : e - 4));
        rest = e == std::string::npos ? "" : p.substr(e + 1);
    } else if (!p.empty() && (p[0] == '"' || p[0] == '\'')) {
        const size_t e = p.find(p[0], 1);
        url = p.substr(1, e == std::string::npos ? std::string::npos : e - 1);
        rest = e == std::string::npos ? "" : p.substr(e + 1);
    }
    if (!url.empty() && (url.front() == '"' || url.front() == '\'')) url = url.substr(1);
    if (!url.empty() && (url.back() == '"' || url.back() == '\'')) url.pop_back();
    if (media) {
        rest = trim(rest);
        // @import ... supports(...) / layer(...) : ignore those, keep the media list
        *media = rest.find("supports(") != std::string::npos ||
                         rest.find("layer") != std::string::npos ? "" : rest;
    }
    return url;
}

thread_local std::string g_layer;   // the @layer block being parsed ("" = none)

void emit_style_rule(const std::string& selector_text, const std::string& body, const std::string& media, const std::string& container,
                     std::vector<Rule>& out, const Loader& loader, int depth, int nest);

// The body of a style rule, split into its declarations and the blocks nested in it (CSS nesting):
// "a: b; .c { ... } d: e; &:hover { ... }" -> decls "a: b; d: e;" and [(".c", "..."), ("&:hover", "...")].
void split_nested(const std::string& body, std::string& decls, std::vector<std::pair<std::string, std::string>>& nested) {
    std::string cur;
    int paren = 0;
    char quote = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (quote) { cur += c; if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '(') ++paren;
        else if (c == ')') --paren;
        if (paren == 0 && c == ';') { decls += cur + ";"; cur.clear(); continue; }
        if (paren == 0 && c == '{' && !trim(cur).starts_with("--")) {
            const size_t end = block_end(body, i);
            const size_t inner_end = (end > i + 1 && body[end - 1] == '}') ? end - 1 : end;
            nested.emplace_back(trim(cur), body.substr(i + 1, inner_end - i - 1));
            cur.clear();
            i = end - 1;
            continue;
        }
        cur += c;
    }
    decls += cur;
}

// The parent's selector list inside a nested selector: & is the parent (":is(parent)"); a selector without & is a
// descendant of it ("> a" too: the combinator stays).
std::string nest_selector(const std::string& nested, const std::string& parent, const char* wrap = "is") {
    std::string out;
    int paren = 0;
    size_t from = 0;
    auto one = [&](std::string sel) {
        sel = trim(sel);
        if (sel.empty()) return;
        std::string r;
        bool has_amp = false;
        const std::string P = std::string(":") + wrap + "(" + parent + ")";
        for (size_t k = 0; k < sel.size(); ++k) {
            if (sel[k] == '&') { has_amp = true; r += P; }
            else if (sel.compare(k, 6, ":scope") == 0 && std::string(wrap) == "where") { has_amp = true; r += P; k += 5; }
            else r += sel[k];
        }
        if (!has_amp) r = P + " " + sel;
        if (!out.empty()) out += ", ";
        out += r;
    };
    for (size_t i = 0; i < nested.size(); ++i) {
        if (nested[i] == '(') ++paren;
        else if (nested[i] == ')') --paren;
        else if (nested[i] == ',' && paren == 0) { one(nested.substr(from, i - from)); from = i + 1; }
    }
    one(nested.substr(from));
    return out;
}

void emit_style_rule(const std::string& selector_text, const std::string& body, const std::string& media, const std::string& container,
                     std::vector<Rule>& out, const Loader& loader, int depth, int nest) {
    std::string decl_text;
    std::vector<std::pair<std::string, std::string>> nested;
    split_nested(body, decl_text, nested);
    Rule rule;
    rule.media = media;
    rule.container = container;
    rule.layer = g_layer;
    rule.selectors = parse_selector_list(selector_text);
    rule.decls = parse_decls(decl_text);
    if (!rule.selectors.empty() && !rule.decls.empty()) {
        rule.order = static_cast<int>(out.size());
        out.push_back(std::move(rule));
    }
    if (nest > 8) return;
    for (const auto& [prelude, block] : nested) {
        if (prelude.empty()) continue;
        if (prelude[0] == '@') {   // @media/@supports/@container inside a rule: the same selector, under the condition
            size_t j = 1;
            while (j < prelude.size() && (std::isalnum(static_cast<unsigned char>(prelude[j])) || prelude[j] == '-')) ++j;
            const std::string name = lower(prelude.substr(1, j - 1)), cond = trim(prelude.substr(j));
            std::string m = media, c = container;
            if (name == "media") {
                const size_t sep = m.find('\x02');
                const std::string q = sep == std::string::npos ? m : m.substr(0, sep);
                m = (q.empty() ? cond : q + " and " + cond) + (sep == std::string::npos ? std::string() : m.substr(sep));
            } else if (name == "supports") m += "\x02" + cond;
            else if (name == "container") c = c.empty() ? cond : c + "\n" + cond;
            else if (name != "layer") continue;
            emit_style_rule(selector_text, block, m, c, out, loader, depth, nest + 1);
        } else {
            emit_style_rule(nest_selector(prelude, selector_text), block, media, container, out, loader, depth, nest + 1);
        }
    }
}

void parse_rules(const std::string& text, const std::string& media,
                 std::vector<Rule>& out, const Loader& loader, int depth, const std::string& container = "") {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ';')) ++i;
        if (i >= text.size()) break;
        if (text.compare(i, 4, "<!--") == 0) { i += 4; continue; }
        if (text.compare(i, 3, "-->") == 0) { i += 3; continue; }

        if (text[i] == '@') {
            size_t j = i + 1;
            while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '-')) ++j;
            const std::string name = lower(text.substr(i + 1, j - i - 1));
            const size_t stop = find_top(text, j, "{;");
            if (stop == std::string::npos) break;
            const std::string prelude = trim(text.substr(j, stop - j));
            if (text[stop] == ';') {
                if (name == "layer") {   // @layer a, b;  declares the order
                    size_t from = 0;
                    for (size_t q = 0; q <= prelude.size(); ++q) {
                        if (q < prelude.size() && prelude[q] != ',') continue;
                        const std::string nm = trim(prelude.substr(from, q - from));
                        from = q + 1;
                        if (nm.empty()) continue;
                        Rule decl; decl.media = "@layerdecl"; decl.layer = g_layer.empty() ? nm : g_layer + "." + nm;
                        decl.order = static_cast<int>(out.size());
                        out.push_back(std::move(decl));
                    }
                }
                if (name == "import" && loader && depth < 8) {
                    std::string m;
                    const std::string url = import_url(prelude, &m);
                    if (!url.empty()) {
                        const std::string css = loader(url);
                        // The imported sheet's own @imports are relative to IT,
                        // not to whoever imported it.
                        const std::string dir = url.substr(0, url.rfind('/') + 1);
                        const Loader nested = [&loader, dir](const std::string& u) {
                            const bool abs = !u.empty() && (u[0] == '/' || u.find(':') != std::string::npos);
                            return loader(abs ? u : dir + u);
                        };
                        if (!css.empty())
                            parse_rules(strip_comments(css),
                                        m.empty() ? media : (media.empty() ? m : media + " and " + m),
                                        out, nested, depth + 1);
                    }
                }
                i = stop + 1;
                continue;
            }
            const size_t end = block_end(text, stop);
            const size_t inner_end = (end > stop + 1 && text[end - 1] == '}') ? end - 1 : end;
            const std::string inner = text.substr(stop + 1, inner_end - stop - 1);
            if (name == "media") {
                // the query part of `media` ends at the first \x02; the @supports conditions after it stay last
                const size_t sep = media.find('\x02');
                const std::string q = sep == std::string::npos ? media : media.substr(0, sep);
                const std::string tail = sep == std::string::npos ? std::string() : media.substr(sep);
                parse_rules(inner, (q.empty() ? prelude : q + " and " + prelude) + tail, out, loader, depth, container);
            } else if (name == "supports")
                parse_rules(inner, media + "\x02" + prelude, out, loader, depth, container);
            else if (name == "layer") {
                // @layer name { } / @layer { } (anonymous: a layer of its own); nested names join with '.'
                static thread_local int anon = 0;
                const std::string saved = g_layer;
                const std::string nm = prelude.empty() ? "\x01anon" + std::to_string(++anon) : trim(prelude);
                g_layer = saved.empty() ? nm : saved + "." + nm;
                Rule decl; decl.media = "@layerdecl"; decl.layer = g_layer; decl.order = static_cast<int>(out.size());
                out.push_back(std::move(decl));   // fixes the layer's place in the order at its first appearance
                parse_rules(inner, media, out, loader, depth, container);
                g_layer = saved;
            }
            else if (name == "container")
                parse_rules(inner, media, out, loader, depth, container.empty() ? prelude : container + "\n" + prelude);
            else if (name == "property") {
                Rule r;
                r.media = "@property|" + prelude;
                r.decls = parse_decls(inner);
                r.order = static_cast<int>(out.size());
                if (!r.decls.empty()) out.push_back(std::move(r));
            } else if (name == "scope") {
                // @scope (.root) [to (.limit)] { rules }: the rules apply inside .root (the limit is not modelled)
                std::string root = prelude;
                if (const size_t to = lower(root).find(" to "); to != std::string::npos) root = root.substr(0, to);
                root = trim(root);
                if (root.size() >= 2 && root.front() == '(' && root.back() == ')') root = trim(root.substr(1, root.size() - 2));
                size_t k = 0;
                while (!root.empty() && k < inner.size()) {
                    const size_t sb = find_top(inner, k, "{");
                    if (sb == std::string::npos) break;
                    const size_t se = block_end(inner, sb);
                    const size_t sbe = (se > sb + 1 && inner[se - 1] == '}') ? se - 1 : se;
                    emit_style_rule(nest_selector(trim(inner.substr(k, sb - k)), root, "where"), inner.substr(sb + 1, sbe - sb - 1), media, container, out, loader, depth, 1);
                    k = se;
                }
            }
            else if (name == "keyframes" || name == "-webkit-keyframes") {
                // one selector-less Rule per keyframe offset; `media` carries "@keyframes|name|offset" (see Rule::keyframe)
                size_t k = 0;
                while (k < inner.size()) {
                    const size_t kb = find_top(inner, k, "{");
                    if (kb == std::string::npos) break;
                    const size_t ke = block_end(inner, kb);
                    const size_t kbody = (ke > kb + 1 && inner[ke - 1] == '}') ? ke - 1 : ke;
                    const auto decls = parse_decls(inner.substr(kb + 1, kbody - kb - 1));
                    std::string list = inner.substr(k, kb - k);
                    size_t p0 = 0;
                    while (p0 <= list.size()) {
                        const size_t c = list.find(',', p0);
                        const std::string sel = lower(trim(list.substr(p0, c == std::string::npos ? c : c - p0)));
                        float off = -1;
                        if (sel == "from") off = 0;
                        else if (sel == "to") off = 1;
                        else if (sel.size() > 1 && sel.back() == '%') { try { off = std::stof(sel) / 100.f; } catch (...) {} }
                        if (off >= 0 && off <= 1 && !decls.empty()) {
                            Rule r;
                            r.media = "@keyframes|" + prelude + "|" + std::to_string(off);
                            r.decls = decls;
                            r.order = static_cast<int>(out.size());
                            out.push_back(std::move(r));
                        }
                        if (c == std::string::npos) break;
                        p0 = c + 1;
                    }
                    k = ke;
                }
            }
            else if (name == "font-face") {
                Rule r;
                r.media = "@font-face";
                r.decls = parse_decls(inner);
                r.order = static_cast<int>(out.size());
                if (!r.decls.empty()) out.push_back(std::move(r));
            }
            i = end;   // @page...: skipped
            continue;
        }

        const size_t brace = find_top(text, i, "{");
        if (brace == std::string::npos) break;
        const size_t end = block_end(text, brace);
        const size_t body_end = (end > brace + 1 && text[end - 1] == '}') ? end - 1 : end;
        emit_style_rule(text.substr(i, brace - i), text.substr(brace + 1, body_end - brace - 1), media, container, out, loader, depth, 0);
        i = end;
    }
}

} // namespace

std::vector<Decl> parse_decls(const std::string& raw) {
    std::vector<Decl> out;
    const std::string text = raw;
    size_t i = 0;
    while (i < text.size()) {
        size_t semi = find_top(text, i, ";");
        const size_t end = semi == std::string::npos ? text.size() : semi;
        const size_t colon = find_top(text, i, ":");
        if (colon != std::string::npos && colon < end) {
            Decl d;
            d.name = trim(text.substr(i, colon - i));
            if (!d.name.starts_with("--")) d.name = lower(d.name);
            d.value = trim(text.substr(colon + 1, end - colon - 1));
            const std::string lv = lower(d.value);
            const size_t bang = lv.rfind('!');
            if (bang != std::string::npos && trim(lv.substr(bang + 1)) == "important") {
                d.important = true;
                d.value = trim(d.value.substr(0, bang));
            }
            if (!d.name.empty() && !d.value.empty()) out.push_back(std::move(d));
        }
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
    return out;
}

void parse_sheet(const std::string& text, std::vector<Rule>& out,
                 const Loader& loader, int depth) {
    parse_rules(strip_comments(text), "", out, loader, depth);
}

// ── media queries ───────────────────────────────────────────────────────────

namespace {

bool parse_media_length(const std::string& v, float& px) {
    const std::string s = lower(trim(v));
    try {
        size_t used = 0;
        const float n = std::stof(s, &used);
        const std::string u = s.substr(used);
        if (u.empty() || u == "px") px = n;
        else if (u == "em" || u == "rem") px = n * 16.f;
        else if (u == "pt") px = n * 96.f / 72.f;
        else return false;
        return true;
    } catch (...) { return false; }
}

// One (feature: value) / (feature) / range test.
bool eval_feature(const std::string& body_raw, float vw, float vh) {
    const std::string body = lower(trim(body_raw));
    // range syntax: width >= 600px, 400px <= width <= 800px
    for (const char* op : {"<=", ">=", "<", ">", "="}) {
        const size_t p = body.find(op);
        if (p == std::string::npos) continue;
        // a op feature op b  |  feature op value  |  value op feature
        auto side = [&](const std::string& t, float& out, bool& is_w, bool& is_feat) {
            const std::string x = trim(t);
            if (x == "width") { out = vw; is_w = true; is_feat = true; return true; }
            if (x == "height") { out = vh; is_w = false; is_feat = true; return true; }
            is_feat = false;
            return parse_media_length(x, out);
        };
        const std::string l = body.substr(0, p);
        std::string r = body.substr(p + std::string(op).size());
        std::string op2;
        for (const char* o2 : {"<=", ">=", "<", ">"}) {
            const size_t q = r.find(o2);
            if (q != std::string::npos) { op2 = o2; r = r.substr(0, q) + "\x01" + r.substr(q + op2.size()); break; }
        }
        auto cmp = [](float a, const std::string& o, float b) {
            if (o == "<") return a < b;
            if (o == "<=") return a <= b;
            if (o == ">") return a > b;
            if (o == ">=") return a >= b;
            return a == b;
        };
        float a, b; bool aw, af, bw, bf;
        if (op2.empty()) {
            if (!side(l, a, aw, af) || !side(r, b, bw, bf)) return false;
            return cmp(a, op, b);
        }
        const size_t sep = r.find('\x01');
        float c; bool cw, cf;
        const std::string mid = r.substr(0, sep);
        const std::string last = r.substr(sep + 1);
        if (!side(l, a, aw, af) || !side(mid, b, bw, bf) || !side(last, c, cw, cf)) return false;
        return cmp(a, op, b) && cmp(b, op2, c);
    }
    const size_t colon = body.find(':');
    const std::string feat = trim(colon == std::string::npos ? body : body.substr(0, colon));
    const std::string val = colon == std::string::npos ? "" : trim(body.substr(colon + 1));
    float px = 0;
    if (feat == "width") return parse_media_length(val, px) && vw == px;
    if (feat == "min-width") return parse_media_length(val, px) && vw >= px;
    if (feat == "max-width") return parse_media_length(val, px) && vw <= px;
    if (feat == "height") return parse_media_length(val, px) && vh == px;
    if (feat == "min-height") return parse_media_length(val, px) && vh >= px;
    if (feat == "max-height") return parse_media_length(val, px) && vh <= px;
    if (feat == "orientation") return val == (vw >= vh ? "landscape" : "portrait");
    if (feat == "prefers-color-scheme") return val == "light";
    if (feat == "prefers-reduced-motion") return val == "no-preference";
    if (feat == "hover" || feat == "any-hover") return val.empty() || val == "hover";
    if (feat == "pointer" || feat == "any-pointer") return val.empty() || val == "fine";
    if (feat == "color") return true;
    if (feat == "min-resolution" || feat == "resolution") {
        try { return val.find("dppx") != std::string::npos ? std::stof(val) <= 1.f
                                                           : std::stof(val) <= 96.f; }
        catch (...) { return false; }
    }
    return false;   // feature desconocida: no cumple
}

bool eval_one_query(std::string q, float vw, float vh) {
    q = lower(trim(q));
    if (q.empty()) return true;
    bool neg = false;
    if (q.starts_with("not ")) { neg = true; q = trim(q.substr(4)); }
    else if (q.starts_with("only ")) q = trim(q.substr(5));
    bool ok = true;
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && std::isspace(static_cast<unsigned char>(q[i]))) ++i;
        if (i >= q.size()) break;
        if (q[i] == '(') {
            int depth = 0;
            size_t j = i;
            for (; j < q.size(); ++j) {
                if (q[j] == '(') ++depth;
                if (q[j] == ')' && --depth == 0) break;
            }
            ok = ok && eval_feature(q.substr(i + 1, j - i - 1), vw, vh);
            i = j + 1;
        } else {
            size_t j = i;
            while (j < q.size() && !std::isspace(static_cast<unsigned char>(q[j])) && q[j] != '(') ++j;
            const std::string w = q.substr(i, j - i);
            if (w == "print" || w == "speech" || w == "tv") ok = false;
            // screen / all / and: no condition
            i = j;
        }
    }
    return neg ? !ok : ok;
}

} // namespace

void split_container_prelude(const std::string& prelude, std::string& name, std::string& cond) {
    const std::string p = trim(prelude);
    size_t j = 0;
    while (j < p.size() && !std::isspace(static_cast<unsigned char>(p[j])) && p[j] != '(') ++j;
    const std::string first = lower(p.substr(0, j));
    if (j == 0 || first == "not") { name.clear(); cond = p; return; }
    name = p.substr(0, j);
    cond = trim(p.substr(j));
}

bool container_query_matches(const std::string& cond, float w, float h) {
    std::string c = lower(cond);
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"inline-size", "width"}, {"block-size", "height"}})
        for (size_t p = c.find(from); p != std::string::npos; p = c.find(from, p + to.size())) c.replace(p, from.size(), to);
    return eval_one_query(c, w, h);
}

namespace {

SupportsHook g_supports_hook = nullptr;

bool eval_supports(const std::string& cond_raw, int depth = 0);

// `a` joined by a top-level word ("and"/"or"): the operands, or just {a} if the word is not there
std::vector<std::string> split_word(const std::string& c, const std::string& word) {
    std::vector<std::string> out;
    int d = 0; size_t from = 0;
    for (size_t i = 0; i + word.size() + 2 <= c.size(); ++i) {
        if (c[i] == '(') ++d;
        else if (c[i] == ')') --d;
        else if (d == 0 && std::isspace(static_cast<unsigned char>(c[i])) &&
                 lower(c.substr(i + 1, word.size())) == word && std::isspace(static_cast<unsigned char>(c[i + 1 + word.size()]))) {
            out.push_back(c.substr(from, i - from));
            from = i + 2 + word.size();
            i = from - 1;
        }
    }
    out.push_back(c.substr(from));
    return out;
}

bool eval_supports(const std::string& cond_raw, int depth) {
    if (depth > 8) return false;
    const std::string c = trim(cond_raw);
    if (c.empty()) return false;
    if (lower(c.substr(0, 3)) == "not" && c.size() > 3 && (std::isspace(static_cast<unsigned char>(c[3])) || c[3] == '('))
        return !eval_supports(c.substr(3), depth + 1);
    const auto ors = split_word(c, "or");
    if (ors.size() > 1) { for (const auto& o : ors) if (eval_supports(o, depth + 1)) return true; return false; }
    const auto ands = split_word(c, "and");
    if (ands.size() > 1) { for (const auto& a : ands) if (!eval_supports(a, depth + 1)) return false; return true; }
    if (lower(c.substr(0, 9)) == "selector(") {
        const size_t e = c.rfind(')');
        return e != std::string::npos && e > 9 && !parse_selector_list(c.substr(9, e - 9)).empty();
    }
    if (c.front() != '(' || c.back() != ')') return false;   // font-tech(), font-format()...: not claimed
    const std::string inner = trim(c.substr(1, c.size() - 2));
    const size_t colon = find_top(inner, 0, ":");
    if (colon == std::string::npos) return eval_supports(inner, depth + 1);   // ((a) and (b))
    const std::string name = trim(inner.substr(0, colon)), value = trim(inner.substr(colon + 1));
    if (name.empty() || value.empty()) return false;
    if (name.starts_with("--")) return true;
    return g_supports_hook ? g_supports_hook(lower(name), value) : true;
}

} // namespace

void set_supports_hook(SupportsHook hook) { g_supports_hook = hook; }

bool media_matches(const std::string& media_all, float vw, float vh) {
    // "<queries>\x02<@supports cond>\x02<cond>...": the queries as before, every condition must hold
    std::string media = media_all;
    if (const size_t sep = media_all.find('\x02'); sep != std::string::npos) {
        media = media_all.substr(0, sep);
        size_t from = sep + 1;
        while (from <= media_all.size()) {
            const size_t next = media_all.find('\x02', from);
            if (!eval_supports(media_all.substr(from, next == std::string::npos ? next : next - from))) return false;
            if (next == std::string::npos) break;
            from = next + 1;
        }
    }
    if (trim(media).empty()) return true;
    // "A and B" built by nesting is a conjunction; a comma list is a disjunction.
    // Nested @media joins with " and " only between whole queries, so split on
    // top-level commas first, then evaluate each query.
    std::string cur;
    int depth = 0;
    bool any = false;
    auto flush = [&] { if (!any && eval_one_query(cur, vw, vh)) any = true; cur.clear(); };
    for (char c : media) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (c == ',' && depth == 0) { flush(); continue; }
        cur += c;
    }
    flush();
    return any;
}

} // namespace luxium::style
