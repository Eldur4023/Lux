#pragma once
#include <string>
#include <string_view>

namespace lux_script::crypto {

// Own SHA-256 and HMAC-SHA256, without OpenSSL.
//
// Lux does not link OpenSSL —TLS is the reverse proxy's business— but it
// still needs a MAC to sign the session cookie and the HS256 JWTs.  Those are
// the only two cryptographic operations in the project, and both fit here.
//
// This is NOT a replacement for a cryptographic library: it implements only
// what is needed, and nothing else.

// Returns the raw 32 bytes of the hash.
std::string sha256(std::string_view data);

// Returns the raw 32 bytes of the MAC (RFC 2104).
std::string hmac_sha256(std::string_view key, std::string_view message);

// PBKDF2-HMAC-SHA256 (RFC 8018), `length` raw bytes: password storage.
std::string pbkdf2_sha256(std::string_view password, std::string_view salt,
                          unsigned iterations, size_t length);

// Base64 with the URL-safe alphabet and no padding, as JWT requires (RFC 7515).
// The decoder takes either alphabet, padded or not.
std::string base64url_encode(std::string_view raw);
bool        base64url_decode(std::string_view text, std::string& out);

// Base64 with the standard alphabet, padded (RFC 4648).  It is how a binary
// goes into a JSON: the database modules use it for the columns that are not
// text, because a raw blob in the response would leave it not valid UTF-8 and
// no client would know how to read it.
std::string base64_encode(std::string_view raw);

// Lowercase hex, two digits per byte.
std::string hex_encode(std::string_view raw);

// Constant-time comparison.  Comparing signatures with == leaks through the
// response time how many leading bytes the attacker got right, which is enough
// to rebuild the signature byte by byte.
bool constant_time_equal(std::string_view a, std::string_view b);

// Random bytes from /dev/urandom.  Returns an empty string if they cannot be
// obtained, and the caller must treat that as a failure, never carry on with a
// predictable value.
std::string random_bytes(size_t n);

} // namespace lux_script::crypto
