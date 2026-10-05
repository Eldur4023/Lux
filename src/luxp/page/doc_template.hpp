#pragma once
// A document section produced from a TEMPLATE, without parsing HTML per
// request.
//
// A server that fills a template with different data on every request would
// otherwise translate the filled HTML again each time: tokenize, build the
// tree, encode it.  But what changes between two requests is the data; the
// structure is the template's.  So the template is translated ONCE, into a
// DocTemplate: the document's encoding with holes where the data goes and
// repeat/choose nodes where {% for %}/{% if %} are.  Each request then only
// evaluates the holes and writes bytes.
//
// How one is made.  The producer renders the template once with MARKERS in
// place of the data -- a "skeleton": text for {{ x }}, and brackets around the
// body of each {% for %} / {% if %} -- and hands its HTML and parsed tree to
// build().  Markers are \x01 K id \x02:
//   H  a value, written escaped by the template: its text lives in a text
//      node or a quoted attribute value, where the parser would decode it
//      back to the raw string -- so the hole holds the RAW string
//   R  a value written raw (|safe): HTML the parser would have to parse; not
//      expressible, build() refuses the template
//   L  a loop starts / I an if starts / N its else / E the block ends
//
// What makes a template ineligible (build() returns null and says why; the
// caller translates the filled HTML every time instead -- always correct):
//   - a marker anywhere the filled page's structure would depend on the data
//     or on how the parser reads it: a tag or attribute name, an unquoted
//     attribute value, a comment, <script>/<style> contents (they compile to
//     the program and the sheet), <link href>, <script src>, or text directly
//     under #document/<html>/<head> (the parser drops or moves it)
//   - a block that does not open and close under the same parent element
//     (a loop that creates a <tbody> the first time round, a tag split by an
//     {% if %}): its output is not a list of whole subtrees
//   - a value glued to an entity ("&{{ x }}"): decode() would not undo it
//
// Not a fast path with a different meaning: the document it writes, decoded
// and written again by encode_document(), is byte for byte what translating
// the filled HTML gives -- Lux checks that on demand (LUX_LUXP_CHECK=1) and
// in its test suite.

#include <dom/node.hpp>
#include <page/doc_codec.hpp>

#include <memory>
#include <string>
#include <vector>

namespace luxium::page {

// \x01 K id \x02
std::string template_marker(char kind, uint32_t id);

class DocTemplate {
public:
    // What the template's engine does at each hole and block; ids are the
    // ones the skeleton carries.
    struct Host {
        virtual ~Host() = default;
        virtual bool hole(uint32_t id, std::string& out) = 0;        // append the value's raw text
        virtual bool test(uint32_t id, bool& value) = 0;             // {% if %}
        virtual bool loop_begin(uint32_t id, size_t& items) = 0;     // evaluates the list
        virtual void loop_item(uint32_t id, size_t index, size_t items) = 0;
        virtual void loop_end(uint32_t id) = 0;
    };

    // nullptr + *why_not when the template cannot be one.
    static std::unique_ptr<DocTemplate> build(const dom::Node& skeleton, const std::string& skeleton_html,
                                              std::string* why_not);

    // Writes the document section (what encode_document() writes) into `out`.
    // false if the host failed (its error is its own).
    bool render(Host& host, std::string& out) const;

    ~DocTemplate();

    struct Item;   // the tree; opaque here

private:
    DocTemplate();
    NameTable                 names_;
    std::vector<Item>         root_;
    std::string               prologue_;   // magic, version, name table: constant
};

} // namespace luxium::page
