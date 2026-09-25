// "Download all as .zip". With zlib (the gzip module's build) an entry is
// deflated unless it is compressed already (photos, video, PDFs...); without
// it everything is stored. Files are streamed, never loaded whole. Past
// 4 GB (an entry, an offset) or 65535 entries it writes ZIP64 records.
#include <lux_script/builtin_module.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <vector>
#include <filesystem>
#include <fstream>

#ifdef LUX_GZIP
#include <zlib.h>
#endif

namespace lux_script {

namespace {

namespace fs = std::filesystem;

uint32_t crc32_update(uint32_t crc, const char* p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ static_cast<uint8_t>(p[i])) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void le(std::string& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

struct Entry { std::string name; uint32_t crc; uint64_t size, packed, offset; uint16_t method; bool zip64; };

constexpr uint64_t kMax32 = 0xFFFFFFFFu;
// Deflate can grow incompressible data a little, so an entry this close to
// 4 GB is written as ZIP64 from its local header on.
constexpr uint64_t kZip64From = 0xF0000000u;

// Deflating a JPEG or an MP4 spends CPU to gain nothing.
bool worth_deflating(const std::string& name) {
#ifdef LUX_GZIP
    std::string ext = fs::path(name).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* e : {".jpg", ".jpeg", ".png", ".gif", ".webp", ".heic", ".avif", ".mp4", ".mov",
                          ".mkv", ".webm", ".mp3", ".m4a", ".ogg", ".flac", ".zip", ".gz", ".7z", ".pdf", ".docx", ".xlsx"})
        if (ext == e) return false;
    return true;
#else
    (void)name;
    return false;
#endif
}

// create(dest, [path, ...] or [[path, name_in_zip], ...]) -> number of
// files written. A name keeps no leading '/' and no '..' part, so the
// archive cannot write outside wherever it is unpacked.
Value fn_create(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::ofstream out(a[0].as_str(), std::ios::binary | std::ios::trunc);
    if (!out) { error = "zip.create(): cannot write '" + a[0].as_str() + "'"; return Value::null(); }
    std::vector<Entry> entries;
    uint64_t offset = 0;
    std::vector<char> buf(1 << 16);

    for (const Value& item : a[1].as_list()) {
        const bool pair = item.is_list() && item.as_list().size() == 2;
        const std::string path = pair ? item.as_list()[0].to_string() : item.to_string();
        std::string name = pair ? item.as_list()[1].to_string() : fs::path(path).filename().string();
        fs::path clean;
        for (const auto& part : fs::path(name).relative_path())
            if (part != ".." && part != ".") clean /= part;
        name = clean.generic_string();

        std::error_code ec;
        const uint64_t size = fs::file_size(path, ec);
        std::ifstream in(path, std::ios::binary);
        if (ec || !in || name.empty()) { error = "zip.create(): cannot read '" + path + "'"; return Value::null(); }

        // Local header with bit 3 set: CRC and sizes follow the data, so
        // the file is read exactly once.
        const uint16_t method = worth_deflating(name) ? 8 : 0;   // 8 deflate, 0 stored
        const bool zip64 = size >= kZip64From;
        std::string h;
        le(h, 0x04034b50, 4); le(h, zip64 ? 45 : 20, 2); le(h, 0x0808, 2);   // bit 3 + UTF-8 names
        le(h, method, 2); le(h, 0, 4);                                        // no DOS time
        le(h, 0, 4); le(h, zip64 ? kMax32 : 0, 4); le(h, zip64 ? kMax32 : 0, 4);
        le(h, name.size(), 2); le(h, zip64 ? 20 : 0, 2);
        h += name;
        if (zip64) { le(h, 0x0001, 2); le(h, 16, 2); le(h, 0, 8); le(h, 0, 8); }   // sizes in the descriptor
        out.write(h.data(), static_cast<std::streamsize>(h.size()));

        uint32_t crc = 0;
        uint64_t packed = 0;
#ifdef LUX_GZIP
        z_stream z{};
        if (method == 8) deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);   // raw deflate
        std::vector<char> zbuf(1 << 16);
#endif
        while (true) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = static_cast<size_t>(in.gcount());
            crc = crc32_update(crc, buf.data(), n);
            if (method == 0) {
                out.write(buf.data(), static_cast<std::streamsize>(n));
                packed += n;
            }
#ifdef LUX_GZIP
            else {
                z.next_in = reinterpret_cast<Bytef*>(buf.data());
                z.avail_in = static_cast<uInt>(n);
                const int flush = in ? Z_NO_FLUSH : Z_FINISH;
                do {
                    z.next_out = reinterpret_cast<Bytef*>(zbuf.data());
                    z.avail_out = static_cast<uInt>(zbuf.size());
                    deflate(&z, flush);
                    const size_t got = zbuf.size() - z.avail_out;
                    out.write(zbuf.data(), static_cast<std::streamsize>(got));
                    packed += got;
                } while (z.avail_out == 0);
            }
#endif
            if (!in) break;
        }
#ifdef LUX_GZIP
        if (method == 8) deflateEnd(&z);
#endif
        if (!zip64 && packed >= kMax32) { error = "zip.create(): '" + path + "' grew past 4 GB while being read"; return Value::null(); }
        std::string d;
        le(d, 0x08074b50, 4); le(d, crc, 4);
        le(d, packed, zip64 ? 8 : 4); le(d, size, zip64 ? 8 : 4);
        out.write(d.data(), static_cast<std::streamsize>(d.size()));
        entries.push_back({name, crc, size, packed, offset, method, zip64});
        offset += h.size() + packed + d.size();
    }

    std::string dir;
    for (const Entry& e : entries) {
        // A ZIP64 extra holds, in this order, only the fields that overflow.
        std::string extra;
        const bool big = e.zip64 || e.size >= kMax32 || e.packed >= kMax32;
        if (big) { le(extra, e.size, 8); le(extra, e.packed, 8); }
        if (e.offset >= kMax32) le(extra, e.offset, 8);
        if (!extra.empty()) { std::string x; le(x, 0x0001, 2); le(x, extra.size(), 2); extra = x + extra; }
        le(dir, 0x02014b50, 4); le(dir, extra.empty() ? 20 : 45, 2); le(dir, extra.empty() ? 20 : 45, 2);
        le(dir, 0x0808, 2); le(dir, e.method, 2); le(dir, 0, 4);
        le(dir, e.crc, 4); le(dir, big ? kMax32 : e.packed, 4); le(dir, big ? kMax32 : e.size, 4);
        le(dir, e.name.size(), 2); le(dir, extra.size(), 2); le(dir, 0, 2); le(dir, 0, 2); le(dir, 0, 2);
        le(dir, 0, 4); le(dir, e.offset >= kMax32 ? kMax32 : e.offset, 4);
        dir += e.name + extra;
    }
    std::string end;
    const uint64_t n = entries.size();
    if (n >= 0xFFFF || dir.size() >= kMax32 || offset >= kMax32) {
        // ZIP64 end record, then the locator that points at it.
        le(end, 0x06064b50, 4); le(end, 44, 8); le(end, 45, 2); le(end, 45, 2);
        le(end, 0, 4); le(end, 0, 4); le(end, n, 8); le(end, n, 8);
        le(end, dir.size(), 8); le(end, offset, 8);
        le(end, 0x07064b50, 4); le(end, 0, 4); le(end, offset + dir.size(), 8); le(end, 1, 4);
    }
    le(end, 0x06054b50, 4); le(end, 0, 2); le(end, 0, 2);
    le(end, std::min<uint64_t>(n, 0xFFFF), 2); le(end, std::min<uint64_t>(n, 0xFFFF), 2);
    le(end, std::min<uint64_t>(dir.size(), kMax32), 4); le(end, std::min<uint64_t>(offset, kMax32), 4); le(end, 0, 2);
    out.write(dir.data(), static_cast<std::streamsize>(dir.size()));
    out.write(end.data(), static_cast<std::streamsize>(end.size()));
    if (!out.flush()) { error = "zip.create(): write failed"; return Value::null(); }
    return Value::integer(static_cast<long long>(entries.size()));
}

} // namespace

LUX_MODULE(zip, {
    {"create", "sl>i", fn_create, /*is_async=*/true},
})

} // namespace lux_script
