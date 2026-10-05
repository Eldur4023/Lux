#include "html/parser.hpp"
#include "html/tokenizer.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace luxium::html {

using dom::Node;
using dom::NodeType;

namespace {

constexpr std::string_view kVoidElements[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr",
};
constexpr std::string_view kHeadOnly[] = { "title", "meta", "link", "base" };

bool in_list(std::string_view name, const std::string_view* list, size_t n) {
    return std::find(list, list + n, name) != list + n;
}
bool is_void(std::string_view name) { return in_list(name, kVoidElements, std::size(kVoidElements)); }
bool is_head_only(std::string_view name) { return in_list(name, kHeadOnly, std::size(kHeadOnly)); }

// Start tags that imply an open <p> must close (subset; the real list is
// longer -- grows with the CSS milestone's layout elements).
bool closes_paragraph(std::string_view name) {
    constexpr std::string_view kList[] = {
        "address", "article", "aside", "blockquote", "details", "div", "dl",
        "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2",
        "h3", "h4", "h5", "h6", "header", "hgroup", "hr", "main", "menu",
        "nav", "ol", "p", "pre", "section", "table", "ul",
    };
    return in_list(name, kList, std::size(kList));
}

// Implied end tags: which open elements a start tag closes, nearest first.
// Only the immediate ancestors listed here are popped (no full scope logic).
std::vector<std::string_view> implied_by(std::string_view name) {
    if (name == "li") return {"li", "p"};
    if (name == "dt" || name == "dd") return {"dt", "dd", "p"};
    if (name == "tr") return {"tr", "td", "th"};
    if (name == "td" || name == "th") return {"td", "th"};
    if (name == "option") return {"option"};
    if (closes_paragraph(name)) return {"p"};
    return {};
}

struct Builder {
    std::vector<std::string> errors;
    std::vector<Node*> stack;
    Node* pending_rawtext = nullptr;  // script/style element waiting for its raw text

    explicit Builder(Node* doc) { stack.push_back(doc); }

    Node* cur() const { return stack.back(); }
    void error(int line, std::string_view msg) {
        errors.push_back("line " + std::to_string(line) + ": " + std::string(msg));
    }

    std::unique_ptr<Node> make(NodeType t, std::string name, int line) {
        auto n = std::make_unique<Node>(t);
        n->name = std::move(name);
        n->line = line;
        return n;
    }

    // Creates <html> under the document if missing; leaves it on the stack
    // until an explicit </html> (or forever -- harmless: stack unwinds into
    // the returned root).
    Node* ensure_html(int line) {
        Node* doc = stack.front();
        for (auto& c : doc->children)
            if (c->is_element("html")) {
                if (stack.size() < 2) stack.push_back(c.get());
                return c.get();
            }
        auto html = make(NodeType::Element, "html", line);
        Node* raw = &doc->append(std::move(html));
        if (stack.size() < 2) stack.push_back(raw);
        return raw;
    }

    Node* html_element(int line) { return ensure_html(line); }

    Node* find_child(Node* parent, std::string_view tag) {
        for (auto& c : parent->children)
            if (c->is_element(tag)) return c.get();
        return nullptr;
    }

    // Creates <head> under <html> (before an existing body); leaves it open.
    Node* ensure_head(int line) {
        Node* html = html_element(line);
        if (Node* h = find_child(html, "head")) {
            if (cur() == html) stack.push_back(h);
            return h;
        }
        auto head = make(NodeType::Element, "head", line);
        Node* raw = head.get();
        if (Node* body = find_child(html, "body")) {
            // insert before body
            auto& v = html->children;
            auto it = std::find_if(v.begin(), v.end(),
                                   [&](auto& c) { return c.get() == body; });
            head->parent = html;
            v.insert(it, std::move(head));
        } else {
            html->append(std::move(head));
        }
        if (cur() == html) stack.push_back(raw);
        return raw;
    }

    // Creates <body> under <html> if missing; leaves it open. Everything that
    // is not head-only goes through here, so content always lands in a body.
    Node* ensure_body(int line) {
        Node* html = html_element(line);
        if (Node* b = find_child(html, "body")) {
            if (stack.back() == html || stack.back() == find_child(html, "head"))
                stack.resize(2), stack.push_back(b);
            return b;
        }
        auto body = make(NodeType::Element, "body", line);
        Node* raw = &html->append(std::move(body));
        // close head if open, then open body
        if (stack.size() > 2 && stack.back() != html) stack.resize(2);
        if (stack.size() < 2) stack.push_back(html);
        stack.resize(2);
        stack.push_back(raw);
        return raw;
    }

    void close_implied(std::string_view tag, int line) {
        while (true) {
            Node* top = cur();
            if (stack.size() <= 2 || !top->is_element()) return;
            auto closers = implied_by(tag);
            if (std::find(closers.begin(), closers.end(), top->name) == closers.end()) return;
            (void)line;
            stack.pop_back();
        }
    }

    // Where a start tag that takes normal content should attach.
    Node* content_parent(int line) {
        if (stack.size() < 2) {  // nothing open but the document
            ensure_body(line);
            return stack.back();
        }
        return cur();
    }
};

} // namespace

ParseResult parse(std::string_view source) {
    auto tokens = tokenize(source);

    ParseResult result;
    result.root = dom::make_document();
    Builder b(result.root.get());

    for (const auto& t : tokens) {
        switch (t.type) {

        case TokType::Doctype: {
            // Attach to the document, whatever the open stack looks like.
            // Strip a leading "doctype" so the payload is just the name.
            std::string text = t.text;
            static constexpr std::string_view kPrefix = "doctype";
            if (text.size() >= kPrefix.size() &&
                std::equal(kPrefix.begin(), kPrefix.end(), text.begin(),
                           [](char a, char b) {
                               return std::tolower(static_cast<unsigned char>(a)) ==
                                      std::tolower(static_cast<unsigned char>(b));
                           }))
                text.erase(0, kPrefix.size());
            auto dt = b.make(NodeType::Doctype, "#doctype", t.line);
            dt->text = text;
            result.root->append(std::move(dt));
            break;
        }

        case TokType::Comment: {
            auto c = b.make(NodeType::Comment, "#comment", t.line);
            c->text = t.text;
            b.cur()->append(std::move(c));
            break;
        }

        case TokType::Text: {
            // Raw text (<script>/<style> contents) attaches to the pending
            // element regardless of the open stack.
            if (b.pending_rawtext) {
                auto tx = b.make(NodeType::Text, "#text", t.line);
                tx->text = t.text;
                b.pending_rawtext->append(std::move(tx));
                break;
            }
            bool blank = std::all_of(t.text.begin(), t.text.end(), [](unsigned char c) {
                return std::isspace(c);
            });
            if (b.stack.size() < 2) {
                if (blank) break;              // whitespace before <html>: dropped
                b.ensure_body(t.line);         // bare text implies body
            } else if (b.cur()->is_element("head")) {
                // whitespace between head-level tags: dropped (in-head mode);
                // real text STARTS THE BODY, like the spec's "after head".
                if (blank) break;
                b.ensure_body(t.line);
            } else if (b.cur()->is_element("html")) {
                if (!blank)
                    b.error(t.line, "text inside <head> ignored");
                break;
            }
            auto tx = b.make(NodeType::Text, "#text", t.line);
            tx->text = t.text;
            b.cur()->append(std::move(tx));
            break;
        }

        case TokType::StartTag: {
            const std::string& name = t.name;

            if (name == "html") {
                if (b.stack.size() >= 2) { b.error(t.line, "duplicate <html> ignored"); break; }
                b.ensure_html(t.line);
                break;
            }
            if (name == "head") {
                if (b.find_child(b.html_element(t.line), "head")) {
                    b.error(t.line, "duplicate <head> ignored");
                    break;
                }
                b.ensure_head(t.line);
                break;
            }
            if (name == "body") {
                Node* html = b.html_element(t.line);
                if (b.find_child(html, "body")) {
                    b.error(t.line, "duplicate <body> ignored");
                    break;
                }
                Node* body = b.ensure_body(t.line);
                body->attrs = t.attrs;   // <body style=...> conserva su estilo
                break;
            }

            if (is_head_only(name) && b.stack.size() < 3 && !b.find_child(b.html_element(t.line), "body")) {
                Node* head = b.ensure_head(t.line);
                auto el = b.make(NodeType::Element, name, t.line);
                el->attrs = t.attrs;
                if (name == "title") {
                    Node* raw = &head->append(std::move(el));  // stays open for its text
                    b.stack.push_back(raw);
                } else {
                    head->append(std::move(el));  // void-ish: no content
                }
                break;
            }

            if (name == "script" || name == "style") {
                // Before any body exists, head-level scripts live in <head>.
                Node* parent;
                if (b.stack.size() < 2 && !b.find_child(b.html_element(t.line), "body"))
                    parent = b.ensure_head(t.line);
                else
                    parent = b.content_parent(t.line);
                auto el = b.make(NodeType::Element, name, t.line);
                el->attrs = t.attrs;
                // Never pushed: the tokenizer guarantees raw text + end tag.
                // The next Text token lands here (pending_rawtext); the end
                // tag clears it.
                b.pending_rawtext = &parent->append(std::move(el));
                break;
            }

            // Primer contenido de cuerpo: el head implicito (creado por un
            // <meta>/<link> suelto) se cierra y se abre el body.  Sin esto,
            // todo el contenido aterriza dentro de <head> y la pagina sale
            // en blanco sin un solo error de parser.
            if (b.cur()->is_element("head") && !is_head_only(name))
                b.ensure_body(t.line);
            else if (b.cur()->is_element("html"))
                b.ensure_body(t.line);

            // table structure: sections close what is open inside the table; a row
            // directly in a table gets an implicit <tbody>, a cell outside a row an
            // implicit <tr> (WHATWG "in table" insertion modes, the common paths)
            auto open_implicit = [&](const char* tag) {
                auto el = b.make(NodeType::Element, tag, t.line);
                Node* raw = &b.cur()->append(std::move(el));
                if (b.stack.size() < size_t(dom::kMaxDepth)) b.stack.push_back(raw);
            };
            auto is_section = [&](Node* n) {
                return n->is_element("tbody") || n->is_element("thead") || n->is_element("tfoot");
            };
            if (name == "tbody" || name == "thead" || name == "tfoot" || name == "caption" || name == "colgroup") {
                while (b.stack.size() > 2 &&
                       (b.cur()->is_element("td") || b.cur()->is_element("th") || b.cur()->is_element("tr") ||
                        is_section(b.cur()) || b.cur()->is_element("caption") || b.cur()->is_element("colgroup")))
                    b.stack.pop_back();
            } else if (name == "tr") {
                b.close_implied("tr", t.line);
                if (b.cur()->is_element("table")) open_implicit("tbody");
            } else if (name == "td" || name == "th") {
                b.close_implied(name, t.line);
                if (b.cur()->is_element("table")) { open_implicit("tbody"); open_implicit("tr"); }
                else if (is_section(b.cur())) open_implicit("tr");
            }

            b.close_implied(name, t.line);
            Node* parent = b.content_parent(t.line);
            auto el = b.make(NodeType::Element, name, t.line);
            el->attrs = t.attrs;
            Node* raw = &parent->append(std::move(el));
            // Past kMaxDepth the element stays a leaf: what it would have
            // contained lands next to it (Chromium does the same).
            if (!t.self_closing && !is_void(name) && b.stack.size() < size_t(dom::kMaxDepth))
                b.stack.push_back(raw);
            break;
        }

        case TokType::EndTag: {
            const std::string& name = t.name;
            b.pending_rawtext = nullptr;  // </script>/</style> closes the raw run
            if (name == "br") {  // WHATWG: </br> means <br>
                Node* parent = b.content_parent(t.line);
                parent->append(b.make(NodeType::Element, "br", t.line));
                break;
            }
            // Search the open stack from the top. `idx` is the index of the
            // nearest open element with this name; resize(idx) pops through
            // and including it.
            size_t idx = b.stack.size() - 1;
            while (idx > 0 && !b.stack[idx]->is_element(name)) --idx;
            if (idx == 0) {
                // Not open anywhere (html/body at idx>0 would have matched).
                // script/style are never on the stack: their end tag always
                // lands here and is silently consumed.
                if (name != "head" && name != "script" && name != "style")
                    b.error(t.line, "stray </" + name + "> ignored");
                break;
            }
            b.stack.resize(idx);
            break;
        }
        }
    }

    return result;
}

} // namespace luxium::html
