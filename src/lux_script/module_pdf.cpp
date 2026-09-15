// A PDF-generation module (NATIVE-MODULES.md), built on cairo's PDF surface
// rather than a PDF-specific library: cairo is already liberally licensed,
// already widely deployed, and already installed on most systems that do any
// graphics work -- no new dependency to vet, just one already trustworthy.
//
// Stateful, like `csv` (module_csv.cpp) -- `create()` hands back an opaque
// `int` handle wrapping a cairo_t*/cairo_surface_t* pair kept in this
// module's own mutex-protected table, freed by `close()`. See that file's
// header comment for why this needed no change to BuiltinModule itself.
#include <lux_script/builtin_module.hpp>
#include <lux_script/crypto.hpp>

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
};

class HandleTable {
public:
    int put(std::unique_ptr<PdfDoc> d) {
        std::lock_guard<std::mutex> lock(mutex_);
        int id = next_id_++;
        docs_.emplace(id, std::move(d));
        return id;
    }
    PdfDoc* get(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = docs_.find(id);
        return it == docs_.end() ? nullptr : it->second.get();
    }
    bool close(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = docs_.find(id);
        if (it == docs_.end()) return false;
        if (it->second->cr) cairo_destroy(it->second->cr);
        if (it->second->surface) cairo_surface_destroy(it->second->surface);
        docs_.erase(it);
        return true;
    }

private:
    std::mutex                                     mutex_;
    std::unordered_map<int, std::unique_ptr<PdfDoc>> docs_;
    int                                              next_id_ = 1;
};

HandleTable& handles() {
    static HandleTable h;
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

Value fn_pdf_create(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_num() || !args[1].is_num()) {
        error = "pdf.create() expects width, height in points";
        return Value::null();
    }
    auto doc = std::make_unique<PdfDoc>();
    doc->surface = cairo_pdf_surface_create_for_stream(write_to_buffer, &doc->buffer,
                                                        args[0].as_float(), args[1].as_float());
    if (cairo_surface_status(doc->surface) != CAIRO_STATUS_SUCCESS) {
        error = "pdf.create(): failed to create the document";
        cairo_surface_destroy(doc->surface);
        return Value::null();
    }
    doc->cr = cairo_create(doc->surface);
    return Value::integer(handles().put(std::move(doc)));
}

// Every drawing function shares this shape: resolve the handle, refuse to
// draw on a document already finish()ed (see finish() above), report the
// same two errors the same way. Kept as a macro-free helper instead of a
// literal macro -- a function pointer capturing the actual draw call would
// need one lambda per caller anyway, so nothing is saved by abstracting the
// two checks out any further.
PdfDoc* resolve_writable(std::vector<Value>& args, std::string& error) {
    auto* d = handles().get(static_cast<int>(args[0].as_int()));
    if (!d) { error = "pdf: unknown handle"; return nullptr; }
    if (d->finished) { error = "pdf: document already saved; cannot draw on it anymore"; return nullptr; }
    return d;
}

Value fn_pdf_add_page(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* d = resolve_writable(args, error);
    if (!d) return Value::null();
    if (!args[1].is_num() || !args[2].is_num()) {
        error = "pdf.add_page() expects width, height in points";
        return Value::null();
    }
    cairo_show_page(d->cr);
    cairo_pdf_surface_set_size(d->surface, args[1].as_float(), args[2].as_float());
    return Value::boolean(true);
}

Value fn_pdf_set_color(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* d = resolve_writable(args, error);
    if (!d) return Value::null();
    if (!args[1].is_num() || !args[2].is_num() || !args[3].is_num()) {
        error = "pdf.set_color() expects r, g, b (0-255)";
        return Value::null();
    }
    auto clamp01 = [](double v) { return std::max(0.0, std::min(1.0, v / 255.0)); };
    cairo_set_source_rgb(d->cr, clamp01(args[1].as_float()), clamp01(args[2].as_float()),
                         clamp01(args[3].as_float()));
    return Value::boolean(true);
}

Value fn_pdf_set_font(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* d = resolve_writable(args, error);
    if (!d) return Value::null();
    if (!args[1].is_str()) { error = "pdf.set_font() expects a family name"; return Value::null(); }
    bool bold   = args.size() > 2 && args[2].is_bool() && args[2].as_bool();
    bool italic = args.size() > 3 && args[3].is_bool() && args[3].as_bool();
    cairo_select_font_face(d->cr, args[1].as_str().c_str(),
                           italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    return Value::boolean(true);
}

Value fn_pdf_text(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* d = resolve_writable(args, error);
    if (!d) return Value::null();
    if (!args[1].is_num() || !args[2].is_num() || !args[3].is_str() || !args[4].is_num()) {
        error = "pdf.text() expects x, y, text, size";
        return Value::null();
    }
    cairo_set_font_size(d->cr, args[4].as_float());
    cairo_move_to(d->cr, args[1].as_float(), args[2].as_float());
    cairo_show_text(d->cr, args[3].as_str().c_str());
    return Value::boolean(true);
}

Value fn_pdf_rect(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* d = resolve_writable(args, error);
    if (!d) return Value::null();
    for (int i = 1; i <= 4; ++i)
        if (!args[i].is_num()) { error = "pdf.rect() expects x, y, w, h"; return Value::null(); }
    bool filled = args.size() > 5 && args[5].is_bool() && args[5].as_bool();
    cairo_rectangle(d->cr, args[1].as_float(), args[2].as_float(), args[3].as_float(),
                    args[4].as_float());
    if (filled) cairo_fill(d->cr); else cairo_stroke(d->cr);
    return Value::boolean(true);
}

Value fn_pdf_line(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* d = resolve_writable(args, error);
    if (!d) return Value::null();
    for (int i = 1; i <= 4; ++i)
        if (!args[i].is_num()) { error = "pdf.line() expects x1, y1, x2, y2"; return Value::null(); }
    cairo_move_to(d->cr, args[1].as_float(), args[2].as_float());
    cairo_line_to(d->cr, args[3].as_float(), args[4].as_float());
    cairo_stroke(d->cr);
    return Value::boolean(true);
}

Value fn_pdf_set_line_width(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* d = resolve_writable(args, error);
    if (!d) return Value::null();
    if (!args[1].is_num()) { error = "pdf.set_line_width() expects a number"; return Value::null(); }
    cairo_set_line_width(d->cr, args[1].as_float());
    return Value::boolean(true);
}

Value fn_pdf_save(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* d = handles().get(static_cast<int>(args[0].as_int()));
    if (!d) { error = "pdf: unknown handle"; return Value::null(); }
    if (!args[1].is_str()) { error = "pdf.save() expects a path"; return Value::null(); }
    finish(*d);
    std::ofstream f(args[1].as_str(), std::ios::binary);
    if (!f) { error = "pdf.save(): cannot write '" + args[1].as_str() + "'"; return Value::null(); }
    f.write(d->buffer.data(), static_cast<std::streamsize>(d->buffer.size()));
    return Value::boolean(true);
}

Value fn_pdf_to_base64(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* d = handles().get(static_cast<int>(args[0].as_int()));
    if (!d) { error = "pdf: unknown handle"; return Value::null(); }
    finish(*d);
    return Value::str(crypto::base64_encode(d->buffer));
}

Value fn_pdf_close(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) { error = "pdf.close() expects a handle"; return Value::null(); }
    return Value::boolean(handles().close(static_cast<int>(args[0].as_int())));
}

class PdfModule : public BuiltinModule {
public:
    const char* name() const override { return "pdf"; }

    const std::vector<BuiltinModuleFn>& functions() const override {
        static const std::vector<BuiltinModuleFn> fns = {
            {"create",         2, 2, fn_pdf_create},
            {"add_page",       3, 3, fn_pdf_add_page},
            {"set_color",      4, 4, fn_pdf_set_color},
            {"set_font",       2, 4, fn_pdf_set_font},
            {"text",           5, 5, fn_pdf_text},
            {"rect",           5, 6, fn_pdf_rect},
            {"line",           5, 5, fn_pdf_line},
            {"set_line_width", 2, 2, fn_pdf_set_line_width},
            {"save",           2, 2, fn_pdf_save},
            {"to_base64",      1, 1, fn_pdf_to_base64},
            {"close",          1, 1, fn_pdf_close},
        };
        return fns;
    }
};

} // namespace

std::unique_ptr<BuiltinModule> make_pdf_module() {
    return std::make_unique<PdfModule>();
}

} // namespace lux_script
