#pragma once
// OS-level helpers the LuxScript runtime and its native modules share, so the
// POSIX/Windows split lives in one place instead of #ifdefs in every module.
// Socket and event-loop helpers are NOT here: those are <lux/platform.hpp>.
//
// Every string crossing this header is UTF-8 (that is what LuxScript holds);
// on Windows it is converted to UTF-16 for the Win32 / filesystem calls.
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOGDI
#    define NOGDI   // wingdi.h defines ERROR and friends
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#  include <strings.h>
#  include <unistd.h>
#endif

namespace lux_script::plat {

#ifdef _WIN32
inline std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
inline std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// "The system cannot find the file specified." for a GetLastError() code.
inline std::string win_error(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string msg = n ? narrow(std::wstring_view(buf, n)) : "error " + std::to_string(code);
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ' || msg.back() == '.')) msg.pop_back();
    return msg;
}
#endif

// UTF-8 string <-> filesystem path. On Windows the string constructor of
// fs::path would use the ANSI code page, so go through UTF-16; paths come
// back with '/' separators (every Win32 API accepts them, and scripts see
// the same separator on both systems).
inline std::filesystem::path to_path(const std::string& s) {
#ifdef _WIN32
    return std::filesystem::path(widen(s));
#else
    return std::filesystem::path(s);
#endif
}
inline std::string from_path(const std::filesystem::path& p) {
#ifdef _WIN32
    return narrow(p.generic_wstring());
#else
    return p.string();
#endif
}

// getenv() that is UTF-8 on Windows (the CRT's narrow environment is ANSI).
// Returns false when the variable is unset.
inline bool get_env(const char* name, std::string& out) {
#ifdef _WIN32
    const wchar_t* v = _wgetenv(widen(name).c_str());
    if (!v) return false;
    out = narrow(v);
    return true;
#else
    const char* v = std::getenv(name);
    if (!v) return false;
    out = v;
    return true;
#endif
}

// The user's home directory ("" if unknown): $HOME, or %USERPROFILE% on Windows.
inline std::string home_dir() {
    std::string h;
#ifdef _WIN32
    if (get_env("USERPROFILE", h) && !h.empty()) return h;
#endif
    get_env("HOME", h);
    return h;
}

// Per-user application data root: %APPDATA% on Windows, $XDG_CONFIG_HOME or ~/.config elsewhere.
inline std::string config_dir() {
    std::string d;
#ifdef _WIN32
    if (get_env("APPDATA", d) && !d.empty()) return d;
    const std::string h = home_dir();
    return h.empty() ? h : h + "/AppData/Roaming";
#else
    if (get_env("XDG_CONFIG_HOME", d) && !d.empty()) return d;
    const std::string h = home_dir();
    return h.empty() ? h : h + "/.config";
#endif
}

// Path of the running executable ("" if unknown).
inline std::string exe_path() {
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); return narrow(buf); }
        buf.resize(buf.size() * 2);
    }
#else
    std::error_code ec;
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
#endif
}

// Names the calling thread for debuggers and top -H (best effort; Linux limits it to 15 chars).
inline void set_thread_name(const char* name) {
#ifdef _WIN32
    using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);   // Windows 10 1607+, so looked up at run time
    static const auto set_desc = reinterpret_cast<SetDesc>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set_desc) set_desc(GetCurrentThread(), widen(name).c_str());
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

// strncasecmp (ASCII case folding).
inline int strncasecmp(const char* a, const char* b, size_t n) {
#ifdef _WIN32
    return _strnicmp(a, b, n);
#else
    return ::strncasecmp(a, b, n);
#endif
}

// ─── time ───────────────────────────────────────────────────────────────────

inline bool gmtime(std::time_t t, std::tm& out) {
#ifdef _WIN32
    const __time64_t t64 = t;
    return _gmtime64_s(&out, &t64) == 0;
#else
    return gmtime_r(&t, &out) != nullptr;
#endif
}

inline std::time_t timegm(std::tm& tm) {
#ifdef _WIN32
    return static_cast<std::time_t>(_mkgmtime64(&tm));
#else
    return ::timegm(&tm);
#endif
}

// strptime for the specifiers a date format realistically uses; Windows has
// no strptime. Same contract as POSIX: pointer past the match, or nullptr.
// Unsupported: %U %W %w %u %C %g %G %V %E* %O* (a format with them fails).
inline const char* strptime_lite(const char* s, const char* f, std::tm* tm) {
    static const char* const kMon[12] = {"january", "february", "march", "april", "may", "june", "july",
                                         "august", "september", "october", "november", "december"};
    static const char* const kDay[7] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
    auto skip_ws = [&] { while (std::isspace(static_cast<unsigned char>(*s))) ++s; };
    auto num = [&](int lo, int hi, int& out) {
        skip_ws();
        if (!std::isdigit(static_cast<unsigned char>(*s))) return false;
        int v = 0, digits = 0;
        const int max_digits = hi > 999 ? 4 : hi > 99 ? 3 : 2;
        while (digits < max_digits && std::isdigit(static_cast<unsigned char>(*s))) { v = v * 10 + (*s++ - '0'); ++digits; }
        out = v;
        return v >= lo && v <= hi;
    };
    // A full or 3-letter abbreviated name, case-insensitive.
    auto name = [&](const char* const* names, int count, int& out) {
        for (int i = 0; i < count; ++i) {
            const size_t full = std::strlen(names[i]);
            for (size_t len : {full, size_t{3}}) {
                size_t k = 0;
                while (k < len && s[k] && std::tolower(static_cast<unsigned char>(s[k])) == names[i][k]) ++k;
                if (k == len) { s += len; out = i; return true; }
            }
        }
        return false;
    };
    bool pm = false, have_pm = false;
    int hour12 = -1;
    for (; *f; ++f) {
        if (std::isspace(static_cast<unsigned char>(*f))) { skip_ws(); continue; }
        if (*f != '%') { if (*s++ != *f) return nullptr; continue; }
        ++f;
        int v = 0;
        switch (*f) {
        case '%': if (*s++ != '%') return nullptr; break;
        case 'n': case 't': skip_ws(); break;
        case 'Y': if (!num(0, 9999, v)) return nullptr; tm->tm_year = v - 1900; break;
        case 'y': if (!num(0, 99, v)) return nullptr; tm->tm_year = v < 69 ? v + 100 : v; break;
        case 'm': if (!num(1, 12, v)) return nullptr; tm->tm_mon = v - 1; break;
        case 'd': case 'e': if (!num(1, 31, v)) return nullptr; tm->tm_mday = v; break;
        case 'H': case 'k': if (!num(0, 23, v)) return nullptr; tm->tm_hour = v; break;
        case 'I': case 'l': if (!num(1, 12, v)) return nullptr; hour12 = v % 12; break;
        case 'M': if (!num(0, 59, v)) return nullptr; tm->tm_min = v; break;
        case 'S': if (!num(0, 61, v)) return nullptr; tm->tm_sec = v; break;
        case 'j': if (!num(1, 366, v)) return nullptr; tm->tm_yday = v - 1; break;
        case 'b': case 'B': case 'h': skip_ws(); if (!name(kMon, 12, v)) return nullptr; tm->tm_mon = v; break;
        case 'a': case 'A': skip_ws(); if (!name(kDay, 7, v)) return nullptr; tm->tm_wday = v; break;
        case 'p': case 'P':
            skip_ws();
            if ((s[0] == 'A' || s[0] == 'a' || s[0] == 'P' || s[0] == 'p') && (s[1] == 'M' || s[1] == 'm')) {
                pm = s[0] == 'P' || s[0] == 'p'; have_pm = true; s += 2;
            } else return nullptr;
            break;
        case 'T': s = strptime_lite(s, "%H:%M:%S", tm); if (!s) return nullptr; break;
        case 'R': s = strptime_lite(s, "%H:%M", tm); if (!s) return nullptr; break;
        case 'D': s = strptime_lite(s, "%m/%d/%y", tm); if (!s) return nullptr; break;
        case 'F': s = strptime_lite(s, "%Y-%m-%d", tm); if (!s) return nullptr; break;
        case 'z': {   // +hh:mm, +hhmm, +hh or Z; accepted and ignored like glibc's tm (no tm_gmtoff use here)
            skip_ws();
            if (*s == 'Z') { ++s; break; }
            if (*s != '+' && *s != '-') return nullptr;
            ++s;
            int digits = 0;
            while (digits < 4 && (std::isdigit(static_cast<unsigned char>(*s)) || (*s == ':' && digits == 2))) { if (*s != ':') ++digits; ++s; }
            if (digits != 2 && digits != 4) return nullptr;
            break;
        }
        case 'Z': while (std::isalpha(static_cast<unsigned char>(*s))) ++s; break;
        default: return nullptr;
        }
    }
    if (hour12 >= 0) tm->tm_hour = hour12 + (have_pm && pm ? 12 : 0);
    else if (have_pm && pm && tm->tm_hour < 12) tm->tm_hour += 12;
    return s;
}

// fnmatch(pattern, name, FNM_PERIOD): * ? [set] [!set] \\x, and a leading '.' of the name
// only matches a literal '.' in the pattern. Windows has no fnmatch.
inline bool fnmatch_match(const char* p, const char* s) {
    for (;; ++p, ++s) {
        switch (*p) {
        case '\0': return *s == '\0';
        case '*':
            while (p[1] == '*') ++p;
            if (p[1] == '\0') return true;
            for (const char* t = s;; ++t) {
                if (fnmatch_match(p + 1, t)) return true;
                if (!*t) return false;
            }
        case '?': if (!*s) return false; break;
        case '[': {
            if (!*s) return false;
            const char* q = p + 1;
            const bool neg = *q == '!' || *q == '^';
            if (neg) ++q;
            bool hit = false, closed = false;
            for (bool first = true; *q; first = false, ++q) {
                if (*q == ']' && !first) { closed = true; break; }
                const char lo = *q;
                if (q[1] == '-' && q[2] && q[2] != ']') { hit |= *s >= lo && *s <= q[2]; q += 2; }
                else hit |= *s == lo;
            }
            if (!closed) { if (*s != '[') return false; break; }   // no closing ']': a literal '['
            if (hit == neg) return false;
            p = q;
            break;
        }
        case '\\': if (p[1]) ++p; [[fallthrough]];
        default: if (*p != *s) return false; break;
        }
    }
}
inline bool fnmatch_period(const char* pattern, const char* name) {
    if (*name == '.' && *pattern != '.') return false;
    return fnmatch_match(pattern, name);
}

inline const char* strptime(const char* s, const char* f, std::tm* tm) {
#ifdef _WIN32
    return strptime_lite(s, f, tm);
#else
    return ::strptime(s, f, tm);
#endif
}

} // namespace lux_script::plat
