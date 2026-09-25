// A PDF-generation module (NATIVE-MODULES.md), built on cairo's PDF surface
// rather than a PDF-specific library: cairo is already liberally licensed,
// already widely deployed, and already installed on most systems that do any
// graphics work -- no new dependency to vet, just one already trustworthy.
//
// create() hands back an int handle to a cairo surface kept in this
// module's table until close().
#include <lux_script/builtin_module.hpp>
#include <lux_script/crypto.hpp>
#include <lux/response.hpp>

#include <cairo/cairo-pdf.h>
#include <cairo/cairo.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace lux_script {

namespace {

struct PdfDoc {
    cairo_surface_t* surface  = nullptr;
    cairo_t*         cr       = nullptr;
    std::string      buffer;      // cairo writes the PDF bytes here (see the
                                   // write callback below), not to a file --
                                   // save() and to_base64() are both just
                                   // different ways of handing this out.
    bool             finished = false;

    PdfDoc() = default;
    PdfDoc(const PdfDoc&) = delete;
    PdfDoc& operator=(const PdfDoc&) = delete;
    ~PdfDoc() {
        if (cr) cairo_destroy(cr);
        if (surface) cairo_surface_destroy(surface);
    }
};

HandleTable<PdfDoc>& handles() {
    static HandleTable<PdfDoc> h;
    return h;
}

cairo_status_t write_to_buffer(void* closure, const unsigned char* data, unsigned int length) {
    static_cast<std::string*>(closure)->append(reinterpret_cast<const char*>(data), length);
    return CAIRO_STATUS_SUCCESS;
}

// cairo_surface_finish() flushes every pending draw call into the write
// callback above -- neither save() nor to_base64() can read `buffer` before
// it runs, and it can only run once (a second call is a harmless no-op in
// cairo itself, but drawing again afterward is not, so `finished` is
// enforced here explicitly rather than trusted to cairo).
void finish(PdfDoc& d) {
    if (d.finished) return;
    cairo_surface_finish(d.surface);
    d.finished = true;
}

Value fn_pdf_create(NativeCtx&, std::vector<Value>& a, std::string& error) {
    auto doc = std::make_unique<PdfDoc>();
    doc->surface = cairo_pdf_surface_create_for_stream(write_to_buffer, &doc->buffer, a[0].as_float(), a[1].as_float());
    if (cairo_surface_status(doc->surface) != CAIRO_STATUS_SUCCESS) {
        error = "pdf.create(): failed to create the document";
        return Value::null();   // ~PdfDoc destroys the surface
    }
    doc->cr = cairo_create(doc->surface);
    return Value::integer(handles().put(std::move(doc)));
}

std::shared_ptr<PdfDoc> doc(std::vector<Value>& a, std::string& error, bool drawing = true) {
    auto d = handles().get(a[0].as_int());
    if (!d) error = "pdf: unknown handle";
    else if (drawing && d->finished) { error = "pdf: document already saved; cannot draw on it anymore"; return nullptr; }
    return d;
}

double num(const std::vector<Value>& a, size_t i) { return a[i].as_float(); }

// Every drawing call: find the document, run the cairo calls, true.
template <class F>
Value draw(std::vector<Value>& a, std::string& error, F f) {
    auto d = doc(a, error);
    if (!d) return Value::null();
    f(d->cr, d.get());
    return Value::boolean(true);
}

Value fn_pdf_add_page(NativeCtx&, std::vector<Value>& a, std::string& e) {
    return draw(a, e, [&](cairo_t* cr, PdfDoc* d) {
        cairo_show_page(cr);
        cairo_pdf_surface_set_size(d->surface, num(a, 1), num(a, 2));
    });
}

// r, g, b in 0-255.
Value fn_pdf_set_color(NativeCtx&, std::vector<Value>& a, std::string& e) {
    auto c = [&](size_t i) { return std::clamp(num(a, i) / 255.0, 0.0, 1.0); };
    return draw(a, e, [&](cairo_t* cr, PdfDoc*) { cairo_set_source_rgb(cr, c(1), c(2), c(3)); });
}

Value fn_pdf_set_font(NativeCtx&, std::vector<Value>& a, std::string& e) {
    const bool bold = a.size() > 2 && a[2].as_bool(), italic = a.size() > 3 && a[3].as_bool();
    return draw(a, e, [&](cairo_t* cr, PdfDoc*) {
        cairo_select_font_face(cr, a[1].as_str().c_str(), italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
                               bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    });
}

Value fn_pdf_text(NativeCtx&, std::vector<Value>& a, std::string& e) {
    return draw(a, e, [&](cairo_t* cr, PdfDoc*) {
        cairo_set_font_size(cr, num(a, 4));
        cairo_move_to(cr, num(a, 1), num(a, 2));
        cairo_show_text(cr, a[3].as_str().c_str());
    });
}

// How wide `text` is at `size` in the current font -- what right-aligning
// an amount or centring a title needs.
Value fn_pdf_text_width(NativeCtx&, std::vector<Value>& a, std::string& error) {
    auto d = doc(a, error);
    if (!d) return Value::null();
    cairo_set_font_size(d->cr, num(a, 2));
    cairo_text_extents_t ext;
    cairo_text_extents(d->cr, a[1].as_str().c_str(), &ext);
    return Value::real(ext.x_advance);
}

Value fn_pdf_rect(NativeCtx&, std::vector<Value>& a, std::string& e) {
    const bool filled = a.size() > 5 && a[5].as_bool();
    return draw(a, e, [&](cairo_t* cr, PdfDoc*) {
        cairo_rectangle(cr, num(a, 1), num(a, 2), num(a, 3), num(a, 4));
        filled ? cairo_fill(cr) : cairo_stroke(cr);
    });
}

Value fn_pdf_line(NativeCtx&, std::vector<Value>& a, std::string& e) {
    return draw(a, e, [&](cairo_t* cr, PdfDoc*) {
        cairo_move_to(cr, num(a, 1), num(a, 2));
        cairo_line_to(cr, num(a, 3), num(a, 4));
        cairo_stroke(cr);
    });
}

Value fn_pdf_set_line_width(NativeCtx&, std::vector<Value>& a, std::string& e) {
    return draw(a, e, [&](cairo_t* cr, PdfDoc*) { cairo_set_line_width(cr, num(a, 1)); });
}

// A PNG file at x, y -- scaled to w x h when given (a logo).
Value fn_pdf_image(NativeCtx&, std::vector<Value>& a, std::string& error) {
    auto d = doc(a, error);
    if (!d) return Value::null();
    cairo_surface_t* img = cairo_image_surface_create_from_png(a[1].as_str().c_str());
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        error = "pdf.image(): cannot read '" + a[1].as_str() + "' as a PNG";
        return Value::null();
    }
    const double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    const double w = a.size() > 4 ? num(a, 4) : iw, h = a.size() > 5 ? num(a, 5) : ih * w / iw;
    cairo_save(d->cr);
    cairo_translate(d->cr, num(a, 2), num(a, 3));
    cairo_scale(d->cr, w / iw, h / ih);
    cairo_set_source_surface(d->cr, img, 0, 0);
    cairo_paint(d->cr);
    cairo_restore(d->cr);
    cairo_surface_destroy(img);
    return Value::boolean(true);
}

// The document, finished (its bytes in ->buffer), or nullptr with `error`.
std::shared_ptr<PdfDoc> finished(std::vector<Value>& a, std::string& error) {
    auto d = doc(a, error, false);
    if (d) finish(*d);
    return d;
}

Value fn_pdf_save(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const auto d = finished(a, error);
    if (!d) return Value::null();
    std::ofstream f(a[1].as_str(), std::ios::binary);
    if (!f.write(d->buffer.data(), static_cast<std::streamsize>(d->buffer.size()))) {
        error = "pdf.save(): cannot write '" + a[1].as_str() + "'";
        return Value::null();
    }
    return Value::boolean(true);
}

Value fn_pdf_to_base64(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const auto d = finished(a, error);
    return d ? Value::str(crypto::base64_encode(d->buffer)) : Value::null();
}

// The PDF as this request's response: `pdf.send(doc, "invoice.pdf")`
// shows it in the browser; pass true as a third argument to download it.
Value fn_pdf_send(NativeCtx& ctx, std::vector<Value>& a, std::string& error) {
    const auto d = finished(a, error);
    if (!d) return Value::null();
    std::string name = a.size() > 1 ? a[1].as_str() : "document.pdf";
    std::erase_if(name, [](char c) { return c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20; });
    const bool download = a.size() > 2 && a[2].as_bool();
    ctx.res.header("Content-Type", "application/pdf")
           .header("Content-Disposition", std::string(download ? "attachment" : "inline") + "; filename=\"" + name + "\"")
           .send(d->buffer);
    ctx.response_written = true;
    return Value::null();
}

Value fn_pdf_close(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::boolean(handles().close(a[0].as_int()));
}

} // namespace

LUX_MODULE(pdf, {
    {"create",         "nn",     fn_pdf_create},
    {"add_page",       "inn",    fn_pdf_add_page},
    {"set_color",      "innn",   fn_pdf_set_color},
    {"set_font",       "is|bb",  fn_pdf_set_font},
    {"text",           "innsn",  fn_pdf_text},
    {"text_width",     "isn",    fn_pdf_text_width},
    {"rect",           "innnn|b", fn_pdf_rect},
    {"line",           "innnn",  fn_pdf_line},
    {"set_line_width", "in",     fn_pdf_set_line_width},
    {"image",          "isnn|nn", fn_pdf_image},
    {"save",           "is",     fn_pdf_save},
    {"to_base64",      "i",      fn_pdf_to_base64},
    {"send",           "i|sb",   fn_pdf_send},
    {"close",          "i",      fn_pdf_close},
})

} // namespace lux_script
