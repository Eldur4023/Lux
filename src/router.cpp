#include "../include/lux/router.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace lux {

// ─────────────────────────────────────────────────────────────────────────────
// Node Helpers
// ─────────────────────────────────────────────────────────────────────────────

Router::Node* Router::Node::find_static_child(const std::string& seg) const {
    for (auto& child : children) {
        if (child->type == NodeType::STATIC && child->segment == seg) return child.get();
    }
    return nullptr;
}

Router::Node* Router::Node::find_param_child() const {
    for (auto& child : children) {
        if (child->type == NodeType::PARAM) return child.get();
    }
    return nullptr;
}

Router::Node* Router::Node::find_wildcard_child() const {
    for (auto& child : children) {
        if (child->type == NodeType::WILDCARD) return child.get();
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Router
// ─────────────────────────────────────────────────────────────────────────────

Router::Router() : root_(std::make_unique<Node>()) {
    root_->segment = "";
    root_->type = NodeType::STATIC;
}

std::string Router::normalize_pattern(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '{') {
            out += ':';
            ++i;
            bool closed = false;
            while (i < p.size()) {
                if (p[i] == '}') { closed = true; break; }
                out += p[i++];
            }
            if (!closed) {
                // Unclosed brace: the pattern is malformed.  Better to fail
                // loudly at registration than silently match weird URLs.
                throw std::invalid_argument(
                    "lux::Router: unterminated '{' in pattern: " + p);
            }
        } else {
            out += p[i];
        }
    }
    if (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

static std::vector<std::string> split_path(const std::string& s) {
    std::vector<std::string> parts;
    std::istringstream ss(s);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (!seg.empty()) parts.push_back(seg);
    }
    return parts;
}

// Percent-decodes one path segment bound to a `:param`.  Static segments are
// matched as literal text on purpose (that is what makes route registration
// predictable), so this is applied ONLY to the value handed to a handler —
// never to the segments used for matching, which stay exactly as received.
//
// Without it, `GET /echo/%34%32` (which is "42") bound `id` to the literal
// text "%34%32" instead of "42": every consumer downstream (int/float
// coercion, a string param compared against a literal) saw percent-escapes
// no client-facing route documents, while query() and form() — which already
// decode — did not have the same problem.  No '+' -> ' ' folding here: that
// convention belongs to application/x-www-form-urlencoded bodies and query
// strings, not path segments.
// Strict hex-nibble check — see the identical helper's comment in
// http_connection.cpp's url_decode() for why std::strtoul() is wrong here:
// it accepts a leading sign/whitespace, so "%+2e" or "%-1" decoded as a
// real byte instead of staying the literal text RFC 3986 says it is.
static inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string url_decode_segment(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_nibble(s[i + 1]), lo = hex_nibble(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                int v = hi * 16 + lo;
                // Drop %00: a NUL bound into a param could desync a later
                // C-string-based use of it from what the router matched on.
                if (v != 0) out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

void Router::add_internal(std::string method, std::string pattern, Handler handler,
                          bool head_alias) {
    std::transform(method.begin(), method.end(), method.begin(), ::toupper);
    std::string norm = normalize_pattern(pattern);
    auto segments = split_path(norm);

    Node* curr = root_.get();
    for (const auto& seg : segments) {
        NodeType type = NodeType::STATIC;
        std::string name = seg;

        if (seg == "*") {
            type = NodeType::WILDCARD;
        } else if (seg[0] == ':') {
            type = NodeType::PARAM;
            name = seg.substr(1);
        }

        Node* next = nullptr;
        if (type == NodeType::STATIC) next = curr->find_static_child(name);
        else if (type == NodeType::PARAM) next = curr->find_param_child();
        else next = curr->find_wildcard_child();

        if (!next) {
            auto node = std::make_unique<Node>();
            node->segment = name;
            node->type = type;
            next = node.get();
            curr->children.push_back(std::move(node));
        }
        curr = next;
    }
    curr->handlers[method] = std::move(handler);
    if (method == "GET" && !head_alias) curr->no_head_alias = true;
}

RouteMatch Router::match(const std::string& method, const std::string& path) const {
    std::string clean_path = path;
    if (clean_path.size() > 1 && clean_path.back() == '/') clean_path.pop_back();

    auto segments = split_path(clean_path);
    std::unordered_map<std::string, std::string> params;
    Handler handler = nullptr;
    bool    via_wildcard = false;

    if (match_recursive(root_.get(), segments, 0, method, params, handler, via_wildcard)) {
        return {true, handler, std::move(params), via_wildcard};
    }
    return {false, nullptr, {}, false};
}

bool Router::match_recursive(
    const Node* node,
    const std::vector<std::string>& segments,
    size_t index,
    const std::string& method,
    std::unordered_map<std::string, std::string>& params,
    Handler& out_handler,
    bool& out_via_wildcard) const
{
    // Terminal case
    if (index == segments.size()) {
        auto it = node->handlers.find(method);
        if (it != node->handlers.end()) {
            out_handler = it->second;
            return true;
        }
        // RFC 9110 §9.3.2: HEAD is defined identically to GET, minus the
        // response body. A route registered with `get endpoint(...)`
        // should answer HEAD for free, the way every other web server
        // handles it, instead of 404ing until the app also declares a
        // separate HEAD route that does the exact same thing. The body
        // itself is stripped afterward, in HttpConnection::finish_dispatch —
        // this only has to find the right handler to run.
        if (method == "HEAD" && !node->no_head_alias) {
            auto git = node->handlers.find("GET");
            if (git != node->handlers.end()) {
                out_handler = git->second;
                return true;
            }
        }
        // Check wildcard handle anyway (*)
        it = node->handlers.find("*");
        if (it != node->handlers.end()) {
            out_handler = it->second;
            // This is an ANY-METHOD fallback (app.any(path, ...)), not a
            // match against the method the caller actually asked for --
            // the same "this claims the path only because nothing more
            // specific does" situation a wildcard PATH SEGMENT is in, and
            // App's static-mount gate (handle_request(), app.cpp) needs to
            // treat it identically: main.cpp registers exactly this shape
            // at the exact path "/" (app.any("/", dispatch), the Lux
            // Script engine's root-path catch-all -- "/*" alone cannot
            // reach a zero-segment path, see find_wildcard_child()'s
            // comment on the non-terminal branch below, hence a second,
            // separate any() just for "/"). Without this, `via_wildcard`
            // caught the "/*" catch-all but not this one, and a root SPA
            // mount's own real `get endpoint("/")` route was reported as
            // "found" here anyway, permanently shadowed by the SAME static
            // mount finding #3 fixed for every OTHER path — confirmed
            // against the real test suite the moment this fix landed.
            out_via_wildcard = true;
            return true;
        }
        return false;
    }

    const std::string& seg = segments[index];

    // 1. Try static
    Node* next = node->find_static_child(seg);
    if (next && match_recursive(next, segments, index + 1, method, params, out_handler, out_via_wildcard)) {
        return true;
    }

    // 2. Try param
    next = node->find_param_child();
    if (next) {
        params[next->segment] = url_decode_segment(seg);
        if (match_recursive(next, segments, index + 1, method, params, out_handler, out_via_wildcard)) {
            return true;
        }
        params.erase(next->segment); // backtrack
    }

    // 3. Try wildcard
    next = node->find_wildcard_child();
    if (next) {
        // Wildcard matches EVERYTHING remaining
        auto it = next->handlers.find(method);
        if (it == next->handlers.end()) it = next->handlers.find("*");

        if (it != next->handlers.end()) {
            out_handler = it->second;
            out_via_wildcard = true;
            // Build the rest of the path for the wildcard if needed?
            // Usually wildcard just captures the rest.
            return true;
        }
    }

    return false;
}

} // namespace lux
