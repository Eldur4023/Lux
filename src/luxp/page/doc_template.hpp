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
//   S  a value inside a <script> body: not part of the document (a script's text is never encoded), it only
//      lets the template's scripts compile; the producer patches the program's constants (luxp_template.cpp)
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

#include <cstdint>
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

    // Appends the document section (what encode_document() writes) to `out`.
    // false if the host failed (its error is its own).
    bool render(Host& host, std::string& out) const;

    ~DocTemplate();

    struct Item;   // the tree build() works on; opaque here
    // A string made of literal runs and holes, for the ops below that carry one.
    struct Part {
        int64_t  hole = -1;    // >= 0: a value, by id
        uint32_t off = 0, len = 0;   // else: lits_[off, off+len)
    };
    struct Op {
        enum Code : uint8_t {
            End,
            Static,      // a=lit offset, b=len, c=nodes: whole encoded nodes (flushes the pending text first)
            Text,        // a=first part, b=count: append to the open element's pending text
            ElemOpen,    // a=lit offset, b=len: flush, then the element's head and its leading static attributes
            AttrStatic,  // a=lit offset, b=len: attribute bytes after a dynamic attribute
            AttrDyn,     // a=first part, b=count: one dynamic attribute value, length-prefixed
            ElemBody,    // reserve the child count; children follow
            ElemClose,   // flush the text, patch the child count
            Leaf,        // a=first part, b=count: the element's only child is this text (no frame, no copy)
            LoopBegin,   // a=id, b=pc past LoopNext
            LoopNext,    // a=id, b=pc of the body's first op
            If,          // a=id, b=pc to jump to when false
            Jump,        // a=pc
        };
        Code     code = End;
        uint32_t a = 0, b = 0, c = 0;
    };

    // render() with the host's concrete type: its hole()/test()/loop_*() inline into the loop (see
    // doc_template_run.hpp, which a caller includes to use this).
    // The limits matter for a program that came over the network (see expand()): the output stops
    // growing past max_out, and the work past max_steps ops.
    template <class H>
    bool run(H& host, std::string& out, size_t max_out = SIZE_MAX, uint64_t max_steps = UINT64_MAX) const;

    // A template as a block: the program render() runs, and back (checked: it comes from a server).
    std::string serialize() const;
    static std::unique_ptr<DocTemplate> from_bytes(const std::string& bytes, std::string* err);
    // What run() would have written, from the TRACE of its decisions: for each hole the value
    // (LEB128 length + bytes), for each loop its item count (LEB128), for each if one byte 0/1.
    // trace() is the producer's half.
    template <class H> bool trace(H& host, std::string& out) const;
    bool expand(const std::string& trace, std::string& document, std::string* err) const;

private:
    DocTemplate();
    NameTable                 names_;
    // The tree, compiled: render() runs these ops front to back with an explicit stack (no recursion, no
    // pointer chasing through nested vectors); literals live together in lits_.
    std::vector<Op>           ops_;
    std::vector<Part>         parts_;
    std::string               lits_;
    std::string               prologue_;   // magic, version, name table: constant
};

} // namespace luxium::page
