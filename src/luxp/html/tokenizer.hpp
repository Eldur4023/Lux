#pragma once
// HTML tokenizer -- the M0 subset of the WHATWG tokenizer.
//
// States covered: data, tag open/name, attributes (double/single/unquoted
// values), comment, doctype/bogus-markup, and raw-text mode for <script> and
// <style>. Everything else is folded into text, which is the recovery policy:
// the tokenizer never fails, it degrades.
//
// Attribute values keep their raw spelling (no entity decoding yet).

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luxium::html {

enum class TokType { Doctype, StartTag, EndTag, Comment, Text };

struct Token {
    TokType type;
    std::string name;  // tag name, lowercased
    // Insertion order kept; duplicate names: first wins (WHATWG behavior).
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;  // Text / Comment / Doctype payload
    bool self_closing = false;
    int  line = 0;     // line where the token starts
};

// Tokenizes a whole document in one pass. The returned vector always ends
// cleanly (a run of text at EOF is flushed).
std::vector<Token> tokenize(std::string_view source);

} // namespace luxium::html
