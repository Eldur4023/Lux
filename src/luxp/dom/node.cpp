#include "dom/node.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace luxium::dom {

Node& Node::append(std::unique_ptr<Node> child) {
    child->parent = this;
    children.push_back(std::move(child));
    return *children.back();
}

bool Node::is_element(std::string_view tag) const {
    return type == NodeType::Element && name == tag;
}

std::string Node::attr_or(std::string_view key, std::string_view fallback) const {
    for (const auto& [k, v] : attrs)
        if (k == key) return v;
    return std::string(fallback);
}

bool Node::has_attr(std::string_view key) const {
    return std::any_of(attrs.begin(), attrs.end(),
                       [&](const auto& kv) { return kv.first == key; });
}

void Node::find_all(std::string_view tag, std::vector<Node*>& out) {
    for (auto& c : children) {
        if (c->is_element(tag)) out.push_back(c.get());
        c->find_all(tag, out);
    }
}

std::vector<Node*> Node::find_all(std::string_view tag) {
    std::vector<Node*> out;
    find_all(tag, out);
    return out;
}

Node* Node::find_first(std::string_view tag) {
    for (auto& c : children) {
        if (c->is_element(tag)) return c.get();
        if (Node* hit = c->find_first(tag)) return hit;
    }
    return nullptr;
}

std::string Node::text_content() const {
    std::string out;
    for (auto& c : children) {
        if (c->type == NodeType::Text)
            out += c->text;
        else
            out += c->text_content();
    }
    return out;
}

// --- debug dump -----------------------------------------------------------

static std::string collapse(std::string_view s) {
    std::string out;
    bool pending_space = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out.push_back(' ');
        pending_space = false;
        out.push_back(c);
    }
    if (out.size() > 48) out = out.substr(0, 45) + "...";
    return out;
}

static void print_attrs(const Node& n) {
    for (const auto& [k, v] : n.attrs) {
        std::printf(" %s=\"%s\"", k.c_str(), v.c_str());
    }
}

void Node::print(int depth) const {
    std::string indent(static_cast<size_t>(depth) * 2, ' ');
    switch (type) {
    case NodeType::Document:
        std::printf("%s#document\n", indent.c_str());
        break;
    case NodeType::Doctype:
        std::printf("%s<!DOCTYPE%s>\n", indent.c_str(), text.c_str());
        break;
    case NodeType::Element: {
        std::printf("%s%s", indent.c_str(), name.c_str());
        print_attrs(*this);
        if (line > 0) std::printf("   [line %d]", line);
        std::printf("\n");
        break;
    }
    case NodeType::Text:
        if (std::all_of(text.begin(), text.end(), [](unsigned char c) {
                return std::isspace(c);
            }))
            return;  // whitespace nodes stay in the tree (M1 inline layout
                     // needs them) but do not clutter the dump
        std::printf("%s\"%s\"\n", indent.c_str(), collapse(text).c_str());
        return;
    case NodeType::Comment:
        std::printf("%s<!-- %s -->\n", indent.c_str(), collapse(text).c_str());
        return;
    }
    for (auto& c : children) c->print(depth + 1);
}

std::unique_ptr<Node> make_document() {
    auto doc = std::make_unique<Node>(NodeType::Document);
    doc->name = "#document";
    return doc;
}

int depth_of(const Node* n) {
    int d = 0;
    for (; n && n->parent; n = n->parent) ++d;
    return d;
}

int subtree_height(const Node& n) {
    int h = 0;
    for (const auto& c : n.children) h = std::max(h, subtree_height(*c));
    return h + 1;
}

} // namespace luxium::dom
