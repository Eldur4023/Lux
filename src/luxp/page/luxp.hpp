#pragma once
// .luxp -- Luxium's application format, and the one translator into it.
//
// A .luxp holds pages already in the engine's own structures plus the
// resources they use:
//
//   page     = document  (DOM tree,                 doc_codec)
//            + sheet     (parsed author rules,      doc_codec)
//            + program   (verifiable bytecode,      script/bytecode_io)
//   resource = name + bytes (images)
//
// Container: "LUXP" 0x05, then
//   pages     { str name, blob document, blob sheet, blob program }  (page 0 = entry)
//   resources { str name, str bytes }
// A blob is { u8 zlib?, uv plain size, str data }: each section is stored
// plain or compressed ON ITS OWN, so a server compresses the parts that never
// change once and sends the part that does change (the document) as it is.
// Integers are LEB128 and strings are length-prefixed (util/wire.hpp).
//
// HTML+CSS+LuxScript is one source a .luxp can be built from, and
// translate_html() is the ONLY code that interprets that source: Luxium
// calls it to show an HTML page, Lux calls it to produce .luxp (lux pack,
// render() for a client that accepts application/x-luxp).  One translator,
// so a page cannot mean two things.
//
// Owned by Lux (src/luxp/), vendored into Luxium by tools/sync_from_lux.sh.

#include <dom/node.hpp>
#include <script/page_compile.hpp>
#include <style/stylesheet.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luxium::page {

inline constexpr const char* kLuxpMime = "application/x-luxp";

// A section as it travels: plain bytes, or zlib of them (+ their plain size).
// `id` names a block a client may already hold (0: not cacheable): the hash of the plain bytes.
// The stylesheet and the program of a template are the same on every visit, so they are blocks;
// the document, which carries the data, is not.
struct Blob {
    std::string data;
    uint32_t    raw_size = 0;
    bool        zlib = false;
    uint64_t    id = 0;
};
uint64_t block_id(const std::string& plain);   // never 0

// The ids a client says it holds (request header X-Luxp-Have: 16 hex digits each, comma separated).
inline constexpr const char* kHaveHeader = "X-Luxp-Have";
struct HaveSet {
    uint64_t ids[16] = {};
    size_t   n = 0;
    bool has(uint64_t id) const { for (size_t i = 0; i < n; ++i) if (ids[i] == id) return true; return false; }
};
HaveSet parse_have(const std::string& header);

// Where a decoder finds the blocks the sender left out, and keeps the ones it received.
struct BlockCache {
    virtual ~BlockCache() = default;
    virtual bool find(uint64_t id, Blob& out) = 0;
    virtual void store(const Blob& b) = 0;
};

// A process-wide, bounded store of blocks, kept per origin (a server can only make a client
// resolve ids to bytes that same server sent).  Blocks are only dropped once older than 30 s,
// so the ones a request announced are still there when its answer arrives.
class BlockStore {
public:
    static BlockStore& global();
    bool find(const std::string& origin, uint64_t id, Blob& out);
    void store(const std::string& origin, const Blob& b);
    std::string have_header(const std::string& origin, size_t max = 16);   // "" when it holds nothing
    // A BlockCache bound to one origin.
    std::unique_ptr<BlockCache> view(const std::string& origin);
private:
    struct Entry { Blob blob; int64_t stamp = 0; };
    struct Impl;
    BlockStore();
    ~BlockStore();
    std::unique_ptr<Impl> impl_;
};
std::string url_origin(const std::string& url);   // "http://host:port", or "" when it is not one
Blob blob_plain(std::string bytes);
// zlib (level 9: for what is compressed once and kept; 1 for what is rebuilt per script)
// when this build has zlib and it helps; plain otherwise.
Blob blob_deflate(const std::string& bytes, int level = 9);
bool blob_inflate(const Blob& b, std::string& out, std::string* err);

// The three sections of a page, plain.
struct PageSections {
    std::string document, sheet, program;   // program empty = no script
    // A page whose stylesheets travel as separate blocks (so each is cached and shared by every page
    // that uses it): `sheet` is the first, these follow in document order.
    std::vector<std::string> more_sheets;
};

using SheetBlobs = std::vector<Blob>;   // one block per stylesheet source, in document order

// The rules of every sheet block of a page, in order (their cascade order renumbered across them).
bool decode_page_sheet(const PageSections& s, std::vector<style::Rule>& rules, std::string* err);

// A page as it travels.
struct WirePage {
    std::string name;
    Blob        document, sheet, program;
};

struct Container {
    std::vector<std::pair<std::string, PageSections>> pages;     // [0] = entry
    std::vector<std::pair<std::string, std::string>>  resources;
};
using Resources = std::vector<std::pair<std::string, std::string>>;

// Writes what the producer already prepared (see Blob).
std::string encode_container(const std::vector<WirePage>& pages, const Resources& resources);
// A container of one page and no resources, assembled straight from blobs the
// caller already holds: what a server answers per request.
std::string encode_single_page(const std::string& name, const Blob& document, const SheetBlobs& sheet, const Blob& program,
                               const HaveSet* have = nullptr);
// The same container written in place, for a document that is rendered straight into the
// response: begin_single_page() starts `out` (empty) and returns where the document goes;
// the caller appends it there; finish_single_page() fixes its length and adds the sections.
size_t begin_single_page(std::string& out, const std::string& name);
void   finish_single_page(std::string& out, size_t document_at, const SheetBlobs& sheet, const Blob& program,
                          const HaveSet* have = nullptr);
// The same, deflating the document first when level > 0 and that makes it smaller (zlib level 1 is
// ~3x the bytes saved per CPU spent that level 9 is; the sections that never change are kept deflated
// at 9 already).  The reader takes either, section by section.
void   finish_single_page(std::string& out, size_t document_at, const SheetBlobs& sheet, const Blob& program, int level,
                          const HaveSet* have = nullptr);
// A page whose document travels as `tmpl` (DocTemplate::serialize(), a block with an id: left out as
// a reference when the client holds it) plus `trace` (DocTemplate::trace()), deflated at `level` if > 0.
std::string encode_traced_page(const std::string& name, const Blob& tmpl, const std::string& trace, int level,
                               const SheetBlobs& sheet, const Blob& program, const HaveSet* have = nullptr);
// Everything compressed (blob_deflate): for a file written once.
std::string encode_container(const Container& c);
// Well-formedness, and every section inflated to plain bytes; each section is
// verified when it is decoded.
// `cache`: where blocks the sender left out are found (a missing one is an error) and the ones it
// sent are kept; without it, a container that leaves a block out is refused.
bool decode_container(const std::string& bytes, Container& out, std::string* err, BlockCache* cache = nullptr);

// What an HTML page translates to.
struct Translation {
    std::unique_ptr<dom::Node>          document;
    std::vector<style::Rule>            rules;      // <style>, <link>, @import flattened
    std::unique_ptr<script::Compiled>   program;    // null: no LuxScript, or it failed
    std::vector<std::string>            warnings;   // html recovery, missing stylesheet
    std::vector<std::string>            errors;     // LuxScript: "line N: message"
    bool                                uses_javascript = false;
    std::vector<std::string>            resources;  // local URLs: <img src>, url(...)
    std::vector<std::string>            links;      // local .html pages linked by <a href>
};

// Reads a URL relative to the page ("css/a.css") -> bytes, or nullopt.
using Fetcher = std::function<std::optional<std::string>(const std::string& url)>;

Translation translate_html(const std::string& html, const Fetcher& fetch);

// The stages translate_html is made of.  A server that translates the same
// page again and again (a template filled with different data) keeps the
// expensive stages' results under these keys: the script and the stylesheet
// are the same whatever the data is.
struct ParsedPage {
    std::unique_ptr<dom::Node> document;
    std::vector<std::string>   warnings;
};
ParsedPage parse_page(const std::string& html);

struct ScriptsResult {
    std::unique_ptr<script::Compiled> program;   // null: no LuxScript, or it failed
    std::vector<std::string>          errors, warnings;
    bool                              uses_javascript = false;
};
ScriptsResult translate_scripts(dom::Node& document);
std::string   scripts_key(dom::Node& document);   // what translate_scripts reads

struct StylesResult {
    std::vector<style::Rule>  rules;
    std::vector<size_t>       ends;     // rules.size() after each stylesheet source, in document order
    std::vector<std::string>  warnings;
};
StylesResult translate_styles(dom::Node& document, const Fetcher& fetch);
std::string  styles_key(dom::Node& document);     // the <style> texts and <link> hrefs

// The three sections of a translated page.  Requires errors.empty() and
// !uses_javascript (a .luxp page has no other way to run code).
PageSections encode_page(const Translation& t);

} // namespace luxium::page
