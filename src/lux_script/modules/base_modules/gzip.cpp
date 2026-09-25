// gzip through zlib: strings and files, in and out. decompress() reads
// gzip or zlib data alike. Output is capped -- a few KB of gzip can expand
// to gigabytes (a "zip bomb"), and a server decompresses what users send.
#include <lux_script/builtin_module.hpp>

#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace lux_script {

namespace {

constexpr long long kDefaultMaxBytes = 64LL << 20;

// Runs `in` through a zlib stream into `out`. inflate is capped at `max`
// output bytes; deflate at `level` (1-9).
bool pump(bool compress, int level, std::istream& in, std::ostream& out, long long max,
          const char* fn, std::string& error) {
    z_stream z{};
    const int ok = compress ? deflateInit2(&z, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY)
                            : inflateInit2(&z, 15 + 32);   // +16 writes gzip, +32 reads gzip or zlib
    if (ok != Z_OK) { error = std::string("gzip.") + fn + "(): could not start zlib"; return false; }
    std::vector<char> src(1 << 16), dst(1 << 16);
    long long produced = 0;
    int rc = Z_OK;
    bool eof = false;
    while (rc != Z_STREAM_END) {
        if (z.avail_in == 0 && !eof) {
            in.read(src.data(), static_cast<std::streamsize>(src.size()));
            z.next_in  = reinterpret_cast<Bytef*>(src.data());
            z.avail_in = static_cast<uInt>(in.gcount());
            eof = !in;
        }
        z.next_out  = reinterpret_cast<Bytef*>(dst.data());
        z.avail_out = static_cast<uInt>(dst.size());
        rc = compress ? deflate(&z, eof ? Z_FINISH : Z_NO_FLUSH) : inflate(&z, Z_NO_FLUSH);
        const size_t n = dst.size() - z.avail_out;
        produced += static_cast<long long>(n);
        if (produced > max) { rc = Z_MEM_ERROR; error = std::string("gzip.") + fn + "(): the output is over the limit"; break; }
        out.write(dst.data(), static_cast<std::streamsize>(n));
        if (rc == Z_BUF_ERROR && eof && z.avail_in == 0 && n == 0) { error = std::string("gzip.") + fn + "(): truncated data"; break; }
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) { error = std::string("gzip.") + fn + "(): invalid data"; break; }
    }
    compress ? deflateEnd(&z) : inflateEnd(&z);
    return error.empty();
}

int level_of(const std::vector<Value>& a, size_t i) {
    return a.size() > i ? static_cast<int>(std::clamp(a[i].as_int(), 1LL, 9LL)) : 6;
}

// compress(s[, level 1-9]) -> the gzip bytes
Value fn_compress(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::istringstream in(a[0].as_str());
    std::ostringstream out;
    return pump(true, level_of(a, 1), in, out, 1LL << 40, "compress", error) ? Value::str(out.str()) : Value::null();
}

// decompress(bytes[, max_bytes = 64 MB])
Value fn_decompress(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::istringstream in(a[0].as_str());
    std::ostringstream out;
    const long long max = a.size() > 1 ? a[1].as_int() : kDefaultMaxBytes;
    return pump(false, 0, in, out, max, "decompress", error) ? Value::str(out.str()) : Value::null();
}

Value files(std::vector<Value>& a, std::string& error, bool compress) {
    const char* fn = compress ? "compress_file" : "decompress_file";
    std::ifstream in(a[0].as_str(), std::ios::binary);
    std::ofstream out(a[1].as_str(), std::ios::binary | std::ios::trunc);
    if (!in || !out) { error = std::string("gzip.") + fn + "(): cannot open '" + (in ? a[1] : a[0]).as_str() + "'"; return Value::null(); }
    // A file decompressed to disk may be large, but not unbounded (4 GB
    // unless told otherwise).
    const long long max = !compress && a.size() > 2 ? a[2].as_int() : 4LL << 30;
    return pump(compress, compress ? level_of(a, 2) : 0, in, out, max, fn, error) ? Value::boolean(true) : Value::null();
}
Value fn_compress_file(NativeCtx&, std::vector<Value>& a, std::string& e)   { return files(a, e, true); }
Value fn_decompress_file(NativeCtx&, std::vector<Value>& a, std::string& e) { return files(a, e, false); }

} // namespace

LUX_MODULE(gzip, {
    {"compress",        "s|i>s",  fn_compress},
    {"decompress",      "s|i>s",  fn_decompress},
    {"compress_file",   "ss|i>b", fn_compress_file,   /*is_async=*/true},
    {"decompress_file", "ss|i>b", fn_decompress_file, /*is_async=*/true},
})

} // namespace lux_script
