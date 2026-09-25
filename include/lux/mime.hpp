#pragma once
// Content-Type from a file extension (".png" -> "image/png"): static files,
// send_file() and os.mime() all read this one table.
#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace lux {

// Without the right Content-Type a <track> subtitle (.vtt) is silently
// ignored and an HLS player refuses the manifest/segments (.m3u8/.ts).
inline const char* mime_for_ext(std::string_view ext) {
    static constexpr std::pair<std::string_view, const char*> kMime[] = {
        {".html",  "text/html; charset=utf-8"},
        {".htm",   "text/html; charset=utf-8"},
        {".css",   "text/css; charset=utf-8"},
        {".js",    "application/javascript; charset=utf-8"},
        {".json",  "application/json; charset=utf-8"},
        {".svg",   "image/svg+xml"},
        {".png",   "image/png"},
        {".jpg",   "image/jpeg"},
        {".jpeg",  "image/jpeg"},
        {".gif",   "image/gif"},
        {".webp",  "image/webp"},
        {".ico",   "image/x-icon"},
        {".woff",  "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf",   "font/ttf"},
        {".pdf",   "application/pdf"},
        {".xml",   "application/xml"},
        {".txt",   "text/plain; charset=utf-8"},
        {".wasm",  "application/wasm"},
        {".mjs",   "application/javascript; charset=utf-8"},
        {".map",   "application/json; charset=utf-8"},
        {".mp4",   "video/mp4"},
        {".webm",  "video/webm"},
        {".mp3",   "audio/mpeg"},
        {".ogg",   "audio/ogg"},
        {".avif",  "image/avif"},
        {".vtt",   "text/vtt; charset=utf-8"},
        {".m3u8",  "application/vnd.apple.mpegurl"},
        {".ts",    "video/mp2t"},
        {".mkv",   "video/x-matroska"},
        {".flac",  "audio/flac"},
        {".wav",   "audio/wav"},
        {".m4a",   "audio/mp4"},
        {".csv",   "text/csv; charset=utf-8"},
        {".md",    "text/markdown; charset=utf-8"},
        {".srt",   "application/x-subrip"},
        {".zip",   "application/zip"},
        {".gz",    "application/gzip"},
        {".docx",  "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".xlsx",  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".pptx",  "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {".heic",  "image/heic"},
        {".mov",   "video/quicktime"},
    };
    // Case-insensitive: a camera's "IMG_0001.JPG" is still a JPEG.
    for (auto& [e, mime] : kMime)
        if (e.size() == ext.size() &&
            std::equal(e.begin(), e.end(), ext.begin(),
                       [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); }))
            return mime;
    return "application/octet-stream";
}

} // namespace lux
