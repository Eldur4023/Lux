#pragma once
// Lux's side of .luxp: PRODUCING them.  The format, its translator and its
// verifiers live in src/luxp/ (page/luxp.hpp); Luxium vendors that directory
// and only opens .luxp files.
//
// Two ways out of Lux:
//   * lux pack page.html out.luxp   -- a static site, every linked page and
//                                      resource, compiled once;
//   * render(...)                   -- the SAME route and template answer a
//                                      .luxp to a client that sends
//                                      `Accept: application/x-luxp` (Luxium),
//                                      and HTML to every other browser.
//
// Templates stay exactly as they are: {{ }} may sit anywhere -- inside an
// attribute, a <style>, a <script>.  How the .luxp of a filled template is
// made depends on what the template allows, from cheapest to dearest:
//
//   1. A PLAN (luxp_template.cpp).  When every {{ }} and {% %} sits where the
//      page's structure does not depend on it, the template is translated ONCE
//      at compile time into a document with holes, plus its stylesheet and
//      program.  A request only evaluates the holes: the same template engine,
//      no HTML tokenizer, no CSS parser, no compiler, no compression.
//   2. Translating the filled HTML (html_to_luxp).  Correct for any template.
//      The stylesheet and the program are the same whatever the data is, so
//      they are cached by their source and compiled once; what stays per
//      request is parsing the page's HTML and encoding its tree.
//   3. HTML: a page with JavaScript (nothing here translates JavaScript), or
//      LuxScript that does not compile.

#include <lux_script/ast.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lux {
class Request;
class Response;
}

namespace lux_script {

class Value;
struct Template;
struct NativeCtx;

// Does this request ask for a .luxp?
bool wants_luxp(const lux::Request& req);

// The app's `static "..." -> "..."` mounts: <link>/@import/url() of a
// rendered page resolve through them, as the browser would request them.
// Set when a module compiles (project.cpp).  Drops what was cached from the
// previous set.
void set_luxp_statics(const std::vector<StaticMount>& mounts);

// A URL as the browser would request it (relative to the page's path) -> the
// file a static mount serves for it, never outside the mount's root.
std::optional<std::string> luxp_fetch_static(const std::string& url, const std::string& page_path);

// HTML that a template produced -> a one-page .luxp.  `url_path` is the
// request path (relative links resolve against it).  nullopt + err when the
// page cannot be one: a LuxScript error, or JavaScript (the caller then
// answers HTML).
std::optional<std::string> html_to_luxp(const std::string& html, const std::string& url_path,
                                        std::string* err);

// The LuxScript of a TEMPLATE's own <script>s, compiled for the browser at
// project compile time (TemplateCtx::check_page), so `lux --check` catches it
// like any other error.  A script that interpolates {{ }} can only be
// compiled once rendered: it is checked per request instead (a failure
// answers HTML and logs why).  One message per error: "line N: ...".
std::vector<std::string> check_page_scripts(const std::string& template_source);

// Answers `html` as text/html or as .luxp, as the request asks.  `allow_luxp`
// false: this page is known to be HTML only (do not even try to translate it).
void send_page(const lux::Request& req, lux::Response& res, const std::string& html, bool allow_luxp = true);

// ── templates compiled to .luxp (luxp_template.cpp) ─────────────────────────

// Called when a template compiles: builds its plan if it can have one and
// stores it in Template::luxp.  Never fails: no plan means translating the
// filled HTML.
void luxp_attach_plan(Template& t);

enum class LuxpOutcome {
    Sent,        // the .luxp is written; nothing else to do
    HtmlOnly,    // this page is HTML (JavaScript): render it and answer HTML, no translating
    Translate,   // no plan: render the HTML and let send_page translate it
};
// Answers a request that wants a .luxp from the template's plan.  `slots` are
// the template's data in the order of Template::names (what render_template
// takes).
LuxpOutcome luxp_respond(NativeCtx& ctx, const Template& t, const std::vector<Value>& slots);
LuxpOutcome luxp_respond_slots(NativeCtx& ctx, size_t template_index, const Value* slots, size_t n);

// lux pack: entry_html, every local page it links to and the resources they
// use -> out_path.  Lists what it wrote in `report`; false + report = why.
bool pack_site(const std::string& entry_html, const std::string& out_path, std::string* report);

} // namespace lux_script
