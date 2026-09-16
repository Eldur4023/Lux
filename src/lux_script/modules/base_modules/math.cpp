// Math module (NATIVE-MODULES.md): the arithmetic Lux Script's own
// operators do not cover -- sqrt, pow, rounding, trig, randomness. Follows
// hash.cpp's shape exactly: zero dependencies (<cmath> + <random>),
// unconditionally compiled in, stateless.
//
// abs()/min()/max() are deliberately HERE, not core builtins the way len()/
// str() are: Python splits the same way (abs/min/max are builtins, sqrt/pow
// live in `math`) but Lux has no strong reason to special-case three
// functions into the core dispatch table when `import math` costs one line
// and keeps every arithmetic helper in one, discoverable place.
#include <lux_script/builtin_module.hpp>

#include <cmath>
#include <random>

namespace lux_script {

namespace {

bool num_arg(std::vector<Value>& args, size_t i, double& out) {
    if (i >= args.size() || !args[i].is_num()) return false;
    out = args[i].as_float();
    return true;
}

// Whole-number results come back as `int` when the input was exact and
// integral (abs(-3) -> -3 as an int, not -3.0), matching how the language's
// own arithmetic already decides Int-vs-Float case by case (GUIDE.md,
// division) rather than fixing every math function to always return one or
// the other regardless of input.
Value fn_math_abs(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_num()) { error = "math.abs() expects a number"; return Value::null(); }
    if (args[0].is_int()) return Value::integer(std::llabs(args[0].as_int()));
    return Value::real(std::fabs(args[0].as_float()));
}

Value fn_math_min(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_num() || !args[1].is_num()) { error = "math.min() expects two numbers"; return Value::null(); }
    if (args[0].is_int() && args[1].is_int())
        return Value::integer(std::min(args[0].as_int(), args[1].as_int()));
    return Value::real(std::min(args[0].as_float(), args[1].as_float()));
}

Value fn_math_max(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_num() || !args[1].is_num()) { error = "math.max() expects two numbers"; return Value::null(); }
    if (args[0].is_int() && args[1].is_int())
        return Value::integer(std::max(args[0].as_int(), args[1].as_int()));
    return Value::real(std::max(args[0].as_float(), args[1].as_float()));
}

Value fn_math_round(NativeCtx&, std::vector<Value>& args, std::string& error) {
    double x; if (!num_arg(args, 0, x)) { error = "math.round() expects a number"; return Value::null(); }
    return Value::integer(static_cast<long long>(std::llround(x)));
}

Value fn_math_floor(NativeCtx&, std::vector<Value>& args, std::string& error) {
    double x; if (!num_arg(args, 0, x)) { error = "math.floor() expects a number"; return Value::null(); }
    return Value::integer(static_cast<long long>(std::floor(x)));
}

Value fn_math_ceil(NativeCtx&, std::vector<Value>& args, std::string& error) {
    double x; if (!num_arg(args, 0, x)) { error = "math.ceil() expects a number"; return Value::null(); }
    return Value::integer(static_cast<long long>(std::ceil(x)));
}

Value fn_math_sqrt(NativeCtx&, std::vector<Value>& args, std::string& error) {
    double x; if (!num_arg(args, 0, x)) { error = "math.sqrt() expects a number"; return Value::null(); }
    if (x < 0) { error = "math.sqrt(): argument must not be negative"; return Value::null(); }
    return Value::real(std::sqrt(x));
}

Value fn_math_pow(NativeCtx&, std::vector<Value>& args, std::string& error) {
    double base, exp;
    if (!num_arg(args, 0, base) || !num_arg(args, 1, exp)) { error = "math.pow() expects two numbers"; return Value::null(); }
    return Value::real(std::pow(base, exp));
}

Value fn_math_log(NativeCtx&, std::vector<Value>& args, std::string& error) {
    double x; if (!num_arg(args, 0, x)) { error = "math.log() expects a number"; return Value::null(); }
    if (x <= 0) { error = "math.log(): argument must be positive"; return Value::null(); }
    return Value::real(std::log(x));
}

// [0.0, 1.0) -- matching Python's random.random() range exactly (a closed
// lower bound, open upper bound), the convention most languages agree on.
// std::random_device seeds a thread-local generator once per thread rather
// than reading from it on every call: it is a real (if slow) entropy
// source, not meant to be drawn from at request-serving rates.
Value fn_math_random(NativeCtx&, std::vector<Value>&, std::string&) {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return Value::real(dist(rng));
}

// Inclusive on both ends -- math.random_int(1, 6) can return 6, matching
// the die-roll intuition "between 1 and 6" carries, unlike a half-open
// range that would need random_int(1, 7) for the same result.
Value fn_math_random_int(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int() || !args[1].is_int()) { error = "math.random_int() expects two ints"; return Value::null(); }
    long long lo = args[0].as_int(), hi = args[1].as_int();
    if (lo > hi) { error = "math.random_int(): the first argument must not be greater than the second"; return Value::null(); }
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<long long> dist(lo, hi);
    return Value::integer(dist(rng));
}

class MathModule : public BuiltinModule {
public:
    const char* name() const override { return "math"; }

    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"abs",        1, 1, fn_math_abs},
            {"min",        2, 2, fn_math_min},
            {"max",        2, 2, fn_math_max},
            {"round",      1, 1, fn_math_round},
            {"floor",      1, 1, fn_math_floor},
            {"ceil",       1, 1, fn_math_ceil},
            {"sqrt",       1, 1, fn_math_sqrt},
            {"pow",        2, 2, fn_math_pow},
            {"log",        1, 1, fn_math_log},
            {"random",     0, 0, fn_math_random},
            {"random_int", 2, 2, fn_math_random_int},
        };
        return fns;
    }
};

} // namespace

LUX_REGISTER_MODULE(MathModule)

} // namespace lux_script
