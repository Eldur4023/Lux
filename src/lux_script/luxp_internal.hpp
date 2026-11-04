#pragma once
// Shared by luxp_serve.cpp (translating a filled page) and luxp_template.cpp
// (templates compiled to a plan): the stylesheet of a page, built once and
// kept while the files it came from do not change.

#include <page/luxp.hpp>

#include <lux/http_cache.hpp>
#include <lux/request.hpp>
#include <lux/response.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace lux_script::luxp_internal {

struct SheetData {
    luxium::page::Blob blob;    // compressed once
    std::string        plain;   // for comparing (LUX_LUXP_CHECK)
    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> deps;
};

// The page's <style> and <link rel=stylesheet>, translated, resolved through
// the static mounts as the browser would request them from `url_path`.
// *relative_used: some URL was relative to the page (its answer depends on
// the path it is served from).
std::shared_ptr<const SheetData> build_sheet(luxium::dom::Node& doc, const std::string& url_path,
                                             bool* relative_used = nullptr, int deflate_level = 9);

// Has any file the sheet came from changed on disk?
bool sheet_stale(const SheetData& s);

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}


// A .luxp answer.  The page can differ per user and per moment, so it is never reused blindly
// (Cache-Control: no-cache); but its ETag is a hash of the bytes, so a client that already holds exactly
// these bytes (If-None-Match) gets a 304 and no body.
inline void send_luxp(const lux::Request& req, lux::Response& res, std::string body, const char* mime) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ body.size();   // 8 bytes at a time: ~10 GB/s, no allocation
    const char* p = body.data();
    size_t n = body.size();
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    uint64_t tail = 0;
    std::memcpy(&tail, p, n);
    h = (h ^ tail) * 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 29;
    char etag[24];
    std::snprintf(etag, sizeof etag, "\"%016llx\"", static_cast<unsigned long long>(h));
    res.header("ETag", etag).header("Cache-Control", "no-cache");
    const std::string* inm = req.header("if-none-match");
    if (inm && lux::etag_listed(*inm, etag)) {
        res.status(304).send("");
        return;
    }
    res.header("Content-Type", mime).send(std::move(body));
}

} // namespace lux_script::luxp_internal
