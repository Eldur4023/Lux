// The first native module (NATIVE-MODULES.md): proof that the mechanism
// works before anything with an external dependency (a PDF library, say)
// is added on top of it. Every function here is a thin wrapper over
// crypto.hpp, which already existed for signing session cookies and JWTs --
// this module's only job is exposing that to Lux Script under `import
// hash`, hex-encoded (crypto::sha256/hmac_sha256 return raw bytes, and a raw
// byte string is not valid UTF-8, so it cannot travel as a Lux `string`
// as-is).
#include <lux_script/builtin_module.hpp>
#include <lux_script/crypto.hpp>

namespace lux_script {

namespace {

Value fn_hash_sha256(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "hash.sha256() expects a string"; return Value::null(); }
    return Value::str(crypto::hex_encode(crypto::sha256(args[0].as_str())));
}

Value fn_hash_hmac_sha256(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str() || !args[1].is_str()) {
        error = "hash.hmac_sha256() expects two strings: key, message";
        return Value::null();
    }
    return Value::str(crypto::hex_encode(crypto::hmac_sha256(args[0].as_str(), args[1].as_str())));
}

Value fn_hash_random_hex(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) { error = "hash.random_hex() expects an int"; return Value::null(); }
    long long n = args[0].as_int();
    if (n < 1 || n > 1024) {
        error = "hash.random_hex(): n must be between 1 and 1024";
        return Value::null();
    }
    std::string raw = crypto::random_bytes(static_cast<size_t>(n));
    if (raw.empty()) { error = "hash.random_hex(): could not get random bytes"; return Value::null(); }
    return Value::str(crypto::hex_encode(raw));
}

class HashModule : public BuiltinModule {
public:
    const char* name() const override { return "hash"; }

    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"sha256",      1, 1, fn_hash_sha256},
            {"hmac_sha256", 2, 2, fn_hash_hmac_sha256},
            {"random_hex",  1, 1, fn_hash_random_hex},
        };
        return fns;
    }
};

} // namespace

LUX_REGISTER_MODULE(HashModule)

} // namespace lux_script
