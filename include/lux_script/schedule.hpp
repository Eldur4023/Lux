#pragma once
// `every "<spec>":` blocks -- scheduled tasks. A spec is an interval
// ("30s", "5m", "2h", "1d") or a daily UTC time ("03:00"). Shared by the
// parser, which rejects a bad spec at compile time, and the scheduler.
#include <cstdlib>
#include <string>

namespace lux_script {

struct EverySpec {
    long long period_ms = 0;    // an interval, or
    int       daily_min = -1;   // minutes after midnight UTC

    // The next run strictly after `now_ms` (Unix ms).
    long long next(long long now_ms) const {
        if (period_ms > 0) return now_ms + period_ms;
        const long long day = 86'400'000, at = static_cast<long long>(daily_min) * 60'000;
        const long long today = now_ms - now_ms % day + at;
        return today > now_ms ? today : today + day;
    }
};

inline bool parse_every_spec(const std::string& s, EverySpec& out) {
    if (s.size() == 5 && s[2] == ':') {
        const int h = std::atoi(s.substr(0, 2).c_str()), m = std::atoi(s.substr(3).c_str());
        if (s.find_first_not_of("0123456789:") != std::string::npos || h > 23 || m > 59) return false;
        out.daily_min = h * 60 + m;
        return true;
    }
    char* end = nullptr;
    const long long n = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || n < 1 || end[0] == '\0' || end[1] != '\0') return false;
    const long long unit = *end == 's' ? 1'000 : *end == 'm' ? 60'000 : *end == 'h' ? 3'600'000 : *end == 'd' ? 86'400'000 : 0;
    out.period_ms = n * unit;
    return unit > 0;
}

} // namespace lux_script
