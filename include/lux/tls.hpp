#pragma once
#include <string>

struct ssl_ctx_st;   // OpenSSL's SSL_CTX, so this header needs no OpenSSL include

// HTTPS: the process-wide server context, built once before App::run().
// Every listener of the process speaks TLS once init() succeeded.
// Without LUX_TLS (cmake -DLUX_TLS=ON) init() fails and nothing else links OpenSSL.
namespace lux::tls {

// Loads the PEM certificate chain and private key. False, with `err` set, if
// they cannot be read, do not match, or this binary was built without LUX_TLS.
bool init(const std::string& cert_file, const std::string& key_file, std::string& err);

bool enabled();
ssl_ctx_st* context();   // null until init() succeeded

} // namespace lux::tls
