// Text helpers every web app ends up writing: URL slugs, truncation,
// number formatting, padding, edit distance. Codepoint-aware (UTF-8), like
// len() and the string methods.
#include <lux_script/builtin_module.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lux_script {

namespace {

// ASCII for U+00C0..U+017F (Latin-1 Supplement + Latin Extended-A); '-' is
// "not a letter". ponytail: one letter per codepoint (Æ -> a, ß -> s);
// a table of strings if "ae"/"ss" ever matter.
constexpr char kFold[] =
    "aaaaaaaceeeeiiiidnooooo-ouuuuytsaaaaaaaceeeeiiiidnooooo-ouuuuyty"
    "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkklllllll"
    "lllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";

static_assert(sizeof(kFold) == 0x180 - 0xC0 + 1, "one entry per codepoint");

// "¡Café con Leche! 2024" -> "cafe-con-leche-2024"
Value fn_slug(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string& s = a[0].as_str();
    std::string out;
    bool dash = false;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = utf8_decode(s, i);
        char c = cp < 0x80 ? static_cast<char>(std::tolower(static_cast<int>(cp)))
               : cp >= 0xC0 && cp <= 0x17F ? kFold[cp - 0xC0] : '-';
        if (!std::isalnum(static_cast<unsigned char>(c))) { dash = !out.empty(); continue; }
        if (dash) out += '-';
        dash = false;
        out += c;
    }
    return Value::str(std::move(out));
}

// At most n characters, suffix included: truncate("Hello world", 8) is
// "Hello w…".
Value fn_truncate(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string& s = a[0].as_str();
    const std::string suffix = a.size() > 2 ? a[2].as_str() : "…";
    const size_t n = static_cast<size_t>(std::max(0LL, a[1].as_int()));
    if (utf8_length(s) <= n) return a[0];
    const size_t keep = n > utf8_length(suffix) ? n - utf8_length(suffix) : 0;
    size_t i = 0;
    for (size_t k = 0; k < keep && i < s.size(); ++k) utf8_decode(s, i);
    std::string out = s.substr(0, i);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return Value::str(out + suffix);
}

// format_number(1234567.891, 2) -> "1,234,567.89";
// format_number(1234567.891, 2, ".", ",") -> "1.234.567,89"
Value fn_format_number(NativeCtx&, std::vector<Value>& a, std::string&) {
    const int decimals = a.size() > 1 ? static_cast<int>(std::clamp(a[1].as_int(), 0LL, 12LL)) : 0;
    const std::string thousands = a.size() > 2 ? a[2].as_str() : ",";
    const std::string point     = a.size() > 3 ? a[3].as_str() : ".";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, std::fabs(a[0].as_float()));
    std::string digits = buf, frac;
    if (const size_t dot = digits.find('.'); dot != std::string::npos) {
        frac = point + digits.substr(dot + 1);
        digits.resize(dot);
    }
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += thousands;
        out += digits[i];
    }
    const bool negative = a[0].as_float() < 0 && out.find_first_not_of("0" + thousands) != std::string::npos;
    return Value::str((negative ? "-" : "") + out + frac);
}

// pad_left("7", 4, "0") -> "0007"; the pad is one character, default " ".
Value pad(std::vector<Value>& a, std::string& error, bool left) {
    const std::string fill = a.size() > 2 ? a[2].as_str() : " ";
    if (utf8_length(fill) != 1) { error = std::string("text.pad_") + (left ? "left" : "right") + "(): the pad must be one character"; return Value::null(); }
    const size_t n = static_cast<size_t>(std::max(0LL, a[1].as_int())), have = utf8_length(a[0].as_str());
    std::string p;
    for (size_t i = have; i < n; ++i) p += fill;
    return Value::str(left ? p + a[0].as_str() : a[0].as_str() + p);
}
Value fn_pad_left(NativeCtx&, std::vector<Value>& a, std::string& e)  { return pad(a, e, true); }
Value fn_pad_right(NativeCtx&, std::vector<Value>& a, std::string& e) { return pad(a, e, false); }

// Levenshtein distance in characters -- "did you mean ...?".
Value fn_distance(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const auto x = utf8_chars(a[0].as_str()), y = utf8_chars(a[1].as_str());
    if (x.size() * y.size() > 10'000'000) { error = "text.distance(): strings too long"; return Value::null(); }
    std::vector<size_t> row(y.size() + 1);
    for (size_t j = 0; j <= y.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= x.size(); ++i) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= y.size(); ++j) {
            const size_t up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (x[i - 1] != y[j - 1])});
            diag = up;
        }
    }
    return Value::integer(static_cast<long long>(row[y.size()]));
}

} // namespace

LUX_MODULE(text, {
    {"slug",          "s",     fn_slug},
    {"truncate",      "si|s",  fn_truncate},
    {"format_number", "n|iss", fn_format_number},
    {"pad_left",      "si|s",  fn_pad_left},
    {"pad_right",     "si|s",  fn_pad_right},
    {"distance",      "ss",    fn_distance},
})

} // namespace lux_script
