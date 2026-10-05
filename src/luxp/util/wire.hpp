#pragma once
// Little-endian binary writer/reader shared by every .luxp section codec
// (document, stylesheet, program).  The reader never throws and never reads
// past the end: any short read flips `ok` and returns zeros, so a decoder
// checks `ok` once at the end instead of after every field.

#include <cstdint>
#include <string>

namespace luxium::wire {

constexpr uint32_t kMaxString = 1u << 24;

struct Writer {
    std::string out;
    void u8(uint8_t v) { out += char(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) out += char((v >> (8 * i)) & 0xFF); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) out += char((v >> (8 * i)) & 0xFF); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    // LEB128: 1 byte below 128, 2 below 16384...  The format's integers.
    void uv(uint64_t v) {
        while (v >= 0x80) { out += char((v & 0x7F) | 0x80); v >>= 7; }
        out += char(v);
    }
    void sv(int64_t v) { uv((uint64_t(v) << 1) ^ uint64_t(v >> 63)); }   // zigzag
    void str(const std::string& s) { uv(s.size()); out += s; }
};

struct Reader {
    const std::string& in;
    size_t at = 0;
    bool ok = true;

    bool need(size_t n) {
        if (!ok || in.size() - at < n) ok = false;
        return ok;
    }
    uint8_t u8() { return need(1) ? uint8_t(in[at++]) : 0; }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(in[at + i])) << (8 * i);
        at += 4;
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(in[at + i])) << (8 * i);
        at += 8;
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    // LEB128, at most 10 bytes; anything longer or overflowing is an error.
    uint64_t uv() {
        uint64_t v = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            if (!need(1)) return 0;
            const uint8_t b = uint8_t(in[at++]);
            if (shift == 63 && b > 1) { ok = false; return 0; }
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    int64_t sv() { const uint64_t v = uv(); return int64_t(v >> 1) ^ -int64_t(v & 1); }
    // A u32-sized integer (counts, indexes, sizes).
    uint32_t uv32() {
        const uint64_t v = uv();
        if (v > 0xFFFFFFFFull) { ok = false; return 0; }
        return uint32_t(v);
    }
    std::string str(uint32_t max = kMaxString) {
        const uint32_t n = uv32();
        if (n > max || !need(n)) { ok = false; return {}; }
        std::string s = in.substr(at, n);
        at += n;
        return s;
    }
    // A count that must fit what is left (each item takes >= min_bytes):
    // stops a forged "4 billion items" before anything is reserved.
    uint32_t count(uint32_t max, size_t min_bytes = 1) {
        const uint32_t n = uv32();
        if (n > max || (ok && uint64_t(n) * min_bytes > in.size() - at)) ok = false;
        return ok ? n : 0;
    }
    bool done() const { return ok && at == in.size(); }
};

} // namespace luxium::wire
