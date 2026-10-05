#include "page/luxp.hpp"

#include <html/parser.hpp>
#include <page/doc_codec.hpp>
#include <script/bytecode_io.hpp>
#include <util/wire.hpp>

// Compression is optional: the container says whether its payload is zlib.
// Luxium always reads both; Lux writes zlib only when built with it (its core
// links no zlib -- over HTTP, compression is the transport's business).
#if defined(LUX_GZIP) && !defined(LUXP_ZLIB)
#define LUXP_ZLIB 1   // Lux built with its gzip module: zlib is linked
#endif
#ifdef LUXP_ZLIB
#include <zlib.h>
#endif

#include <cctype>
#include <cstring>

namespace luxium::page {

namespace {

constexpr char     kVersion  = 5;   // 5: sections as blobs, LEB128
constexpr uint64_t kMaxBytes = 1ull << 30;
constexpr uint32_t kMaxItems = 1u << 16;
constexpr uint32_t kMaxSection = 1u << 28;   // plain size of one section

bool is_local(const std::string& ref) {
    if (ref.empty() || ref[0] == '#' || ref[0] == '/') return false;
    return ref.find(':') == std::string::npos;   // http:, data:, mailto:, javascript:
}

// url(...) inside a declaration value or a style="" attribute.
void url_refs(const std::string& text, std::vector<std::string>& out) {
    for (size_t p = text.find("url("); p != std::string::npos; p = text.find("url(", p + 4)) {
        size_t i = p + 4;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i < text.size() && (text[i] == '"' || text[i] == '\'')) ++i;
        size_t j = i;
        while (j < text.size() && !std::strchr("\"') \t\r\n", text[j])) ++j;
        if (is_local(text.substr(i, j - i))) out.push_back(text.substr(i, j - i));
    }
}

void dom_refs(const dom::Node& n, Translation& t) {
    if (n.is_element()) {
        if (n.name == "img" && is_local(n.attr_or("src"))) t.resources.push_back(n.attr_or("src"));
        if (n.name == "a") {
            std::string h = n.attr_or("href");
            h = h.substr(0, h.find('#'));
            if (is_local(h) && (h.ends_with(".html") || h.ends_with(".htm"))) t.links.push_back(h);
        }
        url_refs(n.attr_or("style"), t.resources);
    }
    for (const auto& c : n.children) dom_refs(*c, t);
}

void collect_sheets(dom::Node& n, std::vector<dom::Node*>& out) {
    if (n.is_element("style") ||
        (n.is_element("link") && n.attr_or("rel") == "stylesheet" && !n.attr_or("href").empty()))
        out.push_back(&n);
    for (auto& c : n.children) collect_sheets(*c, out);
}

} // namespace

// ── blobs and container ─────────────────────────────────────────────────────

Blob blob_plain(std::string bytes) {
    Blob b;
    b.raw_size = uint32_t(bytes.size());
    b.data = std::move(bytes);
    return b;
}

Blob blob_deflate(const std::string& bytes) {
#ifdef LUXP_ZLIB
    if (bytes.size() >= 64 && bytes.size() <= kMaxSection) {
        uLongf zlen = compressBound(bytes.size());
        std::string z(zlen, '\0');
        if (compress2(reinterpret_cast<Bytef*>(z.data()), &zlen,
                      reinterpret_cast<const Bytef*>(bytes.data()), bytes.size(), 9) == Z_OK &&
            zlen < bytes.size()) {
            z.resize(zlen);
            Blob b;
            b.data = std::move(z);
            b.raw_size = uint32_t(bytes.size());
            b.zlib = true;
            return b;
        }
    }
#endif
    return blob_plain(bytes);
}

bool blob_inflate(const Blob& b, std::string& out, std::string* err) {
    if (!b.zlib) { out = b.data; return true; }
#ifdef LUXP_ZLIB
    out.assign(b.raw_size, '\0');
    uLongf got = b.raw_size;
    if (uncompress(reinterpret_cast<Bytef*>(out.data()), &got,
                   reinterpret_cast<const Bytef*>(b.data.data()), b.data.size()) != Z_OK || got != b.raw_size) {
        if (err) *err = ".luxp corrupto (zlib)";
        out.clear();
        return false;
    }
    return true;
#else
    if (err) *err = ".luxp comprimido y este binario no tiene zlib";
    return false;
#endif
}

namespace {

void put_blob(wire::Writer& w, const Blob& b) {
    w.u8(b.zlib ? 1 : 0);
    w.uv(b.raw_size);
    w.str(b.data);
}

bool get_blob(wire::Reader& r, Blob& b, std::string* err) {
    const uint8_t flag = r.u8();
    b.raw_size = r.uv32();
    if (flag > 1 || b.raw_size > kMaxSection) {
        if (err) *err = ".luxp con una seccion invalida";
        return false;
    }
    b.zlib = flag == 1;
    b.data = r.str(b.zlib ? kMaxSection : b.raw_size);
    // deflate cannot expand more than ~1032:1: a bigger claim is a bomb, and
    // it is refused BEFORE a buffer of that size is allocated.
    if (r.ok && b.zlib && uint64_t(b.raw_size) > uint64_t(b.data.size()) * 1100 + 64) {
        if (err) *err = ".luxp con una seccion que declara mas tamano del posible";
        return false;
    }
    if (r.ok && !b.zlib && b.data.size() != b.raw_size) {
        if (err) *err = ".luxp con una seccion de tamano incoherente";
        return false;
    }
    return r.ok;
}

} // namespace

std::string encode_container(const std::vector<WirePage>& pages, const Resources& resources) {
    wire::Writer w;
    w.out = "LUXP";
    w.out += kVersion;
    w.uv(pages.size());
    for (const auto& p : pages) { w.str(p.name); put_blob(w, p.document); put_blob(w, p.sheet); put_blob(w, p.program); }
    w.uv(resources.size());
    for (const auto& [name, bytes] : resources) { w.str(name); w.str(bytes); }
    return w.out;
}

std::string encode_single_page(const std::string& name, const Blob& document, const Blob& sheet, const Blob& program) {
    wire::Writer w;
    w.out.reserve(32 + name.size() + document.data.size() + sheet.data.size() + program.data.size());
    w.out = "LUXP";
    w.out += kVersion;
    w.uv(1);
    w.str(name);
    put_blob(w, document);
    put_blob(w, sheet);
    put_blob(w, program);
    w.uv(0);
    return std::move(w.out);
}

std::string encode_container(const Container& c) {
    std::vector<WirePage> pages;
    for (const auto& [name, s] : c.pages)
        pages.push_back({name, blob_deflate(s.document), blob_deflate(s.sheet), blob_deflate(s.program)});
    return encode_container(pages, c.resources);
}

bool decode_container(const std::string& s, Container& out, std::string* err) {
    auto fail = [&](const char* why) { if (err) *err = why; return false; };
    out = {};
    if (s.size() < 6 || s.compare(0, 4, "LUXP") != 0) return fail("no es un .luxp");
    if (s[4] != kVersion) return fail("version de .luxp distinta: vuelve a generarlo");

    wire::Reader r{s, 5};
    uint64_t plain_total = 0;
    const uint32_t np = r.count(kMaxItems, 8);
    if (!r.ok || np == 0) return fail(".luxp sin paginas");
    for (uint32_t i = 0; i < np && r.ok; ++i) {
        std::string name = r.str(1024);
        Blob blobs[3];
        PageSections p;
        std::string* plain[3] = {&p.document, &p.sheet, &p.program};
        for (int k = 0; k < 3 && r.ok; ++k) {
            if (!get_blob(r, blobs[k], err)) { out = {}; return false; }
            plain_total += blobs[k].raw_size;
            if (plain_total > kMaxBytes) return fail(".luxp demasiado grande");
            if (!blob_inflate(blobs[k], *plain[k], err)) { out = {}; return false; }
        }
        if (r.ok && name.empty()) return fail(".luxp con una pagina sin nombre");
        out.pages.emplace_back(std::move(name), std::move(p));
    }
    const uint32_t nr = r.count(kMaxItems, 2);
    for (uint32_t i = 0; i < nr && r.ok; ++i) {
        std::string name = r.str(1024);
        std::string bytes = r.str(1u << 28);
        if (r.ok && name.empty()) return fail(".luxp con un recurso sin nombre");
        out.resources.emplace_back(std::move(name), std::move(bytes));
    }
    if (!r.done()) { out = {}; return fail(".luxp truncado o con bytes sobrantes"); }
    return true;
}

// ── the translator ──────────────────────────────────────────────────────────

ParsedPage parse_page(const std::string& html) {
    ParsedPage p;
    html::ParseResult parsed = html::parse(html);
    p.document = std::move(parsed.root);
    for (const auto& e : parsed.errors) p.warnings.push_back("html: " + e);
    return p;
}

std::string styles_key(dom::Node& doc) {
    std::vector<dom::Node*> sheets;
    collect_sheets(doc, sheets);
    std::string key;
    for (dom::Node* el : sheets) {
        key += el->is_element("style") ? 's' : 'l';
        key += el->is_element("style") ? el->text_content() : el->attr_or("href");
        key += '\0';
    }
    return key;
}

// Stylesheets in document order, <style> and <link rel=stylesheet> alike.  An
// @import resolves against the sheet that contains it: the page for a
// <style>, the linked file's own folder for a <link>.
StylesResult translate_styles(dom::Node& doc, const Fetcher& fetch) {
    StylesResult t;
    std::vector<dom::Node*> sheets;
    collect_sheets(doc, sheets);
    for (dom::Node* el : sheets) {
        if (el->is_element("style")) {
            style::parse_sheet(el->text_content(), t.rules,
                               [&](const std::string& url) { return fetch(url).value_or(""); });
            continue;
        }
        const std::string href = el->attr_or("href");
        auto sheet = fetch(href);
        if (!sheet) { t.warnings.push_back("stylesheet not found: " + href); continue; }
        const std::string dir = href.substr(0, href.rfind('/') + 1);
        style::parse_sheet(*sheet, t.rules, [&](const std::string& url) {
            const bool abs = !url.empty() && (url[0] == '/' || url.find(':') != std::string::npos);
            return fetch(abs ? url : dir + url).value_or("");
        });
    }
    for (size_t i = 0; i < t.rules.size(); ++i) t.rules[i].order = static_cast<int>(i);
    return t;
}

namespace {

// The <script> bodies that are LuxScript, with the document line they start
// at; sets the flags for the ones that are not.
std::vector<std::pair<std::string, int>> script_blocks(dom::Node& doc, ScriptsResult* r) {
    std::vector<std::pair<std::string, int>> blocks;
    for (dom::Node* s : doc.find_all("script")) {
        const std::string type = s->attr_or("type");
        if (type.find("javascript") != std::string::npos) { if (r) r->uses_javascript = true; continue; }
        if (s->has_attr("src")) {
            const std::string src = s->attr_or("src");
            if (!src.ends_with(".lux")) { if (r) r->uses_javascript = true; continue; }   // the web's <script src> is JS
            if (r) r->warnings.push_back("script src=\"" + src + "\": external .lux not loaded yet");
            continue;
        }
        blocks.emplace_back(s->text_content(), s->line);
    }
    return blocks;
}

} // namespace

std::string scripts_key(dom::Node& doc) {
    std::string key;
    for (const auto& [body, line] : script_blocks(doc, nullptr)) {
        key += std::to_string(line);
        key += '\0';
        key += body;
        key += '\0';
    }
    // JavaScript is part of the answer (the page is not a .luxp), not of the bytecode.
    for (dom::Node* s : doc.find_all("script"))
        if (s->attr_or("type").find("javascript") != std::string::npos ||
            (s->has_attr("src") && !s->attr_or("src").ends_with(".lux")))
            key += 'J';
    return key;
}

// Every inline non-JavaScript <script> is LuxScript; all of them compile into
// one program.  JavaScript is detected, never translated.
ScriptsResult translate_scripts(dom::Node& doc) {
    ScriptsResult t;
    const auto blocks = script_blocks(doc, &t);
    if (!blocks.empty()) {
        auto prog = script::compile_blocks(blocks, "page");
        if (prog->ok()) t.program = std::move(prog);
        else t.errors = prog->errors;
    }
    return t;
}

Translation translate_html(const std::string& html, const Fetcher& fetch) {
    Translation t;
    ParsedPage parsed = parse_page(html);
    t.document = std::move(parsed.document);
    t.warnings = std::move(parsed.warnings);
    dom::Node& doc = *t.document;

    StylesResult styles = translate_styles(doc, fetch);
    t.rules = std::move(styles.rules);
    for (auto& w : styles.warnings) t.warnings.push_back(std::move(w));

    ScriptsResult scripts = translate_scripts(doc);
    t.program = std::move(scripts.program);
    t.errors = std::move(scripts.errors);
    t.uses_javascript = scripts.uses_javascript;
    for (auto& w : scripts.warnings) t.warnings.push_back(std::move(w));

    dom_refs(doc, t);
    for (const auto& r : t.rules)
        for (const auto& d : r.decls) url_refs(d.value, t.resources);
    return t;
}

PageSections encode_page(const Translation& t) {
    return {encode_document(*t.document), encode_sheet(t.rules),
            t.program ? script::serialize(*t.program) : std::string()};
}

} // namespace luxium::page
