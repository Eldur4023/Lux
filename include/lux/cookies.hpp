#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <algorithm>

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

// RFC 3986 percent-encoding of everything outside the unreserved set
// (ALPHA / DIGIT / '-' / '_' / '.' / '~') -- the same escaping
// http.url_encode() (modules/base_modules/http.cpp) does, duplicated here in
// a few lines rather than shared, since that module is only conditionally
// compiled in (CMakeLists.txt) while this header is always available.
//
// Used to encode a cookie NAME or VALUE before it is spliced into a
// Set-Cookie header. Every byte outside the unreserved set -- notably the
// separators RFC 6265's cookie-octet forbids (';', ',', space, '"', '\\')
// and CR/LF (header injection) -- becomes a harmless "%XX" triplet instead
// of being silently dropped, which is what this replaced: stripping ';',
// ',', space and tab out of a value meant `cookie("name", "José García")`
// was stored as "JoséGarcía" with no error, and a value built from
// `x.to_json_text()` lost every comma. Percent-encoding is lossless and
// reversible (see cookie_decode(), used by parse_cookie_header() below), so
// nothing needs to change on the reading side except decoding it back.
inline std::string cookie_encode(const std::string& s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) out += static_cast<char>(c);
        else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

inline std::string cookie_decode(const std::string& s) {
    auto hex_nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_nibble(s[i + 1]), lo = hex_nibble(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// ─── build_set_cookie() ─────────────────────────────────────────────────────
//
// Serialises a Set-Cookie header value following RFC 6265.
// Name and value are percent-encoded (see cookie_encode() above) to prevent
// header injection and silent data loss, not stripped.
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
    name  = cookie_encode(name);
    value = cookie_encode(value);
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
// Percent-decodes both name and value (see cookie_decode() above), the
// inverse of the percent-encoding build_set_cookie() applies on the way out
// -- a cookie this same app set (or read back from a browser that round-
// tripped it verbatim, which every real browser does) decodes losslessly. A
// cookie from somewhere that never percent-encoded it in the first place
// (e.g. a `%` that is not a valid escape) passes through unchanged, since
// cookie_decode() only touches well-formed "%XX" triplets.
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
            std::string k = cookie_decode(std::string(pair.substr(0, eq)));
            std::string v = cookie_decode(std::string(pair.substr(eq + 1)));
            if (!k.empty()) out.emplace(std::move(k), std::move(v));
        }
        pos = end + 1;
    }
    return out;
}

} // namespace lux
