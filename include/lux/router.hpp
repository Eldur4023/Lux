#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include "types.hpp"
#include "handler_traits.hpp"

namespace lux {

struct RouteMatch {
    bool    found   = false;
    Handler handler = nullptr;
    std::unordered_map<std::string, std::string> params;
    // True when `handler` was reached only through a CATCH-ALL, not
    // something that specifically named this request's own (method, path):
    // either a wildcard path segment (a `*` pattern segment, e.g.
    // app.any("/*", ...)), or the terminal any-METHOD fallback (a `*`
    // registered as the method, e.g. app.any("/", ...), found only because
    // nothing was registered for the request's actual method). App's own
    // static-mount gate (handle_request(), app.cpp) needs this: the Lux
    // Script engine (main.cpp) registers exactly two such catch-alls on
    // App's own router_ (any("/", ...) — a same-exact-path any-method
    // registration, needed because a *-segment wildcard structurally
    // cannot match a zero-segment path like "/" at all — and any("/*", ...)
    // for everything else) that match every path unconditionally, so
    // router_.match(...).found alone can never tell "there is a real
    // declared route here" apart from "nothing matched but the catch-all
    // still will". A concrete route -- whether App's own (health/docs/
    // metrics, each registered for one specific method on one specific
    // path) or the live module's (checked separately via
    // App::set_route_probe) -- always has this false. The one accepted
    // trade-off: a plain C++-API app that itself calls app.any(path, ...)
    // on some exact path also covered by a static mount now has the mount
    // win for GET/HEAD on that path, where round 1 of this fix had the
    // any() handler win -- an unusual combination, and a far smaller
    // regression than the one this broadening fixes (every Lux Script app
    // using a root SPA mount had its OWN real `get endpoint("/")` route
    // permanently shadowed the same way finding #3 fixed for every other
    // path, confirmed by the project's own test suite the moment this
    // broader check was added).
    bool    via_wildcard = false;
};

class Router {
public:
    Router();

    // Register any callable; HandlerTraits wraps it into Handler (Task<void>).
    template<typename F>
    void add(std::string method, std::string pattern, F&& handler) {
        auto wrapped = [h = std::forward<F>(handler)](Request& req, Response& res) mutable -> Task<void> {
            return HandlerTraits<std::decay_t<F>>::call(h, req, res);
        };
        add_internal(std::move(method), std::move(pattern), std::move(wrapped));
    }

    // head_alias: whether a HEAD request may fall back to this route's GET
    // handler (see match_recursive()'s comment on the alias itself). false
    // for ws/sse routes (build_routes(), project.cpp): both write their
    // response bytes straight to the socket via _raw_write, bypassing
    // finish_dispatch()'s HEAD body-stripping entirely, and RFC 9110's
    // "identical to GET but no body" definition does not map onto an
    // open-ended stream with no fixed content anyway (a WS upgrade IS its
    // own response with no separate body to strip; an SSE response never
    // has a knowable end).  A plain GET route needs no change: it already
    // defaults to true.
    void add_internal(std::string method, std::string pattern, Handler handler,
                      bool head_alias = true);

    RouteMatch match(const std::string& method, const std::string& path) const;

private:
    enum class NodeType { STATIC, PARAM, WILDCARD };

    struct Node {
        std::string segment;
        NodeType    type = NodeType::STATIC;
        std::unordered_map<std::string, Handler> handlers; // method -> handler
        // Set when this node's GET handler was registered with
        // head_alias=false -- see add_internal()'s comment.
        bool no_head_alias = false;
        std::vector<std::unique_ptr<Node>> children;

        Node* find_child(NodeType t, const std::string& seg = {}) const;
    };

    std::unique_ptr<Node> root_;

    static std::string normalize_pattern(const std::string& p);

    bool match_recursive(
        const Node* node,
        const std::vector<std::string>& segments,
        size_t index,
        const std::string& method,
        std::unordered_map<std::string, std::string>& params,
        Handler& out_handler,
        bool& out_via_wildcard) const;
};

} // namespace lux
