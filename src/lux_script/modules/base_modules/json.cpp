// JSON from Lux Script, over the framework's own parser and writer
// (json_parse.cpp, value.cpp). parse() raises a catchable error on invalid
// input and returns a Value only on success -- so a document that IS the
// literal `null` stays distinguishable from a broken one.
#include <lux_script/builtin_module.hpp>

namespace lux_script {

namespace {

Value fn_parse(NativeCtx&, std::vector<Value>& a, std::string& error) {
    Value out;
    if (!Value::parse_json(a[0].as_str(), out)) { error = "json.parse(): invalid JSON"; return Value::null(); }
    return out;
}

// Same rules as the compact writer (it does the scalars): keys starting
// with "__" are internal and never come out.
void pretty(const Value& v, size_t indent, size_t depth, std::string& out) {
    const bool list = v.is_list();
    if (!list && !v.is_dict()) { v.write_json(out); return; }
    if (list ? v.as_list().empty() : v.as_dict().empty()) { out += list ? "[]" : "{}"; return; }
    const std::string pad(indent * (depth + 1), ' ');
    out += list ? '[' : '{';
    bool first = true;
    auto next = [&] { out += first ? "\n" : ",\n"; first = false; out += pad; };
    if (list) for (const Value& x : v.as_list()) { next(); pretty(x, indent, depth + 1, out); }
    else for (const auto& [k, x] : v.as_dict()) {
        if (k.rfind("__", 0) == 0) continue;
        next();
        Value::str(k).write_json(out);
        out += ": ";
        pretty(x, indent, depth + 1, out);
    }
    out += '\n' + std::string(indent * depth, ' ') + (list ? ']' : '}');
}

// stringify(v) is compact; stringify(v, 2) indents by 2 spaces.
Value fn_stringify(NativeCtx&, std::vector<Value>& a, std::string&) {
    if (a.size() < 2 || a[1].as_int() <= 0) return Value::str(a[0].to_json_text());
    std::string out;
    pretty(a[0], static_cast<size_t>(std::min(a[1].as_int(), 8LL)), 0, out);
    return Value::str(std::move(out));
}

} // namespace

LUX_MODULE(json, {
    {"parse",     "s",   fn_parse},
    {"stringify", "x|i>s", fn_stringify},
})

} // namespace lux_script
