// Encryption and public-key signatures, through OpenSSL's libcrypto --
// never hand-rolled: AES and RSA written from scratch leak through timing.
//
//   crypto.encrypt / decrypt   AES-256-GCM: confidential and tamper-evident
//   crypto.sign / verify       RS*, PS*, ES256/ES384, EdDSA
//   crypto.jwt_verify / jwt_sign   asymmetric JWTs -- "Sign in with Google"
//
// jwt_verify refuses alg "none" and any alg that does not fit the key's
// type (the RS256 -> HS256 confusion attack), and checks exp/nbf/aud/iss.
#include <lux_script/builtin_module.hpp>
#include <lux_script/crypto.hpp>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <chrono>
#include <memory>

namespace lux_script {

namespace {

using PKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
PKey no_key() { return PKey(nullptr, EVP_PKEY_free); }

std::string b64(std::string_view raw) { return crypto::base64url_encode(raw); }
bool unb64(const std::string& text, std::string& out) { return crypto::base64url_decode(text, out); }

// ─── Keys ────────────────────────────────────────────────────────────────────

// A PEM public key, private key or X.509 certificate (what Google's older
// certs endpoint serves).
PKey from_pem(const std::string& pem, bool priv) {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    if (priv) return PKey(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (pem.find("BEGIN CERTIFICATE") != std::string::npos) {
        std::unique_ptr<X509, decltype(&X509_free)> cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
        return PKey(cert ? X509_get_pubkey(cert.get()) : nullptr, EVP_PKEY_free);
    }
    return PKey(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
}

std::string field(const Value::Dict& d, const char* k) {
    auto it = d.find(k);
    return it != d.end() && it->second.is_str() ? it->second.as_str() : "";
}

// A JWK public key: RSA (n, e), EC P-256/P-384 (x, y) or OKP Ed25519 (x).
PKey from_jwk(const Value::Dict& jwk) {
    const std::string kty = field(jwk, "kty");
    std::string a, b;
    if (kty == "OKP" && field(jwk, "crv") == "Ed25519" && unb64(field(jwk, "x"), a))
        return PKey(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                reinterpret_cast<const unsigned char*>(a.data()), a.size()), EVP_PKEY_free);
    std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> bld(OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> n(nullptr, BN_free), e(nullptr, BN_free);
    const char* type = nullptr;
    std::string point;
    if (kty == "RSA" && unb64(field(jwk, "n"), a) && unb64(field(jwk, "e"), b)) {
        n.reset(BN_bin2bn(reinterpret_cast<const unsigned char*>(a.data()), static_cast<int>(a.size()), nullptr));
        e.reset(BN_bin2bn(reinterpret_cast<const unsigned char*>(b.data()), static_cast<int>(b.size()), nullptr));
        OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_N, n.get());
        OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_E, e.get());
        type = "RSA";
    } else if (kty == "EC" && unb64(field(jwk, "x"), a) && unb64(field(jwk, "y"), b)) {
        const std::string crv = field(jwk, "crv");
        const char* group = crv == "P-256" ? "prime256v1" : crv == "P-384" ? "secp384r1" : nullptr;
        if (!group) return no_key();
        point = "\x04" + a + b;   // uncompressed point
        OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, group, 0);
        OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
        type = "EC";
    } else {
        return no_key();
    }
    std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> params(OSSL_PARAM_BLD_to_param(bld.get()), OSSL_PARAM_free);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* key = nullptr;
    if (!ctx || EVP_PKEY_fromdata_init(ctx.get()) <= 0 ||
        EVP_PKEY_fromdata(ctx.get(), &key, EVP_PKEY_PUBLIC_KEY, params.get()) <= 0)
        return no_key();
    return PKey(key, EVP_PKEY_free);
}

PKey public_key(const Value& k) { return k.is_dict() ? from_jwk(k.as_dict()) : from_pem(k.to_string(), false); }

// ─── Algorithms ──────────────────────────────────────────────────────────────

struct Alg { const char* name; const char* key_type; const EVP_MD* (*md)(); bool pss; size_t ec_half; };

const Alg* find_alg(const std::string& name) {
    static const Alg kAlgs[] = {
        {"RS256", "RSA", EVP_sha256, false, 0}, {"RS384", "RSA", EVP_sha384, false, 0}, {"RS512", "RSA", EVP_sha512, false, 0},
        {"PS256", "RSA", EVP_sha256, true, 0},  {"PS384", "RSA", EVP_sha384, true, 0},  {"PS512", "RSA", EVP_sha512, true, 0},
        {"ES256", "EC", EVP_sha256, false, 32}, {"ES384", "EC", EVP_sha384, false, 48},
        {"EdDSA", "ED25519", nullptr, false, 0},
    };
    for (const Alg& a : kAlgs) if (name == a.name) return &a;
    return nullptr;
}

// JWS carries an ECDSA signature as raw r||s; OpenSSL wants DER.
std::string ec_raw_to_der(const std::string& raw) {
    const size_t h = raw.size() / 2;
    ECDSA_SIG* sig = ECDSA_SIG_new();
    ECDSA_SIG_set0(sig, BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data()), static_cast<int>(h), nullptr),
                   BN_bin2bn(reinterpret_cast<const unsigned char*>(raw.data() + h), static_cast<int>(h), nullptr));
    unsigned char* der = nullptr;
    const int n = i2d_ECDSA_SIG(sig, &der);
    std::string out = n > 0 ? std::string(reinterpret_cast<char*>(der), static_cast<size_t>(n)) : "";
    OPENSSL_free(der);
    ECDSA_SIG_free(sig);
    return out;
}

std::string ec_der_to_raw(const std::string& der, size_t half) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.data());
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()));
    if (!sig) return "";
    std::string out(2 * half, '\0');
    BN_bn2binpad(ECDSA_SIG_get0_r(sig), reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(half));
    BN_bn2binpad(ECDSA_SIG_get0_s(sig), reinterpret_cast<unsigned char*>(out.data() + half), static_cast<int>(half));
    ECDSA_SIG_free(sig);
    return out;
}

// One EVP_DigestSign/Verify for every algorithm: `sig` is read (verify) or
// written (sign), in JWS form.
bool run(const Alg& alg, EVP_PKEY* key, const std::string& data, std::string& sig, bool signing) {
    if (!key || !EVP_PKEY_is_a(key, alg.key_type)) return false;   // the alg must fit the key
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_PKEY_CTX* pctx = nullptr;
    const EVP_MD* md = alg.md ? alg.md() : nullptr;
    if ((signing ? EVP_DigestSignInit(ctx.get(), &pctx, md, nullptr, key)
                 : EVP_DigestVerifyInit(ctx.get(), &pctx, md, nullptr, key)) <= 0)
        return false;
    if (alg.pss) {
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING);
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST);
    }
    const auto* d = reinterpret_cast<const unsigned char*>(data.data());
    if (signing) {
        size_t n = 0;
        if (EVP_DigestSign(ctx.get(), nullptr, &n, d, data.size()) <= 0) return false;
        std::string out(n, '\0');
        if (EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &n, d, data.size()) <= 0) return false;
        out.resize(n);
        sig = alg.ec_half ? ec_der_to_raw(out, alg.ec_half) : out;
        return !sig.empty();
    }
    const std::string s = alg.ec_half ? (sig.size() == 2 * alg.ec_half ? ec_raw_to_der(sig) : "") : sig;
    return !s.empty() && EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(s.data()), s.size(), d, data.size()) == 1;
}

// ─── Functions ───────────────────────────────────────────────────────────────

Value fn_key(NativeCtx&, std::vector<Value>&, std::string& error) {
    const std::string k = crypto::random_bytes(32);
    if (k.empty()) { error = "crypto.key(): could not get random bytes"; return Value::null(); }
    return Value::str(b64(k));
}

bool aes_key(const std::string& text, std::string& out, const char* fn, std::string& error) {
    if (!unb64(text, out) || out.size() != 32) {
        error = std::string("crypto.") + fn + "(): the key must be 32 bytes from crypto.key(), not a password";
        return false;
    }
    return true;
}

// "v1." + base64url(nonce 12 | ciphertext | tag 16). `aad`, when given, is
// authenticated but not encrypted (a user id the token must belong to).
Value fn_encrypt(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::string key;
    if (!aes_key(a[1].as_str(), key, "encrypt", error)) return Value::null();
    const std::string& plain = a[0].as_str();
    const std::string aad = a.size() > 2 ? a[2].as_str() : "";
    std::string nonce = crypto::random_bytes(12), out(plain.size(), '\0'), tag(16, '\0');
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int n = 0, m = 0;
    const bool ok = nonce.size() == 12 &&
        EVP_EncryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(nonce.data())) == 1 &&
        EVP_EncryptUpdate(c.get(), nullptr, &n, reinterpret_cast<const unsigned char*>(aad.data()), static_cast<int>(aad.size())) == 1 &&
        EVP_EncryptUpdate(c.get(), reinterpret_cast<unsigned char*>(out.data()), &n,
                          reinterpret_cast<const unsigned char*>(plain.data()), static_cast<int>(plain.size())) == 1 &&
        EVP_EncryptFinal_ex(c.get(), reinterpret_cast<unsigned char*>(out.data()) + n, &m) == 1 &&
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
    if (!ok) { error = "crypto.encrypt(): failed"; return Value::null(); }
    return Value::str("v1." + b64(nonce + out + tag));
}

// The plaintext, or null if the token was altered, is for another key, or
// another aad.
Value fn_decrypt(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::string key, raw;
    if (!aes_key(a[1].as_str(), key, "decrypt", error)) return Value::null();
    const std::string& token = a[0].as_str();
    if (token.rfind("v1.", 0) != 0 || !unb64(token.substr(3), raw) || raw.size() < 28) return Value::null();
    const std::string aad = a.size() > 2 ? a[2].as_str() : "";
    std::string tag = raw.substr(raw.size() - 16), ct = raw.substr(12, raw.size() - 28), out(ct.size(), '\0');
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> c(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int n = 0, m = 0;
    const bool ok =
        EVP_DecryptInit_ex(c.get(), EVP_aes_256_gcm(), nullptr, reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(raw.data())) == 1 &&
        EVP_DecryptUpdate(c.get(), nullptr, &n, reinterpret_cast<const unsigned char*>(aad.data()), static_cast<int>(aad.size())) == 1 &&
        EVP_DecryptUpdate(c.get(), reinterpret_cast<unsigned char*>(out.data()), &n,
                          reinterpret_cast<const unsigned char*>(ct.data()), static_cast<int>(ct.size())) == 1 &&
        EVP_CIPHER_CTX_ctrl(c.get(), EVP_CTRL_GCM_SET_TAG, 16, tag.data()) == 1 &&
        EVP_DecryptFinal_ex(c.get(), reinterpret_cast<unsigned char*>(out.data()) + n, &m) == 1;
    return ok ? Value::str(std::move(out)) : Value::null();
}

// sign(alg, private_key_pem, data) -> base64url signature (JWS form)
Value fn_sign(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const Alg* alg = find_alg(a[0].as_str());
    if (!alg) { error = "crypto.sign(): unknown algorithm '" + a[0].as_str() + "'"; return Value::null(); }
    PKey key = from_pem(a[1].as_str(), true);
    std::string sig;
    if (!run(*alg, key.get(), a[2].as_str(), sig, true)) {
        error = "crypto.sign(): the key is not a " + std::string(alg->key_type) + " private key in PEM";
        return Value::null();
    }
    return Value::str(b64(sig));
}

// verify(alg, public_key (PEM, certificate or JWK), data, signature) -> bool
Value fn_verify(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const Alg* alg = find_alg(a[0].as_str());
    if (!alg) { error = "crypto.verify(): unknown algorithm '" + a[0].as_str() + "'"; return Value::null(); }
    std::string sig;
    if (!unb64(a[3].as_str(), sig)) return Value::boolean(false);
    return Value::boolean(run(*alg, public_key(a[1]).get(), a[2].as_str(), sig, false));
}

long long now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// jwt_verify(token, key[, options]) -> the claims, or null. key: PEM,
// certificate, JWK, or a JWKS ({"keys": [...]}, picked by the token's kid).
// options: {"aud": ..., "iss": ..., "leeway_s": 60}.
Value fn_jwt_verify(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string& token = a[0].as_str();
    const size_t d1 = token.find('.'), d2 = token.find('.', d1 + 1);
    if (d1 == std::string::npos || d2 == std::string::npos || token.find('.', d2 + 1) != std::string::npos) return Value::null();
    std::string head_json, body_json, sig;
    Value head, claims;
    if (!unb64(token.substr(0, d1), head_json) || !Value::parse_json(head_json, head) || !head.is_dict() ||
        !unb64(token.substr(d1 + 1, d2 - d1 - 1), body_json) || !Value::parse_json(body_json, claims) || !claims.is_dict() ||
        !unb64(token.substr(d2 + 1), sig))
        return Value::null();
    const Alg* alg = find_alg(field(head.as_dict(), "alg"));   // "none" and HS* are not in the table
    if (!alg) return Value::null();

    Value key = a[1];
    if (key.is_dict() && key.as_dict().count("keys")) {   // a JWKS: the key with the token's kid
        const std::string kid = field(head.as_dict(), "kid");
        const Value keys = key.as_dict().find("keys")->second;
        Value found;
        if (keys.is_list())
            for (const Value& k : keys.as_list())
                if (k.is_dict() && (kid.empty() || field(k.as_dict(), "kid") == kid)) { found = k; break; }
        if (found.is_null()) return Value::null();
        key = found;
    }
    if (key.is_dict() && !field(key.as_dict(), "alg").empty() && field(key.as_dict(), "alg") != alg->name) return Value::null();
    if (!run(*alg, public_key(key).get(), token.substr(0, d2), sig, false)) return Value::null();

    const Value::Dict& c = claims.as_dict();
    const Value::Dict empty;
    const Value::Dict& o = a.size() > 2 && a[2].is_dict() ? a[2].as_dict() : empty;
    const long long leeway = o.count("leeway_s") ? o.find("leeway_s")->second.as_int() : 60, now = now_s();
    auto num = [&](const char* k, long long& v) {
        auto it = c.find(k);
        if (it == c.end() || !it->second.is_num()) return false;
        v = static_cast<long long>(it->second.as_float());
        return true;
    };
    long long t;
    if (num("exp", t) && now > t + leeway) return Value::null();
    if (num("nbf", t) && now + leeway < t) return Value::null();
    if (auto it = o.find("iss"); it != o.end() && field(c, "iss") != it->second.to_string()) return Value::null();
    if (auto it = o.find("aud"); it != o.end()) {
        auto aud = c.find("aud");
        bool ok = aud != c.end() && aud->second.is_str() && aud->second.as_str() == it->second.to_string();
        if (aud != c.end() && aud->second.is_list())
            for (const Value& x : aud->second.as_list()) ok |= x.is_str() && x.as_str() == it->second.to_string();
        if (!ok) return Value::null();
    }
    return claims;
}

// jwt_sign(claims, alg, private_key_pem[, kid]) -> token
Value fn_jwt_sign(NativeCtx& ctx, std::vector<Value>& a, std::string& error) {
    Value::Dict head;
    head["alg"] = a[1];
    head["typ"] = Value::str("JWT");
    if (a.size() > 3) head["kid"] = a[3];
    const std::string input = b64(Value::dict(std::move(head)).to_json_text()) + "." + b64(a[0].to_json_text());
    std::vector<Value> args{a[1], a[2], Value::str(input)};
    Value sig = fn_sign(ctx, args, error);
    return sig.is_null() ? Value::null() : Value::str(input + "." + sig.as_str());
}

} // namespace

LUX_MODULE(crypto, {
    {"key",        ">s",      fn_key},
    {"encrypt",    "ss|s>s",  fn_encrypt},
    {"decrypt",    "ss|s",    fn_decrypt},
    {"sign",       "sss>s",   fn_sign},
    {"verify",     "sxss>b",  fn_verify},
    {"jwt_verify", "sx|d",    fn_jwt_verify},
    {"jwt_sign",   "dss|s>s", fn_jwt_sign},
})

} // namespace lux_script
