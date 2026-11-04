#include "page/doc_template.hpp"
#include "page/doc_template_run.hpp"
#include <util/wire.hpp>

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

// Every marker in `s` is an S (a value inside a script body).
bool only_script_markers(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        if (s[i] != kStart) { ++i; continue; }
        char kind;
        uint32_t id;
        if (!read_marker(s, i, kind, id) || kind != 'S') return false;
    }
    return true;
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
            if (!raw_close.empty() && raw_close.rfind("<pending:", 0) != 0 && !(kind == 'S' && raw_close == "</script"))
                return "un valor dentro de <script>/<style>";
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
    struct Attr { uint32_t name = 0; std::vector<Piece> parts; bool dynamic = false; std::string enc; };
    std::string        head;           // Elem: type, name, attr count, pre-encoded
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
        // A <script> body may hold values (S markers only: they sit inside a string literal and the program
        // takes them as constants; the document never carries a script's text).  A <style> body, or any
        // other marker there, makes the template ineligible.
        if (raw)
            for (const auto& c : n.children)
                if (has_marker(c->text) && !(n.is_element("script") && only_script_markers(c->text)))
                    return fail("un valor dentro de <script>/<style>");

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
        it.head += char(dom::NodeType::Element);
        put_uv(it.head, it.name);
        put_uv(it.head, it.attrs.size());
        for (auto& a : it.attrs) {
            put_uv(a.enc, a.name);
            if (!a.dynamic) put_str(a.enc, a.parts.empty() ? std::string() : a.parts[0].lit);
        }
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

} // namespace

// ── the program ─────────────────────────────────────────────────────────────

namespace {

using Op = DocTemplate::Op;

struct Emitter {
    std::vector<Op>&                  ops;
    std::string&                      lits;
    std::vector<DocTemplate::Part>&   parts;

    size_t put(Op::Code code, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
        ops.push_back({code, a, b, c});
        return ops.size() - 1;
    }
    void bytes(Op::Code code, const std::string& s, uint32_t count = 0) {
        if (s.empty() && code != Op::Static) return;
        const uint32_t off = uint32_t(lits.size());
        lits += s;
        put(code, off, uint32_t(s.size()), count);
    }
    // The pieces as a run of Parts; returns {first, count}.
    std::pair<uint32_t, uint32_t> run(const std::vector<Piece>& ps) {
        const uint32_t first = uint32_t(parts.size());
        for (const auto& p : ps) {
            DocTemplate::Part part;
            if (p.hole >= 0) {
                part.hole = p.hole;
            } else {
                if (p.lit.empty()) continue;
                part.off = uint32_t(lits.size());
                part.len = uint32_t(p.lit.size());
                lits += p.lit;
            }
            parts.push_back(part);
        }
        return {first, uint32_t(parts.size()) - first};
    }
    void seq(const std::vector<Item>& items) {
        for (const Item& it : items) {
            switch (it.kind) {
                case Kind::Static: bytes(Op::Static, it.bytes, it.count); break;
                case Kind::Text: {
                    auto [first, n] = run(it.parts);
                    if (n) put(Op::Text, first, n);
                    break;
                }
                case Kind::Elem: {
                    std::string pre = it.head;
                    bool opened = false;
                    auto flush_pre = [&] {
                        bytes(opened ? Op::AttrStatic : Op::ElemOpen, pre);
                        opened = true;
                        pre.clear();
                    };
                    for (const auto& a : it.attrs) {
                        pre += a.enc;
                        if (!a.dynamic) continue;
                        flush_pre();
                        auto [first, n] = run(a.parts);
                        put(Op::AttrDyn, first, n);
                    }
                    flush_pre();
                    if (it.children.size() == 1 && it.children[0].kind == Kind::Text) {
                        auto [first, n] = run(it.children[0].parts);
                        put(Op::Leaf, first, n);
                        break;
                    }
                    put(Op::ElemBody);
                    seq(it.children);
                    put(Op::ElemClose);
                    break;
                }
                case Kind::Loop: {
                    const size_t begin = put(Op::LoopBegin, it.id);
                    const uint32_t body = uint32_t(ops.size());
                    seq(it.children);
                    put(Op::LoopNext, it.id, body);
                    ops[begin].b = uint32_t(ops.size());
                    break;
                }
                case Kind::If: {
                    const size_t iff = put(Op::If, it.id);
                    seq(it.children);
                    if (!it.orelse.empty()) {
                        const size_t j = put(Op::Jump);
                        ops[iff].b = uint32_t(ops.size());
                        seq(it.orelse);
                        ops[j].a = uint32_t(ops.size());
                    } else {
                        ops[iff].b = uint32_t(ops.size());
                    }
                    break;
                }
            }
        }
    }
};

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
    std::vector<Item> root;
    if (!b.parse_seq(toks, pos, root, 0)) return no(b.why);
    if (pos != toks.size()) return no("una estructura se abre y se cierra en elementos distintos");
    b.fold(root, true);
    Emitter em{t->ops_, t->lits_, t->parts_};
    em.seq(root);
    em.put(Op::End);

    wire::Writer w;
    encode_prologue(w, t->names_);
    t->prologue_ = std::move(w.out);
    return t;
}

bool DocTemplate::render(Host& host, std::string& out) const { return run(host, out); }

// ── the template as a block ─────────────────────────────────────────────────

std::string DocTemplate::serialize() const {
    wire::Writer w;
    w.uv(1);   // layout of this block
    w.str(prologue_);
    w.str(lits_);
    w.uv(parts_.size());
    for (const Part& p : parts_) { w.uv(uint64_t(p.hole + 1)); w.uv(p.off); w.uv(p.len); }
    w.uv(ops_.size());
    for (const Op& o : ops_) { w.u8(o.code); w.uv(o.a); w.uv(o.b); w.uv(o.c); }
    return std::move(w.out);
}

std::unique_ptr<DocTemplate> DocTemplate::from_bytes(const std::string& bytes, std::string* err) {
    auto fail = [&](const char* why) -> std::unique_ptr<DocTemplate> { if (err) *err = why; return nullptr; };
    wire::Reader r{bytes};
    if (r.uv() != 1) return fail("plantilla de documento de otra version");
    std::unique_ptr<DocTemplate> t(new DocTemplate());
    t->prologue_ = r.str(1u << 24);
    t->lits_ = r.str(1u << 26);
    const uint32_t np = r.count(1u << 22, 3);
    t->parts_.reserve(np);
    for (uint32_t i = 0; i < np && r.ok; ++i) {
        Part p;
        p.hole = int64_t(r.uv()) - 1;
        p.off = r.uv32();
        p.len = r.uv32();
        if (p.hole < 0 && (uint64_t(p.off) + p.len > t->lits_.size())) return fail("plantilla con un literal fuera de rango");
        t->parts_.push_back(p);
    }
    const uint32_t no = r.count(1u << 22, 4);
    t->ops_.reserve(no);
    for (uint32_t i = 0; i < no && r.ok; ++i) {
        Op o;
        const uint8_t code = r.u8();
        if (code > Op::Jump) return fail("plantilla con una operacion desconocida");
        o.code = Op::Code(code);
        o.a = r.uv32(); o.b = r.uv32(); o.c = r.uv32();
        t->ops_.push_back(o);
    }
    if (!r.done()) return fail("plantilla de documento truncada");
    const size_t n = t->ops_.size();
    if (n == 0 || t->ops_[n - 1].code != Op::End) return fail("plantilla sin final");
    // Shape.  Everything that could loop or leave the program is checked here, so running it
    // terminates (jumps go forward, a loop's one backward jump is its LoopNext) and stays inside.
    std::vector<size_t> loops;   // pc of each open LoopBegin
    for (size_t pc = 0; pc < n; ++pc) {
        const Op& o = t->ops_[pc];
        switch (o.code) {
            case Op::End: if (pc != n - 1) return fail("plantilla con un final en medio"); break;
            case Op::Static: case Op::ElemOpen: case Op::AttrStatic:
                if (uint64_t(o.a) + o.b > t->lits_.size()) return fail("plantilla con bytes fuera de rango");
                break;
            case Op::Text: case Op::AttrDyn: case Op::Leaf:
                if (uint64_t(o.a) + o.b > t->parts_.size()) return fail("plantilla con partes fuera de rango");
                break;
            case Op::LoopBegin:
                if (o.b <= pc + 1 || o.b >= n) return fail("plantilla con un bucle mal cerrado");
                loops.push_back(pc);
                break;
            case Op::LoopNext:
                if (loops.empty() || o.b != loops.back() + 1 || t->ops_[loops.back()].b != pc + 1)
                    return fail("plantilla con un bucle mal cerrado");
                loops.pop_back();
                break;
            case Op::If: if (o.b <= pc || o.b >= n) return fail("plantilla con un salto fuera de rango"); break;
            case Op::Jump: if (o.a <= pc || o.a >= n) return fail("plantilla con un salto fuera de rango"); break;
            default: break;
        }
    }
    if (!loops.empty()) return fail("plantilla con un bucle sin cerrar");
    return t;
}

namespace {
// The host of expand(): the values come from the trace instead of from a template engine.
struct TraceReader final {
    const std::string& in;
    size_t at = 0;
    bool   ok = true;
    size_t items_left = 1u << 20;   // loop iterations the whole page may have
    uint64_t uv() {
        uint64_t v = 0;
        for (int shift = 0; shift < 63; shift += 7) {
            if (at >= in.size()) { ok = false; return 0; }
            const uint8_t b = uint8_t(in[at++]);
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    bool hole(uint32_t, std::string& out) {
        const uint64_t n = uv();
        if (!ok || n > in.size() - at) { ok = false; return false; }
        out.append(in, at, size_t(n));
        at += size_t(n);
        return true;
    }
    bool test(uint32_t, bool& v) {
        if (at >= in.size()) { ok = false; return false; }
        v = in[at++] != 0;
        return true;
    }
    bool loop_begin(uint32_t, size_t& items) {
        const uint64_t n = uv();
        if (!ok || n > items_left) { ok = false; return false; }
        items_left -= size_t(n);
        items = size_t(n);
        return true;
    }
    void loop_item(uint32_t, size_t, size_t) {}
    void loop_end(uint32_t) {}
};
} // namespace

bool DocTemplate::expand(const std::string& trace, std::string& document, std::string* err) const {
    TraceReader rd{trace};
    document.clear();
    constexpr size_t kMaxDocument = 64u << 20;
    if (!run(rd, document, kMaxDocument, 1u << 26) || !rd.ok || rd.at != trace.size()) {
        if (err) *err = "traza de documento invalida";
        document.clear();
        return false;
    }
    return true;
}

} // namespace luxium::page
