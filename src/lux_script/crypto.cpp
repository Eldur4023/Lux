#include <lux_script/crypto.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/random.h>

namespace lux_script::crypto {

namespace {

// ─── SHA-256 (FIPS 180-4) ────────────────────────────────────────────────────

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t S1    = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch    = (e & f) ^ (~e & g);
        uint32_t temp1 = hh + S1 + ch + kK[i] + w[i];
        uint32_t S0    = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        hh = g; g = f; f = e;
        e  = d + temp1;
        d  = c; c = b; b = a;
        a  = temp1 + temp2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

constexpr size_t kBlock = 64;

} // namespace

namespace {
constexpr uint32_t kIV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

// Finishes a hash whose state `h` has already absorbed `prefix` bytes (a
// multiple of the block): the IV and 0 for a plain sha256, a precomputed
// HMAC pad for pbkdf2_sha256.
std::string sha256_from(const uint32_t start[8], size_t prefix, std::string_view data) {
    uint32_t h[8];
    std::memcpy(h, start, sizeof h);

    const auto* p   = reinterpret_cast<const uint8_t*>(data.data());
    size_t      len = data.size();

    size_t full = len / kBlock;
    for (size_t i = 0; i < full; ++i) compress(h, p + i * kBlock);

    // Padding: 0x80, zeros, and the length in bits as a 64-bit big-endian.
    uint8_t tail[128] = {};
    size_t  rest      = len % kBlock;
    std::memcpy(tail, p + full * kBlock, rest);
    tail[rest] = 0x80;

    size_t tail_len = (rest + 1 + 8 <= kBlock) ? kBlock : 2 * kBlock;
    uint64_t bits   = static_cast<uint64_t>(prefix + len) * 8;
    for (int i = 0; i < 8; ++i)
        tail[tail_len - 1 - static_cast<size_t>(i)] =
            static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);

    for (size_t off = 0; off < tail_len; off += kBlock) compress(h, tail + off);

    std::string out(32, '\0');
    for (int i = 0; i < 8; ++i) {
        out[static_cast<size_t>(i * 4 + 0)] = static_cast<char>((h[i] >> 24) & 0xFF);
        out[static_cast<size_t>(i * 4 + 1)] = static_cast<char>((h[i] >> 16) & 0xFF);
        out[static_cast<size_t>(i * 4 + 2)] = static_cast<char>((h[i] >> 8) & 0xFF);
        out[static_cast<size_t>(i * 4 + 3)] = static_cast<char>(h[i] & 0xFF);
    }
    return out;
}
} // namespace

std::string sha256(std::string_view data) { return sha256_from(kIV, 0, data); }

std::string hmac_sha256(std::string_view key, std::string_view message) {
    // RFC 2104: a key longer than the block is replaced by its hash.
    std::string k(key);
    if (k.size() > kBlock) k = sha256(k);
    k.resize(kBlock, '\0');

    std::string inner(kBlock, '\0'), outer(kBlock, '\0');
    for (size_t i = 0; i < kBlock; ++i) {
        inner[i] = static_cast<char>(static_cast<uint8_t>(k[i]) ^ 0x36);
        outer[i] = static_cast<char>(static_cast<uint8_t>(k[i]) ^ 0x5c);
    }

    inner.append(message);
    std::string inner_hash = sha256(inner);
    outer.append(inner_hash);
    return sha256(outer);
}

std::string pbkdf2_sha256(std::string_view password, std::string_view salt,
                          unsigned iterations, size_t length) {
    // HMAC with its two padded-key blocks compressed once, up front: every
    // iteration then costs two compressions instead of four.
    std::string k(password);
    if (k.size() > kBlock) k = sha256(k);
    k.resize(kBlock, '\0');
    uint8_t  ipad[kBlock], opad[kBlock];
    uint32_t inner[8], outer[8];
    for (size_t i = 0; i < kBlock; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i]) ^ 0x36;
        opad[i] = static_cast<uint8_t>(k[i]) ^ 0x5c;
    }
    std::memcpy(inner, kIV, sizeof inner); compress(inner, ipad);
    std::memcpy(outer, kIV, sizeof outer); compress(outer, opad);
    auto hmac = [&](std::string_view m) { return sha256_from(outer, kBlock, sha256_from(inner, kBlock, m)); };

    std::string out;
    for (uint32_t block = 1; out.size() < length; ++block) {
        std::string first(salt);
        for (int s = 24; s >= 0; s -= 8) first += static_cast<char>((block >> s) & 0xFF);
        std::string u = hmac(first), t = u;
        for (unsigned i = 1; i < iterations; ++i) {
            u = hmac(u);
            for (size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
        }
        out += t;
    }
    out.resize(length);
    return out;
}

// ─── Base64url (RFC 4648 §5, no padding) ─────────────────────────────────────

namespace {
constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}
} // namespace

namespace {
// One encoder for both alphabets: the url variant (JWT, session cookies)
// drops the '=' padding, the standard one (sqlite BLOBs, PDF export) keeps it.
std::string encode_b64(std::string_view raw, const char* alphabet, bool pad) {
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);
    for (size_t i = 0; i < raw.size(); i += 3) {
        size_t   n = std::min<size_t>(3, raw.size() - i);
        uint32_t v = uint32_t(uint8_t(raw[i])) << 16;
        if (n > 1) v |= uint32_t(uint8_t(raw[i + 1])) << 8;
        if (n > 2) v |= uint32_t(uint8_t(raw[i + 2]));
        for (size_t k = 0; k < 4; ++k) {
            if (k <= n) out += alphabet[(v >> (18 - 6 * k)) & 0x3F];
            else if (pad) out += '=';
        }
    }
    return out;
}
} // namespace

std::string hex_encode(std::string_view raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out += kHex[c >> 4];
        out += kHex[c & 0xF];
    }
    return out;
}

std::string base64url_encode(std::string_view raw) {
    return encode_b64(raw, kAlphabet, false);
}

std::string base64_encode(std::string_view raw) {
    return encode_b64(raw,
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

bool base64url_decode(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size() * 3 / 4);

    uint32_t acc = 0;
    int      bits = 0;
    for (char c : text) {
        if (c == '=') break;              // padding tolerated, though it is not emitted
        int d = decode_char(c);
        if (d < 0) return false;
        acc  = (acc << 6) | static_cast<uint32_t>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return true;
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

std::string random_bytes(size_t n) {
    std::string out(n, '\0');
    size_t got = 0;
    while (got < n) {
        // getrandom(2) over /dev/urandom: it draws from the same CSPRNG but
        // needs no file descriptor, so it cannot fail merely because the
        // process is out of them (a real failure mode under load -- see
        // SECURITY-AUDIT.md #13) and it blocks instead of returning
        // low-quality output before the kernel's entropy pool is
        // initialised at boot (irrelevant days into a server's uptime, but
        // free correctness). Available unconditionally: this project only
        // targets Linux, and getrandom() has existed since Linux 3.17
        // (2014)/glibc 2.25.
        ssize_t r = ::getrandom(out.data() + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return {};   // caller must treat empty as failure, never a fallback value
        }
        got += static_cast<size_t>(r);
    }
    return out;
}

} // namespace lux_script::crypto
