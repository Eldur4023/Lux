// A thin wrapper over Value::parse_json/to_json_text (value.cpp,
// json_parse.cpp), which already existed for the framework's own use
// (request bodies, JWTs, http.*'s auto-parsed response body -- see
// project.cpp, auth.cpp, modules/base_modules/http.cpp) but was never
// reachable from Lux Script itself for an arbitrary string. Before this
// module, a script that needed to parse JSON out of a string it built or
// received some other way (a file, a websocket message, a query param) had
// no way to do it except writing its own parser in Lux Script -- and Lux
// Script's only "missing value" is `null`, the same value a real top-level
// JSON `null` literal parses to, so a hand-written parser that returns
// `null` on failure cannot tell "the document is invalid" apart from "the
// document is the literal `null`" without a second out-of-band signal.
//
// `error` (BuiltinModuleFn's normal error channel, builtin_module.hpp) IS
// that out-of-band signal here: parse() raises a catchable Lux Script error
// (`try`/`catch`, GUIDE.md §17) on invalid JSON, and only ever returns a
// Value on success -- including a genuine top-level `null`. That fully
// separates the two cases the string-return convention could not.
#include <lux_script/builtin_module.hpp>

namespace lux_script {

namespace {

Value fn_json_parse(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "json.parse() expects a string"; return Value::null(); }
    Value out;
    if (!Value::parse_json(args[0].as_str(), out)) {
        error = "json.parse(): invalid JSON";
        return Value::null();
    }
    return out;
}

Value fn_json_stringify(NativeCtx&, std::vector<Value>& args, std::string& /*error*/) {
    return Value::str(args[0].to_json_text());
}

class JsonModule : public BuiltinModule {
public:
    const char* name() const override { return "json"; }

    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"parse",     1, 1, fn_json_parse},
            {"stringify", 1, 1, fn_json_stringify},
        };
        return fns;
    }
};

} // namespace

LUX_REGISTER_MODULE(JsonModule)

} // namespace lux_script
