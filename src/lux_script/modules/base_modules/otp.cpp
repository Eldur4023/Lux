// One-time passwords (HOTP RFC 4226, TOTP RFC 6238) for the authenticator
// apps -- Google Authenticator, Aegis, 1Password -- which all speak
// HMAC-SHA1, 6 digits, 30 s steps and a base32 secret.
#include <lux_script/builtin_module.hpp>
#include <lux_script/crypto.hpp>

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace lux_script {

namespace {

constexpr long long kStep = 30;

std::string code_at(const std::string& key, long long counter) {
    std::string msg(8, '\0');
    for (int i = 7; i >= 0; --i, counter >>= 8) msg[i] = static_cast<char>(counter & 0xFF);
    const std::string h = crypto::hmac_sha1(key, msg);
    const size_t o = static_cast<unsigned char>(h[19]) & 0x0F;
    const uint32_t bin = ((uint32_t(uint8_t(h[o])) & 0x7F) << 24) | (uint32_t(uint8_t(h[o + 1])) << 16) |
                         (uint32_t(uint8_t(h[o + 2])) << 8) | uint32_t(uint8_t(h[o + 3]));
    std::string s = std::to_string(bin % 1'000'000);
    return std::string(6 - s.size(), '0') + s;
}

long long now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 160 bits of entropy, base32: what an authenticator app is given.
Value fn_new_secret(NativeCtx&, std::vector<Value>&, std::string& error) {
    std::string raw = crypto::random_bytes(20);
    if (raw.empty()) { error = "otp: could not get random bytes"; return Value::null(); }
    return Value::str(crypto::base32_encode(raw));
}

// Last accepted step per secret (keyed by the secret's SHA-256, never the secret itself).
bool accept_once(const std::string& key, long long matched, long long now_step) {
    static std::mutex m;
    static std::unordered_map<std::string, long long> last;
    const std::string id = crypto::hex_encode(crypto::sha256(key));
    std::lock_guard<std::mutex> lock(m);
    // Entries older than any window can still match are dead weight: drop them so the map stays small.
    if (last.size() > 4096)
        std::erase_if(last, [&](const auto& kv) { return kv.second < now_step - 20; });
    auto [it, fresh] = last.try_emplace(id, matched);
    if (fresh) return true;
    if (matched <= it->second) return false;
    it->second = matched;
    return true;
}

// The current code for a secret; code(secret, unix_seconds) for a given moment.
Value fn_code(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::string key;
    if (!crypto::base32_decode(a[0].as_str(), key) || key.empty()) { error = "otp.code(): invalid base32 secret"; return Value::null(); }
    const long long t = a.size() > 1 ? a[1].as_int() : now_seconds();
    return Value::str(code_at(key, t / kStep));
}

// True if `code` is the code of this 30 s step or of `window` steps either
// side (default 1: tolerates a clock a few seconds off).
Value fn_verify(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::string key;
    if (!crypto::base32_decode(a[0].as_str(), key) || key.empty()) { error = "otp.verify(): invalid base32 secret"; return Value::null(); }
    const long long window = a.size() > 2 ? a[2].as_int() : 1;
    if (window < 0 || window > 10) { error = "otp.verify(): window must be between 0 and 10"; return Value::null(); }
    const long long step = now_seconds() / kStep;
    bool ok = false;
    long long matched = 0;
    // No early exit: the time taken does not say which step matched.
    for (long long d = -window; d <= window; ++d)
        if (crypto::constant_time_equal(code_at(key, step + d), a[1].as_str())) { ok = true; matched = step + d; }
    // RFC 6238 §5.2: a code is accepted once. Without this an observed code (shoulder-surfing, a phishing
    // proxy relaying it) stays valid for the whole window, ~90 s with the default of 1.
    // ponytail: per process and in memory; across several instances or a restart a code can be replayed
    // once more -- keep the last step in the database for that.
    if (ok) ok = accept_once(key, matched, step);
    return Value::boolean(ok);
}

std::string uri_encode(const std::string& s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// otpauth:// URI for the QR code (the app scans it).
Value fn_uri(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string& secret = a[0].as_str();
    const std::string& account = a[1].as_str();
    const std::string issuer = a.size() > 2 ? a[2].as_str() : "";
    std::string label = issuer.empty() ? uri_encode(account) : uri_encode(issuer) + ":" + uri_encode(account);
    std::string u = "otpauth://totp/" + label + "?secret=" + secret;
    if (!issuer.empty()) u += "&issuer=" + uri_encode(issuer);
    return Value::str(u + "&algorithm=SHA1&digits=6&period=30");
}

} // namespace

LUX_MODULE(otp, {
    {"new_secret", ">s",    fn_new_secret},
    {"code",       "s|i>s", fn_code},
    {"verify",     "ss|i>b", fn_verify},
    {"uri",        "ss|s>s", fn_uri},
})

} // namespace lux_script
