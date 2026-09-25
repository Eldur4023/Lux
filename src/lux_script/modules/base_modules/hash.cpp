// Hashing, signing and randomness for Lux Script, over crypto.hpp (the same
// SHA-256/HMAC that signs session cookies and JWTs). Digests come back hex:
// raw bytes are not valid UTF-8, so they cannot travel as a Lux `string`.
#include <lux_script/builtin_module.hpp>
#include <lux_script/crypto.hpp>

#include <chrono>

namespace lux_script {

namespace {

// OWASP's 2023 figure for PBKDF2-HMAC-SHA256; ~0.3s here, on purpose.
constexpr unsigned kPasswordIterations = 600'000;

std::string random_or_fail(size_t n, std::string& error) {
    std::string raw = crypto::random_bytes(n);
    if (raw.empty()) error = "hash: could not get random bytes";
    return raw;
}

long long now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Value fn_sha256(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::str(crypto::hex_encode(crypto::sha256(a[0].as_str())));
}

Value fn_hmac_sha256(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::str(crypto::hex_encode(crypto::hmac_sha256(a[0].as_str(), a[1].as_str())));
}

// Constant time: comparing a signature with == tells an attacker, through
// the response time, how many leading bytes they already got right.
Value fn_equal(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::boolean(crypto::constant_time_equal(a[0].as_str(), a[1].as_str()));
}

Value fn_random_hex(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const long long n = a[0].as_int();
    if (n < 1 || n > 1024) { error = "hash.random_hex(): n must be between 1 and 1024"; return Value::null(); }
    std::string raw = random_or_fail(static_cast<size_t>(n), error);
    return error.empty() ? Value::str(crypto::hex_encode(raw)) : Value::null();
}

// URL-safe random token -- API keys, reset links, CSRF tokens. n bytes of
// entropy (default 32), base64url text.
Value fn_token(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const long long n = a.empty() ? 32 : a[0].as_int();
    if (n < 16 || n > 256) { error = "hash.token(): n must be between 16 and 256"; return Value::null(); }
    std::string raw = random_or_fail(static_cast<size_t>(n), error);
    return error.empty() ? Value::str(crypto::base64url_encode(raw)) : Value::null();
}

// uuid() is a random v4; uuid(7) puts the Unix time in ms in front (RFC
// 9562), so ids sort by creation -- kinder to a database index than v4.
Value fn_uuid(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const long long version = a.empty() ? 4 : a[0].as_int();
    if (version != 4 && version != 7) { error = "hash.uuid(): version must be 4 or 7"; return Value::null(); }
    std::string b = random_or_fail(16, error);
    if (!error.empty()) return Value::null();
    if (version == 7) {
        const auto ms = static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        for (int i = 0; i < 6; ++i) b[i] = static_cast<char>((ms >> (40 - 8 * i)) & 0xFF);
    }
    b[6] = static_cast<char>((b[6] & 0x0F) | (version << 4));
    b[8] = static_cast<char>((b[8] & 0x3F) | 0x80);
    std::string h = crypto::hex_encode(b);
    return Value::str(h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" +
                      h.substr(16, 4) + "-" + h.substr(20));
}

// pbkdf2_sha256$<iterations>$<salt>$<base64 hash> -- Django's format, so a
// hash moves between the two as is. The iteration count travels with it:
// raising kPasswordIterations later leaves the old hashes verifiable.
Value fn_password(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::string salt = random_or_fail(16, error);
    if (!error.empty()) return Value::null();
    salt = crypto::base64url_encode(salt);
    const std::string dk = crypto::pbkdf2_sha256(a[0].as_str(), salt, kPasswordIterations, 32);
    return Value::str("pbkdf2_sha256$" + std::to_string(kPasswordIterations) + "$" + salt + "$" +
                      crypto::base64_encode(dk));
}

Value fn_verify(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string& stored = a[1].as_str();
    const size_t p1 = stored.find('$'), p2 = stored.find('$', p1 + 1), p3 = stored.find('$', p2 + 1);
    if (p3 == std::string::npos || stored.compare(0, p1, "pbkdf2_sha256") != 0) return Value::boolean(false);
    const unsigned long iters = std::strtoul(stored.c_str() + p1 + 1, nullptr, 10);
    std::string want;
    if (iters < 1 || iters > 10'000'000 || !crypto::base64url_decode(stored.substr(p3 + 1), want) || want.empty())
        return Value::boolean(false);
    const std::string got = crypto::pbkdf2_sha256(a[0].as_str(), stored.substr(p2 + 1, p3 - p2 - 1),
                                                  static_cast<unsigned>(iters), want.size());
    return Value::boolean(crypto::constant_time_equal(got, want));
}

// A tamper-proof token carrying any value -- email verification and
// password-reset links, "unsubscribe" URLs -- in the style of Python's
// itsdangerous: <payload>.<issued at>.<signature>, all URL-safe.
std::string signature(const std::string& key, const std::string& body) {
    return crypto::base64url_encode(crypto::hmac_sha256(key, body));
}

Value fn_sign(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string body = crypto::base64url_encode(a[0].to_json_text()) + "." + std::to_string(now_seconds());
    return Value::str(body + "." + signature(a[1].as_str(), body));
}

// The value, or null if the token was tampered with or is older than
// max_age seconds (when given).
Value fn_unsign(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string& token = a[0].as_str();
    const size_t dot = token.rfind('.');
    if (dot == std::string::npos) return Value::null();
    const std::string body = token.substr(0, dot);
    if (!crypto::constant_time_equal(token.substr(dot + 1), signature(a[1].as_str(), body)))
        return Value::null();
    const size_t mid = body.find('.');
    if (a.size() > 2 && now_seconds() - std::atoll(body.c_str() + mid + 1) > a[2].as_int())
        return Value::null();
    std::string json;
    Value out;
    if (!crypto::base64url_decode(body.substr(0, mid), json) || !Value::parse_json(json, out))
        return Value::null();
    return out;
}

} // namespace

LUX_MODULE(hash, {
    {"sha256",      "s",   fn_sha256},
    {"hmac_sha256", "ss",  fn_hmac_sha256},
    {"equal",       "ss",  fn_equal},
    {"random_hex",  "i",   fn_random_hex},
    {"token",       "|i",  fn_token},
    {"uuid",        "|i",  fn_uuid},
    {"sign",        "xs",  fn_sign},
    {"unsign",      "ss|i", fn_unsign},
    // CPU-bound by design: off the event loop.
    {"password",    "s",   fn_password, /*is_async=*/true},
    {"verify",      "ss",  fn_verify,   /*is_async=*/true},
})

} // namespace lux_script
