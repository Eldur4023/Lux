#pragma once
#include <string>
#include <unordered_map>

namespace lux {

// RFC 3986 percent-encoding: every byte outside the unreserved set
// (ALPHA / DIGIT / '-' / '_' / '.' / '~') becomes a "%XX" triplet. Same
// escaping curl_easy_escape()/JavaScript's encodeURIComponent() do.
inline std::string percent_encode(const std::string& s) {
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

// Percent-decodes a string. Never std::strtoul() for the hex digits: it
// accepts a leading sign/whitespace, so "%+2e"/"%-1"/"% e" would decode as
// a real byte instead of staying the literal text RFC 3986 says a
// malformed escape is -- a decoder/filter differential a path-traversal or
// auth check could be bypassed through. Always drops a decoded NUL byte
// (%00): it can desync a later C-string use, truncate a filesystem path
// after canonicalization, or slip a "\0admin" past a "!= \"admin\""
// comparison. `fold_plus` turns '+' into a space -- the
// application/x-www-form-urlencoded convention for a query string or form
// body, not for a path segment or a cookie value, which keep a literal '+'.
inline std::string percent_decode(const std::string& s, bool fold_plus) {
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
                int v = hi * 16 + lo;
                if (v != 0) out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        if (fold_plus && s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

// Parses "a=1&b=2" (a query string or an application/x-www-form-urlencoded
// body), calling on_pair(key, value) in order, decoded with '+' folded to a
// space. Empty pairs are skipped.
template <class OnPair>
inline void for_each_form_pair(const std::string& src, OnPair on_pair) {
    size_t pos = 0;
    while (pos < src.size()) {
        size_t amp = src.find('&', pos);
        if (amp == std::string::npos) amp = src.size();
        size_t eq = src.find('=', pos);
        if (eq < amp)
            on_pair(percent_decode(src.substr(pos, eq - pos), true),
                    percent_decode(src.substr(eq + 1, amp - eq - 1), true));
        else if (amp > pos)
            on_pair(percent_decode(src.substr(pos, amp - pos), true), std::string());
        pos = amp + 1;
    }
}

// The same into a map; a later duplicate key wins.
inline void parse_form_encoded(const std::string& src,
                               std::unordered_map<std::string, std::string>& out) {
    for_each_form_pair(src, [&](std::string k, std::string v) { out[std::move(k)] = std::move(v); });
}

} // namespace lux
