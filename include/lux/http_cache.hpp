#pragma once
// Conditional-request helpers shared by the static mounts (app.cpp) and send_file().

#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace lux {

// Is `etag` listed in an If-None-Match value ("*", or a comma list, W/ ignored)?
inline bool etag_listed(std::string_view header, std::string_view etag) {
    if (header == "*") return true;
    while (!header.empty()) {
        const size_t comma = header.find(',');
        std::string_view t = header.substr(0, comma);
        header = comma == std::string_view::npos ? std::string_view() : header.substr(comma + 1);
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
        while (!t.empty() && (t.back()  == ' ' || t.back()  == '\t')) t.remove_suffix(1);
        if (t.substr(0, 2) == "W/") t.remove_prefix(2);
        if (t == etag) return true;
    }
    return false;
}

inline constexpr const char* kHttpDays[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
inline constexpr const char* kHttpMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Days since 1970-01-01 <-> civil date (H. Hinnant's algorithms): no gmtime_r/timegm,
// which Windows lacks, and no locale.
inline long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// IMF-fixdate ("Tue, 03 Nov 2026 10:00:00 GMT").
inline std::string http_date(std::time_t t) {
    long long days = static_cast<long long>(t) / 86400;
    long long rem  = static_cast<long long>(t) % 86400;
    if (rem < 0) { rem += 86400; --days; }
    const long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const long long y = static_cast<long long>(yoe) + era * 400 + (m <= 2);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%s, %02u %s %04lld %02d:%02d:%02d GMT", kHttpDays[((days % 7) + 11) % 7], d,
                  kHttpMonths[m - 1], y, static_cast<int>(rem / 3600), static_cast<int>(rem % 3600 / 60),
                  static_cast<int>(rem % 60));
    return buf;
}

// Parses an IMF-fixdate only (what every client sends); false for anything else.
inline bool parse_http_date(std::string_view s, std::time_t& out) {
    if (s.size() != 29 || s.substr(25) != " GMT" || s[3] != ',') return false;
    int mon = -1;
    for (int i = 0; i < 12; ++i) if (s.substr(8, 3) == kHttpMonths[i]) mon = i;
    unsigned d = 0; int y = 0, h = 0, mi = 0, se = 0;
    const std::string z(s.substr(5));
    if (mon < 0 || std::sscanf(z.c_str(), "%u %*3s %d %d:%d:%d", &d, &y, &h, &mi, &se) != 5) return false;
    out = static_cast<std::time_t>(days_from_civil(y, static_cast<unsigned>(mon) + 1, d) * 86400 + h * 3600 + mi * 60 + se);
    return true;
}

} // namespace lux
