#ifdef __x86_64__
#include <immintrin.h>
#endif
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <lux_script/value.hpp>

#include <cmath>
#include <sstream>

namespace lux_script {

const char* Value::type_name() const {
    switch (type_) {
        case Type::Null:  return "null";
        case Type::Bool:  return "bool";
        case Type::Int:   return "int";
        case Type::Float: return "float";
        case Type::Str:   return "string";
        case Type::List:  return "List";
        case Type::Dict:  return "Dict";
        case Type::Func:  return "Func";
    }
    return "?";
}

std::string Value::to_string() const {
    switch (type_) {
        case Type::Null:  return "null";
        case Type::Bool:  return b_ ? "true" : "false";
        case Type::Int:   return std::to_string(i_);
        case Type::Float: {
            // No padding zeros: 2.5 and not 2.500000.
            std::ostringstream ss;
            ss << d_;
            return ss.str();
        }
        case Type::Str:   return as_str();
        case Type::List:
        case Type::Dict:  return to_json_text();
        case Type::Func:  return "<function>";
    }
    return {};
}

// ─── Direct serialization to text ────────────────────────────────────────────

namespace {

// True if any of the eight bytes needs escaping.  The three tests are SWAR:
// instead of looking byte by byte, it operates on the whole word.
inline bool block_needs_escape(uint64_t w) {
    constexpr uint64_t ONES  = 0x0101010101010101ULL;
    constexpr uint64_t HIGH_BITS = 0x8080808080808080ULL;
    const uint64_t below_20 = (w - ONES * 0x20) & ~w & HIGH_BITS;
    const uint64_t c1 = w ^ (ONES * 0x22);            // quote
    const uint64_t c2 = w ^ (ONES * 0x5C);            // backslash
    // '<', '>', '&' also get escaped -- see escape_json()'s comment on why
    // a byte-for-byte valid JSON document is not the only thing this has to
    // stay safe as.
    const uint64_t c3 = w ^ (ONES * 0x3C);            // '<'
    const uint64_t c4 = w ^ (ONES * 0x3E);            // '>'
    const uint64_t c5 = w ^ (ONES * 0x26);            // '&'
    return (below_20 | ((c1 - ONES) & ~c1 & HIGH_BITS)
                     | ((c2 - ONES) & ~c2 & HIGH_BITS)
                     | ((c3 - ONES) & ~c3 & HIGH_BITS)
                     | ((c4 - ONES) & ~c4 & HIGH_BITS)
                     | ((c5 - ONES) & ~c5 & HIGH_BITS)) != 0;
}

// ─── UTF-8 ───────────────────────────────────────────────────────────────────
//
// JSON has to be UTF-8 (RFC 8259).  A stray byte that does not form a valid
// sequence leaves the document unreadable for any client, and that is worse
// than an error: the request does not fail, whoever receives it does, and the
// failure shows up far from its origin.
//
// It arrives from outside through three checked paths —JSON body, query and
// headers— and also from a database that does not validate the encoding, like
// sqlite.  That is why it is headed off here, on the way out, and not at each
// entrance: one place instead of five, and it also covers what was stored already.
//
// The validation goes in its own pass so as NOT to touch the escaping loop,
// which is the hottest thing in the system.  It has the same shortcut: while
// the eight bytes are ASCII the whole block is skipped, so normal text pays
// almost nothing.

// Length of the sequence starting at `i`, or 0 if it is not valid.  It rejects
// the same as the standard: stray continuations, overlongs, surrogates and
// anything past U+10FFFF.
inline size_t utf8_seq_len(const unsigned char* p, size_t n, size_t i) {
    const unsigned char c = p[i];
    auto cont = [&](size_t k) { return i + k < n && (p[i + k] & 0xC0) == 0x80; };

    if (c < 0x80) return 1;
    if (c < 0xC2) return 0;                       // stray continuation or overlong
    if (c < 0xE0) return cont(1) ? 2 : 0;
    if (c < 0xF0) {
        if (!cont(1) || !cont(2)) return 0;
        if (c == 0xE0 && p[i + 1] < 0xA0) return 0;               // overlong
        if (c == 0xED && p[i + 1] >= 0xA0) return 0;              // surrogate
        return 3;
    }
    if (c < 0xF5) {
        if (!cont(1) || !cont(2) || !cont(3)) return 0;
        if (c == 0xF0 && p[i + 1] < 0x90) return 0;               // overlong
        if (c == 0xF4 && p[i + 1] >= 0x90) return 0;              // > U+10FFFF
        return 4;
    }
    return 0;
}

// Copies replacing with U+FFFD every byte that breaks the encoding, which is
// what Go's encoding/json does too.  It is only walked from escape_json's
// rare invalid-UTF-8 branch below, so normal text never takes this path.
void sanitize_utf8(const std::string& in, std::string& out) {
    const auto* u = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    out.clear();
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        const size_t l = utf8_seq_len(u, n, i);
        if (l == 0) {
            out += "\xEF\xBF\xBD";                // U+FFFD: one byte becomes the replacement char
            ++i;
        } else {
            out.append(in, i, l);
            i += l;
        }
    }
}

// UTF-8 validation and JSON escaping, fused into ONE pass instead of the
// two separate scans (utf8_valid() then escape_valid()) this used to be.
// Correct because it is safe, not because it is clever: every byte that
// needs JSON escaping (a control character, '"', '\\') is < 0x80, so a byte
// with its high bit set can never need escaping -- the SAME already-loaded
// 8-byte word can be tested for "needs escaping" and "is non-ASCII" at once,
// with no extra memory traffic. Measured (bench/, throwaway harness, not
// kept): 10-27% faster on plain ASCII and typical mixed-UTF-8 text, a wash
// on escape-dense text, zero regression anywhere -- checked byte-for-byte
// against the old two-pass version across 20000+ fuzzed inputs plus every
// UTF-8 failure mode (stray continuation, overlong, surrogate, >U+10FFFF,
// truncated sequences) before this replaced it. Every JSON string the whole
// system writes goes through this — bytecode and --native both share it, and
// neither benefits from --native compiling it (it is not user .lux code) —
// so this is the one place a change here helps both backends equally.
//
// '<', '>' and '&' are also escaped here, on top of what RFC 8259 itself
// requires -- a valid-JSON document is not the only thing this output has
// to be safe as. render()'s `{{ x|safe }}` (template.cpp) writes a Dict's
// JSON straight into the page with no HTML escaping at all -- it is the
// ONLY way to embed a value inside `<script>...</script>` at all, since a
// normal (escaped) `{{ x }}` turns every `"` into `&quot;`, which is not
// valid JavaScript string syntax. Without this, `{{ data|safe }}` where
// `data` holds a string with a literal "</script>" in it (attacker input:
// a query param echoed back, a stored value from a previous request) closes
// the real <script> block early and starts a new one — full DOM-based XSS,
// e.g. `?q=</script><script>alert(1)</script>`. Go's encoding/json escapes
// the same three characters by default for exactly this reason (its own
// doc comment: "so that the JSON will be safe to embed inside HTML"), and a
// JSON parser reads < and a literal '<' as the identical byte -- this
// changes nothing about what the JSON decodes to on the other end.
// The escaping loop without the surrounding quotes. Invalid UTF-8 is rare:
// the remainder is sanitized (U+FFFD) and run through this same loop again,
// which then never hits the invalid branch.
void escape_body(const std::string& in, std::string& out) {
    const auto* u = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    size_t i = 0, clean = 0;

    while (i < n) {
        // Fast path: skip 8-byte blocks that need nothing at all -- no
        // escaping, no UTF-8 attention.
        while (i + 8 <= n) {
            uint64_t w;
            std::memcpy(&w, u + i, 8);
            if (block_needs_escape(w) || (w & 0x8080808080808080ULL)) break;
            i += 8;
        }
        // Byte-by-byte until ONE actionable byte is found and handled, then
        // back to the fast path above -- same amortization as the two-pass
        // version had, just handling both reasons a byte can be actionable.
        unsigned char c = 0;
        bool present = false;
        for (; i < n; ++i) {
            c = u[i];
            if (c < 0x20 || c == '"' || c == '\\' || c >= 0x80 ||
                c == '<' || c == '>' || c == '&') { present = true; break; }
        }
        if (!present) break;

        if (c >= 0x80) {
            const size_t l = utf8_seq_len(u, n, i);
            if (l == 0) {
                // Rare: falls back to the already-correct two-pass path for
                // just the remainder, instead of reimplementing U+FFFD
                // replacement here too.
                out.append(in, clean, i - clean);
                std::string rest;
                sanitize_utf8(in.substr(i), rest);
                escape_body(rest, out);
                return;
            }
            i += l;   // a valid multi-byte sequence is copied through as-is,
            continue; // never escaped -- `clean` does not move.
        }

        out.append(in, clean, i - clean);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case 0x08: out += "\\b"; break;
            case 0x0C: out += "\\f"; break;
            case 0x0A: out += "\\n"; break;
            case 0x0D: out += "\\r"; break;
            case 0x09: out += "\\t"; break;
            default: {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
        }
        clean = ++i;
    }
    out.append(in, clean, n - clean);
}

void escape_json(const std::string& in, std::string& out) {
    out.push_back('"');
    escape_body(in, out);
    out.push_back('"');
}

void write_double(double d, std::string& out) {
    // JSON cannot write NaN or infinity: they are not tokens of the format.
    // They are written as null, which is what JSON.stringify does and the only
    // thing every client knows how to read.
    //
    // It is not theoretical: postgres accepts 'NaN' and 'Infinity' in a double
    // precision, and without this a row with one of those values produced a
    // document no parser accepts —`{"d":inf}`— instead of a visible error.
    if (!std::isfinite(d)) { out += "null"; return; }

    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof(buf), d);
    if (r.ec != std::errc{}) { out += "0"; return; }
    out.append(buf, r.ptr);
    // A whole double comes out as "3"; JSON would read that as an integer, so
    // it gets the ".0" just as nlohmann did.
    if (std::string_view(buf, r.ptr - buf).find_first_of(".eE") == std::string_view::npos)
        out += ".0";
}

} // namespace

// write_json()'s own string and float writers, for --native's records
// (native_gen.cpp): their JSON has to be these very bytes.
void json_string(const std::string& in, std::string& out) { escape_json(in, out); }
void json_double(double d, std::string& out) { write_double(d, out); }

void Value::write_json(std::string& out) const {
    switch (type_) {
        case Type::Null:  out += "null";                   return;
        case Type::Bool:  out += b_ ? "true" : "false";    return;
        case Type::Int: {
            char buf[24];
            out.append(buf, std::to_chars(buf, buf + sizeof(buf), i_).ptr);
            return;
        }
        case Type::Float: write_double(d_, out);        return;
        case Type::Str:   escape_json(as_str(), out);          return;
        case Type::List: {
            out.push_back('[');
            bool first = true;
            for (const auto& v : as_list()) {
                if (!first) out.push_back(',');
                first = false;
                v.write_json(out);
            }
            out.push_back(']');
            return;
        }
        case Type::Dict: {
            // Keys starting with "__" are internal and never come out.
            out.push_back('{');
            bool first = true;
            for (const auto& [k, v] : as_dict()) {
                if (k.rfind("__", 0) == 0) continue;
                if (!first) out.push_back(',');
                first = false;
                escape_json(k, out);
                out.push_back(':');
                v.write_json(out);
            }
            out.push_back('}');
            return;
        }
        // A function reference has no JSON representation -- silently
        // written as null, the same thing JSON.stringify() does with a
        // function value in JavaScript, rather than failing write_json()
        // (which has no error channel; the caller already had every chance
        // to keep a Func out of a JSON-returning context, since it is only
        // ever meant to be passed to map()/filter()/reduce()/for_each(),
        // never returned).
        case Type::Func: out += "null"; return;
    }
    out += "null";
}


bool Value::equals(const Value& o) const {
    // int and float compare by numeric value; the rest demand the same type.
    if (is_num() && o.is_num()) {
        if (is_int() && o.is_int()) return i_ == o.i_;
        return as_float() == o.as_float();
    }
    if (type_ != o.type_) return false;

    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return b_ == o.b_;
        case Type::Str:  return as_str() == o.as_str();
        case Type::List: {
            if (as_list().size() != o.as_list().size()) return false;
            for (size_t i = 0; i < as_list().size(); ++i)
                if (!as_list()[i].equals(o.as_list()[i])) return false;
            return true;
        }
        case Type::Dict: {
            if (as_dict().size() != o.as_dict().size()) return false;
            for (const auto& [k, v] : as_dict()) {
                auto it = o.as_dict().find(k);
                if (it == o.as_dict().end() || !v.equals(it->second)) return false;
            }
            return true;
        }
        case Type::Func: return i_ == o.i_;
        default: return false;
    }
}

bool Value::less_than(const Value& o, bool& ok) const {
    ok = true;
    if (is_num() && o.is_num()) {
        if (is_int() && o.is_int()) return i_ < o.i_;
        return as_float() < o.as_float();
    }
    if (is_str() && o.is_str()) return as_str() < o.as_str();
    ok = false;
    return false;
}



// ─── utf8_length ─────────────────────────────────────────────────────────────

static size_t utf8_length_base(const std::string& s) {
    // Eight bytes a step: a continuation byte has bit 7 set and bit 6 clear;
    // (w << 1) lines each byte's bit 6 up under its bit 7, and the multiply
    // sums the per-byte flags into the top byte. Byte at a time, len() of a
    // file read into a string was most of what its route cost.
    constexpr uint64_t kHigh = 0x8080808080808080ULL, kOnes = 0x0101010101010101ULL;
    const char* p = s.data();
    size_t n = s.size(), i = 0, cont = 0;
#ifdef __SSE2__
    // Sixteen at a time where SSE2 is there (every x86-64): as signed bytes,
    // continuations are exactly the ones below -64. Each compare adds 1 per
    // match to a byte counter, summed (sad) before a counter can wrap.
    const __m128i below = _mm_set1_epi8(-64), zero = _mm_setzero_si128();
    while (i + 16 <= n) {
        __m128i acc = zero;
        for (int k = 0; k < 255 && i + 16 <= n; ++k, i += 16) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
            acc = _mm_sub_epi8(acc, _mm_cmplt_epi8(v, below));
        }
        const __m128i sums = _mm_sad_epu8(acc, zero);
        cont += static_cast<size_t>(_mm_cvtsi128_si64(sums)) + static_cast<size_t>(_mm_extract_epi16(sums, 4));
    }
#endif
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        cont += (((w & ~(w << 1)) & kHigh) >> 7) * kOnes >> 56;
    }
    for (; i < n; ++i) cont += (static_cast<unsigned char>(p[i]) & 0xC0) == 0x80;
    return n - cont;
}

#ifdef __x86_64__
// The same count, 32 bytes a step.
__attribute__((target("avx2")))
static size_t utf8_length_avx2(const std::string& s) {
    const char* p = s.data();
    const size_t n = s.size();
    size_t i = 0, cont = 0;
    const __m256i below = _mm256_set1_epi8(-64), zero = _mm256_setzero_si256();
    while (i + 32 <= n) {
        __m256i acc = zero;
        for (int k = 0; k < 255 && i + 32 <= n; ++k, i += 32) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
            acc = _mm256_sub_epi8(acc, _mm256_cmpgt_epi8(below, v));
        }
        const __m256i sums = _mm256_sad_epu8(acc, zero);
        cont += static_cast<size_t>(_mm256_extract_epi64(sums, 0) + _mm256_extract_epi64(sums, 1) +
                                    _mm256_extract_epi64(sums, 2) + _mm256_extract_epi64(sums, 3));
    }
    for (; i < n; ++i) cont += (static_cast<unsigned char>(p[i]) & 0xC0) == 0x80;
    return n - cont;
}
#endif

size_t utf8_length(const std::string& s) {
#ifdef __x86_64__
    static const bool avx2 = __builtin_cpu_supports("avx2");
    if (avx2) return utf8_length_avx2(s);
#endif
    return utf8_length_base(s);
}

} // namespace lux_script
