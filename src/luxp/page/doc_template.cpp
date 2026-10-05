#include "page/doc_template.hpp"

#include <cctype>
#include <cstring>

namespace luxium::page {

namespace {

constexpr char kStart = '\x01', kEnd = '\x02';

// A string of a text node or an attribute value: literal runs and holes.
struct Piece {
    std::string lit;
    int64_t     hole = -1;   // >= 0: a value, by id; the literal is ignored
};

bool has_marker(const std::string& s) { return s.find(kStart) != std::string::npos; }

// Marker at s[i] (which is kStart): fills kind and id, moves i past kEnd.
bool read_marker(const std::string& s, size_t& i, char& kind, uint32_t& id) {
    size_t j = i + 1;
    if (j >= s.size()) return false;
    kind = s[j++];
    uint64_t v = 0;
    size_t digits = 0;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) && digits < 9) {
        v = v * 10 + uint64_t(s[j++] - '0');
        ++digits;
    }
    if (!digits || j >= s.size() || s[j] != kEnd) return false;
    id = uint32_t(v);
    i = j + 1;
    return true;
}

void put_uv(std::string& out, uint64_t v) {
    while (v >= 0x80) { out += char((v & 0x7F) | 0x80); v >>= 7; }
    out += char(v);
}

void put_str(std::string& out, const std::string& s) { put_uv(out, s.size()); out += s; }

// A count that is not known yet: one byte now, patched when it is -- a node
// with children is written in place, with no buffer of its own for them.  Most
// counts fit the byte; the rare 128+ child list is lengthened (the reader only
// sees LEB128).
size_t reserve_count(std::string& out) { out.push_back('\0'); return out.size() - 1; }

void patch_count(std::string& out, size_t at, uint32_t n) {
    if (n < 0x80) {
        out[at] = char(n);
    } else {
        std::string enc;
        put_uv(enc, n);
        out.replace(at, 1, enc);   // moves what was written after it, once
    }
}

// ── scanning the skeleton's source ──────────────────────────────────────────
// Where a marker is allowed is decided on the TEXT of the skeleton, because
// that is where the template's author put it: inside quotes of a tag, or in
// ordinary text -- nowhere the tokenizer reads differently.

bool entity_glued(const std::string& s, size_t marker_at) {
    size_t j = marker_at;
    while (j > 0 && (std::isalnum(static_cast<unsigned char>(s[j - 1])) || s[j - 1] == '#')) --j;
    return j > 0 && s[j - 1] == '&';
}

std::string scan_source(const std::string& s) {
    enum { Text, Tag, Comment, Bang } state = Text;
    char quote = 0;
    std::string raw_close;   // "</script" while inside a script/style element
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == kStart) {
            char kind;
            uint32_t id;
            const size_t at = i;
            if (!read_marker(s, i, kind, id)) return "marker mal formado";
            if (!raw_close.empty() && raw_close.rfind("<pending:", 0) != 0) return "un valor dentro de <script>/<style>";
            if (state == Comment || state == Bang) return "un valor dentro de un comentario o doctype";
            if (state == Tag) {
                if (!quote) return "un valor fuera de comillas en una etiqueta";
                if (kind != 'H') return "una estructura ({% %}) dentro de una etiqueta";
            }
            if (kind == 'H' && entity_glued(s, at)) return "un valor pegado a una entidad";
            continue;
        }
        if (!raw_close.empty() && raw_close.rfind("<pending:", 0) != 0) {   // inside <script>/<style>: only its closing tag matters
            if (c == '<' && s.size() - i >= raw_close.size()) {
                std::string head = s.substr(i, raw_close.size());
                for (char& h : head) h = char(std::tolower(static_cast<unsigned char>(h)));
                if (head == raw_close) {
                    i += raw_close.size();
                    raw_close.clear();
                    state = Tag;
                    quote = 0;
                    continue;
                }
            }
            ++i;
            continue;
        }
        switch (state) {
            case Text:
                if (c == '<') {
                    if (s.compare(i, 4, "<!--") == 0) { state = Comment; i += 4; continue; }
                    if (i + 1 < s.size() && s[i + 1] == '!') { state = Bang; i += 2; continue; }
                    if (i + 1 < s.size() && (std::isalpha(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '/')) {
                        const bool closing = s[i + 1] == '/';
                        size_t j = i + (closing ? 2 : 1);
                        std::string name;
                        while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '-'))
                            name += char(std::tolower(static_cast<unsigned char>(s[j++])));
                        state = Tag;
                        quote = 0;
                        // A script/style start tag turns the text after it raw: remember it
                        // for when its '>' comes.
                        if (!closing && (name == "script" || name == "style")) raw_close = "<pending:" + name;
                        i = j;
                        continue;
                    }
                }
                break;
            case Tag:
                if (quote) {
                    if (c == quote) quote = 0;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '>') {
                    state = Text;
                    if (raw_close.rfind("<pending:", 0) == 0) raw_close = "</" + raw_close.substr(9);
                }
                break;
            case Comment:
                if (s.compare(i, 3, "-->") == 0) { state = Text; i += 3; continue; }
                break;
            case Bang:
                if (c == '>') state = Text;
                break;
        }
        ++i;
    }
    return {};
}

} // namespace

std::string template_marker(char kind, uint32_t id) {
    return std::string(1, kStart) + kind + std::to_string(id) + kEnd;
}

// ── the tree ────────────────────────────────────────────────────────────────

struct DocTemplate::Item {
    enum class Kind : uint8_t { Static, Text, Elem, Loop, If } kind = Kind::Static;
    std::string        bytes;          // Static: whole encoded nodes
    uint32_t           count = 0;      //   ...and how many
    std::vector<Piece> parts;          // Text
    struct Attr { uint32_t name = 0; std::vector<Piece> parts; bool dynamic = false; };
    uint32_t           name = 0;       // Elem
    std::vector<Attr>  attrs;
    std::vector<Item>  children;       // Elem's children; the body of a Loop; the `then` of an If
    std::vector<Item>  orelse;         // If
    uint32_t           id = 0;         // Loop, If
};

namespace {

using Item = DocTemplate::Item;
using Kind = Item::Kind;

struct Tok {
    enum class K { Node, Text, Open, Else, Close } k = K::Text;
    char        block = 0;             // Open: 'L' or 'I'
    uint32_t    id = 0;
    const dom::Node* node = nullptr;
    std::vector<Piece> parts;          // Text
};

struct Builder {
    NameTable& names;
    std::string why;

    bool fail(std::string m) { if (why.empty()) why = std::move(m); return false; }

    // "a {{ x }} b" -> pieces; control markers are the caller's business.
    // Returns false at a control marker only if !allow_control.
    void append_text(std::vector<Piece>& parts, const std::string& s) {
        if (s.empty()) return;
        if (!parts.empty() && parts.back().hole < 0) parts.back().lit += s;
        else parts.push_back({s, -1});
    }

    bool value_pieces(const std::string& s, std::vector<Piece>& parts) {
        std::string lit;
        for (size_t i = 0; i < s.size();) {
            if (s[i] != kStart) { lit += s[i++]; continue; }
            char kind;
            uint32_t id;
            if (!read_marker(s, i, kind, id)) return fail("marker mal formado");
            if (kind == 'R') return fail("un valor |safe: es HTML y el parser tendria que leerlo");
            if (kind != 'H') return fail("una estructura ({% %}) dentro de un atributo");
            append_text(parts, lit);
            lit.clear();
            parts.push_back({{}, int64_t(id)});
        }
        append_text(parts, lit);
        return true;
    }

    // One parent's children, flattened to tokens.
    bool flatten(const dom::Node& parent, std::vector<Tok>& toks) {
        const bool restricted = parent.type == dom::NodeType::Document || parent.is_element("html") ||
                                parent.is_element("head");
        for (const auto& up : parent.children) {
            const dom::Node& c = *up;
            if (c.type != dom::NodeType::Text) {
                Tok t;
                t.k = Tok::K::Node;
                t.node = &c;
                toks.push_back(std::move(t));
                continue;
            }
            if (restricted && has_marker(c.text))
                return fail("texto con valores directamente bajo #document, <html> o <head>");
            Tok cur;
            cur.k = Tok::K::Text;
            const std::string& s = c.text;
            std::string lit;
            auto flush_text = [&] {
                append_text(cur.parts, lit);
                lit.clear();
                if (!cur.parts.empty()) { toks.push_back(std::move(cur)); cur = Tok(); cur.k = Tok::K::Text; }
            };
            for (size_t i = 0; i < s.size();) {
                if (s[i] != kStart) { lit += s[i++]; continue; }
                char kind;
                uint32_t id;
                if (!read_marker(s, i, kind, id)) return fail("marker mal formado");
                switch (kind) {
                    case 'H':
                        append_text(cur.parts, lit);
                        lit.clear();
                        cur.parts.push_back({{}, int64_t(id)});
                        break;
                    case 'R': return fail("un valor |safe: es HTML y el parser tendria que leerlo");
                    case 'L': case 'I': case 'N': case 'E': {
                        flush_text();
                        Tok t;
                        t.k = kind == 'N' ? Tok::K::Else : (kind == 'E' ? Tok::K::Close : Tok::K::Open);
                        t.block = kind;
                        t.id = id;
                        toks.push_back(std::move(t));
                        break;
                    }
                    default: return fail("marker desconocido");
                }
            }
            flush_text();
        }
        return true;
    }

    static bool is_static(const Item& it) {
        switch (it.kind) {
            case Kind::Static: return true;
            case Kind::Text:
                for (const auto& p : it.parts) if (p.hole >= 0) return false;
                return true;
            case Kind::Elem:
                for (const auto& a : it.attrs) if (a.dynamic) return false;
                for (const auto& c : it.children) if (!is_static(c)) return false;
                return true;
            default: return false;
        }
    }

    bool node_item(const dom::Node& n, Item& out) {
        if (n.type == dom::NodeType::Comment || n.type == dom::NodeType::Doctype) {
            if (has_marker(n.text)) return fail("un valor dentro de un comentario o doctype");
            return static_item(n, out);
        }
        // An element.
        const bool raw = n.is_element("script") || n.is_element("style");
        const bool resource = n.is_element("link") || n.is_element("script");
        if (has_marker(n.name)) return fail("un valor en un nombre de etiqueta");
        for (const auto& [k, v] : n.attrs) {
            if (has_marker(k)) return fail("un valor en un nombre de atributo");
            if (resource && has_marker(v)) return fail("un valor en un atributo de <link>/<script>");
        }
        if (raw)
            for (const auto& c : n.children)
                if (has_marker(c->text)) return fail("un valor dentro de <script>/<style>");

        Item it;
        it.kind = Kind::Elem;
        it.name = names.id(n.name);
        for (const auto& [k, v] : n.attrs) {
            Item::Attr a;
            a.name = names.id(k);
            if (!value_pieces(v, a.parts)) return false;
            for (const auto& p : a.parts) if (p.hole >= 0) a.dynamic = true;
            it.attrs.push_back(std::move(a));
        }
        if (!raw) {
            std::vector<Tok> toks;
            if (!flatten(n, toks)) return false;
            size_t pos = 0;
            if (!parse_seq(toks, pos, it.children, 0)) return false;
            if (pos != toks.size()) return fail("una estructura se abre y se cierra en elementos distintos");
        }
        if (is_static(it)) return static_item(n, out);   // the original subtree, written once
        out = std::move(it);
        return true;
    }

    bool static_item(const dom::Node& n, Item& out) {
        wire::Writer w;
        encode_node(w, n, names);
        out = Item();
        out.kind = Kind::Static;
        out.bytes = std::move(w.out);
        out.count = 1;
        return true;
    }

    // Items of one list until a block boundary (Else/Close) or the end.
    bool parse_seq(const std::vector<Tok>& toks, size_t& pos, std::vector<Item>& out, int depth) {
        if (depth > 32) return fail("bloques anidados en exceso");
        while (pos < toks.size()) {
            const Tok& t = toks[pos];
            switch (t.k) {
                case Tok::K::Text: {
                    Item it;
                    it.kind = Kind::Text;
                    it.parts = t.parts;
                    out.push_back(std::move(it));
                    ++pos;
                    break;
                }
                case Tok::K::Node: {
                    Item it;
                    if (!node_item(*t.node, it)) return false;
                    out.push_back(std::move(it));
                    ++pos;
                    break;
                }
                case Tok::K::Open: {
                    Item it;
                    it.kind = t.block == 'L' ? Kind::Loop : Kind::If;
                    it.id = t.id;
                    ++pos;
                    if (!parse_seq(toks, pos, it.children, depth + 1)) return false;
                    if (pos < toks.size() && toks[pos].k == Tok::K::Else) {
                        if (it.kind != Kind::If || toks[pos].id != it.id) return fail("{% else %} fuera de su {% if %}");
                        ++pos;
                        if (!parse_seq(toks, pos, it.orelse, depth + 1)) return false;
                    }
                    if (pos >= toks.size() || toks[pos].k != Tok::K::Close || toks[pos].id != it.id)
                        return fail("un bloque se abre y se cierra en elementos distintos");
                    ++pos;
                    out.push_back(std::move(it));
                    break;
                }
                case Tok::K::Else: case Tok::K::Close:
                    return true;   // the caller decides whether that is legal here
            }
        }
        return true;
    }

    // Whole text nodes between element boundaries become bytes; runs of bytes
    // become one run.  A text item next to a block is NOT folded: the block's
    // output may be text that has to merge with it.
    void fold(std::vector<Item>& seq, bool edges_are_boundaries) {
        for (auto& it : seq) {
            if (it.kind == Kind::Elem) fold(it.children, true);
            else if (it.kind == Kind::Loop || it.kind == Kind::If) { fold(it.children, false); fold(it.orelse, false); }
        }
        auto boundary = [&](size_t i, bool before) {
            if (before ? i == 0 : i + 1 == seq.size()) return edges_are_boundaries;
            const Kind k = seq[before ? i - 1 : i + 1].kind;
            return k == Kind::Static || k == Kind::Elem;
        };
        for (size_t i = 0; i < seq.size(); ++i) {
            Item& it = seq[i];
            if (it.kind != Kind::Text || !is_static(it) || !boundary(i, true) || !boundary(i, false)) continue;
            std::string text;
            for (const auto& p : it.parts) text += p.lit;
            Item st;
            st.kind = Kind::Static;
            st.bytes += char(dom::NodeType::Text);
            put_str(st.bytes, text);
            st.count = 1;
            it = std::move(st);
        }
        std::vector<Item> merged;
        for (auto& it : seq) {
            if (it.kind == Kind::Static && !merged.empty() && merged.back().kind == Kind::Static) {
                merged.back().bytes += it.bytes;
                merged.back().count += it.count;
            } else {
                merged.push_back(std::move(it));
            }
        }
        seq = std::move(merged);
    }
};

// ── rendering ───────────────────────────────────────────────────────────────

void flush(std::string& buf, uint32_t& count, std::string& pending) {
    if (pending.empty()) return;
    buf += char(dom::NodeType::Text);
    put_str(buf, pending);
    ++count;
    pending.clear();
}

bool render_seq(const std::vector<Item>& seq, DocTemplate::Host& h, std::string& buf, uint32_t& count,
                std::string& pending);

bool assemble(const std::vector<Piece>& parts, DocTemplate::Host& h, std::string& out) {
    for (const auto& p : parts) {
        if (p.hole < 0) out += p.lit;
        else if (!h.hole(uint32_t(p.hole), out)) return false;
    }
    return true;
}

bool render_seq(const std::vector<Item>& seq, DocTemplate::Host& h, std::string& buf, uint32_t& count,
                std::string& pending) {
    for (const Item& it : seq) {
        switch (it.kind) {
            case Kind::Static:
                flush(buf, count, pending);
                buf += it.bytes;
                count += it.count;
                break;
            case Kind::Text:
                if (!assemble(it.parts, h, pending)) return false;
                break;
            case Kind::Elem: {
                flush(buf, count, pending);
                buf += char(dom::NodeType::Element);
                put_uv(buf, it.name);
                put_uv(buf, it.attrs.size());
                std::string value;
                for (const auto& a : it.attrs) {
                    put_uv(buf, a.name);
                    if (!a.dynamic) {
                        put_str(buf, a.parts.empty() ? std::string() : a.parts[0].lit);
                    } else {
                        value.clear();
                        if (!assemble(a.parts, h, value)) return false;
                        put_str(buf, value);
                    }
                }
                const size_t at = reserve_count(buf);
                uint32_t kids = 0;
                std::string text;
                if (!render_seq(it.children, h, buf, kids, text)) return false;
                flush(buf, kids, text);
                patch_count(buf, at, kids);
                ++count;
                break;
            }
            case Kind::Loop: {
                size_t items = 0;
                if (!h.loop_begin(it.id, items)) return false;
                for (size_t i = 0; i < items; ++i) {
                    h.loop_item(it.id, i, items);
                    if (!render_seq(it.children, h, buf, count, pending)) { h.loop_end(it.id); return false; }
                }
                h.loop_end(it.id);
                break;
            }
            case Kind::If: {
                bool v = false;
                if (!h.test(it.id, v)) return false;
                if (!render_seq(v ? it.children : it.orelse, h, buf, count, pending)) return false;
                break;
            }
        }
    }
    return true;
}

} // namespace

DocTemplate::DocTemplate() = default;
DocTemplate::~DocTemplate() = default;

std::unique_ptr<DocTemplate> DocTemplate::build(const dom::Node& skeleton, const std::string& skeleton_html,
                                                std::string* why_not) {
    auto no = [&](const std::string& why) -> std::unique_ptr<DocTemplate> {
        if (why_not) *why_not = why;
        return nullptr;
    };
    if (const std::string why = scan_source(skeleton_html); !why.empty()) return no(why);

    std::unique_ptr<DocTemplate> t(new DocTemplate());
    Builder b{t->names_, {}};
    std::vector<Tok> toks;
    if (!b.flatten(skeleton, toks)) return no(b.why);
    size_t pos = 0;
    if (!b.parse_seq(toks, pos, t->root_, 0)) return no(b.why);
    if (pos != toks.size()) return no("una estructura se abre y se cierra en elementos distintos");
    b.fold(t->root_, true);

    wire::Writer w;
    encode_prologue(w, t->names_);
    t->prologue_ = std::move(w.out);
    return t;
}

bool DocTemplate::render(Host& host, std::string& out) const {
    out = prologue_;
    out += char(dom::NodeType::Document);
    const size_t at = reserve_count(out);
    uint32_t count = 0;
    std::string pending;
    if (!render_seq(root_, host, out, count, pending)) return false;
    flush(out, count, pending);
    patch_count(out, at, count);
    return true;
}

} // namespace luxium::page
