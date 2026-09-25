// Math module: what the language's own operators do not cover -- rounding,
// roots, logs, trig, randomness (Python's `math` and `random` together).
// Zero dependencies, stateless.
//
// A result stays an `int` when the input was one and the answer is whole
// (abs(-3) is -3, not -3.0), the same Int-vs-Float choice the language's
// own arithmetic makes.
#include <lux_script/builtin_module.hpp>

#include <algorithm>
#include <cmath>
#include <random>

namespace lux_script {

namespace {

std::mt19937_64& rng() {
    // Seeded once per thread: random_device is a slow entropy source, not
    // something to draw from at request rate.
    thread_local std::mt19937_64 r{std::random_device{}()};
    return r;
}

Value number(double x) { return Value::real(x); }

// One-argument float functions. A finite input with a non-finite answer is
// Python's "math domain error": sqrt(-1), log(0), acos(2).
template <double (*F)(double)>
Value unary(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const double x = a[0].as_float(), r = F(x);
    if (std::isfinite(x) && !std::isfinite(r)) { error = "math domain error"; return Value::null(); }
    return number(r);
}

Value fn_abs(NativeCtx&, std::vector<Value>& a, std::string&) {
    return a[0].is_int() ? Value::integer(std::llabs(a[0].as_int())) : number(std::fabs(a[0].as_float()));
}

Value fn_sign(NativeCtx&, std::vector<Value>& a, std::string&) {
    const double x = a[0].as_float();
    return Value::integer((x > 0) - (x < 0));
}

// min/max take two or more numbers, or one List of them.
Value extreme(std::vector<Value>& a, std::string& error, bool want_max, const char* name) {
    const Value::List& xs = a.size() == 1 && a[0].is_list() ? a[0].as_list() : a;
    if (xs.empty()) { error = std::string("math.") + name + "(): no values"; return Value::null(); }
    const Value* best = nullptr;
    for (const Value& v : xs) {
        if (!v.is_num()) { error = std::string("math.") + name + "(): every value must be a number"; return Value::null(); }
        if (!best || (want_max ? v.as_float() > best->as_float() : v.as_float() < best->as_float())) best = &v;
    }
    return *best;
}
Value fn_min(NativeCtx&, std::vector<Value>& a, std::string& e) { return extreme(a, e, false, "min"); }
Value fn_max(NativeCtx&, std::vector<Value>& a, std::string& e) { return extreme(a, e, true, "max"); }

Value fn_clamp(NativeCtx& c, std::vector<Value>& a, std::string& e) {
    std::vector<Value> lo{a[0], a[1]};
    std::vector<Value> hi{fn_max(c, lo, e), a[2]};
    return fn_min(c, hi, e);
}

// round(x) is an int; round(x, digits) keeps `digits` decimals -- prices.
Value fn_round(NativeCtx&, std::vector<Value>& a, std::string&) {
    const double x = a[0].as_float();
    if (a.size() < 2) return Value::integer(std::llround(x));
    const double f = std::pow(10.0, static_cast<double>(a[1].as_int()));
    return number(std::round(x * f) / f);
}

Value fn_floor(NativeCtx&, std::vector<Value>& a, std::string&) { return Value::integer(static_cast<long long>(std::floor(a[0].as_float()))); }
Value fn_ceil(NativeCtx&, std::vector<Value>& a, std::string&)  { return Value::integer(static_cast<long long>(std::ceil(a[0].as_float()))); }
Value fn_pow(NativeCtx&, std::vector<Value>& a, std::string&)   { return number(std::pow(a[0].as_float(), a[1].as_float())); }
Value fn_atan2(NativeCtx&, std::vector<Value>& a, std::string&) { return number(std::atan2(a[0].as_float(), a[1].as_float())); }
Value fn_hypot(NativeCtx&, std::vector<Value>& a, std::string&) { return number(std::hypot(a[0].as_float(), a[1].as_float())); }
Value fn_pi(NativeCtx&, std::vector<Value>&, std::string&)      { return number(M_PI); }
Value fn_e(NativeCtx&, std::vector<Value>&, std::string&)       { return number(M_E); }

Value fn_log(NativeCtx& c, std::vector<Value>& a, std::string& error) {
    Value r = unary<std::log>(c, a, error);
    if (a.size() < 2 || !error.empty()) return r;
    const double base = std::log(a[1].as_float());
    if (!std::isfinite(base) || base == 0) { error = "math domain error"; return Value::null(); }
    return number(r.as_float() / base);
}

// [0.0, 1.0), like Python's random.random().
Value fn_random(NativeCtx&, std::vector<Value>&, std::string&) {
    return number(std::uniform_real_distribution<double>(0.0, 1.0)(rng()));
}

// Inclusive on both ends: random_int(1, 6) is a die.
Value fn_random_int(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const long long lo = a[0].as_int(), hi = a[1].as_int();
    if (lo > hi) { error = "math.random_int(): the first argument must not be greater than the second"; return Value::null(); }
    return Value::integer(std::uniform_int_distribution<long long>(lo, hi)(rng()));
}

Value fn_choice(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const auto& l = a[0].as_list();
    if (l.empty()) { error = "math.choice(): the List is empty"; return Value::null(); }
    return l[std::uniform_int_distribution<size_t>(0, l.size() - 1)(rng())];
}

// A shuffled copy; sample(l, k) is its first k, no repeats.
Value fn_shuffle(NativeCtx&, std::vector<Value>& a, std::string&) {
    Value::List l = a[0].as_list();
    std::shuffle(l.begin(), l.end(), rng());
    if (a.size() > 1) l.resize(std::min(l.size(), static_cast<size_t>(std::max(0LL, a[1].as_int()))));
    return Value::list(std::move(l));
}

} // namespace

LUX_MODULE(math, {
    {"abs",        "n",   fn_abs},
    {"sign",       "n>i",   fn_sign},
    {"min",        "x*",  fn_min},
    {"max",        "x*",  fn_max},
    {"clamp",      "nnn", fn_clamp},
    {"round",      "n|i", fn_round},
    {"floor",      "n>i",   fn_floor},
    {"ceil",       "n>i",   fn_ceil},
    {"sqrt",       "n>r",   unary<std::sqrt>},
    {"exp",        "n>r",   unary<std::exp>},
    {"log",        "n|n>r", fn_log},
    {"log10",      "n>r",   unary<std::log10>},
    {"pow",        "nn>r",  fn_pow},
    {"sin",        "n>r",   unary<std::sin>},
    {"cos",        "n>r",   unary<std::cos>},
    {"tan",        "n>r",   unary<std::tan>},
    {"asin",       "n>r",   unary<std::asin>},
    {"acos",       "n>r",   unary<std::acos>},
    {"atan",       "n>r",   unary<std::atan>},
    {"atan2",      "nn>r",  fn_atan2},
    {"hypot",      "nn>r",  fn_hypot},
    {"pi",         ">r",    fn_pi},
    {"e",          ">r",    fn_e},
    {"random",     ">r",    fn_random},
    {"random_int", "ii>i",  fn_random_int},
    {"choice",     "l",   fn_choice},
    {"shuffle",    "l>l",   fn_shuffle},
    {"sample",     "li>l",  fn_shuffle},
})

} // namespace lux_script
