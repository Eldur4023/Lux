// Thumbnails and conversions: JPEG, PNG and WebP in and out, through
// libjpeg, libpng and libwebp.
//
//   await image.info("up.jpg")                  -> {width, height, format}
//   await image.resize("up.jpg", "thumb.webp", { "width": 400, "height": 400, "fit": "cover" })
//
// A phone photo is turned upright from its EXIF orientation, and the
// output carries no metadata -- no camera, no GPS position. An image over
// kMaxPixels is refused before it is decoded: a small file can declare a
// huge canvas.
#include <lux_script/builtin_module.hpp>

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/encode.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <csetjmp>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

namespace lux_script {

namespace {

constexpr long long kMaxPixels = 50'000'000;

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width * height * 4
};

std::string lower_ext(const std::string& path) {
    const size_t dot = path.rfind('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string format_of(const std::string& head) {
    if (head.rfind("\xFF\xD8\xFF", 0) == 0) return "jpeg";
    if (head.rfind("\x89PNG", 0) == 0) return "png";
    if (head.size() >= 12 && head.compare(0, 4, "RIFF") == 0 && head.compare(8, 4, "WEBP") == 0) return "webp";
    return "";
}

// ─── JPEG ────────────────────────────────────────────────────────────────────

struct JpegError { jpeg_error_mgr mgr; std::jmp_buf jump; };
void jpeg_fail(j_common_ptr c) { std::longjmp(reinterpret_cast<JpegError*>(c->err)->jump, 1); }

// EXIF orientation (1-8) out of the APP1 marker, 1 when there is none.
int exif_orientation(jpeg_decompress_struct& c) {
    for (jpeg_saved_marker_ptr m = c.marker_list; m; m = m->next) {
        if (m->marker != JPEG_APP0 + 1 || m->data_length < 14 || std::memcmp(m->data, "Exif\0\0", 6) != 0) continue;
        const uint8_t* t = m->data + 6;
        const size_t n = m->data_length - 6;
        const bool le = t[0] == 'I';
        auto u16 = [&](size_t o) { return o + 2 > n ? 0 : le ? t[o] | t[o + 1] << 8 : t[o] << 8 | t[o + 1]; };
        auto u32 = [&](size_t o) -> size_t { return o + 4 > n ? 0 : le ? u16(o) | u16(o + 2) << 16 : u16(o) << 16 | u16(o + 2); };
        const size_t ifd = u32(4);
        for (int i = 0, count = u16(ifd); i < count; ++i) {
            const size_t e = ifd + 2 + static_cast<size_t>(i) * 12;
            if (u16(e) == 0x0112) { const int v = u16(e + 8); return v >= 1 && v <= 8 ? v : 1; }
        }
    }
    return 1;
}

// `want` > 0 lets libjpeg downscale by 1/2, 1/4 or 1/8 while decoding, as
// long as the result stays at least that wide or tall.
bool decode_jpeg(const std::string& data, Image& img, int& orientation, int want, std::string& error) {
    jpeg_decompress_struct c;
    JpegError err;
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_fail;
    if (setjmp(err.jump)) { jpeg_destroy_decompress(&c); error = "not a readable JPEG"; return false; }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, reinterpret_cast<const unsigned char*>(data.data()), data.size());
    jpeg_save_markers(&c, JPEG_APP0 + 1, 0xFFFF);
    jpeg_read_header(&c, TRUE);
    if (static_cast<long long>(c.image_width) * c.image_height > kMaxPixels) {
        jpeg_destroy_decompress(&c);
        error = "the image is over " + std::to_string(kMaxPixels / 1'000'000) + " megapixels";
        return false;
    }
    orientation = exif_orientation(c);
    c.out_color_space = JCS_EXT_RGBA;
    if (want > 0)
        for (int d = 8; d > 1; d /= 2)
            if (std::min(c.image_width, c.image_height) / static_cast<unsigned>(d) >= static_cast<unsigned>(want)) {
                c.scale_num = 1; c.scale_denom = static_cast<unsigned>(d); break;
            }
    jpeg_start_decompress(&c);
    img.width = static_cast<int>(c.output_width);
    img.height = static_cast<int>(c.output_height);
    img.rgba.resize(static_cast<size_t>(img.width) * img.height * 4);
    while (c.output_scanline < c.output_height) {
        JSAMPROW row = img.rgba.data() + static_cast<size_t>(c.output_scanline) * img.width * 4;
        jpeg_read_scanlines(&c, &row, 1);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return true;
}

bool encode_jpeg(const Image& img, int quality, std::string& out, std::string& error) {
    jpeg_compress_struct c;
    JpegError err;
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_fail;
    unsigned char* buf = nullptr;
    unsigned long size = 0;
    if (setjmp(err.jump)) { jpeg_destroy_compress(&c); free(buf); error = "could not encode the JPEG"; return false; }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &buf, &size);
    c.image_width = static_cast<JDIMENSION>(img.width);
    c.image_height = static_cast<JDIMENSION>(img.height);
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    jpeg_start_compress(&c, TRUE);
    // JPEG has no alpha: transparent pixels go over white.
    std::vector<uint8_t> row(static_cast<size_t>(img.width) * 3);
    while (c.next_scanline < c.image_height) {
        const uint8_t* p = img.rgba.data() + static_cast<size_t>(c.next_scanline) * img.width * 4;
        for (int x = 0; x < img.width; ++x, p += 4)
            for (int k = 0; k < 3; ++k) row[x * 3 + k] = static_cast<uint8_t>((p[k] * p[3] + 255 * (255 - p[3])) / 255);
        JSAMPROW r = row.data();
        jpeg_write_scanlines(&c, &r, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    out.assign(reinterpret_cast<char*>(buf), size);
    free(buf);
    return true;
}

// ─── PNG / WebP ──────────────────────────────────────────────────────────────

bool decode_png(const std::string& data, Image& img, std::string& error) {
    png_image p{};
    p.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&p, data.data(), data.size())) { error = "not a readable PNG"; return false; }
    if (static_cast<long long>(p.width) * p.height > kMaxPixels) { png_image_free(&p); error = "the image is too large"; return false; }
    p.format = PNG_FORMAT_RGBA;
    img.width = static_cast<int>(p.width);
    img.height = static_cast<int>(p.height);
    img.rgba.resize(PNG_IMAGE_SIZE(p));
    if (!png_image_finish_read(&p, nullptr, img.rgba.data(), 0, nullptr)) { error = "not a readable PNG"; return false; }
    return true;
}

bool encode_png(const Image& img, std::string& out, std::string& error) {
    png_image p{};
    p.version = PNG_IMAGE_VERSION;
    p.width = static_cast<png_uint_32>(img.width);
    p.height = static_cast<png_uint_32>(img.height);
    p.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0;
    if (!png_image_write_get_memory_size(p, size, 0, img.rgba.data(), 0, nullptr)) { error = "could not encode the PNG"; return false; }
    out.resize(size);
    if (!png_image_write_to_memory(&p, out.data(), &size, 0, img.rgba.data(), 0, nullptr)) { error = "could not encode the PNG"; return false; }
    out.resize(size);
    return true;
}

bool decode_webp(const std::string& data, Image& img, std::string& error) {
    int w = 0, h = 0;
    if (!WebPGetInfo(reinterpret_cast<const uint8_t*>(data.data()), data.size(), &w, &h)) { error = "not a readable WebP"; return false; }
    if (static_cast<long long>(w) * h > kMaxPixels) { error = "the image is too large"; return false; }
    uint8_t* px = WebPDecodeRGBA(reinterpret_cast<const uint8_t*>(data.data()), data.size(), &w, &h);
    if (!px) { error = "not a readable WebP"; return false; }
    img.width = w;
    img.height = h;
    img.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    WebPFree(px);
    return true;
}

bool encode_webp(const Image& img, int quality, std::string& out, std::string& error) {
    uint8_t* buf = nullptr;
    const size_t n = WebPEncodeRGBA(img.rgba.data(), img.width, img.height, img.width * 4, static_cast<float>(quality), &buf);
    if (!n) { error = "could not encode the WebP"; return false; }
    out.assign(reinterpret_cast<char*>(buf), n);
    WebPFree(buf);
    return true;
}

// ─── Transform ───────────────────────────────────────────────────────────────

// EXIF orientations 2-8: mirrored and/or rotated in steps of 90 degrees.
Image upright(const Image& in, int o) {
    if (o <= 1 || o > 8) return in;
    const bool swap = o >= 5;
    Image out;
    out.width = swap ? in.height : in.width;
    out.height = swap ? in.width : in.height;
    out.rgba.resize(in.rgba.size());
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            int sx, sy;
            switch (o) {
                case 2: sx = in.width - 1 - x; sy = y; break;
                case 3: sx = in.width - 1 - x; sy = in.height - 1 - y; break;
                case 4: sx = x; sy = in.height - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = in.height - 1 - x; break;
                case 7: sx = in.width - 1 - y; sy = in.height - 1 - x; break;
                default: sx = in.width - 1 - y; sy = x; break;   // 8
            }
            std::memcpy(&out.rgba[(static_cast<size_t>(y) * out.width + x) * 4],
                        &in.rgba[(static_cast<size_t>(sy) * in.width + sx) * 4], 4);
        }
    return out;
}

// One separable pass with a triangle filter that widens with the scale
// factor: area averaging going down, bilinear going up. Works on
// premultiplied alpha, so transparent edges do not darken.
std::vector<float> pass(const std::vector<float>& src, int sw, int sh, int dw, bool horizontal) {
    const int n = horizontal ? sw : sh, m = dw;
    const int lines = horizontal ? sh : sw;
    const float scale = static_cast<float>(n) / m, support = std::max(1.0f, scale);
    std::vector<float> out(static_cast<size_t>(m) * lines * 4);
    for (int i = 0; i < m; ++i) {
        const float center = (i + 0.5f) * scale;
        const int lo = std::max(0, static_cast<int>(std::floor(center - support))),
                  hi = std::min(n - 1, static_cast<int>(std::ceil(center + support)));
        std::vector<float> w(static_cast<size_t>(hi - lo + 1));
        float total = 0;
        for (int j = lo; j <= hi; ++j) total += w[j - lo] = std::max(0.0f, 1 - std::fabs((j + 0.5f - center) / support));
        for (int line = 0; line < lines; ++line)
            for (int k = 0; k < 4; ++k) {
                float acc = 0;
                for (int j = lo; j <= hi; ++j) {
                    const size_t s = horizontal ? (static_cast<size_t>(line) * n + j) : (static_cast<size_t>(j) * lines + line);
                    acc += src[s * 4 + k] * w[j - lo];
                }
                const size_t d = horizontal ? (static_cast<size_t>(line) * m + i) : (static_cast<size_t>(i) * lines + line);
                out[d * 4 + k] = total > 0 ? acc / total : 0;
            }
    }
    return out;
}

Image scaled(const Image& in, int w, int h) {
    if (w == in.width && h == in.height) return in;
    std::vector<float> px(in.rgba.size());
    for (size_t i = 0; i < in.rgba.size(); i += 4) {
        const float a = in.rgba[i + 3] / 255.0f;
        for (int k = 0; k < 3; ++k) px[i + k] = in.rgba[i + k] * a;
        px[i + 3] = in.rgba[i + 3];
    }
    const auto across = pass(px, in.width, in.height, w, true);
    const auto both = pass(across, w, in.height, h, false);
    Image out;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < out.rgba.size(); i += 4) {
        const float a = both[i + 3];
        for (int k = 0; k < 3; ++k)
            out.rgba[i + k] = static_cast<uint8_t>(std::lround(std::clamp(a > 0 ? both[i + k] * 255.0f / a : 0.0f, 0.0f, 255.0f)));
        out.rgba[i + 3] = static_cast<uint8_t>(std::lround(std::clamp(a, 0.0f, 255.0f)));
    }
    return out;
}

Image cropped(const Image& in, int x0, int y0, int w, int h) {
    Image out;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(&out.rgba[static_cast<size_t>(y) * w * 4], &in.rgba[(static_cast<size_t>(y + y0) * in.width + x0) * 4],
                    static_cast<size_t>(w) * 4);
    return out;
}

// ─── Functions ───────────────────────────────────────────────────────────────

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool load(const std::string& path, int want, Image& img, std::string& format, std::string& error) {
    std::string data;
    if (!read_file(path, data)) { error = "cannot read '" + path + "'"; return false; }
    format = format_of(data.substr(0, 16));
    int orientation = 1;
    const bool ok = format == "jpeg" ? decode_jpeg(data, img, orientation, want, error)
                  : format == "png"  ? decode_png(data, img, error)
                  : format == "webp" ? decode_webp(data, img, error)
                  : (error = "'" + path + "' is not a JPEG, PNG or WebP image", false);
    if (ok) img = upright(img, orientation);
    return ok;
}

// Width and height from the header alone, as displayed (EXIF applied).
bool probe(const std::string& data, const std::string& format, int& w, int& h) {
    if (format == "png") {
        png_image p{};
        p.version = PNG_IMAGE_VERSION;
        if (!png_image_begin_read_from_memory(&p, data.data(), data.size())) return false;
        w = static_cast<int>(p.width); h = static_cast<int>(p.height);
        png_image_free(&p);
        return true;
    }
    if (format == "webp") return WebPGetInfo(reinterpret_cast<const uint8_t*>(data.data()), data.size(), &w, &h);
    jpeg_decompress_struct c;
    JpegError err;
    c.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_fail;
    if (setjmp(err.jump)) { jpeg_destroy_decompress(&c); return false; }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, reinterpret_cast<const unsigned char*>(data.data()), data.size());
    jpeg_save_markers(&c, JPEG_APP0 + 1, 0xFFFF);
    jpeg_read_header(&c, TRUE);
    const bool turned = exif_orientation(c) >= 5;
    w = static_cast<int>(turned ? c.image_height : c.image_width);
    h = static_cast<int>(turned ? c.image_width : c.image_height);
    jpeg_destroy_decompress(&c);
    return true;
}

Value fn_info(NativeCtx&, std::vector<Value>& a, std::string& error) {
    std::string data;
    if (!read_file(a[0].as_str(), data)) { error = "image.info(): cannot read '" + a[0].as_str() + "'"; return Value::null(); }
    const std::string format = format_of(data.substr(0, 16));
    int w = 0, h = 0;
    if (format.empty() || !probe(data, format, w, h)) { error = "image.info(): '" + a[0].as_str() + "' is not a JPEG, PNG or WebP image"; return Value::null(); }
    Value::Dict d;
    d["width"] = Value::integer(w);
    d["height"] = Value::integer(h);
    d["format"] = Value::str(format);
    return Value::dict(std::move(d));
}

// resize(src, dst[, {width, height, fit, quality}]) -> {width, height}.
// fit "contain" (default: inside the box, aspect kept), "cover" (fills the
// box, centre cropped) or "fill" (stretched). With no size it converts:
// the format comes from dst's extension (.jpg .png .webp).
Value fn_resize(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const Value::Dict empty;
    const Value::Dict& o = a.size() > 2 && a[2].is_dict() ? a[2].as_dict() : empty;
    auto num = [&](const char* k, int def) { auto it = o.find(k); return it != o.end() && it->second.is_num() ? static_cast<int>(it->second.as_float()) : def; };
    const int bw = num("width", 0), bh = num("height", 0), quality = std::clamp(num("quality", 85), 1, 100);
    const std::string fit = o.count("fit") ? o.find("fit")->second.to_string() : "contain";
    if (fit != "contain" && fit != "cover" && fit != "fill") { error = "image.resize(): fit is \"contain\", \"cover\" or \"fill\""; return Value::null(); }
    if (bw < 0 || bh < 0 || bw > 20000 || bh > 20000) { error = "image.resize(): width/height must be between 1 and 20000"; return Value::null(); }
    const std::string ext = lower_ext(a[1].as_str());
    if (ext != "jpg" && ext != "jpeg" && ext != "png" && ext != "webp") { error = "image.resize(): the destination must end in .jpg, .png or .webp"; return Value::null(); }

    Image img;
    std::string format;
    if (!load(a[0].as_str(), fit == "cover" ? std::max(bw, bh) : std::min(bw ? bw : bh, bh ? bh : bw), img, format, error)) {
        error = "image.resize(): " + error;
        return Value::null();
    }
    const double sw = img.width, sh = img.height;
    int w = img.width, h = img.height;
    if (bw || bh) {
        const double rx = bw ? bw / sw : bh / sh, ry = bh ? bh / sh : bw / sw;
        if (fit == "fill") { w = bw ? bw : static_cast<int>(std::lround(sw * ry)); h = bh ? bh : static_cast<int>(std::lround(sh * rx)); }
        else {
            const double r = fit == "cover" && bw && bh ? std::max(rx, ry) : std::min(rx, ry);
            w = std::max(1, static_cast<int>(std::lround(sw * r)));
            h = std::max(1, static_cast<int>(std::lround(sh * r)));
        }
    }
    img = scaled(img, w, h);
    if (fit == "cover" && bw && bh && (w > bw || h > bh))
        img = cropped(img, (w - std::min(w, bw)) / 2, (h - std::min(h, bh)) / 2, std::min(w, bw), std::min(h, bh));

    std::string out;
    const bool ok = ext == "png" ? encode_png(img, out, error)
                  : ext == "webp" ? encode_webp(img, quality, out, error)
                  : encode_jpeg(img, quality, out, error);
    if (!ok) { error = "image.resize(): " + error; return Value::null(); }
    std::ofstream f(a[1].as_str(), std::ios::binary | std::ios::trunc);
    if (!f.write(out.data(), static_cast<std::streamsize>(out.size()))) { error = "image.resize(): cannot write '" + a[1].as_str() + "'"; return Value::null(); }
    Value::Dict d;
    d["width"] = Value::integer(img.width);
    d["height"] = Value::integer(img.height);
    return Value::dict(std::move(d));
}

} // namespace

LUX_MODULE(image, {
    {"info",   "s>d",    fn_info,   /*is_async=*/true},
    {"resize", "ss|d>d", fn_resize, /*is_async=*/true},
})

} // namespace lux_script
