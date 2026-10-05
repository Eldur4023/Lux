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
struct Blob {
    std::string data;
    uint32_t    raw_size = 0;
    bool        zlib = false;
};
Blob blob_plain(std::string bytes);
// zlib level 9 when this build has zlib and it helps; plain otherwise.
Blob blob_deflate(const std::string& bytes);
bool blob_inflate(const Blob& b, std::string& out, std::string* err);

// The three sections of a page, plain.
struct PageSections {
    std::string document, sheet, program;   // program empty = no script
};

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
std::string encode_single_page(const std::string& name, const Blob& document, const Blob& sheet, const Blob& program);
// Everything compressed (blob_deflate): for a file written once.
std::string encode_container(const Container& c);
// Well-formedness, and every section inflated to plain bytes; each section is
// verified when it is decoded.
bool decode_container(const std::string& bytes, Container& out, std::string* err);

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
    std::vector<std::string>  warnings;
};
StylesResult translate_styles(dom::Node& document, const Fetcher& fetch);
std::string  styles_key(dom::Node& document);     // the <style> texts and <link> hrefs

// The three sections of a translated page.  Requires errors.empty() and
// !uses_javascript (a .luxp page has no other way to run code).
PageSections encode_page(const Translation& t);

} // namespace luxium::page
