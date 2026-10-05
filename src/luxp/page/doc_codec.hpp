#pragma once
// The document and stylesheet sections of a .luxp.
//
// A .luxp page is not HTML: it is a DOM tree and a parsed rule list, the
// same structures the engine builds from HTML+CSS, stored ready to use.
// Loading one runs no HTML tokenizer and no CSS parser.
//
// Compact on purpose (the bytes a page costs on the wire): integers are
// LEB128, the names that repeat (tags, attribute names, CSS properties,
// selector names) live once in a table and are referenced by index, and
// source line numbers are not stored (they only mattered while compiling).
//
// Both decoders take untrusted bytes and verify while they read: bounded
// counts and lengths, tree depth <= dom::kMaxDepth, only the shapes the
// engine can produce (text nodes have no children, one #document root,
// selector enums in range, `combinator` exactly parts-1 long, :nth-*
// coefficients small enough not to overflow).  nullptr / false on anything
// else -- never a partial result.

#include <dom/node.hpp>
#include <style/stylesheet.hpp>
#include <util/wire.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace luxium::page {

// Names that repeat, written once.  ids are assigned on first use, in the
// order the bytes that use them are written.
struct NameTable {
    std::vector<std::string>                     names;
    std::unordered_map<std::string, uint32_t>    index;
    uint32_t id(const std::string& s) {
        auto [it, inserted] = index.try_emplace(s, uint32_t(names.size()));
        if (inserted) names.push_back(s);
        return it->second;
    }
};

// <script>/<style> contents are compile inputs, not document content: the
// elements stay (they count for :nth-child) but go out empty.
std::string encode_document(const dom::Node& doc);
std::unique_ptr<dom::Node> decode_document(const std::string& bytes, std::string* err);

// The pieces encode_document is made of, for a producer that writes a
// document without building its tree first (page/doc_template.hpp): the
// prologue (magic, version, name table) goes first, then one encode_node()
// of the #document, or any child list written by hand.
void encode_prologue(wire::Writer& w, const NameTable& names);
void encode_node(wire::Writer& w, const dom::Node& n, NameTable& names);   // type, payload, children

std::string encode_sheet(const std::vector<style::Rule>& rules);
bool decode_sheet(const std::string& bytes, std::vector<style::Rule>& out, std::string* err);

} // namespace luxium::page
