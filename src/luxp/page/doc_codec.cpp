#include "page/doc_codec.hpp"

namespace luxium::page {

namespace {

using wire::Reader;
using wire::Writer;
using dom::Node;
using dom::NodeType;

constexpr uint32_t kDocFormat   = 2;
constexpr uint32_t kSheetFormat = 2;
constexpr uint32_t kMaxNodes    = 1u << 20;
constexpr uint32_t kMaxAttrs    = 1u << 10;
constexpr uint32_t kMaxName     = 256;
constexpr uint32_t kMaxTable    = 1u << 16;
constexpr uint32_t kMaxRules    = 1u << 18;
constexpr uint32_t kMaxList     = 1u << 12;   // selectors per rule, decls, parts, simples
constexpr int      kMaxSelNest  = 8;          // :not(:is(:not(...)))
constexpr int      kMaxNth      = 1 << 20;

bool name_ok(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (c <= ' ' || c == 0x7F) return false;   // what the HTML tokenizer can never produce
    return true;
}

// The table as it travels: count, then each name.
void put_table(Writer& w, const NameTable& t) {
    w.uv(t.names.size());
    for (const auto& n : t.names) w.str(n);
}

// Reads a table; `check` vets every name.
bool get_table(Reader& r, std::vector<std::string>& out, bool (*check)(const std::string&)) {
    const uint32_t n = r.count(kMaxTable, 1);
    for (uint32_t i = 0; i < n && r.ok; ++i) {
        std::string s = r.str(kMaxName);
        if (r.ok && check && !check(s)) return false;
        out.push_back(std::move(s));
    }
    return r.ok;
}

bool any_name(const std::string&) { return true; }

// ── document ────────────────────────────────────────────────────────────────

struct DocReader {
    Reader r;
    std::string* err;
    std::vector<std::string> names;
    uint32_t nodes = 0;

    bool bad(const std::string& why) { if (err && err->empty()) *err = "documento: " + why; return false; }

    const std::string* name() {
        const uint32_t i = r.uv32();
        if (!r.ok || i >= names.size()) { bad("nombre fuera de la tabla"); return nullptr; }
        return &names[i];
    }

    // Reads one node's children into `parent` (depth = parent's depth).
    bool children(Node& parent, int depth) {
        const uint32_t n = r.count(kMaxNodes, 2);
        if (!r.ok) return bad("truncado");
        if (n && depth + 1 > dom::kMaxDepth) return bad("arbol mas profundo que " + std::to_string(dom::kMaxDepth));
        for (uint32_t i = 0; i < n; ++i) {
            if (++nodes > kMaxNodes) return bad("demasiados nodos");
            const uint8_t t = r.u8();
            if (t > uint8_t(NodeType::Comment) || t == uint8_t(NodeType::Document))
                return bad("tipo de nodo invalido");
            auto node = std::make_unique<Node>(NodeType(t));
            switch (node->type) {
                case NodeType::Element: {
                    const std::string* nm = name();
                    if (!nm) return false;
                    node->name = *nm;
                    const uint32_t na = r.count(kMaxAttrs, 2);
                    for (uint32_t a = 0; a < na && r.ok; ++a) {
                        const std::string* k = name();
                        if (!k) return false;
                        node->attrs.emplace_back(*k, r.str());
                    }
                    break;
                }
                case NodeType::Text:    node->name = "#text";    node->text = r.str(); break;
                case NodeType::Comment: node->name = "#comment"; node->text = r.str(); break;
                case NodeType::Doctype: node->name = "#doctype"; node->text = r.str(); break;
                default: break;
            }
            if (!r.ok) return bad("truncado");
            Node& added = parent.append(std::move(node));
            if (added.type == NodeType::Element && !children(added, depth + 1)) return false;
        }
        return true;
    }
};

// ── stylesheet ──────────────────────────────────────────────────────────────

// Writes the body and lets the table fill as names are first used.
struct SheetWriter {
    Writer w;
    NameTable names;

    void selector(const style::Selector& s) {
        w.u8(uint8_t(s.pseudo));
        w.sv(s.specificity);
        w.uv(s.parts.size());
        for (size_t i = 0; i < s.parts.size(); ++i) {
            if (i > 0) w.u8(uint8_t(s.combinator[i - 1]));
            w.uv(names.id(s.parts[i].tag));
            w.uv(s.parts[i].simples.size());
            for (const auto& ss : s.parts[i].simples) {
                w.u8(uint8_t(ss.kind));
                w.uv(names.id(ss.name));
                w.str(ss.value);
                w.u8(uint8_t(ss.op));
                w.u8(uint8_t((ss.icase ? 1 : 0) | (ss.from_end ? 2 : 0) | (ss.of_type ? 4 : 0)));
                w.sv(ss.a);
                w.sv(ss.b);
                w.uv(ss.args.size());
                for (const auto& a : ss.args) selector(a);
            }
        }
    }
};

struct SheetReader {
    Reader r;
    std::string* err;
    std::vector<std::string> names;

    bool bad(const std::string& why) { if (err && err->empty()) *err = "hoja de estilos: " + why; return false; }

    const std::string* name() {
        const uint32_t i = r.uv32();
        if (!r.ok || i >= names.size()) { bad("nombre fuera de la tabla"); return nullptr; }
        return &names[i];
    }

    bool selector(style::Selector& s, int nest) {
        if (nest > kMaxSelNest) return bad("selectores anidados en exceso");
        const uint8_t pseudo = r.u8();
        if (pseudo > uint8_t(style::Pseudo::After)) return bad("pseudo-elemento invalido");
        s.pseudo = style::Pseudo(pseudo);
        const int64_t spec = r.sv();
        if (spec < 0 || spec > INT32_MAX) return bad("especificidad invalida");
        s.specificity = int(spec);
        const uint32_t np = r.count(kMaxList, 3);
        if (!r.ok || np == 0) return bad("selector sin partes");
        for (uint32_t i = 0; i < np; ++i) {
            if (i > 0) {
                const char c = char(r.u8());
                if (c != ' ' && c != '>' && c != '+' && c != '~') return bad("combinador invalido");
                s.combinator.push_back(c);
            }
            style::Compound comp;
            const std::string* tag = name();
            if (!tag) return false;
            comp.tag = *tag;
            const uint32_t ns = r.count(kMaxList, 8);
            for (uint32_t k = 0; k < ns && r.ok; ++k) {
                style::SimpleSel ss;
                const uint8_t kind = r.u8();
                if (kind > uint8_t(style::SimpleSel::Kind::OnlyOfType)) return bad("tipo de selector invalido");
                ss.kind = style::SimpleSel::Kind(kind);
                const std::string* nm = name();
                if (!nm) return false;
                ss.name = *nm;
                ss.value = r.str();
                ss.op = char(r.u8());
                if (ss.op != 0 && ss.op != '=' && ss.op != '~' && ss.op != '|' && ss.op != '^' &&
                    ss.op != '$' && ss.op != '*')
                    return bad("operador de atributo invalido");
                const uint8_t flags = r.u8();
                if (flags > 7) return bad("banderas de selector invalidas");
                ss.icase = flags & 1;
                ss.from_end = flags & 2;
                ss.of_type = flags & 4;
                const int64_t a = r.sv(), b = r.sv();
                if (a < -kMaxNth || a > kMaxNth || b < -kMaxNth || b > kMaxNth) return bad(":nth fuera de rango");
                ss.a = int(a);
                ss.b = int(b);
                const uint32_t na = r.count(kMaxList, 8);
                for (uint32_t x = 0; x < na && r.ok; ++x) {
                    style::Selector arg;
                    if (!selector(arg, nest + 1)) return false;
                    ss.args.push_back(std::move(arg));
                }
                comp.simples.push_back(std::move(ss));
            }
            s.parts.push_back(std::move(comp));
        }
        return r.ok || bad("truncado");
    }
};

} // namespace

// ── document ────────────────────────────────────────────────────────────────

void encode_prologue(Writer& w, const NameTable& names) {
    w.out += "LXDOC";
    w.uv(kDocFormat);
    put_table(w, names);
}

void encode_node(Writer& w, const Node& n, NameTable& names) {
    w.u8(uint8_t(n.type));
    if (n.type == NodeType::Element) {
        w.uv(names.id(n.name));
        w.uv(n.attrs.size());
        for (const auto& [k, v] : n.attrs) { w.uv(names.id(k)); w.str(v); }
    } else if (n.type != NodeType::Document) {
        w.str(n.text);
    }
    const bool raw = n.is_element("script") || n.is_element("style");
    if (n.type == NodeType::Document || n.type == NodeType::Element) {
        w.uv(raw ? 0 : n.children.size());
        if (!raw)
            for (const auto& c : n.children) encode_node(w, *c, names);
    }
}

std::string encode_document(const Node& doc) {
    // The table fills while the body is written, but travels first.
    NameTable names;
    Writer body;
    encode_node(body, doc, names);
    Writer w;
    encode_prologue(w, names);
    return w.out + body.out;
}

std::unique_ptr<Node> decode_document(const std::string& bytes, std::string* err) {
    if (err) err->clear();
    if (bytes.compare(0, 5, "LXDOC") != 0) { if (err) *err = "documento: cabecera invalida"; return nullptr; }
    DocReader d{Reader{bytes, 5}, err, {}, 0};
    if (d.r.uv32() != kDocFormat) { d.bad("version distinta"); return nullptr; }
    if (!get_table(d.r, d.names, name_ok)) { d.bad("tabla de nombres invalida"); return nullptr; }
    if (d.r.u8() != uint8_t(NodeType::Document)) { d.bad("la raiz no es #document"); return nullptr; }
    auto doc = dom::make_document();
    if (!d.children(*doc, 0)) return nullptr;
    if (!d.r.done()) { d.bad("bytes sobrantes o truncado"); return nullptr; }
    return doc;
}

// ── stylesheet ──────────────────────────────────────────────────────────────

std::string encode_sheet(const std::vector<style::Rule>& rules) {
    SheetWriter s;
    s.w.uv(rules.size());
    for (const auto& r : rules) {
        s.w.str(r.media);
        s.w.uv(r.selectors.size());
        for (const auto& sel : r.selectors) s.selector(sel);
        s.w.uv(r.decls.size());
        for (const auto& d : r.decls) { s.w.uv(s.names.id(d.name)); s.w.str(d.value); s.w.u8(d.important); }
    }
    Writer w;
    w.out = "LXCSS";
    w.uv(kSheetFormat);
    put_table(w, s.names);
    return w.out + s.w.out;
}

bool decode_sheet(const std::string& bytes, std::vector<style::Rule>& out, std::string* err) {
    if (err) err->clear();
    out.clear();
    if (bytes.compare(0, 5, "LXCSS") != 0) { if (err) *err = "hoja de estilos: cabecera invalida"; return false; }
    SheetReader s{Reader{bytes, 5}, err, {}};
    if (s.r.uv32() != kSheetFormat) return s.bad("version distinta");
    if (!get_table(s.r, s.names, any_name)) return s.bad("tabla de nombres invalida");
    const uint32_t nr = s.r.count(kMaxRules, 3);
    std::vector<style::Rule> rules;
    for (uint32_t i = 0; i < nr && s.r.ok; ++i) {
        style::Rule rule;
        rule.media = s.r.str();
        const uint32_t nsel = s.r.count(kMaxList, 4);
        for (uint32_t k = 0; k < nsel && s.r.ok; ++k) {
            style::Selector sel;
            if (!s.selector(sel, 0)) return false;
            rule.selectors.push_back(std::move(sel));
        }
        const uint32_t nd = s.r.count(kMaxList, 3);
        for (uint32_t k = 0; k < nd && s.r.ok; ++k) {
            style::Decl d;
            const std::string* nm = s.name();
            if (!nm) return false;
            d.name = *nm;
            d.value = s.r.str();
            d.important = s.r.u8() != 0;
            if (s.r.ok && d.name.empty()) return s.bad("declaracion sin nombre");
            rule.decls.push_back(std::move(d));
        }
        rule.order = int(i);
        rules.push_back(std::move(rule));
    }
    if (!s.r.done()) return s.bad("bytes sobrantes o truncado");
    out = std::move(rules);
    return true;
}

} // namespace luxium::page
