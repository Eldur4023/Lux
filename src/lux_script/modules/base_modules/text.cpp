// Text helpers every web app ends up writing: URL slugs, truncation,
// number formatting, padding, edit distance. Codepoint-aware (UTF-8), like
// len() and the string methods.
#include <lux_script/builtin_module.hpp>

#include <algorithm>
#include <cmath>
#include <charconv>

namespace lux_script {

namespace {

// ASCII for U+00C0..U+017F (Latin-1 Supplement + Latin Extended-A); '-' is
// "not a letter". The few that become two letters are in kTwo.
constexpr char kFold[] =
    "aaaaaaaceeeeiiiidnooooo-ouuuuytsaaaaaaaceeeeiiiidnooooo-ouuuuyty"
    "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkklllllll"
    "lllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";

static_assert(sizeof(kFold) == 0x180 - 0xC0 + 1, "one entry per codepoint");

constexpr std::pair<uint32_t, const char*> kTwo[] = {
    {0xC6, "ae"}, {0xE6, "ae"}, {0xDF, "ss"}, {0xDE, "th"}, {0xFE, "th"},
    {0x152, "oe"}, {0x153, "oe"}, {0x132, "ij"}, {0x133, "ij"},
};

// "¡Café con Leche! 2024" -> "cafe-con-leche-2024"
Value fn_slug(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string& s = a[0].as_str();
    std::string out;
    bool dash = false;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = utf8_decode(s, i);
        std::string c(1, cp < 0x80 ? static_cast<char>(std::tolower(static_cast<int>(cp)))
                        : cp >= 0xC0 && cp <= 0x17F ? kFold[cp - 0xC0] : '-');
        for (const auto& [from, to] : kTwo) if (cp == from) c = to;
        if (!std::isalnum(static_cast<unsigned char>(c[0]))) { dash = !out.empty(); continue; }
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
//
// Rounded half-up in decimal, on the shortest text that reads back as the
// same number -- what the user wrote -- not on the binary double: 2.675 is
// "2.68", where printf says "2.67". A decimal string ("1234.565") is taken
// as is, for amounts that should never touch a float at all.
Value fn_format_number(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const size_t decimals = a.size() > 1 ? static_cast<size_t>(std::clamp(a[1].as_int(), 0LL, 12LL)) : 0;
    const std::string thousands = a.size() > 2 ? a[2].as_str() : ",";
    const std::string point     = a.size() > 3 ? a[3].as_str() : ".";
    std::string num;
    if (a[0].is_int()) num = std::to_string(a[0].as_int());
    else if (a[0].is_float()) {
        char buf[400];
        const auto r = std::to_chars(buf, buf + sizeof buf, a[0].as_float(), std::chars_format::fixed);
        num.assign(buf, r.ptr);
    } else num = a[0].as_str();
    const bool negative = !num.empty() && num[0] == '-';
    if (negative) num.erase(0, 1);
    const size_t dot = num.find('.');
    std::string whole = num.substr(0, dot), frac = dot == std::string::npos ? "" : num.substr(dot + 1);
    if (whole.empty() || whole.find_first_not_of("0123456789") != std::string::npos ||
        frac.find_first_not_of("0123456789") != std::string::npos) {
        error = "text.format_number(): '" + num + "' is not a number";
        return Value::null();
    }
    // Round half-up at `decimals`, carrying into the whole part if needed.
    const bool up = frac.size() > decimals && frac[decimals] >= '5';
    frac.resize(decimals, '0');
    std::string digits = whole + frac;
    for (size_t i = digits.size(); up && i-- > 0;) {
        if (digits[i] != '9') { ++digits[i]; break; }
        digits[i] = '0';
        if (i == 0) digits.insert(0, "1");
    }
    whole = digits.substr(0, digits.size() - decimals);
    frac  = digits.substr(digits.size() - decimals);
    std::string out;
    for (size_t i = 0; i < whole.size(); ++i) {
        if (i && (whole.size() - i) % 3 == 0) out += thousands;
        out += whole[i];
    }
    const bool zero = digits.find_first_not_of('0') == std::string::npos;
    return Value::str((negative && !zero ? "-" : "") + out + (decimals ? point + frac : ""));
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
    {"slug",          "s>s",     fn_slug},
    {"truncate",      "si|s>s",  fn_truncate},
    {"format_number", "x|iss>s", fn_format_number},
    {"pad_left",      "si|s>s",  fn_pad_left},
    {"pad_right",     "si|s>s",  fn_pad_right},
    {"distance",      "ss>i",    fn_distance},
})

} // namespace lux_script
