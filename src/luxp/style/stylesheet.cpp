#include "style/stylesheet.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace luxium::style {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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

void parse_rules(const std::string& text, const std::string& media,
                 std::vector<Rule>& out, const Loader& loader, int depth) {
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
            if (name == "media")
                parse_rules(inner, media.empty() ? prelude : media + " and " + prelude, out, loader, depth);
            else if (name == "supports" || name == "layer")
                parse_rules(inner, media, out, loader, depth);
            i = end;   // @font-face, @keyframes, @page...: skipped
            continue;
        }

        const size_t brace = find_top(text, i, "{");
        if (brace == std::string::npos) break;
        const size_t end = block_end(text, brace);
        Rule rule;
        rule.media = media;
        rule.selectors = parse_selector_list(text.substr(i, brace - i));
        const size_t body_end = (end > brace + 1 && text[end - 1] == '}') ? end - 1 : end;
        rule.decls = parse_decls(text.substr(brace + 1, body_end - brace - 1));
        if (!rule.selectors.empty() && !rule.decls.empty()) {
            rule.order = static_cast<int>(out.size());
            out.push_back(std::move(rule));
        }
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

bool media_matches(const std::string& media, float vw, float vh) {
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
