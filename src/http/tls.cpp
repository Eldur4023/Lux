#include <lux/tls.hpp>

#ifdef LUX_TLS
#include <cpuid.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace lux::tls {

#ifdef LUX_TLS

static SSL_CTX* g_ctx = nullptr;

static std::string last_error() {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
    return buf;
}

// AES-GCM is only fast with both AES-NI and PCLMULQDQ (GHASH). A VM with a
// generic CPU model often lacks the latter: measured on such a VPS, AES-GCM
// 385 MB/s against ChaCha20-Poly1305 810 MB/s.
static bool fast_aes_gcm() {
    unsigned a, b, c, d;
    return __get_cpuid(1, &a, &b, &c, &d) && (c & bit_AES) && (c & bit_PCLMUL);
}

bool init(const std::string& cert_file, const std::string& key_file, std::string& err) {
    SSL_CTX* c = SSL_CTX_new(TLS_server_method());
    if (!c) { err = last_error(); return false; }
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    // The server picks the suite: the cheapest one on this CPU first. AES-128
    // over AES-256 (same security margin in practice, fewer rounds). With fast
    // AES, PRIORITIZE_CHACHA still gives ChaCha to a client that asks for it
    // first (a phone without AES instructions).
    if (fast_aes_gcm()) {
        SSL_CTX_set_ciphersuites(c, "TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_256_GCM_SHA384");
        SSL_CTX_set_cipher_list(c, "ECDHE+AESGCM+AES128:ECDHE+CHACHA20:ECDHE+AESGCM");
        SSL_CTX_set_options(c, SSL_OP_PRIORITIZE_CHACHA);
    } else {
        SSL_CTX_set_ciphersuites(c, "TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384");
        SSL_CTX_set_cipher_list(c, "ECDHE+CHACHA20:ECDHE+AESGCM+AES128:ECDHE+AESGCM");
    }
    // No renegotiation (a DoS lever, and TLS 1.3 has none). Partial writes and a
    // moving buffer because HttpConnection retries from write_buf_ + offset.
    // kTLS on transmit (the kernel encrypts, so a static file goes out through
    // sendfile instead of pread + user-space AES): only where the kernel's AES-GCM
    // is fast. On a VM without PCLMULQDQ it measured slower than OpenSSL's own.
    // Without the `tls` kernel module this silently stays on user-space TLS.
#ifndef OPENSSL_NO_KTLS
    if (fast_aes_gcm()) SSL_CTX_set_options(c, SSL_OP_ENABLE_KTLS);
#endif
    // RELEASE_BUFFERS: an idle keep-alive connection holds no 30+ KB of record buffers.
    SSL_CTX_set_options(c, SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_mode(c, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                        SSL_MODE_RELEASE_BUFFERS);
    // Resumption through stateless tickets only: no server-side session cache
    // (a lock and memory per handshake), and one ticket instead of OpenSSL's two
    // (a browser uses one): -10% CPU per new connection, measured.
    SSL_CTX_set_session_cache_mode(c, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_num_tickets(c, 1);

    if (SSL_CTX_use_certificate_chain_file(c, cert_file.c_str()) != 1) {
        err = "tls cert " + cert_file + ": " + last_error(); SSL_CTX_free(c); return false;
    }
    if (SSL_CTX_use_PrivateKey_file(c, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        err = "tls key " + key_file + ": " + last_error(); SSL_CTX_free(c); return false;
    }
    if (SSL_CTX_check_private_key(c) != 1) {
        err = "tls cert and key do not match: " + last_error(); SSL_CTX_free(c); return false;
    }
    g_ctx = c;
    return true;
}

bool enabled() { return g_ctx != nullptr; }
ssl_ctx_st* context() { return g_ctx; }

#else

bool init(const std::string&, const std::string&, std::string& err) {
    err = "this binary was built without HTTPS (rebuild with cmake -DLUX_TLS=ON, needs libssl-dev)";
    return false;
}
bool enabled() { return false; }
ssl_ctx_st* context() { return nullptr; }

#endif

} // namespace lux::tls
