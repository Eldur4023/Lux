#include "html/tokenizer.hpp"

#include <algorithm>
#include <cctype>

namespace luxium::html {

#include "html/entities.inc"

namespace {

// Entity decoding for text and attribute values (structure is already
// tokenized, so a decoded '<' cannot open a tag).  Raw-text runs
// (<script>/<style>) skip this: Lux code with an & stays as written.
// Unknown entities keep their literal spelling -- the recovery policy of
// the whole tokenizer.
void decode_entities(std::string& s) {
    if (s.find('&') == std::string::npos) return;
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') { out += s[i++]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 33) { out += s[i++]; continue; }
        std::string name = s.substr(i + 1, semi - i - 1);
        if (!name.empty() && name[0] != '#') {
            // named character reference: binary search in the generated WHATWG table
            size_t lo = 0, hi = sizeof(kEntities) / sizeof(kEntities[0]);
            const char* found = nullptr;
            while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                const int cmp = name.compare(kEntities[mid].name);
                if (cmp == 0) { found = kEntities[mid].utf8; break; }
                if (cmp < 0) hi = mid; else lo = mid + 1;
            }
            if (found) out += found;
            else out += s.substr(i, semi - i + 1);   // unknown entity: literal
        }
        else if (!name.empty() && name[0] == '#') {
            long cp = -1;
            try {
                if (name.size() > 1 && (name[1] == 'x' || name[1] == 'X'))
                    cp = std::stol(name.substr(2), nullptr, 16);
                else
                    cp = std::stol(name.substr(1));
            } catch (...) {}
            if (cp > 0 && cp <= 0x10FFFF) {
                // UTF-8 encode
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            } else {
                out += s.substr(i, semi - i + 1);   // keep the literal
            }
        }
        else out += s.substr(i, semi - i + 1);      // unknown entity: literal
        i = semi + 1;
    }
    s = std::move(out);
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_alnum(char c) {
    return is_alpha(c) || (c >= '0' && c <= '9');
}
bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}
std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
int count_lines(std::string_view s) {
    return static_cast<int>(std::count(s.begin(), s.end(), '\n'));
}

constexpr size_t kNpos = std::string_view::npos;

// Case-insensitive find of `needle` starting at `from`.
size_t ifind(std::string_view s, size_t from, std::string_view needle) {
    if (needle.empty() || s.size() < needle.size()) return kNpos;
    for (size_t i = from; i + needle.size() <= s.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() &&
               std::tolower(static_cast<unsigned char>(s[i + j])) ==
                   std::tolower(static_cast<unsigned char>(needle[j])))
            ++j;
        if (j == needle.size()) return i;
    }
    return kNpos;
}

} // namespace

std::vector<Token> tokenize(std::string_view src) {
    std::vector<Token> out;
    const size_t n = src.size();
    size_t i = 0;
    int line = 1;

    std::string text;
    int text_line = 1;
    bool raw_mode = false;   // inside <script>/<style>: entities stay raw
    auto flush_text = [&] {
        if (!text.empty()) {
            Token t;
            t.type = TokType::Text;
            t.text = std::move(text);
            if (!raw_mode) decode_entities(t.text);
            t.line = text_line;
            text.clear();
            out.push_back(std::move(t));
        }
    };

    while (i < n) {
        char c = src[i];

        // Plain data: everything up to the next '<'.
        if (c != '<') {
            if (text.empty()) text_line = line;
            if (c == '\n') ++line;
            text.push_back(c);
            ++i;
            continue;
        }

        // Comments: <!-- ... -->
        if (src.compare(i, 4, "<!--") == 0) {
            flush_text();
            size_t end = src.find("-->", i + 4);
            size_t stop = (end == kNpos) ? n : end;
            Token t;
            t.type  = TokType::Comment;
            t.text  = std::string(src.substr(i + 4, stop - (i + 4)));
            t.line  = line;
            line   += count_lines(src.substr(i, (end == kNpos ? n : end + 3) - i));
            i       = (end == kNpos) ? n : end + 3;
            out.push_back(std::move(t));
            continue;
        }

        // Doctype / bogus markup: <! ... >
        if (src.compare(i, 2, "<!") == 0) {
            flush_text();
            size_t end = src.find('>', i);
            size_t stop = (end == kNpos) ? n : end;
            Token t;
            t.type = TokType::Doctype;
            t.text = std::string(src.substr(i + 2, stop - (i + 2)));
            t.line = line;
            line  += count_lines(src.substr(i, (end == kNpos ? n : end + 1) - i));
            i      = (end == kNpos) ? n : end + 1;
            out.push_back(std::move(t));
            continue;
        }

        // End tag: </name ...>
        if (src.compare(i, 2, "</") == 0 && i + 2 < n && is_alpha(src[i + 2])) {
            flush_text();
            i += 2;
            size_t name_start = i;
            while (i < n && (is_alnum(src[i]) || src[i] == '-')) ++i;
            Token t;
            t.type = TokType::EndTag;
            t.name = lower(src.substr(name_start, i - name_start));
            t.line = line;
            // Skip anything up to '>' (stray attributes on end tags).
            while (i < n && src[i] != '>') {
                if (src[i] == '\n') ++line;
                ++i;
            }
            if (i < n) ++i;  // consume '>'
            line += count_lines(src.substr(name_start, i - name_start));
            out.push_back(std::move(t));
            continue;
        }

        // Start tag: <name attr=...>
        if (i + 1 < n && is_alpha(src[i + 1])) {
            flush_text();
            size_t start = i;
            ++i;
            size_t name_start = i;
            while (i < n && (is_alnum(src[i]) || src[i] == '-')) ++i;
            Token t;
            t.type = TokType::StartTag;
            t.name = lower(src.substr(name_start, i - name_start));
            t.line = line;

            bool rawtext_done = false;
            while (i < n && !rawtext_done) {
                while (i < n && is_ws(src[i])) {
                    if (src[i] == '\n') ++line;
                    ++i;
                }
                if (i >= n) break;
                if (src[i] == '>') { ++i; break; }
                if (src[i] == '/') {
                    ++i;
                    if (i < n && src[i] == '>') { t.self_closing = true; ++i; break; }
                    continue;  // stray '/' inside the tag
                }

                // Attribute name: up to ws, '=', '>' or '/'.
                size_t astart = i;
                while (i < n && !is_ws(src[i]) && src[i] != '=' && src[i] != '>' && src[i] != '/')
                    ++i;
                std::string aname = lower(src.substr(astart, i - astart));
                std::string avalue;
                size_t look = i;
                while (look < n && is_ws(src[look])) ++look;
                if (look < n && src[look] == '=') {
                    i = look + 1;
                    while (i < n && is_ws(src[i])) {
                        if (src[i] == '\n') ++line;
                        ++i;
                    }
                    if (i < n && (src[i] == '"' || src[i] == '\'')) {
                        char q = src[i++];
                        size_t vstart = i;
                        while (i < n && src[i] != q) {
                            if (src[i] == '\n') ++line;
                            ++i;
                        }
                        avalue = std::string(src.substr(vstart, i - vstart));
                        if (i < n) ++i;  // closing quote
                    } else {
                        size_t vstart = i;
                        while (i < n && !is_ws(src[i]) && src[i] != '>')
                            ++i;
                        avalue = std::string(src.substr(vstart, i - vstart));
                    }
                }
                decode_entities(avalue);
                bool dup = std::any_of(t.attrs.begin(), t.attrs.end(),
                                       [&](const auto& kv) { return kv.first == aname; });
                if (!aname.empty() && !dup) t.attrs.emplace_back(std::move(aname), std::move(avalue));
            }
            line += count_lines(src.substr(start, i - start));
            const std::string tag_name = t.name;  // captured before the move below
            const bool is_rawtext = (tag_name == "script" || tag_name == "style");
            out.push_back(std::move(t));

            // Raw-text mode: <script>/<style> swallow everything up to their
            // end tag as one Text token; the end tag itself is consumed by the
            // normal loop above on the next iteration.
            if (!t.self_closing && is_rawtext) {
                std::string closer = "</" + tag_name;
                raw_mode = true;
                size_t r = i;
                while (true) {
                    size_t hit = ifind(src, r, closer);
                    if (hit == kNpos) {
                        Token rt;
                        rt.type = TokType::Text;
                        rt.text = std::string(src.substr(i));
                        rt.line = line;
                        line   += count_lines(rt.text);
                        i       = n;
                        out.push_back(std::move(rt));
                        break;
                    }
                    size_t after = hit + closer.size();
                    if (after >= n || is_ws(src[after]) || src[after] == '/' || src[after] == '>') {
                        Token rt;
                        rt.type = TokType::Text;
                        rt.text = std::string(src.substr(i, hit - i));
                        rt.line = line;
                        line   += count_lines(rt.text);
                        i       = hit;
                        out.push_back(std::move(rt));
                        break;
                    }
                    r = hit + 1;  // e.g. "</scriptx" -- keep looking
                }
                raw_mode = false;
            }
            continue;
        }

        // Lone '<' is text.
        if (text.empty()) text_line = line;
        text.push_back('<');
        ++i;
    }

    flush_text();
    return out;
}

} // namespace luxium::html
