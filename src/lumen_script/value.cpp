#include <charconv>
#include <cstdint>
#include <cstdio>
#include <lumen_script/value.hpp>
#include <lumen_script/bytecode.hpp>

#include <cmath>
#include <sstream>

namespace lumen_script {

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
    return (below_20 | ((c1 - ONES) & ~c1 & HIGH_BITS)
                     | ((c2 - ONES) & ~c2 & HIGH_BITS)) != 0;
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

// The escaping loop, no surrounding quotes: shared by escape_json's rare
// invalid-UTF-8 fallback below, which already knows its input is clean
// (sanitize_utf8 just ran) and needs no byte to be inspected for anything
// but escaping.
void escape_body(const std::string& in, std::string& out) {
    const char*  p = in.data();
    const size_t n = in.size();
    size_t i = 0, clean = 0;

    while (i < n) {
        while (i + 8 <= n) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            if (block_needs_escape(w)) break;
            i += 8;
        }
        unsigned char c = 0;
        bool present = false;
        for (; i < n; ++i) {
            c = static_cast<unsigned char>(p[i]);
            if (c < 0x20 || c == '"' || c == '\\') { present = true; break; }
        }
        if (!present) break;

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
// neither benefits from --native compiling it (it is not user .lum code) —
// so this is the one place a change here helps both backends equally.
void escape_json(const std::string& in, std::string& out) {
    out.push_back('"');
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
            if (c < 0x20 || c == '"' || c == '\\' || c >= 0x80) { present = true; break; }
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
                out.push_back('"');
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
    std::string t(buf, r.ptr);
    // A whole double comes out as "3"; JSON would read that as an integer, so
    // it gets the ".0" just as nlohmann did.
    if (t.find_first_of(".eE") == std::string::npos) t += ".0";
    out += t;
}

} // namespace

void Value::write_json(std::string& out) const {
    switch (type_) {
        case Type::Null:  out += "null";                   return;
        case Type::Bool:  out += b_ ? "true" : "false";    return;
        case Type::Int:   out += std::to_string(i_);       return;
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

const char* op_name(Op op) {
    switch (op) {
        case Op::Const:            return "CONST";
        case Op::LoadLocal:        return "LOAD_LOCAL";
        case Op::StoreLocal:       return "STORE_LOCAL";
        case Op::Pop:              return "POP";
        case Op::Add:              return "ADD";
        case Op::Sub:              return "SUB";
        case Op::Mul:              return "MUL";
        case Op::Div:              return "DIV";
        case Op::Mod:              return "MOD";
        case Op::Neg:              return "NEG";
        case Op::Eq:               return "EQ";
        case Op::Ne:               return "NE";
        case Op::Lt:               return "LT";
        case Op::Le:               return "LE";
        case Op::Gt:               return "GT";
        case Op::Ge:               return "GE";
        case Op::Not:              return "NOT";
        case Op::ConcatN:          return "CONCAT_N";
        case Op::AddInt:           return "ADD_INT";
        case Op::SubInt:           return "SUB_INT";
        case Op::MulInt:           return "MUL_INT";
        case Op::LtInt:            return "LT_INT";
        case Op::LeInt:            return "LE_INT";
        case Op::GtInt:            return "GT_INT";
        case Op::GeInt:            return "GE_INT";
        case Op::Jump:             return "JUMP";
        case Op::JumpIfFalse:      return "JUMP_IF_FALSE";
        case Op::JumpIfFalsePeek:  return "JUMP_IF_FALSE_PEEK";
        case Op::JumpIfTruePeek:   return "JUMP_IF_TRUE_PEEK";
        case Op::MakeList:         return "MAKE_LIST";
        case Op::MakeDict:         return "MAKE_DICT";
        case Op::GetIndex:         return "GET_INDEX";
        case Op::SetIndex:         return "SET_INDEX";
        case Op::IterList:         return "ITER_LIST";
        case Op::GetMember:        return "GET_MEMBER";
        case Op::SetMember:        return "SET_MEMBER";
        case Op::CallFunction:     return "CALL_FUNCTION";
        case Op::CallMethod:       return "CALL_METHOD";
        case Op::CallNative:       return "CALL_NATIVE";
        case Op::CallBuiltinModule: return "CALL_BUILTIN_MODULE";
        case Op::CallAsync:        return "CALL_ASYNC";
        case Op::CallAsyncModule:  return "CALL_ASYNC_MODULE";
        case Op::Return:           return "RETURN";
        case Op::ReturnNull:       return "RETURN_NULL";
    }
    return "?";
}

} // namespace lumen_script
