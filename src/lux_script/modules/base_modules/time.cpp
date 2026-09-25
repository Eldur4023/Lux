// Time module. A time is a plain `int`: milliseconds since the Unix epoch,
// always UTC -- so the language's own arithmetic and comparisons work on it
// ("5 minutes from now" is `time.now() + 5 * 60 * 1000`). What arithmetic
// cannot do (calendars, formatting, parsing) lives here. Where a function
// takes a UTC offset it is a fixed number of minutes, never an implicit
// server timezone: `time.format(ts, fmt, 60)` is CET.
#include <lux_script/builtin_module.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>

namespace lux_script {

namespace {

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

long long offset_ms(const std::vector<Value>& a, size_t i) {
    return a.size() > i ? a[i].as_int() * 60'000 : 0;
}

// Floor division: a timestamp before 1970 still lands in the right second.
bool to_tm(long long ms, std::tm& tm) {
    std::time_t secs = static_cast<std::time_t>(ms / 1000 - (ms % 1000 < 0));
    return gmtime_r(&secs, &tm) != nullptr;
}

long long from_tm(std::tm& tm, long long ms_part = 0) {
    return static_cast<long long>(timegm(&tm)) * 1000 + ms_part;
}

Value fn_now(NativeCtx&, std::vector<Value>&, std::string&)         { return Value::integer(now_ms()); }
Value fn_now_seconds(NativeCtx&, std::vector<Value>&, std::string&) { return Value::integer(now_ms() / 1000); }

// strftime: every specifier (%Y %m %d %H %M %S %A %B ...) done right.
Value fn_format(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::tm tm{};
    if (!to_tm(a[0].as_int() + offset_ms(a, 2), tm)) { error = "time.format(): timestamp out of range"; return Value::null(); }
    char buf[256];
    const size_t n = std::strftime(buf, sizeof buf, a[1].as_str().c_str(), &tm);
    if (n == 0) { error = "time.format(): the formatted result is too long"; return Value::null(); }
    return Value::str(std::string(buf, n));
}

Value fn_format_iso(NativeCtx& c, std::vector<Value>& a, std::string& e) {
    std::vector<Value> args{a[0], Value::str("%Y-%m-%dT%H:%M:%SZ")};
    return fn_format(c, args, e);
}

// A string that does not match is null, not an error: input from a query
// parameter failing to parse is an outcome a route checks for.
Value fn_parse(NativeCtx&, std::vector<Value>& a, std::string&) {
    std::tm tm{};
    const char* end = strptime(a[0].as_str().c_str(), a[1].as_str().c_str(), &tm);
    if (!end || *end) return Value::null();
    return Value::integer(from_tm(tm));
}

// ISO 8601 the way it really arrives: "2026-09-23", "...T12:00:00Z",
// JavaScript's "...12:00:00.123Z", an offset ("+02:00", "-0500"), or
// SQLite's "2026-09-23 12:00:00". No offset means UTC.
Value fn_parse_iso(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string& s = a[0].as_str();
    size_t pos = 0;
    auto num = [&](int width, int& out) {
        if (pos + width > s.size()) return false;
        out = 0;
        for (int i = 0; i < width; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
            out = out * 10 + (s[pos++] - '0');
        }
        return true;
    };
    auto is = [&](char c) { return pos < s.size() && s[pos] == c && ++pos; };

    int y, mo, d, h = 0, mi = 0, se = 0, ms = 0;
    if (!num(4, y) || !is('-') || !num(2, mo) || !is('-') || !num(2, d)) return Value::null();
    if (is('T') || is(' ')) {
        if (!num(2, h) || !is(':') || !num(2, mi) || !is(':') || !num(2, se)) return Value::null();
        if (is('.')) {
            const size_t start = pos;
            for (int scale = 100; pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])); ++pos, scale /= 10)
                ms += (s[pos] - '0') * scale;
            if (pos == start) return Value::null();
        }
    }
    long long offset = 0;
    if (!is('Z') && pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
        const int sign = s[pos++] == '-' ? -1 : 1;
        int oh, om = 0;
        if (!num(2, oh)) return Value::null();
        is(':');
        if (pos < s.size() && !num(2, om)) return Value::null();
        offset = sign * (oh * 60 + om);
    }
    if (pos != s.size() || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60)
        return Value::null();
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h;        tm.tm_min = mi;     tm.tm_sec  = se;
    return Value::integer(from_tm(tm, ms) - offset * 60'000);
}

// {"year", "month" (1-12), "day", "hour", "minute", "second", "weekday"
// (1 = Monday .. 7 = Sunday, ISO), "yearday" (1-366)}
Value fn_parts(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::tm tm{};
    if (!to_tm(a[0].as_int() + offset_ms(a, 1), tm)) { error = "time.parts(): timestamp out of range"; return Value::null(); }
    Value::Dict d;
    d["year"]    = Value::integer(tm.tm_year + 1900);
    d["month"]   = Value::integer(tm.tm_mon + 1);
    d["day"]     = Value::integer(tm.tm_mday);
    d["hour"]    = Value::integer(tm.tm_hour);
    d["minute"]  = Value::integer(tm.tm_min);
    d["second"]  = Value::integer(tm.tm_sec);
    d["weekday"] = Value::integer(tm.tm_wday == 0 ? 7 : tm.tm_wday);
    d["yearday"] = Value::integer(tm.tm_yday + 1);
    return Value::dict(std::move(d));
}

// The first millisecond of the day/week (Monday)/month/year containing ts,
// in the given offset: "today's orders" is `created >= start_of(now, "day")`.
Value fn_start_of(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const std::string& unit = a[1].as_str();
    const long long off = offset_ms(a, 2);
    std::tm tm{};
    if (!to_tm(a[0].as_int() + off, tm)) { error = "time.start_of(): timestamp out of range"; return Value::null(); }
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    if (unit == "week")       tm.tm_mday -= (tm.tm_wday + 6) % 7;
    else if (unit == "month") tm.tm_mday = 1;
    else if (unit == "year")  tm.tm_mday = 1, tm.tm_mon = 0;
    else if (unit != "day") { error = "time.start_of(): the unit is \"day\", \"week\", \"month\" or \"year\""; return Value::null(); }
    return Value::integer(from_tm(tm) - off);
}

// Calendar months, clamped: Jan 31 + 1 month is Feb 28 (or 29). n may be
// negative.
Value fn_add_months(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const long long ts = a[0].as_int();
    std::tm tm{};
    if (!to_tm(ts, tm)) { error = "time.add_months(): timestamp out of range"; return Value::null(); }
    const long long months = tm.tm_mon + a[1].as_int();
    tm.tm_year += static_cast<int>(months / 12 - (months % 12 < 0));
    tm.tm_mon   = static_cast<int>((months % 12 + 12) % 12);
    const int day = tm.tm_mday;
    std::tm first = tm;
    first.tm_mday = 1;
    first.tm_mon += 1;
    first.tm_hour = first.tm_min = first.tm_sec = 0;
    std::time_t next_month = timegm(&first);
    std::tm last{};
    next_month -= 86400;
    gmtime_r(&next_month, &last);
    tm.tm_mday = std::min(day, last.tm_mday);
    return Value::integer(from_tm(tm, ((ts % 1000) + 1000) % 1000));
}

// "3 minutes ago" / "in 2 hours" relative to now (or to a given `from`);
// time.ago(ts, null, "es") says "hace 3 minutos" / "dentro de 2 horas".
Value fn_ago(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const bool es = a.size() > 2 && a[2].as_str() == "es";
    if (a.size() > 2 && !es && a[2].as_str() != "en") { error = "time.ago(): the language is \"en\" or \"es\""; return Value::null(); }
    const long long from = a.size() > 1 && !a[1].is_null() ? a[1].as_int() : now_ms();
    const long long diff = (from - a[0].as_int()) / 1000, secs = std::llabs(diff);
    struct Unit { long long secs; const char *en, *es; };
    static constexpr Unit kUnits[] = {
        {31536000, "year", "año"}, {2592000, "month", "mes"}, {604800, "week", "semana"},
        {86400, "day", "día"}, {3600, "hour", "hora"}, {60, "minute", "minuto"},
    };
    if (secs < 60) return Value::str(es ? "ahora mismo" : "just now");
    for (const Unit& u : kUnits) {
        if (secs < u.secs) continue;
        const long long n = secs / u.secs;
        std::string word = es ? u.es : u.en;
        if (n != 1) word += es && word.back() != 'a' && word.back() != 'o' ? "es" : "s";
        const std::string count = std::to_string(n) + " " + word;
        if (es) return Value::str((diff >= 0 ? "hace " : "dentro de ") + count);
        return Value::str(diff >= 0 ? count + " ago" : "in " + count);
    }
    return Value::null();
}

} // namespace

LUX_MODULE(time, {
    {"now",         ">i",      fn_now},
    {"now_seconds", ">i",      fn_now_seconds},
    {"format",      "is|i>s",  fn_format},
    {"format_iso",  "i>s",     fn_format_iso},
    {"parse",       "ss",    fn_parse},
    {"parse_iso",   "s",     fn_parse_iso},
    {"parts",       "i|i>d",   fn_parts},
    {"start_of",    "is|i>i",  fn_start_of},
    {"add_months",  "ii>i",    fn_add_months},
    {"ago",         "i|Is>s",  fn_ago},
})

} // namespace lux_script
