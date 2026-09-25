#pragma once
// IP address parsing and classification, shared by the net module and
// http's public_only (SSRF) check. IPv4 is kept as an IPv4-mapped IPv6
// address, so one comparison covers both families.
#include <arpa/inet.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lux_script::netaddr {

using Addr = std::array<uint8_t, 16>;

inline bool parse(const std::string& text, Addr& out) {
    in_addr v4;
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        out = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        std::memcpy(out.data() + 12, &v4, 4);
        return true;
    }
    return inet_pton(AF_INET6, text.c_str(), out.data()) == 1;
}

inline bool is_v4(const Addr& a) {
    static constexpr uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    return std::memcmp(a.data(), kMapped, 12) == 0;
}

// "10.0.0.0/8", "fd00::/8", or a bare address (a /32 or /128).
inline bool in_cidr(const Addr& a, const std::string& cidr) {
    const size_t slash = cidr.find('/');
    Addr net;
    if (!parse(cidr.substr(0, slash), net)) return false;
    int bits = is_v4(net) ? 32 : 128;
    if (slash != std::string::npos) bits = std::atoi(cidr.c_str() + slash + 1);
    if (is_v4(net)) {
        if (!is_v4(a) || bits < 0 || bits > 32) return false;
        bits += 96;
    } else if (is_v4(a) || bits < 0 || bits > 128) return false;   // families never cross
    for (int i = 0; i < bits; ++i)
        if (((a[i / 8] ^ net[i / 8]) >> (7 - i % 8)) & 1) return false;
    return true;
}

// Loopback, private, link-local (cloud metadata lives at 169.254.169.254),
// carrier-grade NAT, "this network", IPv6 unique-local and unspecified.
inline bool is_private(const Addr& a) {
    for (const char* c : {"127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
                          "169.254.0.0/16", "100.64.0.0/10", "0.0.0.0/8",
                          "::1/128", "::/128", "fc00::/7", "fe80::/10"})
        if (in_cidr(a, c)) return true;
    return false;
}

} // namespace lux_script::netaddr
