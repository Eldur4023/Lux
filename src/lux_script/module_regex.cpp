// Regex module (NATIVE-MODULES.md): pattern matching, search, and
// replacement -- Lux Script had no text-pattern facility at all before
// this beyond starts_with/ends_with/contains/index_of (natives.cpp), all of
// which need an exact substring. Zero third-party dependencies (C++'s own
// <regex>), unconditionally compiled in, stateless -- same shape as every
// other dependency-free module (hash/csv/os/math/time).
//
// ECMAScript syntax (std::regex's default grammar) -- the same dialect
// JavaScript's RegExp and most online regex testers use, and close enough
// to Python's `re` for the common cases (character classes, quantifiers,
// groups, anchors) that patterns copied from either mostly just work.
//
// Every call compiles the pattern fresh with std::regex's own constructor
// -- no handle, no cache, nothing to close. Simpler, and consistent with
// how small every other dependency-free module's functions are (hash/csv/
// math finish in microseconds regardless) -- but unlike those, a genuinely
// complex pattern reused across many requests DOES pay real, repeated
// compilation cost this way. Not addressed here, the same "real but not
// theoretical, not solved yet" honesty NATIVE-MODULES.md already gives
// http.*'s blocking-thread limitation, not a gap unique to this module.
#include <lux_script/builtin_module.hpp>

#include <regex>

namespace lux_script {

namespace {

// std::regex throws on a malformed pattern -- turned into the same `error`
// out-param convention every other module function already uses, not an
// uncaught C++ exception reaching the VM.
bool compile(const std::string& pattern, std::regex& out, std::string& error) {
    try {
        out = std::regex(pattern, std::regex::ECMAScript);
        return true;
    } catch (const std::regex_error& e) {
        error = std::string("invalid regex pattern: ") + e.what();
        return false;
    }
}

Value fn_regex_test(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) { error = "regex.test() expects two strings"; return Value::null(); }
    std::regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    return Value::boolean(std::regex_search(args[1].as_str(), re));
}

// null on no match -- the same "absence is a normal, checkable outcome"
// convention os.read_file()/time.parse() already use, not an error: a
// pattern not matching arbitrary input (user text, a header, a query
// parameter) is the routine case, not exceptional.
Value fn_regex_find(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) { error = "regex.find() expects two strings"; return Value::null(); }
    std::regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    std::smatch m;
    const std::string& text = args[1].as_str();
    if (!std::regex_search(text, m, re)) return Value::null();
    return Value::str(m.str(0));
}

Value fn_regex_find_all(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) { error = "regex.find_all() expects two strings"; return Value::null(); }
    std::regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    const std::string& text = args[1].as_str();
    Value::List out;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
         it != std::sregex_iterator(); ++it)
        out.push_back(Value::str(it->str(0)));
    return Value::list(std::move(out));
}

// Index 0 is the whole match, 1.. are the capture groups -- std::smatch's
// own indexing, kept as-is rather than renumbered, since it is the
// convention most regex documentation (including the one for this exact
// grammar) already uses. null, not an empty List, when there is no match
// at all -- the same absence-is-null rule as find().
Value fn_regex_groups(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) { error = "regex.groups() expects two strings"; return Value::null(); }
    std::regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    std::smatch m;
    const std::string& text = args[1].as_str();
    if (!std::regex_search(text, m, re)) return Value::null();
    Value::List out;
    for (const auto& g : m) out.push_back(g.matched ? Value::str(g.str()) : Value::null());
    return Value::list(std::move(out));
}

// Replaces every match, not just the first -- matching Python's
// re.sub()/JavaScript's String.replace(/g) default expectation for a regex
// (as opposed to string.replace(), natives.cpp, which is also all-
// occurrences already, for consistency between the two). $1, $2, ... in
// `replacement` refer to capture groups -- std::regex_replace's own
// ECMAScript-format syntax, not reimplemented here.
Value fn_regex_replace(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str() || !args[2].is_str()) {
        error = "regex.replace() expects three strings"; return Value::null();
    }
    std::regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    return Value::str(std::regex_replace(args[1].as_str(), re, args[2].as_str()));
}

Value fn_regex_split(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) { error = "regex.split() expects two strings"; return Value::null(); }
    std::regex re;
    if (!compile(args[0].as_str(), re, error)) return Value::null();
    const std::string& text = args[1].as_str();
    Value::List out;
    std::sregex_token_iterator it(text.begin(), text.end(), re, -1), end;
    for (; it != end; ++it) out.push_back(Value::str(*it));
    return Value::list(std::move(out));
}

class RegexModule : public BuiltinModule {
public:
    const char* name() const override { return "regex"; }

    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"test",     2, 2, fn_regex_test},
            {"find",     2, 2, fn_regex_find},
            {"find_all", 2, 2, fn_regex_find_all},
            {"groups",   2, 2, fn_regex_groups},
            {"replace",  3, 3, fn_regex_replace},
            {"split",    2, 2, fn_regex_split},
        };
        return fns;
    }
};

} // namespace

std::unique_ptr<BuiltinModule> make_regex_module() {
    return std::make_unique<RegexModule>();
}

} // namespace lux_script
