// Assertions for `test "name":` blocks (run with `lux test`). A failed one
// ends its test, like abort(); the runner reports it and goes on to the next.
#include <lux_script/builtin_module.hpp>

namespace lux_script {

namespace {

Value fail_test(std::string why, std::string& error) {
    script_mode().failure = std::move(why);
    error = kAbortMessage;
    return Value::null();
}

std::string with_msg(const std::string& what, const std::vector<Value>& a, size_t msg_at) {
    return a.size() > msg_at ? a[msg_at].to_string() + " (" + what + ")" : what;
}

// ok(cond[, message])
Value fn_ok(NativeCtx&, std::vector<Value>& a, std::string& error) {
    return a[0].truthy() ? Value::null() : fail_test(with_msg("not true: " + a[0].to_json_text(), a, 1), error);
}

// eq(actual, expected[, message])
Value fn_eq(NativeCtx&, std::vector<Value>& a, std::string& error) {
    if (a[0].equals(a[1])) return Value::null();
    return fail_test(with_msg("expected " + a[1].to_json_text() + ", got " + a[0].to_json_text(), a, 2), error);
}

// contains(text, part[, message])
Value fn_contains(NativeCtx&, std::vector<Value>& a, std::string& error) {
    if (a[0].as_str().find(a[1].as_str()) != std::string::npos) return Value::null();
    return fail_test(with_msg("'" + a[1].as_str() + "' not found in " + a[0].to_json_text(), a, 2), error);
}

Value fn_fail(NativeCtx&, std::vector<Value>& a, std::string& error) {
    return fail_test(a[0].to_string(), error);
}

// "http://127.0.0.1:PORT": the instance `lux test` started for the run.
Value fn_base_url(NativeCtx&, std::vector<Value>&, std::string&) {
    return Value::str(script_mode().base_url);
}

} // namespace

LUX_MODULE(test, {
    {"ok",       "x|s",    fn_ok},
    {"eq",       "xx|s",   fn_eq},
    {"contains", "ss|s",   fn_contains},
    {"fail",     "s",      fn_fail},
    {"base_url", ">s",     fn_base_url},
})

} // namespace lux_script
