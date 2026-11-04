#include "page/luxp.hpp"

#include <html/parser.hpp>
#include <page/doc_template.hpp>
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

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace luxium::page {

namespace {

constexpr char     kVersion  = 7;   // 7: the document may travel as a template block + the trace of its values
// 6: a blob may carry an id, or be only a reference to a block the reader holds
constexpr char     kMinVersion = 5; // 5: sections as blobs, LEB128
constexpr uint64_t kMaxBytes = 64ull << 20;   // plain bytes of all the sections of a container
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


// url(x) in a linked sheet is relative to THAT sheet; the page that loads the compiled sheet resolves it
// against itself, so each relative one is rewritten to be relative to the page (or root-absolute).
std::string normalize_path(const std::string& p) {
    std::vector<std::string> parts;
    size_t i = 0;
    const bool root = !p.empty() && p[0] == '/';
    while (i <= p.size()) {
        size_t e = p.find('/', i);
        if (e == std::string::npos) e = p.size();
        const std::string seg = p.substr(i, e - i);
        if (seg == "..") { if (!parts.empty() && parts.back() != "..") parts.pop_back(); else if (!root) parts.push_back(seg); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        i = e + 1;
    }
    std::string out = root ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k) out += (k ? "/" : "") + parts[k];
    return out;
}

std::string rewrite_urls(const std::string& v, const std::string& dir) {
    std::string out;
    size_t pos = 0;
    for (size_t p = v.find("url("); p != std::string::npos; p = v.find("url(", pos)) {
        size_t i = p + 4;
        while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
        const char q = (i < v.size() && (v[i] == '"' || v[i] == '\'')) ? v[i] : 0;
        if (q) ++i;
        size_t j = i;
        while (j < v.size() && (q ? v[j] != q : !std::strchr(") \t\r\n", v[j]))) ++j;
        const std::string arg = v.substr(i, j - i);
        const bool local = !arg.empty() && arg[0] != '/' && arg[0] != '#' && arg.find(':') == std::string::npos;
        out += v.substr(pos, i - pos);
        out += local ? normalize_path(dir + arg) : arg;
        pos = j;
    }
    return out + v.substr(pos);
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

Blob blob_deflate(const std::string& bytes, int level) {
#ifdef LUXP_ZLIB
    if (bytes.size() >= 64 && bytes.size() <= kMaxSection) {
        uLongf zlen = compressBound(bytes.size());
        std::string z(zlen, '\0');
        if (compress2(reinterpret_cast<Bytef*>(z.data()), &zlen,
                      reinterpret_cast<const Bytef*>(bytes.data()), bytes.size(), level) == Z_OK &&
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

// flag: bits 0-1 = 0 plain, 1 zlib, 2 reference (only the id follows); bit 2 = an id follows
// (before the sizes).  `have`: blocks the reader already holds go out as references.
void put_blob(wire::Writer& w, const Blob& b, const HaveSet* have = nullptr) {
    if (b.id && have && have->has(b.id)) { w.u8(2); w.u64(b.id); return; }
    w.u8(uint8_t((b.zlib ? 1 : 0) | (b.id ? 4 : 0)));
    if (b.id) w.u64(b.id);
    w.uv(b.raw_size);
    w.str(b.data);
}

// The sheet section: one block as any section, or (flag 3) a count and that many blocks.
void put_sheet(wire::Writer& w, const SheetBlobs& sheet, const HaveSet* have) {
    if (sheet.size() == 1) { put_blob(w, sheet[0], have); return; }
    w.u8(3);
    w.uv(sheet.size());
    for (const Blob& b : sheet) put_blob(w, b, have);
}

bool get_blob(wire::Reader& r, Blob& b, std::string* err, BlockCache* cache, bool* from_cache) {
    const uint8_t flag = r.u8();
    const uint8_t kind = flag & 3;
    if (flag > 7 || kind == 3 || (kind == 2 && (flag & 4))) {
        if (err) *err = ".luxp con una seccion invalida";
        return false;
    }
    if (kind == 2) {
        const uint64_t id = r.u64();
        if (!r.ok) return false;
        if (!cache || !cache->find(id, b)) {
            if (err) *err = ".luxp referencia un bloque que no tengo";
            return false;
        }
        *from_cache = true;
        return true;   // found in transport form (plain or zlib), already checked when it was stored
    }
    if (flag & 4) b.id = r.u64();
    b.raw_size = r.uv32();
    if (b.raw_size > kMaxSection) {
        if (err) *err = ".luxp con una seccion invalida";
        return false;
    }
    b.zlib = kind == 1;
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

std::string encode_single_page(const std::string& name, const Blob& document, const SheetBlobs& sheet, const Blob& program,
                               const HaveSet* have) {
    wire::Writer w;
    w.out.reserve(32 + name.size() + document.data.size() + sheet.size() * 128 + program.data.size());
    w.out = "LUXP";
    w.out += kVersion;
    w.uv(1);
    w.str(name);
    put_blob(w, document);
    put_sheet(w, sheet, have);
    put_blob(w, program, have);
    w.uv(0);
    return std::move(w.out);
}

namespace {
// A uint32 as a 5-byte LEB128 at `at`: room reserved before the value is known. The reader
// takes non-minimal encodings (Reader::uv only bounds the length).
void put_uv5(std::string& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = char(((v >> (7 * i)) & 0x7F) | 0x80);
    out[at + 4] = char(v >> 28);
}
}

size_t begin_single_page(std::string& out, const std::string& name) {
    wire::Writer w;
    w.out = std::move(out);
    w.out += "LUXP";
    w.out += kVersion;
    w.uv(1);
    w.str(name);
    w.u8(0);                    // the document: plain
    w.out.append(10, '\0');     // its raw_size and its length, as two padded LEBs
    out = std::move(w.out);
    return out.size();
}

void finish_single_page(std::string& out, size_t document_at, const SheetBlobs& sheet, const Blob& program,
                        const HaveSet* have) {
    const auto n = uint32_t(out.size() - document_at);
    put_uv5(out, document_at - 10, n);
    put_uv5(out, document_at - 5, n);
    wire::Writer w;
    w.out = std::move(out);
    put_sheet(w, sheet, have);
    put_blob(w, program, have);
    w.uv(0);
    out = std::move(w.out);
}

void finish_single_page(std::string& out, size_t document_at, const SheetBlobs& sheet, const Blob& program, int level,
                        const HaveSet* have) {
#ifdef LUXP_ZLIB
    const size_t n = out.size() - document_at;
    if (level > 0 && n >= 512 && n <= kMaxSection) {
        // One deflate state per thread, reset between pages: compress2() would allocate and clear a
        // ~256 KB state each call, more than compressing a page costs.  An 8 KB window and small hash
        // tables are plenty for a document of a few KB (the reader's inflate takes any window).
        struct Deflater {
            z_stream zs{};
            int      level = -1;
            bool     ok = false;
            ~Deflater() { if (ok) deflateEnd(&zs); }
        };
        thread_local Deflater d;
        thread_local std::string z;
        if (!d.ok || d.level != level) {
            if (d.ok) deflateEnd(&d.zs);
            d.zs = z_stream{};
            d.ok = deflateInit2(&d.zs, level, Z_DEFLATED, 13, 5, Z_DEFAULT_STRATEGY) == Z_OK;
            d.level = level;
        }
        if (!d.ok) { finish_single_page(out, document_at, sheet, program, have); return; }
        deflateReset(&d.zs);
        uLongf zlen = deflateBound(&d.zs, n);
        z.resize(zlen);
        d.zs.next_in = reinterpret_cast<Bytef*>(out.data() + document_at);
        d.zs.avail_in = uInt(n);
        d.zs.next_out = reinterpret_cast<Bytef*>(z.data());
        d.zs.avail_out = uInt(zlen);
        const bool done = deflate(&d.zs, Z_FINISH) == Z_STREAM_END;
        zlen = d.zs.total_out;
        if (done && zlen < n) {
            out.resize(document_at - 11);   // back to before the document's flag byte and its two padded LEBs
            wire::Writer w;
            w.out = std::move(out);
            w.u8(1);
            w.uv(uint32_t(n));
            w.uv(uint32_t(zlen));
            w.out.append(z.data(), zlen);
            put_sheet(w, sheet, have);
            put_blob(w, program, have);
            w.uv(0);
            out = std::move(w.out);
            return;
        }
    }
#endif
    finish_single_page(out, document_at, sheet, program, have);
}

std::string encode_traced_page(const std::string& name, const Blob& tmpl, const std::string& trace, int level,
                               const SheetBlobs& sheet, const Blob& program, const HaveSet* have) {
    wire::Writer w;
    w.out.reserve(48 + name.size() + trace.size() + sheet.size() * 128 + program.data.size());
    w.out = "LUXP";
    w.out += kVersion;
    w.uv(1);
    w.str(name);
    w.u8(3);
    put_blob(w, tmpl, have);
    Blob t = level > 0 ? blob_deflate(trace, level) : blob_plain(trace);
    put_blob(w, t);
    put_sheet(w, sheet, have);
    put_blob(w, program, have);
    w.uv(0);
    return std::move(w.out);
}

std::string encode_container(const Container& c) {
    std::vector<WirePage> pages;
    for (const auto& [name, s] : c.pages)
        pages.push_back({name, blob_deflate(s.document), blob_deflate(s.sheet), blob_deflate(s.program)});
    return encode_container(pages, c.resources);
}

bool decode_container(const std::string& s, Container& out, std::string* err, BlockCache* cache) {
    auto fail = [&](const char* why) { if (err) *err = why; return false; };
    out = {};
    if (s.size() < 6 || s.compare(0, 4, "LUXP") != 0) return fail("no es un .luxp");
    if (s[4] < kMinVersion || s[4] > kVersion) return fail("version de .luxp distinta: vuelve a generarlo");

    wire::Reader r{s, 5};
    uint64_t plain_total = 0;
    const uint32_t np = r.count(kMaxItems, 8);
    if (!r.ok || np == 0) return fail(".luxp sin paginas");
    for (uint32_t i = 0; i < np && r.ok; ++i) {
        std::string name = r.str(1024);
        Blob blobs[3];
        PageSections p;
        std::string* plain[3] = {&p.document, &p.sheet, &p.program};
        // One blob read, inflated to `plain`, accounted, and (a block that arrived in full) checked and kept.
        auto settle = [&](Blob& b, std::string& plain, bool from_cache) {
            plain_total += b.raw_size;
            if (plain_total > kMaxBytes) { if (err) *err = ".luxp demasiado grande"; return false; }
            const Blob kept = (cache && b.id && !from_cache) ? b : Blob();   // as it travelled
            if (!b.zlib) plain = std::move(b.data);   // already read once
            else if (!blob_inflate(b, plain, err)) return false;
            if (kept.id) {   // its id must be its content's, then keep it
                if (block_id(plain) != kept.id) { if (err) *err = ".luxp con un bloque cuyo id no corresponde a su contenido"; return false; }
                cache->store(kept);
            }
            return true;
        };
        for (int k = 0; k < 3 && r.ok; ++k) {
            bool from_cache = false;
            if (k == 0 && r.at < s.size() && (uint8_t(s[r.at]) & 3) == 3) {
                // The document as the template that makes it (a block, usually held already) and the
                // trace of the values: run the template over the trace.
                r.at++;
                Blob tb, trb;
                std::string tmpl, trace, why;
                if (!get_blob(r, tb, err, cache, &from_cache) || !settle(tb, tmpl, from_cache)) { out = {}; return false; }
                bool unused = false;
                if (!get_blob(r, trb, err, nullptr, &unused) || !settle(trb, trace, false)) { out = {}; return false; }
                auto t = DocTemplate::from_bytes(tmpl, &why);
                if (!t || !t->expand(trace, p.document, &why)) { if (err) *err = ".luxp: " + why; out = {}; return false; }
                plain_total += p.document.size();
                if (plain_total > kMaxBytes) return fail(".luxp demasiado grande");
                continue;
            }
            if (k == 1 && r.at < s.size() && (uint8_t(s[r.at]) & 3) == 3) {   // several stylesheet blocks
                r.at++;
                const uint32_t n = r.count(256, 2);
                if (!r.ok || n == 0) return fail(".luxp con una hoja de estilos mal formada");
                for (uint32_t j = 0; j < n && r.ok; ++j) {
                    Blob b;
                    bool fc = false;
                    std::string one;
                    if (!get_blob(r, b, err, cache, &fc) || !settle(b, one, fc)) { out = {}; return false; }
                    if (j == 0) p.sheet = std::move(one); else p.more_sheets.push_back(std::move(one));
                }
                continue;
            }
            if (!get_blob(r, blobs[k], err, cache, &from_cache) || !settle(blobs[k], *plain[k], from_cache)) { out = {}; return false; }
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

bool decode_page_sheet(const PageSections& s, std::vector<style::Rule>& rules, std::string* err) {
    if (!decode_sheet(s.sheet, rules, err)) return false;
    for (const std::string& more : s.more_sheets) {
        std::vector<style::Rule> extra;
        if (!decode_sheet(more, extra, err)) return false;
        for (auto& r : extra) rules.push_back(std::move(r));
    }
    if (!s.more_sheets.empty())
        for (size_t i = 0; i < rules.size(); ++i) rules[i].order = static_cast<int>(i);   // one cascade across the blocks
    return true;
}

// ── blocks ──────────────────────────────────────────────────────────────────

uint64_t block_id(const std::string& plain) {
    // Four independent lanes (a single multiply chain is latency-bound), then folded.
    uint64_t l[4] = {0x9E3779B97F4A7C15ull ^ plain.size(), 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    const char* p = plain.data();
    size_t n = plain.size();
    for (; n >= 32; p += 32, n -= 32) {
        uint64_t w[4];
        std::memcpy(w, p, 32);
        for (int i = 0; i < 4; ++i) { l[i] = (l[i] ^ w[i]) * 0xFF51AFD7ED558CCDull; l[i] ^= l[i] >> 32; }
    }
    uint64_t tail[4] = {};
    std::memcpy(tail, p, n);
    for (int i = 0; i < 4; ++i) { l[i] = (l[i] ^ tail[i]) * 0xC4CEB9FE1A85EC53ull; l[i] ^= l[i] >> 29; }
    uint64_t h = l[0] ^ (l[1] * 3) ^ (l[2] * 5) ^ (l[3] * 7);
    h = (h ^ (h >> 33)) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return h ? h : 1;
}

HaveSet parse_have(const std::string& header) {
    HaveSet s;
    size_t i = 0;
    while (i < header.size() && s.n < 16) {
        while (i < header.size() && (header[i] == ',' || header[i] == ' ')) ++i;
        uint64_t v = 0;
        size_t digits = 0;
        while (i < header.size() && std::isxdigit(static_cast<unsigned char>(header[i])) && digits < 16) {
            const char c = header[i++];
            v = (v << 4) | uint64_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            ++digits;
        }
        if (digits == 16 && v) s.ids[s.n++] = v;
        while (i < header.size() && header[i] != ',') ++i;
    }
    return s;
}

std::string url_origin(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const size_t end = url.find('/', scheme + 3);
    return url.substr(0, end);
}

struct BlockStore::Impl {
    std::mutex m;
    std::map<std::string, std::map<uint64_t, Entry>> by_origin;
    size_t bytes = 0;
    int64_t clock = 0;   // a counter for recency, plus real time for the 30 s rule
    static int64_t now_s() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    struct Stamp { int64_t order, when; };
    std::map<std::pair<std::string, uint64_t>, Stamp> stamps;
};

BlockStore::BlockStore() : impl_(new Impl) {}
BlockStore::~BlockStore() = default;

BlockStore& BlockStore::global() {
    static BlockStore s;
    return s;
}

bool BlockStore::find(const std::string& origin, uint64_t id, Blob& out) {
    std::lock_guard<std::mutex> lk(impl_->m);
    auto o = impl_->by_origin.find(origin);
    if (o == impl_->by_origin.end()) return false;
    auto e = o->second.find(id);
    if (e == o->second.end()) return false;
    e->second.stamp = ++impl_->clock;
    impl_->stamps[{origin, id}] = {impl_->clock, Impl::now_s()};
    out = e->second.blob;
    return true;
}

void BlockStore::store(const std::string& origin, const Blob& b) {
    if (!b.id || origin.empty()) return;
    constexpr size_t kCap = 16u << 20;   // 16 MB of blocks in transport form
    std::lock_guard<std::mutex> lk(impl_->m);
    auto& slot = impl_->by_origin[origin][b.id];
    impl_->bytes += b.data.size();
    impl_->bytes -= slot.blob.data.size();
    slot.blob = b;
    slot.stamp = ++impl_->clock;
    impl_->stamps[{origin, b.id}] = {impl_->clock, Impl::now_s()};
    // Over the cap: drop the least recently used, but nothing younger than 30 s (it may be one a
    // request in flight announced); a burst of fresh blocks may overshoot for a moment.
    while (impl_->bytes > kCap) {
        auto victim = impl_->stamps.end();
        const int64_t now = Impl::now_s();
        for (auto it = impl_->stamps.begin(); it != impl_->stamps.end(); ++it)
            if (now - it->second.when >= 30 && (victim == impl_->stamps.end() || it->second.order < victim->second.order))
                victim = it;
        if (victim == impl_->stamps.end()) break;
        auto& blocks = impl_->by_origin[victim->first.first];
        impl_->bytes -= blocks[victim->first.second].blob.data.size();
        blocks.erase(victim->first.second);
        impl_->stamps.erase(victim);
    }
}

std::string BlockStore::have_header(const std::string& origin, size_t max) {
    std::lock_guard<std::mutex> lk(impl_->m);
    std::vector<std::pair<int64_t, uint64_t>> ids;   // (recency, id), newest first
    for (const auto& [key, st] : impl_->stamps)
        if (key.first == origin) ids.push_back({st.order, key.second});
    std::sort(ids.rbegin(), ids.rend());
    if (ids.size() > max) ids.resize(max);
    std::string h;
    for (const auto& [order, id] : ids) {
        char buf[20];
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(id));
        if (!h.empty()) h += ',';
        h += buf;
    }
    return h;
}

std::unique_ptr<BlockCache> BlockStore::view(const std::string& origin) {
    struct View : BlockCache {
        BlockStore& s; std::string origin;
        View(BlockStore& s, std::string o) : s(s), origin(std::move(o)) {}
        bool find(uint64_t id, Blob& out) override { return s.find(origin, id, out); }
        void store(const Blob& b) override { s.store(origin, b); }
    };
    return std::make_unique<View>(*this, origin);
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
            t.ends.push_back(t.rules.size());
            continue;
        }
        const std::string href = el->attr_or("href");
        auto sheet = fetch(href);
        if (!sheet) { t.warnings.push_back("stylesheet not found: " + href); t.ends.push_back(t.rules.size()); continue; }
        const std::string dir = href.substr(0, href.rfind('/') + 1);
        const size_t first = t.rules.size();
        style::parse_sheet(*sheet, t.rules, [&](const std::string& url) {
            const bool abs = !url.empty() && (url[0] == '/' || url.find(':') != std::string::npos);
            return fetch(abs ? url : dir + url).value_or("");
        });
        if (!dir.empty() && dir.find(':') == std::string::npos)   // an http(s) href is its own base: left as it was
            for (size_t k = first; k < t.rules.size(); ++k)
                for (auto& d : t.rules[k].decls)
                    if (d.value.find("url(") != std::string::npos) d.value = rewrite_urls(d.value, dir);
        t.ends.push_back(t.rules.size());
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
