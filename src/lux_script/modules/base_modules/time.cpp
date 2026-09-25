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

#include <algorithm>
#include <cctype>
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
//
// Optional 3rd argument: a fixed UTC offset, in minutes, applied to the
// timestamp before formatting -- NOT a named tzdata zone (no tzdata
// dependency, and no DST-transition ambiguity to get wrong), and never
// implicit (there is still no server-local timezone anywhere in this
// module, matching the file's own header comment). Before this, a
// deployment where "what the user sees" is not UTC (which is most of
// them) had literally no way to produce that with this module: the
// timestamp itself has to stay UTC (that is the whole point of storing
// one plain int, see the file header), so the offset has to be applied at
// the one place that turns it into display text, on demand, by the
// caller's own explicit choice — `time.format(ts, fmt, 60)` for CET,
// `time.format(ts, fmt, -300)` for US Eastern (standard time), etc.
Value fn_time_format(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) { error = "time.format() expects a timestamp (int, ms since epoch)"; return Value::null(); }
    if (!args[1].is_str()) { error = "time.format() expects a format string"; return Value::null(); }
    if (args.size() > 2 && !args[2].is_int()) {
        error = "time.format(): the UTC offset must be an int (minutes)"; return Value::null();
    }
    long long offset_min = args.size() > 2 ? args[2].as_int() : 0;
    std::time_t secs = static_cast<std::time_t>(args[0].as_int() / 1000 + offset_min * 60);
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

// A hand-rolled scan, not strptime() with one fixed format string: real
// ISO 8601 timestamps a Lux app actually receives vary in ways the single
// "%Y-%m-%dT%H:%M:%SZ" this used to require does not cover, and every
// caller of an API needing to know and match the ONE exact spelling this
// accepts is not a reasonable thing to ask of "parse the standard format".
// Rejected before this fix: milliseconds (JavaScript's own
// `Date.toISOString()` always includes them: "...12:00:00.123Z"), an
// explicit numeric offset ("+02:00"/"-0500", not just "Z"), a date with no
// time at all ("2026-09-23", what an `<input type="date">` submits), and
// a space instead of 'T' (what SQLite's own `CURRENT_TIMESTAMP` column
// default produces: "2026-09-23 12:00:00"). All still resolve to UTC
// milliseconds since the epoch, this module's one time representation
// (the file's own header comment) -- an explicit offset is subtracted out,
// and a timestamp with none (SQLite's, or a date-only one) is treated as
// UTC, the same assumption the rest of this module already makes
// everywhere there is no offset information to go on.
Value fn_time_parse_iso(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "time.parse_iso() expects a string"; return Value::null(); }
    const std::string& s = args[0].as_str();
    size_t pos = 0;

    auto read_int = [&](int width, int& out) -> bool {
        if (pos + static_cast<size_t>(width) > s.size()) return false;
        for (int i = 0; i < width; ++i)
            if (!std::isdigit(static_cast<unsigned char>(s[pos + static_cast<size_t>(i)]))) return false;
        out = std::stoi(s.substr(pos, static_cast<size_t>(width)));
        pos += static_cast<size_t>(width);
        return true;
    };
    auto expect_char = [&](char c) -> bool {
        if (pos >= s.size() || s[pos] != c) return false;
        ++pos;
        return true;
    };

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, ms = 0;
    if (!read_int(4, y) || !expect_char('-') || !read_int(2, mo) ||
        !expect_char('-') || !read_int(2, d))
        return Value::null();

    if (pos < s.size() && (s[pos] == 'T' || s[pos] == ' ')) {
        ++pos;
        if (!read_int(2, h) || !expect_char(':') || !read_int(2, mi) ||
            !expect_char(':') || !read_int(2, se))
            return Value::null();
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            size_t frac_start = pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
            if (pos == frac_start) return Value::null();
            std::string frac = s.substr(frac_start, std::min<size_t>(pos - frac_start, 3));
            while (frac.size() < 3) frac += '0';
            ms = std::stoi(frac);
        }
    }

    long long offset_min = 0;
    if (pos < s.size() && s[pos] == 'Z') {
        ++pos;
    } else if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
        int sign = s[pos] == '-' ? -1 : 1;
        ++pos;
        int oh = 0, om = 0;
        if (!read_int(2, oh)) return Value::null();
        if (pos < s.size() && s[pos] == ':') ++pos;
        if (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])) && !read_int(2, om))
            return Value::null();
        offset_min = sign * (oh * 60 + om);
    }
    // Anything else (nothing at all): no offset in the source string --
    // treated as UTC, see the function comment above.

    if (pos != s.size()) return Value::null(); // trailing garbage
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60)
        return Value::null();

    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = se;
    std::time_t secs = timegm(&tm);
    if (secs == static_cast<std::time_t>(-1)) return Value::null();

    return Value::integer(static_cast<long long>(secs) * 1000 - offset_min * 60 * 1000 + ms);
}

} // namespace

LUX_MODULE(time, {
    {"now",         0, 0, fn_time_now},
    {"now_seconds", 0, 0, fn_time_now_seconds},
    {"format",      2, 3, fn_time_format},
    {"format_iso",  1, 1, fn_time_format_iso},
    {"parse",       2, 2, fn_time_parse},
    {"parse_iso",   1, 1, fn_time_parse_iso},
})

} // namespace lux_script
