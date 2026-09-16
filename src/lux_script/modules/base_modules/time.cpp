// Time module (NATIVE-MODULES.md): the current time, formatting, and
// parsing -- Lux Script had no way to answer "what time is it" at all
// before this. Zero dependencies (<ctime>/<chrono>, both already used
// elsewhere in the project), unconditionally compiled in, stateless.
//
// Time itself is a plain `int`: milliseconds since the Unix epoch, UTC
// always (no local-timezone anything -- a server has no reliable notion of
// "the user's timezone" without it being told, and UTC-everywhere avoids an
// entire class of DST/offset bugs a web backend has no business getting
// wrong). Representing it as a plain int rather than inventing a Time
// class means every arithmetic operation the language already has (+, -,
// comparisons) works on a timestamp for free -- "5 minutes from now" is
// just `time.now() + 5 * 60 * 1000`, not a method this module has to
// separately provide and someone has to separately learn.
#include <lux_script/builtin_module.hpp>

#include <chrono>
#include <cstring>
#include <ctime>

namespace lux_script {

namespace {

using Clock = std::chrono::system_clock;

long long epoch_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
}

Value fn_time_now(NativeCtx&, std::vector<Value>&, std::string&) {
    return Value::integer(epoch_ms_now());
}

Value fn_time_now_seconds(NativeCtx&, std::vector<Value>&, std::string&) {
    return Value::integer(epoch_ms_now() / 1000);
}

// strftime, not a hand-rolled formatter: it already knows every format
// specifier (%Y %m %d %H %M %S %A %B ...) correctly, including locale-
// dependent ones this module has no business reimplementing.
Value fn_time_format(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) { error = "time.format() expects a timestamp (int, ms since epoch)"; return Value::null(); }
    if (!args[1].is_str()) { error = "time.format() expects a format string"; return Value::null(); }
    std::time_t secs = static_cast<std::time_t>(args[0].as_int() / 1000);
    std::tm tm{};
    if (!gmtime_r(&secs, &tm)) { error = "time.format(): timestamp out of range"; return Value::null(); }
    char buf[256];
    size_t n = std::strftime(buf, sizeof(buf), args[1].as_str().c_str(), &tm);
    if (n == 0) { error = "time.format(): the formatted result is too long"; return Value::null(); }
    return Value::str(std::string(buf, n));
}

Value fn_time_format_iso(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) { error = "time.format_iso() expects a timestamp (int, ms since epoch)"; return Value::null(); }
    std::vector<Value> fmt_args = { args[0], Value::str("%Y-%m-%dT%H:%M:%SZ") };
    return fn_time_format(ctx, fmt_args, error);
}

// Absence (a string that does not match `fmt`) is null, not an error: the
// same "the caller decides what a failed lookup means" convention
// os.cpp already uses for a missing file -- a timestamp parsed from
// untrusted input (a query parameter, a header) failing to parse is a
// routine, expected outcome a route should be able to check for, not a
// 500 the caller cannot react to.
Value fn_time_parse(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "time.parse() expects a string"; return Value::null(); }
    if (!args[1].is_str()) { error = "time.parse() expects a format string"; return Value::null(); }
    std::tm tm{};
    const std::string& s = args[0].as_str();
    char* end = strptime(s.c_str(), args[1].as_str().c_str(), &tm);
    if (!end || *end != '\0') return Value::null();
    // timegm(), not mktime(): mktime() interprets `tm` in the LOCAL
    // timezone, which is exactly the ambiguity this whole module exists to
    // avoid -- every timestamp here is UTC, in and out.
    std::time_t secs = timegm(&tm);
    if (secs == static_cast<std::time_t>(-1)) return Value::null();
    return Value::integer(static_cast<long long>(secs) * 1000);
}

Value fn_time_parse_iso(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "time.parse_iso() expects a string"; return Value::null(); }
    std::vector<Value> fmt_args = { args[0], Value::str("%Y-%m-%dT%H:%M:%SZ") };
    return fn_time_parse(ctx, fmt_args, error);
}

class TimeModule : public BuiltinModule {
public:
    const char* name() const override { return "time"; }

    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"now",         0, 0, fn_time_now},
            {"now_seconds", 0, 0, fn_time_now_seconds},
            {"format",      2, 2, fn_time_format},
            {"format_iso",  1, 1, fn_time_format_iso},
            {"parse",       2, 2, fn_time_parse},
            {"parse_iso",   1, 1, fn_time_parse_iso},
        };
        return fns;
    }
};

} // namespace

LUX_REGISTER_MODULE(TimeModule)

} // namespace lux_script
