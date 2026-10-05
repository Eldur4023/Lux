#pragma once
// Shared by luxp_serve.cpp (translating a filled page) and luxp_template.cpp
// (templates compiled to a plan): the stylesheet of a page, built once and
// kept while the files it came from do not change.

#include <page/luxp.hpp>

#include <chrono>
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
                                             bool* relative_used = nullptr);

// Has any file the sheet came from changed on disk?
bool sheet_stale(const SheetData& s);

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace lux_script::luxp_internal
