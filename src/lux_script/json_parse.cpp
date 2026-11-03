#include <lux_script/json_bind.hpp>
#include <lux_script/value.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace lux_script {

// ─── JSON parsing ────────────────────────────────────────────────────────────
//
// A small, strict recursive descent that produces a Value directly, with no
// intermediate tree.  It accepts no comments, no trailing commas, no NaN and
// no garbage after the document: only JSON.
//
// What comes in here arrives from the network, so there is a nesting cap and
// nothing is taken on trust.

namespace {

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    bool document(Value& out) {
        skip_ws();
        if (!value(out, 0)) return false;
        skip_ws();
        return i_ == t_.size();
    }

private:
    // Without a cap, a document with thousands of nested brackets takes the
    // thread stack down before anyone can complain.
    static constexpr int kMaxDepth = 200;

    std::string_view t_;
    size_t           i_ = 0;

    static bool is_digit(char c) { return c >= '0' && c <= '9'; }

    void skip_ws() {
        i_ = std::min(t_.size(), t_.find_first_not_of(" \t\n\r", i_));
    }

    bool literal(std::string_view lit) {
        if (t_.size() - i_ < lit.size()) return false;
        if (t_.compare(i_, lit.size(), lit) != 0) return false;
        i_ += lit.size();
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth || i_ >= t_.size()) return false;
        switch (t_[i_]) {
            case 'n':
                if (!literal("null")) return false;
                out = Value::null();
                return true;
            case 't':
                if (!literal("true")) return false;
                out = Value::boolean(true);
                return true;
            case 'f':
                if (!literal("false")) return false;
                out = Value::boolean(false);
                return true;
            case '"': {
                std::string s;
                if (!string_value(s)) return false;
                out = Value::str(std::move(s));
                return true;
            }
            case '[': return list_(out, depth);
            case '{': return object(out, depth);
            default:  return number(out);
        }
    }

    bool list_(Value& out, int depth) {
        ++i_;                                   // '['
        Value::List l;
        skip_ws();
        if (i_ < t_.size() && t_[i_] == ']') {
            ++i_;
            out = Value::list(std::move(l));
            return true;
        }
        for (;;) {
            skip_ws();
            Value v;
            if (!value(v, depth + 1)) return false;
            l.push_back(std::move(v));
            skip_ws();
            if (i_ >= t_.size()) return false;
            if (t_[i_] == ',') { ++i_; continue; }
            if (t_[i_] == ']') { ++i_; break; }
            return false;
        }
        out = Value::list(std::move(l));
        return true;
    }

    bool object(Value& out, int depth) {
        ++i_;                                   // '{'
        Value::Dict d;
        skip_ws();
        if (i_ < t_.size() && t_[i_] == '}') {
            ++i_;
            out = Value::dict(std::move(d));
            return true;
        }
        for (;;) {
            skip_ws();
            if (i_ >= t_.size() || t_[i_] != '"') return false;
            std::string k;
            if (!string_value(k)) return false;
            skip_ws();
            if (i_ >= t_.size() || t_[i_] != ':') return false;
            ++i_;
            skip_ws();
            Value v;
            if (!value(v, depth + 1)) return false;
            d[k] = std::move(v);
            skip_ws();
            if (i_ >= t_.size()) return false;
            if (t_[i_] == ',') { ++i_; continue; }
            if (t_[i_] == '}') { ++i_; break; }
            return false;
        }
        out = Value::dict(std::move(d));
        return true;
    }

    // Exactly four hex digits (from_chars rejects signs/whitespace for an
    // unsigned type, so "+1ab" never slips through).
    bool hex4(uint32_t& cp) {
        if (i_ + 4 > t_.size()) return false;
        const char* p = t_.data() + i_;
        auto [end, ec] = std::from_chars(p, p + 4, cp, 16);
        if (ec != std::errc{} || end != p + 4) return false;
        i_ += 4;
        return true;
    }

    bool string_value(std::string& out) {
        ++i_;                                   // opening quote

        // Fast path: what runs up to the first backslash or quote is copied in
        // one go.  The vast majority of strings end here.
        const size_t start = i_;
        while (i_ < t_.size()) {
            const char c = t_[i_];
            if (c == '"' || c == '\\') break;
            if (static_cast<unsigned char>(c) < 0x20) return false;  // unescaped control character
            ++i_;
        }
        out.append(t_.data() + start, i_ - start);
        if (i_ >= t_.size()) return false;
        if (t_[i_] == '"') { ++i_; return true; }

        for (;;) {
            if (i_ >= t_.size()) return false;
            const char c = t_[i_];
            if (c == '"') { ++i_; return true; }
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out.push_back(c); ++i_; continue; }

            ++i_;
            if (i_ >= t_.size()) return false;
            const char e = t_[i_++];
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    // A high surrogate has to be followed by its low one; a
                    // stray one represents nothing and is rejected.
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i_ + 1 >= t_.size() || t_[i_] != '\\' || t_[i_ + 1] != 'u')
                            return false;
                        i_ += 2;
                        uint32_t low = 0;
                        if (!hex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    out += utf8_encode(cp);
                    break;
                }
                default: return false;
            }
        }
    }

    bool number(Value& out) {
        const size_t start = i_;
        if (i_ < t_.size() && t_[i_] == '-') ++i_;
        if (i_ >= t_.size()) return false;

        if (t_[i_] == '0') {
            ++i_;
        } else if (t_[i_] >= '1' && t_[i_] <= '9') {
            while (i_ < t_.size() && is_digit(t_[i_])) ++i_;
        } else {
            return false;
        }

        bool real = false;
        if (i_ < t_.size() && t_[i_] == '.') {
            real = true;
            ++i_;
            if (i_ >= t_.size() || !is_digit(t_[i_])) return false;
            while (i_ < t_.size() && is_digit(t_[i_])) ++i_;
        }
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            real = true;
            ++i_;
            if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-')) ++i_;
            if (i_ >= t_.size() || !is_digit(t_[i_])) return false;
            while (i_ < t_.size() && is_digit(t_[i_])) ++i_;
        }

        const char* p = t_.data() + start;
        const char* f = t_.data() + i_;
        if (!real) {
            long long n = 0;
            const auto r = std::from_chars(p, f, n);
            if (r.ec == std::errc{} && r.ptr == f) {
                out = Value::integer(n);
                return true;
            }
            // Out of long long range: falls to double, as everyone does.
        }
        double d = 0;
#ifdef __ANDROID__
        // libc++ in the NDK has no floating-point from_chars; the grammar was already checked above.
        const std::string tmp(p, f);
        char* end = nullptr;
        d = std::strtod(tmp.c_str(), &end);
        if (end != tmp.c_str() + tmp.size()) return false;
#else
        const auto r = std::from_chars(p, f, d);
        if (r.ec != std::errc{} || r.ptr != f) return false;
#endif
        out = Value::real(d);
        return true;
    }

    // ── schema-aware binding (see json_bind.hpp) ─────────────────────────────
    //
    // The same scanner as above, driven by the class's fields instead of
    // building the whole generic tree. Declared scalars are read straight
    // into Values; unknown keys are validated and skipped, nothing built;
    // nested classes and List<Class> recurse through the same path. The
    // grammar, the nesting cap and every rejection are the ones `value()`
    // has always enforced: what fits and what doesn't is decided per token
    // exactly like value_matches() (project.cpp) decided it over the tree,
    // so the answers stay byte for byte.

    // What `class_object` decided. Err means invalid JSON (the input could
    // not be consumed); No means valid JSON that just doesn't fit the class.
    enum class Fit { Err, No, Yes };

    // Consumes a string without building it: keys of objects nobody declared.
    // Validates the exact same escapes `string_value` does.
    bool string_skip() {
        ++i_;                                   // opening quote
        while (i_ < t_.size()) {
            const char c = t_[i_];
            if (c == '"') { ++i_; return true; }
            if (static_cast<unsigned char>(c) < 0x20) return false;  // unescaped control character
            if (c != '\\') { ++i_; continue; }

            ++i_;
            if (i_ >= t_.size()) return false;
            const char e = t_[i_++];
            switch (e) {
                case '"': case '\\': case '/':
                case 'b': case 'f': case 'n': case 'r': case 't':
                    break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    // A high surrogate has to be followed by its low one; a
                    // stray one represents nothing and is rejected.
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i_ + 1 >= t_.size() || t_[i_] != '\\' || t_[i_ + 1] != 'u')
                            return false;
                        i_ += 2;
                        uint32_t low = 0;
                        if (!hex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return false;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    // Consumes one JSON value of any shape, building nothing.
    bool skip_value(int depth) {
        if (depth > kMaxDepth || i_ >= t_.size()) return false;
        switch (t_[i_]) {
            case 'n': return literal("null");
            case 't': return literal("true");
            case 'f': return literal("false");
            case '"': return string_skip();
            case '[':
                ++i_;
                skip_ws();
                if (i_ < t_.size() && t_[i_] == ']') { ++i_; return true; }
                for (;;) {
                    skip_ws();
                    if (!skip_value(depth + 1)) return false;
                    skip_ws();
                    if (i_ >= t_.size()) return false;
                    if (t_[i_] == ',') { ++i_; continue; }
                    if (t_[i_] == ']') { ++i_; return true; }
                    return false;
                }
            case '{':
                ++i_;
                skip_ws();
                if (i_ < t_.size() && t_[i_] == '}') { ++i_; return true; }
                for (;;) {
                    skip_ws();
                    if (i_ >= t_.size() || t_[i_] != '"') return false;
                    if (!string_skip()) return false;
                    skip_ws();
                    if (i_ >= t_.size() || t_[i_] != ':') return false;
                    ++i_;
                    skip_ws();
                    if (!skip_value(depth + 1)) return false;
                    skip_ws();
                    if (i_ >= t_.size()) return false;
                    if (t_[i_] == ',') { ++i_; continue; }
                    if (t_[i_] == '}') { ++i_; return true; }
                    return false;
                }
            default: {
                Value scratch;
                return number(scratch);
            }
        }
    }

    // Parses one value against a field spec. Consumes the value whatever the
    // verdict — a field with the wrong type is the CALLER's 422, never a
    // reason to leave the input half-read. False ONLY for invalid JSON.
    // `out` is meaningful only when it comes back with st == Ok.
    bool typed_value(int depth, const JsonFieldSpec& spec, const JsonShapeTable& shapes,
                     Value& out, JsonBindStatus& st) {
        if (depth > kMaxDepth || i_ >= t_.size()) return false;
        switch (t_[i_]) {
            case 'n':
                if (!literal("null")) return false;
                st = JsonBindStatus::Missing;
                return true;
            case '"': {
                std::string s;
                if (!string_value(s)) return false;
                if (!spec.is_list && spec.kind == JsonScalar::Str) {
                    out = Value::str(std::move(s));
                    st = JsonBindStatus::Ok;
                } else {
                    st = JsonBindStatus::BadType;
                }
                return true;
            }
            case 't':
                if (!literal("true")) return false;
                if (!spec.is_list && spec.kind == JsonScalar::Bool) {
                    out = Value::boolean(true);
                    st = JsonBindStatus::Ok;
                } else {
                    st = JsonBindStatus::BadType;
                }
                return true;
            case 'f':
                if (!literal("false")) return false;
                if (!spec.is_list && spec.kind == JsonScalar::Bool) {
                    out = Value::boolean(false);
                    st = JsonBindStatus::Ok;
                } else {
                    st = JsonBindStatus::BadType;
                }
                return true;
            case '[': {
                if (!spec.is_list) {
                    Value scratch;
                    if (!list_(scratch, depth)) return false;
                    st = JsonBindStatus::BadType;
                    return true;
                }
                // The element is the same scalar (or the same nested class),
                // never a list of its own, and null is just a wrong element:
                // value_matches() would have refused it the same way.
                JsonFieldSpec el;
                el.kind     = spec.kind;
                el.optional = false;
                el.is_list  = false;
                el.nested   = spec.nested;
                Value::List result;
                bool ok = true;
                ++i_;
                skip_ws();
                if (i_ < t_.size() && t_[i_] == ']') {
                    ++i_;
                } else {
                    for (;;) {
                        skip_ws();
                        Value ev;
                        JsonBindStatus est;
                        if (!typed_value(depth + 1, el, shapes, ev, est)) return false;
                        if (est == JsonBindStatus::Ok) result.push_back(std::move(ev));
                        else ok = false;    // keep consuming: one bad element fails the field
                        skip_ws();
                        if (i_ >= t_.size()) return false;
                        if (t_[i_] == ',') { ++i_; continue; }
                        if (t_[i_] == ']') { ++i_; break; }
                        return false;
                    }
                }
                if (ok) {
                    out = Value::list(std::move(result));
                    st = JsonBindStatus::Ok;
                } else {
                    st = JsonBindStatus::BadType;
                }
                return true;
            }
            case '{': {
                if (!spec.is_list && spec.nested != kJsonNoNested) {
                    st = class_object(depth + 1, spec.nested, shapes, out) == Fit::Yes
                             ? JsonBindStatus::Ok : JsonBindStatus::BadType;
                    return true;
                }
                Value scratch;
                if (!object(scratch, depth)) return false;
                st = JsonBindStatus::BadType;
                return true;
            }
            default: {
                if (!number(out)) return false;     // Int, or Real when it had to be
                if (!spec.is_list) {
                    switch (spec.kind) {
                        case JsonScalar::Int:
                            // "30.0" is a Real here: not an int, same answer
                            // value_matches() gave a tree with a Real in it.
                            st = out.is_int() ? JsonBindStatus::Ok : JsonBindStatus::BadType;
                            return true;
                        case JsonScalar::Float:
                            // An integer JSON in a double field is stored as a
                            // Real either way -- the normalization the tree
                            // path did in value_matches().
                            out = Value::real(out.as_float());
                            st = JsonBindStatus::Ok;
                            return true;
                        default:
                            break;
                    }
                }
                st = JsonBindStatus::BadType;
                return true;
            }
        }
    }

    // A JSON object bound against a nested class: the recursive half of
    // value_matches()'s Class branch, reading the fields as they arrive.
    // Fit::Yes → `out` is the instance (declared field order, optional nulls).
    // Fit::No  → valid JSON that doesn't fit; `out` untouched, input consumed.
    Fit class_object(int depth, uint32_t cls, const JsonShapeTable& shapes, Value& out) {
        if (depth > kMaxDepth) return Fit::Err;
        ++i_;                                   // '{'
        const size_t n = shapes.field_count(cls);
        Value::Dict result;
        result.reserve(n);
        std::vector<char> seen(n, 0);
        bool ok = true;
        skip_ws();
        if (i_ < t_.size() && t_[i_] == '}') {
            ++i_;
        } else {
            for (;;) {
                skip_ws();
                if (i_ >= t_.size() || t_[i_] != '"') return Fit::Err;
                std::string k;
                if (!string_value(k)) return Fit::Err;
                const JsonFieldSpec* spec = nullptr;
                for (size_t i = 0; i < n; ++i) {
                    const JsonFieldSpec& f = shapes.field(cls, i);
                    if (f.name == k) { spec = &f; seen[i] = 1; break; }
                }
                skip_ws();
                if (i_ >= t_.size() || t_[i_] != ':') return Fit::Err;
                ++i_;
                skip_ws();
                if (!spec) {
                    if (!skip_value(depth + 1)) return Fit::Err;
                } else {
                    Value fv;
                    JsonBindStatus st;
                    if (!typed_value(depth + 1, *spec, shapes, fv, st)) return Fit::Err;
                    if (ok) {
                        if (st == JsonBindStatus::Ok) {
                            result.set(std::string(spec->name), std::move(fv));
                        } else if (st == JsonBindStatus::Missing) {
                            if (spec->optional) result.set(std::string(spec->name), Value::null());
                            else ok = false;
                        } else {
                            ok = false;
                        }
                    }
                }
                skip_ws();
                if (i_ >= t_.size()) return Fit::Err;
                if (t_[i_] == ',') { ++i_; continue; }
                if (t_[i_] == '}') { ++i_; break; }
                return Fit::Err;
            }
        }
        // The keys the body never mentioned: an optional field is still a
        // null in the instance (the tree walk appended it), a required one
        // fails the whole field.
        if (ok) {
            for (size_t i = 0; i < n; ++i) {
                if (seen[i]) continue;
                const JsonFieldSpec& f = shapes.field(cls, i);
                if (f.optional) result.set(std::string(f.name), Value::null());
                else ok = false;
            }
        }
        if (!ok) return Fit::No;
        out = Value::dict(std::move(result));
        return Fit::Yes;
    }

    // The whole document bound to one class. NotAnObject is decided the same
    // way the tree path decided it: the body parsed fine, it just was not a
    // dict.
public:
    JsonBindError bind_document(const JsonShapeTable& shapes, uint32_t cls,
                                JsonBound& out, bool build_instance) {
        const size_t n = shapes.field_count(cls);
        out.status.assign(n, JsonBindStatus::Missing);
        out.values.assign(n, Value::null());
        skip_ws();
        if (i_ >= t_.size()) return JsonBindError::InvalidJson;
        if (t_[i_] != '{') {
            Value scratch;
            return document(scratch) ? JsonBindError::NotAnObject : JsonBindError::InvalidJson;
        }
        ++i_;
        Value::Dict instance;
        if (build_instance) instance.reserve(n);
        skip_ws();
        bool empty = i_ < t_.size() && t_[i_] == '}';
        if (empty) {
            ++i_;
        } else {
            for (;;) {
                skip_ws();
                if (i_ >= t_.size() || t_[i_] != '"') return JsonBindError::InvalidJson;
                std::string k;
                if (!string_value(k)) return JsonBindError::InvalidJson;
                size_t slot = n;
                for (size_t i = 0; i < n; ++i) {
                    if (shapes.field(cls, i).name == k) { slot = i; break; }
                }
                skip_ws();
                if (i_ >= t_.size() || t_[i_] != ':') return JsonBindError::InvalidJson;
                ++i_;
                skip_ws();
                if (slot == n) {
                    if (!skip_value(1)) return JsonBindError::InvalidJson;
                } else {
                    Value v;
                    JsonBindStatus st;
                    if (!typed_value(1, shapes.field(cls, slot), shapes, v, st)) return JsonBindError::InvalidJson;
                    out.status[slot] = st;
                    out.values[slot] = st == JsonBindStatus::Ok ? std::move(v) : Value::null();
                }
                skip_ws();
                if (i_ >= t_.size()) return JsonBindError::InvalidJson;
                if (t_[i_] == ',') { ++i_; continue; }
                if (t_[i_] == '}') { ++i_; break; }
                return JsonBindError::InvalidJson;
            }
        }
        skip_ws();
        if (i_ != t_.size()) return JsonBindError::InvalidJson;   // garbage after the document
        if (build_instance) {
            for (size_t i = 0; i < n; ++i) {
                const JsonFieldSpec& spec = shapes.field(cls, i);
                instance.set(std::string(spec.name), std::move(out.values[i]));
            }
            out.values.clear();                 // moved into the instance
            out.instance = Value::dict(std::move(instance));
        }
        return JsonBindError::None;
    }
};

} // namespace

bool Value::parse_json(std::string_view text, Value& out) {
    Parser p(text);
    return p.document(out);
}

namespace {

// The one-class table `bind_json_flat` serves: the --native route binder
// only ever binds scalar/optional-scalar classes, so a plain pointer pair
// is the whole table.
struct FlatTable final : JsonShapeTable {
    const JsonFieldSpec* fields;
    size_t               count;

    FlatTable(const JsonFieldSpec* f, size_t c) : fields(f), count(c) {}

    size_t               field_count(uint32_t) const override   { return count; }
    const JsonFieldSpec& field(uint32_t, size_t i) const override { return fields[i]; }
};

} // namespace

JsonBindError bind_json_class(std::string_view text, const JsonShapeTable& shapes,
                              uint32_t cls, JsonBound& out, bool build_instance) {
    Parser p(text);
    return p.bind_document(shapes, cls, out, build_instance);
}

JsonBindError bind_json_flat(std::string_view text, const JsonFieldSpec* fields,
                             size_t count, JsonBound& out, bool build_instance) {
    FlatTable table{fields, count};
    Parser    p(text);
    return p.bind_document(table, 0, out, build_instance);
}

} // namespace lux_script
