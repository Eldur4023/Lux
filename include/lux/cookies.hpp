#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <algorithm>

#include "percent_encoding.hpp"

namespace lux {

// ─── SameSite ────────────────────────────────────────────────────────────────
//
// Controls the SameSite cookie attribute (RFC 6265bis §4.1.2.7).
//
//   Strict — cookie sent only on same-site requests.  Strongest CSRF defence.
//   Lax    — cookie sent on top-level navigations and same-site requests.
//            Default for modern browsers.
//   None   — cookie sent on all cross-site requests.  REQUIRES Secure=true.
//
enum class SameSite { Strict, Lax, None };

// ─── CookieOptions ───────────────────────────────────────────────────────────
struct CookieOptions {
    std::string                path     = "/";
    std::string                domain;                 // empty → no Domain attr
    std::optional<int>         max_age;                // seconds; nullopt → session cookie
    bool                       secure    = false;      // Secure attribute
    bool                       http_only = true;       // HttpOnly attribute (JS cannot read)
    SameSite                   same_site = SameSite::Lax;
};

// ─── build_set_cookie() ─────────────────────────────────────────────────────
//
// Serialises a Set-Cookie header value following RFC 6265.
// Name and value are percent-encoded (lux::percent_encode(), shared with
// http.url_encode()) to prevent header injection and silent data loss, not
// stripped -- notably the separators RFC 6265's cookie-octet forbids (';',
// ',', space, '"', '\\') and CR/LF (header injection), which used to be
// stripped out silently: `cookie("name", "José García")` was stored as
// "JoséGarcía" with no error, and a value built from `x.to_json_text()`
// lost every comma. Percent-encoding is lossless and reversible (see
// parse_cookie_header() below), so nothing needs to change on the reading
// side except decoding it back.
// SameSite=None without Secure is auto-promoted to Secure (browsers reject it
// otherwise) and a warning is emitted to stderr.
//
inline std::string build_set_cookie(std::string name, std::string value,
                                     CookieOptions opts = {}) {
    // Path/Domain are concatenated into the Set-Cookie header below.  Without
    // sanitising them, a handler that passes user input (e.g. tenant-specific
    // path) could inject arbitrary headers via CR/LF.
    auto strip_crlf = [](std::string& s) {
        s.erase(std::remove_if(s.begin(), s.end(),
            [](char c){ return c == '\r' || c == '\n'; }), s.end());
    };
    name  = percent_encode(name);
    value = percent_encode(value);
    strip_crlf(opts.path);
    strip_crlf(opts.domain);

    // Browsers silently drop SameSite=None without Secure.  Auto-correct so the
    // cookie actually reaches the client.
    if (opts.same_site == SameSite::None && !opts.secure) {
        std::cerr << "[lux] cookie '" << name
                  << "': SameSite=None requires Secure — auto-enabling Secure.\n";
        opts.secure = true;
    }

    std::string out;
    out.reserve(name.size() + value.size() + 64);
    out += name;
    out += '=';
    out += value;

    if (!opts.path.empty()) { out += "; Path="; out += opts.path; }
    if (!opts.domain.empty()) { out += "; Domain="; out += opts.domain; }
    if (opts.max_age) {
        out += "; Max-Age=";
        out += std::to_string(*opts.max_age);
    }
    if (opts.http_only) out += "; HttpOnly";
    if (opts.secure)    out += "; Secure";
    switch (opts.same_site) {
        case SameSite::Strict: out += "; SameSite=Strict"; break;
        case SameSite::Lax:    out += "; SameSite=Lax";    break;
        case SameSite::None:   out += "; SameSite=None";   break;
    }
    return out;
}

// ─── parse_cookie_header() ──────────────────────────────────────────────────
//
// Parses an HTTP `Cookie:` request header into a name→value map.
// Tolerates leading whitespace and missing values.  Quoted values keep their
// quotes (Servlet/RFC ambiguity — the caller can strip them if needed).
// Percent-decodes both name and value (lux::percent_decode(), no '+'
// folding: that is a form/query-string convention, not a cookie one), the
// inverse of the percent-encoding build_set_cookie() applies on the way out
// -- a cookie this same app set (or read back from a browser that round-
// tripped it verbatim, which every real browser does) decodes losslessly. A
// cookie from somewhere that never percent-encoded it in the first place
// (e.g. a `%` that is not a valid escape) passes through unchanged, since
// percent_decode() only touches well-formed "%XX" triplets.
//
inline std::unordered_map<std::string, std::string>
parse_cookie_header(std::string_view header) {
    std::unordered_map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < header.size()) {
        // Skip leading whitespace
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t'))
            ++pos;
        size_t end = header.find(';', pos);
        if (end == std::string_view::npos) end = header.size();
        auto pair = header.substr(pos, end - pos);
        auto eq = pair.find('=');
        if (eq != std::string_view::npos) {
            std::string k = percent_decode(std::string(pair.substr(0, eq)), false);
            std::string v = percent_decode(std::string(pair.substr(eq + 1)), false);
            if (!k.empty()) out.emplace(std::move(k), std::move(v));
        }
        pos = end + 1;
    }
    return out;
}

} // namespace lux
