// Regex module (NATIVE-MODULES.md): pattern matching, search, and
// replacement -- Lux Script had no text-pattern facility at all before
// this beyond starts_with/ends_with/contains/index_of (natives.cpp), all of
// which need an exact substring.
//
// Built on lux_script::tiny_regex::Regex (tiny_regex.hpp/.cpp), NOT
// std::regex. std::regex's ECMAScript grammar (libstdc++'s implementation)
// is a classic backtracking engine with no bound on running time as a
// function of subject length, and it recurses in the C++ call stack once
// per subject character for most patterns -- see tiny_regex.hpp's class
// comment for the exact measurements (a stack-overflow SEGV from a ~4000-
// character subject, and 5.7 SECONDS from a 28-BYTE one against an
// ordinary-looking pattern) that made std::regex a real, unauthenticated,
// process-wide DoS for any app that ran regex.* against request data --
// which is exactly what this module exists for. tiny_regex is a Thompson-
// NFA / Pike-VM engine: O(pattern size * subject size) always, no
// exponential case exists for it to hit, and its own internals never
// recurse over the subject either.
//
// The subject length cap (kMaxSubjectLength) that used to be this
// module's ONLY defense stays, unchanged: a bound on a single call's cost
// is still good hygiene even with a linear-time engine underneath, and a
// smaller ecosystem of "what does regex.* accept" than "unbounded, but
// fast" is one less thing to reason about.
//
// ECMAScript-ish syntax, a deliberate SUBSET -- see tiny_regex.hpp for
// exactly what is (and, notably, is not: non-capturing groups, lookaround,
// backreferences inside a pattern) supported.
#include <lux_script/builtin_module.hpp>
#include <lux_script/tiny_regex.hpp>

namespace lux_script {

namespace {

using tiny_regex::Regex;

bool compile(const std::string& pattern, Regex& out, std::string& error) {
    return out.compile(pattern, error);
}

constexpr size_t kMaxSubjectLength = 4096;

bool check_subject(const std::string& text, const char* fn_name, std::string& error) {
    if (text.size() > kMaxSubjectLength) {
        error = std::string("regex.") + fn_name + "(): subject text is " +
                std::to_string(text.size()) + " bytes, over the " +
                std::to_string(kMaxSubjectLength) + "-byte limit -- keep "
                "patterns applied to a bounded piece of text, not an "
                "entire request body";
        return false;
    }
    return true;
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
    if (!args[0].is_str() || !args[1].is_str()) { error = "regex.test() expects two strings"; return Value::null(); }
    if (!check_subject(args[1].as_str(), "test", error)) return Value::null();
    Regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    Regex::Match m;
    return Value::boolean(re.search(args[1].as_str(), 0, m));
}

// null on no match -- the same "absence is a normal, checkable outcome"
// convention os.read_file()/time.parse() already use, not an error: a
// pattern not matching arbitrary input (user text, a header, a query
// parameter) is the routine case, not exceptional.
Value fn_regex_find(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) { error = "regex.find() expects two strings"; return Value::null(); }
    if (!check_subject(args[1].as_str(), "find", error)) return Value::null();
    Regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    const std::string& text = args[1].as_str();
    Regex::Match m;
    if (!re.search(text, 0, m)) return Value::null();
    return Value::str(text.substr(static_cast<size_t>(m.slots[0]),
                                  static_cast<size_t>(m.slots[1] - m.slots[0])));
}

Value fn_regex_find_all(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) { error = "regex.find_all() expects two strings"; return Value::null(); }
    if (!check_subject(args[1].as_str(), "find_all", error)) return Value::null();
    Regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    const std::string& text = args[1].as_str();
    Value::List out;
    Regex::Match m;
    size_t pos = 0;
    while (pos <= text.size() && re.search(text, pos, m)) {
        out.push_back(Value::str(text.substr(static_cast<size_t>(m.slots[0]),
                                             static_cast<size_t>(m.slots[1] - m.slots[0]))));
        pos = advance_past(m, static_cast<size_t>(m.slots[1]));
    }
    return Value::list(std::move(out));
}

// Index 0 is the whole match, 1.. are the capture groups -- kept as-is
// rather than renumbered, since it is the convention most regex
// documentation (including the one for this exact grammar) already uses.
// null, not an empty List, when there is no match at all -- the same
// absence-is-null rule as find().
Value fn_regex_groups(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) { error = "regex.groups() expects two strings"; return Value::null(); }
    if (!check_subject(args[1].as_str(), "groups", error)) return Value::null();
    Regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    const std::string& text = args[1].as_str();
    Regex::Match m;
    if (!re.search(text, 0, m)) return Value::null();
    Value::List out;
    for (int i = 0; i < re.group_count(); ++i) {
        int s = m.slots[2 * static_cast<size_t>(i)], e = m.slots[2 * static_cast<size_t>(i) + 1];
        out.push_back(s < 0 ? Value::null()
                            : Value::str(text.substr(static_cast<size_t>(s), static_cast<size_t>(e - s))));
    }
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
        if (n == '&') { // whole match
            out += text.substr(static_cast<size_t>(m.slots[0]),
                               static_cast<size_t>(m.slots[1] - m.slots[0]));
            ++i;
            continue;
        }
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
                int s = m.slots[2 * static_cast<size_t>(g)], e = m.slots[2 * static_cast<size_t>(g) + 1];
                if (s >= 0) out += text.substr(static_cast<size_t>(s), static_cast<size_t>(e - s));
                i = end - 1;
                continue;
            }
        }
        out += repl[i]; // '$' followed by something that names no group: literal
    }
    return out;
}

Value fn_regex_replace(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str() || !args[2].is_str()) {
        error = "regex.replace() expects three strings"; return Value::null();
    }
    if (!check_subject(args[1].as_str(), "replace", error)) return Value::null();
    Regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    const std::string& text = args[1].as_str();
    const std::string& repl = args[2].as_str();

    std::string out;
    size_t pos = 0, last = 0;
    Regex::Match m;
    while (pos <= text.size() && re.search(text, pos, m)) {
        out.append(text, last, static_cast<size_t>(m.slots[0]) - last);
        out += expand_replacement(repl, text, m, re.group_count());
        last = static_cast<size_t>(m.slots[1]);
        pos = advance_past(m, last);
    }
    out.append(text, last, text.size() - last);
    return Value::str(std::move(out));
}

Value fn_regex_split(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) { error = "regex.split() expects two strings"; return Value::null(); }
    if (!check_subject(args[1].as_str(), "split", error)) return Value::null();
    Regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    const std::string& text = args[1].as_str();

    Value::List out;
    size_t pos = 0, last = 0;
    Regex::Match m;
    while (pos <= text.size() && re.search(text, pos, m)) {
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

} // namespace

LUX_MODULE(regex, {
    {"test",     2, 2, fn_regex_test},
    {"find",     2, 2, fn_regex_find},
    {"find_all", 2, 2, fn_regex_find_all},
    {"groups",   2, 2, fn_regex_groups},
    {"replace",  3, 3, fn_regex_replace},
    {"split",    2, 2, fn_regex_split},
})

} // namespace lux_script
