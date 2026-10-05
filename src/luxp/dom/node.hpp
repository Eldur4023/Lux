#pragma once
// Minimal DOM for Luxium's M0 skeleton.
//
// Node kinds are exactly what the HTML parser can produce; the CSS engine
// (M3) and the LuxScript element handles (M2) hang off the same tree. Parent
// pointers are raw and owned by `children` -- a Node is only destroyed from
// the root down, which the page model guarantees.
//
// No entities are decoded yet (skeleton): attribute values and text keep the
// raw source spelling. The entity table lands with the CSS milestone's
// tokenizer cleanup.

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luxium::dom {

enum class NodeType { Document, Doctype, Element, Text, Comment };

// Deepest tree the engine accepts (Chromium's parser uses the same 512).
// Layout and style recurse over the tree: without a cap, 8000 nested <div>
// overflow the stack.  Enforced by every way a tree is built: the HTML
// parser, Element.append() from LuxScript and the .luxp document decoder.
constexpr int kMaxDepth = 512;

struct Node;
int depth_of(const Node* n);          // 0 for the document
int subtree_height(const Node& n);    // 1 for a leaf

struct Node {
    NodeType type;
    std::string name;   // element tag (lowercase); "#text"/"#comment"/"#document"/"#doctype" for the rest
    std::string text;   // payload of Text / Comment / Doctype
    std::vector<std::pair<std::string, std::string>> attrs;  // insertion order kept; first duplicate wins
    Node*        parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
    int          line = 0;  // source line of the start tag (elements) / of the run (text)
    // dynamic pseudo-class state, set by the shell (hover chain, pressed, focus)
    enum State : unsigned char { Hover = 1, Active = 2, Focus = 4 };
    unsigned char state = 0;

    explicit Node(NodeType t) : type(t) {}

    // Takes ownership. Returns a reference to the appended child.
    Node& append(std::unique_ptr<Node> child);

    bool        is_element(std::string_view tag) const;
    bool        is_element() const { return type == NodeType::Element; }
    std::string attr_or(std::string_view key, std::string_view fallback = {}) const;
    bool        has_attr(std::string_view key) const;

    // Depth-first search among element descendants (self excluded).
    std::vector<Node*> find_all(std::string_view tag);
    Node*              find_first(std::string_view tag);

private:
    void find_all(std::string_view tag, std::vector<Node*>& out);

public:

    // Concatenated Text descendants, document order. Elements contribute
    // nothing of their own.
    std::string text_content() const;

    // Indented debug dump (what `luxium file.html` prints).
    void print(int depth = 0) const;
};

std::unique_ptr<Node> make_document();

} // namespace luxium::dom
