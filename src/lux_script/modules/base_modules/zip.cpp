// "Download all as .zip" without a compression library: entries are
// stored, not deflated -- what goes in such an archive (photos, PDFs,
// video) is compressed already. Files are streamed, never loaded whole.
// ponytail: no ZIP64, so no entry or archive past 4 GB; add it if that is
// ever needed.
#include <lux_script/builtin_module.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>

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

struct Entry { std::string name; uint32_t crc, size, offset; };

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
        const auto size = fs::file_size(path, ec);
        std::ifstream in(path, std::ios::binary);
        if (ec || !in || name.empty()) { error = "zip.create(): cannot read '" + path + "'"; return Value::null(); }
        if (size >= 0xFFFFFFFFu || offset >= 0xFFFFFFFFu) { error = "zip.create(): over 4 GB is not supported"; return Value::null(); }

        // Local header with bit 3 set: CRC and sizes follow the data, so
        // the file is read exactly once.
        std::string h;
        le(h, 0x04034b50, 4); le(h, 20, 2); le(h, 0x0808, 2);   // bit 3 + UTF-8 names
        le(h, 0, 2); le(h, 0, 4);                                // stored, no DOS time
        le(h, 0, 4); le(h, 0, 4); le(h, 0, 4);
        le(h, name.size(), 2); le(h, 0, 2);
        h += name;
        out.write(h.data(), static_cast<std::streamsize>(h.size()));

        uint32_t crc = 0;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = static_cast<size_t>(in.gcount());
            crc = crc32_update(crc, buf.data(), n);
            out.write(buf.data(), static_cast<std::streamsize>(n));
        }
        std::string d;
        le(d, 0x08074b50, 4); le(d, crc, 4); le(d, size, 4); le(d, size, 4);
        out.write(d.data(), static_cast<std::streamsize>(d.size()));
        entries.push_back({name, crc, static_cast<uint32_t>(size), static_cast<uint32_t>(offset)});
        offset += h.size() + size + d.size();
    }

    std::string dir;
    for (const Entry& e : entries) {
        le(dir, 0x02014b50, 4); le(dir, 20, 2); le(dir, 20, 2); le(dir, 0x0808, 2);
        le(dir, 0, 2); le(dir, 0, 4);
        le(dir, e.crc, 4); le(dir, e.size, 4); le(dir, e.size, 4);
        le(dir, e.name.size(), 2); le(dir, 0, 2); le(dir, 0, 2); le(dir, 0, 2); le(dir, 0, 2);
        le(dir, 0, 4); le(dir, e.offset, 4);
        dir += e.name;
    }
    std::string end;
    le(end, 0x06054b50, 4); le(end, 0, 2); le(end, 0, 2);
    le(end, entries.size(), 2); le(end, entries.size(), 2);
    le(end, dir.size(), 4); le(end, offset, 4); le(end, 0, 2);
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
