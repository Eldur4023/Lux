// Regex module over tiny_regex (a Pike VM: linear time in the subject, no
// backtracking, so no ReDoS -- see tiny_regex.hpp for the supported subset).
// Subjects are still capped at kMaxSubjectLength as a bound on one call.
#include <lux_script/builtin_module.hpp>
#include <lux_script/tiny_regex.hpp>

#include <string_view>
#include <unordered_map>

namespace lux_script {

namespace {

using tiny_regex::Regex;

constexpr size_t kMaxSubjectLength = 4096;

// The compiled pattern for this call, or nullptr with `error` set. Patterns
// are source literals in practice, so each thread keeps what it compiled.
// ponytail: dropped wholesale at 128 patterns; an LRU if an app ever
// builds patterns at run time in a hot loop.
const Regex* prepare(const std::vector<Value>& a, std::string& error) {
    if (a[1].as_str().size() > kMaxSubjectLength) {
        error = "regex: the subject is " + std::to_string(a[1].as_str().size()) + " bytes, over the " +
                std::to_string(kMaxSubjectLength) + "-byte limit";
        return nullptr;
    }
    thread_local std::unordered_map<std::string, Regex> cache;
    if (auto it = cache.find(a[0].as_str()); it != cache.end()) return &it->second;
    Regex re;
    if (!re.compile(a[0].as_str(), error)) return nullptr;
    if (cache.size() >= 128) cache.clear();
    return &cache.emplace(a[0].as_str(), std::move(re)).first->second;
}

std::string group(const std::string& text, const Regex::Match& m, size_t g) {
    const int s = m.slots[2 * g], e = m.slots[2 * g + 1];
    return text.substr(static_cast<size_t>(s), static_cast<size_t>(e - s));
}

// Advances the search cursor past a match, the same "+1 on an empty match"
// rule std::sregex_iterator itself uses (and every other regex "find all"/
// "split" implementation copies) -- without it, a pattern that can match
// zero-width (`a*`, `\s*`, a group made entirely of `?`/`*` pieces) matches
// the SAME empty string at the SAME position forever, since search() has
// no reason on its own to skip past a position it just successfully
// (if vacuously) matched.
size_t advance_past(const Regex::Match& m, size_t match_end) {
    return (static_cast<size_t>(m.slots[1]) == static_cast<size_t>(m.slots[0]))
           ? match_end + 1 : match_end;
}

Value fn_regex_test(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Regex* re = prepare(args, error);
    Regex::Match m;
    return re ? Value::boolean(re->search(args[1].as_str(), 0, m)) : Value::null();
}

// null on no match: not matching user input is the routine case.
Value fn_regex_find(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Regex* re = prepare(args, error);
    Regex::Match m;
    if (!re || !re->search(args[1].as_str(), 0, m)) return Value::null();
    return Value::str(group(args[1].as_str(), m, 0));
}

Value fn_regex_find_all(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Regex* re = prepare(args, error);
    if (!re) return Value::null();
    const std::string& text = args[1].as_str();
    Value::List out;
    Regex::Match m;
    size_t pos = 0;
    while (pos <= text.size() && re->search(text, pos, m)) {
        out.push_back(Value::str(group(text, m, 0)));
        pos = advance_past(m, static_cast<size_t>(m.slots[1]));
    }
    return Value::list(std::move(out));
}

// [whole match, group 1, group 2, ...]; a group that did not take part is
// null. null when nothing matches, like find().
Value fn_regex_groups(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Regex* re = prepare(args, error);
    Regex::Match m;
    if (!re || !re->search(args[1].as_str(), 0, m)) return Value::null();
    Value::List out;
    for (size_t g = 0; g < static_cast<size_t>(re->group_count()); ++g)
        out.push_back(m.slots[2 * g] < 0 ? Value::null() : Value::str(group(args[1].as_str(), m, g)));
    return Value::list(std::move(out));
}

// Replaces every match, not just the first -- matching Python's
// re.sub()/JavaScript's String.replace(/g) default expectation for a regex
// (as opposed to string.replace(), natives.cpp, which is also all-
// occurrences already, for consistency between the two). `$1`, `$2`, ...
// in `replacement` refer to capture groups, `$$` a literal `$` -- the same
// ECMAScript-replacement-string convention std::regex_replace used, kept
// so existing patterns/replacement strings do not need to change even
// though the matching engine underneath did.
std::string expand_replacement(const std::string& repl, const std::string& text,
                               const Regex::Match& m, int group_count) {
    std::string out;
    out.reserve(repl.size());
    for (size_t i = 0; i < repl.size(); ++i) {
        if (repl[i] != '$' || i + 1 >= repl.size()) { out += repl[i]; continue; }
        char n = repl[i + 1];
        if (n == '$') { out += '$'; ++i; continue; }
        if (n == '&') { out += group(text, m, 0); ++i; continue; }   // whole match
        if (n >= '0' && n <= '9') {
            // Greedily take a second digit too (e.g. $12) only if that
            // still names a real group -- otherwise `$1` followed by a
            // literal '2' would wrongly swallow it.
            size_t j = i + 1, end = j + 1;
            int g = n - '0';
            if (end < repl.size() && repl[end] >= '0' && repl[end] <= '9') {
                int g2 = g * 10 + (repl[end] - '0');
                if (g2 < group_count) { g = g2; ++end; }
            }
            if (g > 0 && g < group_count) {
                if (m.slots[2 * static_cast<size_t>(g)] >= 0) out += group(text, m, static_cast<size_t>(g));
                i = end - 1;
                continue;
            }
        }
        out += repl[i]; // '$' followed by something that names no group: literal
    }
    return out;
}

Value fn_regex_replace(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Regex* re = prepare(args, error);
    if (!re) return Value::null();
    const std::string& text = args[1].as_str();
    const std::string& repl = args[2].as_str();

    std::string out;
    size_t pos = 0, last = 0;
    Regex::Match m;
    while (pos <= text.size() && re->search(text, pos, m)) {
        out.append(text, last, static_cast<size_t>(m.slots[0]) - last);
        out += expand_replacement(repl, text, m, re->group_count());
        last = static_cast<size_t>(m.slots[1]);
        pos = advance_past(m, last);
    }
    out.append(text, last, text.size() - last);
    return Value::str(std::move(out));
}

Value fn_regex_split(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Regex* re = prepare(args, error);
    if (!re) return Value::null();
    const std::string& text = args[1].as_str();

    Value::List out;
    size_t pos = 0, last = 0;
    Regex::Match m;
    while (pos <= text.size() && re->search(text, pos, m)) {
        // A zero-width match (the separator pattern matched "") would
        // split every single position into its own piece -- not useful,
        // and not what std::sregex_token_iterator(-1) did either (it
        // never treats a zero-width delimiter as a separator boundary).
        if (m.slots[1] == m.slots[0]) { pos = static_cast<size_t>(m.slots[0]) + 1; continue; }
        out.push_back(Value::str(text.substr(last, static_cast<size_t>(m.slots[0]) - last)));
        last = static_cast<size_t>(m.slots[1]);
        pos = last;
    }
    out.push_back(Value::str(text.substr(last)));
    return Value::list(std::move(out));
}

// The text with every metacharacter backslashed: user input dropped into
// a pattern matches only itself -- Python's re.escape().
Value fn_regex_escape(NativeCtx&, std::vector<Value>& args, std::string&) {
    std::string out;
    for (char c : args[0].as_str()) {
        if (std::string_view("\\^$.|?*+()[]{}/-").find(c) != std::string_view::npos) out += '\\';
        out += c;
    }
    return Value::str(std::move(out));
}

} // namespace

LUX_MODULE(regex, {
    {"test",     "ss>b",  fn_regex_test},
    {"find",     "ss",  fn_regex_find},
    {"find_all", "ss>l",  fn_regex_find_all},
    {"groups",   "ss",  fn_regex_groups},
    {"replace",  "sss>s", fn_regex_replace},
    {"split",    "ss>l",  fn_regex_split},
    {"escape",   "s>s",   fn_regex_escape},
})

} // namespace lux_script
