// Templates compiled to .luxp: see luxp_serve.hpp and page/doc_template.hpp.
//
// At compile time a template becomes a PLAN: a DocTemplate (the page's
// document with holes where the data goes), its stylesheet and its program
// (both the same whatever the data is, compressed once).  At request time the
// plan only evaluates the holes, with the template engine's own machinery.

#include <lux/atomic_shared.hpp>
#include <page/doc_template_run.hpp>
#include <charconv>
#include "luxp_internal.hpp"
#include <lux_script/luxp_serve.hpp>
#include <lux_script/natives.hpp>
#include <lux_script/template.hpp>
#include <lux_script/vm.hpp>

#include <lux/logger.hpp>
#include <lux/request.hpp>
#include <lux/response.hpp>

#include <page/doc_template.hpp>
#include <page/luxp.hpp>
#include <script/bytecode_io.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace lux_script {

namespace {

using luxium::page::DocTemplate;
namespace li = luxp_internal;

// A hole {{ x }} / {{ x.a.b }} resolved when the plan is built: the slot and the member names, so a
// request neither reads the expression's bytecode nor hashes a name; `hint` remembers where in the
// dict each name was last found (see Dict::find_hint).
struct Direct {
    bool                       ok = false;
    uint32_t                   slot = 0;
    uint8_t                    nkeys = 0;
    std::string_view           keys[4];
    mutable std::atomic<uint8_t> hint[4]{};
};

const Value kNullValue{};

struct Plan {
    std::unique_ptr<Direct[]>                         direct;     // one per t.exprs entry
    std::unique_ptr<DocTemplate>                      doc;        // null with html_only
    bool                                              html_only = false;   // JavaScript: never a .luxp
    std::string                                       skeleton;   // to rebuild the sheet when a file changes
    lux::atomic_shared_ptr<const li::SheetData>       sheet;
    std::atomic<int64_t>                              checked{0};
    luxium::page::Blob                                program;    // compressed once; empty without a script
    // A script with {{ }} in string literals: the program is compiled once with a marker in each such constant,
    // and every request patches the constants (no recompiling) -- see patched_program().
    struct Part { std::string lit; int64_t hole = -1; };
    struct Patch { size_t string = 0; std::vector<Part> parts; };   // an entry of the program's string table
    std::shared_ptr<const luxium::script::Layout>     layout;     // set with patches
    std::vector<Patch>                                patches;    // sorted by string index
    std::atomic<size_t>                               size_hint{0};
    luxium::page::Blob                                doc_blob;   // the document's template as a block (serialized, deflated once)
    std::atomic<size_t>                               trace_hint{0};
};

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && *v == '1';
}

// ── the skeleton: the template rendered once, with markers ──────────────────

bool plain(const std::string& s) { return s.find('\x01') == std::string::npos && s.find('\x02') == std::string::npos; }

// Ops [b, e) of the template, as text with markers.  The structure comes
// back out of the jumps: `if` is JumpIfFalse (target = the else, or the end),
// with a Jump just before the target when there is an else (to the end of
// the whole chain: an `elif` is a nested `if` in the else); a loop is
// LoopStart (target = past its LoopNext) ... LoopNext.
bool skeleton_range(const Template& t, size_t b, size_t e, std::string& out, int depth) {
    if (depth > 32) return false;
    size_t pc = b;
    while (pc < e) {
        const Template::Instr& in = t.code[pc];
        switch (in.op) {
            case Template::Op::Text:
                if (!plain(t.texts[in.a])) return false;
                out += t.texts[in.a];
                ++pc;
                break;
            case Template::Op::Write:
                if (in.b) {   // inside a <script> body: a string literal's content, patched into the program's constants
                    if (depth > 0) return false;   // under an if/for the number of scripts would depend on the data
                    out += luxium::page::template_marker('S', in.a);
                } else {
                    out += luxium::page::template_marker('H', in.a);
                }
                ++pc;
                break;
            case Template::Op::WriteRaw:
                out += luxium::page::template_marker('R', in.a);
                ++pc;
                break;
            case Template::Op::JumpIfFalse: {
                const size_t then_end = in.b;
                if (then_end <= pc || then_end > e) return false;
                // A Jump to the very next instruction is a no-op (an else that
                // is empty): it is not an `else`.
                const bool has_else = then_end - 1 > pc && t.code[then_end - 1].op == Template::Op::Jump &&
                                      t.code[then_end - 1].b > then_end;
                out += luxium::page::template_marker('I', uint32_t(pc));
                if (has_else) {
                    const size_t else_end = t.code[then_end - 1].b;
                    if (else_end > e) return false;
                    if (!skeleton_range(t, pc + 1, then_end - 1, out, depth + 1)) return false;
                    out += luxium::page::template_marker('N', uint32_t(pc));
                    if (!skeleton_range(t, then_end, else_end, out, depth + 1)) return false;
                    out += luxium::page::template_marker('E', uint32_t(pc));
                    pc = else_end;
                } else {
                    if (!skeleton_range(t, pc + 1, then_end, out, depth + 1)) return false;
                    out += luxium::page::template_marker('E', uint32_t(pc));
                    pc = then_end;
                }
                break;
            }
            case Template::Op::Jump:
                if (in.b != pc + 1) return false;   // a jump that is not a no-op, outside an `if`
                ++pc;
                break;
            case Template::Op::LoopStart: {
                const size_t end = in.b;
                if (end <= pc + 1 || end > e || t.code[end - 1].op != Template::Op::LoopNext) return false;
                out += luxium::page::template_marker('L', uint32_t(pc));
                if (!skeleton_range(t, pc + 1, end - 1, out, depth + 1)) return false;
                out += luxium::page::template_marker('E', uint32_t(pc));
                pc = end;
                break;
            }
            case Template::Op::LoopNext:
                return false;   // consumed by its LoopStart
        }
    }
    return true;
}

// ── evaluating the holes: render_template's machinery ───────────────────────

class TplHost final : public DocTemplate::Host {
public:
    // Works on the caller's slots (the loops write their variables into them): no copy.
    TplHost(const Template& t, std::vector<Value>& slots, NativeCtx& ctx, const Direct* direct = nullptr)
        : t_(t), ctx_(ctx), values_(slots), direct_(direct) {
        if (values_.size() < t.names.size()) values_.resize(t.names.size());
    }

    [[gnu::always_inline]] bool hole(uint32_t id, std::string& out) override {
        if (const Value* v = direct(id)) {
            if (v->is_str()) { const std::string& t = v->as_str(); luxium::page::append_fast(out, t.data(), t.size()); }
            else if (v->is_int()) { char buf[24]; const char* e = std::to_chars(buf, buf + sizeof buf, v->as_int()).ptr; luxium::page::append_fast(out, buf, size_t(e - buf)); }
            else write_template_value(*v, false, out);   // raw: the document stores the string, not its HTML escape
            return true;
        }
        return hole_slow(id, out);
    }

    [[gnu::noinline]] bool hole_slow(uint32_t id, std::string& out) {
        Value tmp;
        const Value* v = evaluate(id, tmp);
        if (!v) return false;
        write_template_value(*v, false, out);   // raw: the document stores the string, not its HTML escape
        return true;
    }

    // What a string literal's constant holds for {{ id }}: the lexer has decoded the \" \\ \n escapes the source
    // carried, so only the entities (& < > ' -- see write_template_script) are left of the escaping.
    bool constant_text(uint32_t id, std::string& out) {
        Value tmp;
        const Value* v = evaluate(id, tmp);
        if (!v) return false;
        if (v->is_int()) { write_template_value(*v, false, out); return true; }
        const std::string text = v->is_str() ? std::string() : v->to_string();
        for (char c : v->is_str() ? v->as_str() : text) {
            switch (c) {
                case '&':  out += "&amp;"; break;
                case '<':  out += "&lt;"; break;
                case '>':  out += "&gt;"; break;
                case '\'': out += "&#39;"; break;
                default:   out += c;
            }
        }
        return true;
    }

    bool test(uint32_t pc, bool& value) override {
        Value tmp;
        const Value* v = evaluate(t_.code[pc].a, tmp);
        if (!v) return false;
        value = v->truthy();
        return true;
    }

    bool loop_begin(uint32_t pc, size_t& items) override {
        Value tmp;
        const Value* v = evaluate(t_.code[pc].a, tmp);
        if (!v) return false;
        if (!v->is_list()) {
            error = std::string("{% for %} needs a list, not ") + v->type_name();
            return false;
        }
        lists_.push_back(*v);
        items = v->as_list().size();
        return true;
    }

    void loop_item(uint32_t pc, size_t i, size_t n) override {
        const Template::Instr& in = t_.code[pc];
        values_[in.slot] = lists_.back().as_list()[i];
        if (in.slot_loop != kNoLoop) values_[in.slot_loop] = template_loop_value(i, n);
    }

    void loop_end(uint32_t) override { lists_.pop_back(); }

    std::string error;

private:
    // {{ x }} and {{ x.a.b }} read straight from the slots; anything else, or
    // anything that would fail, goes through the VM -- the same as
    // render_template, so the value and the error are the VM's own.
    // The plan's resolved form of the same read; null when the expression is not that simple, or
    // a member is read from something that is not a dict (the VM reports that).
    [[gnu::always_inline]] inline const Value* direct(uint32_t idx) {
        if (!direct_) return nullptr;
        const Direct& d = direct_[idx];
        if (!d.ok || d.slot >= values_.size()) return nullptr;
        const Value* v = &values_[d.slot];
        for (uint8_t i = 0; i < d.nkeys; ++i) {
            if (!v->is_dict()) return nullptr;
            uint8_t h = d.hint[i].load(std::memory_order_relaxed);
            const uint8_t h0 = h;
            const Value* n = v->as_dict().find_hint(d.keys[i], h);
            if (h != h0) d.hint[i].store(h, std::memory_order_relaxed);
            v = n ? n : &kNullValue;
        }
        return v;
    }

    const Value* evaluate(uint32_t idx, Value& tmp) {
        static const Value kNull{};
        if (const Value* v = direct(idx)) return v;
        const Chunk& c = t_.exprs[idx];
        const auto& code = c.code;
        if (code.size() >= 2 && code.front().op == Op::LoadLocal && code.back().op == Op::Return &&
            code.front().operand < values_.size()) {
            const Value* v = &values_[code.front().operand];
            bool direct = true;
            for (size_t i = 1; i + 1 < code.size(); ++i) {
                if (code[i].op != Op::GetMember || !v->is_dict()) { direct = false; break; }
                const auto& d = v->as_dict();
                const auto it = d.find(c.constants[code[i].operand].as_str());
                v = it == d.end() ? &kNull : &it->second;
            }
            if (direct) return v;
        }
        thread_local VM vm;
        VM::Result r = vm.start(c, values_, ctx_, ctx_.functions);
        if (r.status != VM::Status::Done) {
            error = r.error;
            return nullptr;
        }
        tmp = std::move(r.value);
        return &tmp;
    }

    const Template&    t_;
    NativeCtx&         ctx_;
    std::vector<Value>& values_;
    const Direct*      direct_;
    std::vector<Value> lists_;   // each running loop's list, alive for as long as it runs
};

// The sheet, refreshed when a file it came from changed (looked at most once a second).
std::shared_ptr<const li::SheetData> current_sheet(Plan& plan) {
    auto cur = plan.sheet.load();
    const int64_t now = li::now_ms();
    int64_t last = plan.checked.load(std::memory_order_relaxed);
    if (now - last >= 1000 && plan.checked.compare_exchange_strong(last, now) && li::sheet_stale(*cur)) {
        auto page = luxium::page::parse_page(plan.skeleton);
        cur = li::build_sheet(*page.document, "/");
        plan.sheet.store(cur);
    }
    return cur;
}

// LUX_LUXP_CHECK=1: every answer is also computed the long way (fill the
// template, translate the HTML) and compared; a difference is logged and the
// long way's answer is the one sent.
bool plan_matches_translation(NativeCtx& ctx, const Template& t, const std::vector<Value>& slots,
                              const std::string& plan_doc, const li::SheetData& sheet) {
    std::string html, err;
    if (!render_template(t, slots, ctx, ctx.functions, html, err)) return true;   // the HTML path will say why
    luxium::page::Translation tr = luxium::page::translate_html(
        html, [](const std::string& url) { return luxp_fetch_static(url, "/"); });
    std::string why;
    auto got = luxium::page::decode_document(plan_doc, &why);
    const std::string want_doc = luxium::page::encode_document(*tr.document);
    const std::string got_doc = got ? luxium::page::encode_document(*got) : std::string();
    if (want_doc == got_doc && luxium::page::encode_sheet(tr.rules) == sheet.plain) return true;

    size_t at = 0;
    while (at < want_doc.size() && at < got_doc.size() && want_doc[at] == got_doc[at]) ++at;
    auto around = [&](const std::string& s) {
        std::string out;
        for (size_t i = at > 24 ? at - 24 : 0; i < s.size() && i < at + 40; ++i)
            out += (s[i] >= 32 && s[i] < 127) ? std::string(1, s[i]) : "\\x" + std::string(1, "0123456789abcdef"[(uint8_t(s[i]) >> 4)]) + "0123456789abcdef"[s[i] & 15];
        return out;
    };
    lux::log().error("luxp: a template's plan differs from translating its HTML (document ", want_doc.size(),
                     " vs ", got_doc.size(), " bytes, first difference at ", at, "): translating: ", around(want_doc),
                     " | plan: ", around(got_doc), got ? "" : " | the plan's document does not decode: " + why);
    return false;
}

// The string table of a program compiled from a skeleton: the entries holding S markers become patches.
// False when a marker is somewhere a constant cannot take it (the compiler would have refused most such
// places already): then the template gets no plan.
bool find_patches(const luxium::script::Layout& layout, Plan& plan) {
    constexpr char kStart = '\x01', kEnd = '\x02';
    for (size_t k = 0; k < layout.strings.size(); ++k) {
        const std::string& s = layout.strings[k];
        if (s.find(kStart) == std::string::npos) continue;
        Plan::Patch patch;
        patch.string = k;
        std::string lit;
        for (size_t i = 0; i < s.size();) {
            if (s[i] != kStart) { lit += s[i++]; continue; }
            if (i + 2 >= s.size() || s[i + 1] != 'S') return false;
            size_t j = i + 2;
            uint64_t id = 0;
            size_t digits = 0;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9' && digits < 9) { id = id * 10 + uint64_t(s[j++] - '0'); ++digits; }
            if (!digits || j >= s.size() || s[j] != kEnd) return false;
            if (!lit.empty()) patch.parts.push_back({std::move(lit), -1});
            lit.clear();
            patch.parts.push_back({{}, int64_t(id)});
            i = j + 1;
        }
        if (!lit.empty()) patch.parts.push_back({std::move(lit), -1});
        plan.patches.push_back(std::move(patch));
    }
    return !plan.patches.empty();
}

// The program for this request: the plan's, serialized once, with each patched string rebuilt from the
// holes' values.  Same bytes the translation of the filled page gives (the tests compare them).
bool patched_program(const Plan& plan, TplHost& host, luxium::page::Blob& out) {
    std::vector<std::pair<uint32_t, std::string>> values;   // each hole evaluated once
    std::vector<std::pair<size_t, std::string>> replaced;
    replaced.reserve(plan.patches.size());
    for (const auto& patch : plan.patches) {
        std::string text;
        for (const auto& part : patch.parts) {
            if (part.hole < 0) { text += part.lit; continue; }
            const uint32_t id = uint32_t(part.hole);
            auto it = std::find_if(values.begin(), values.end(), [&](const auto& v) { return v.first == id; });
            if (it == values.end()) {
                std::string v;
                if (!host.constant_text(id, v)) return false;
                it = values.insert(values.end(), {id, std::move(v)});
            }
            text += it->second;
        }
        replaced.emplace_back(patch.string, std::move(text));
    }
    out = luxium::page::blob_deflate(luxium::script::assemble(*plan.layout, replaced), 1);   // per request: speed over the last bytes
    return true;
}

} // namespace

void luxp_attach_plan(Template& t) {
    t.luxp.reset();
    // Diagnostics: LUX_LUXP_PLAN=0 translates every filled page (with its sections cached), plans off.
    if (const char* v = std::getenv("LUX_LUXP_PLAN"); v && *v == '0') return;
    std::string skeleton;
    if (!skeleton_range(t, 0, t.code.size(), skeleton, 0)) return;

    auto plan = std::make_shared<Plan>();
    plan->skeleton = skeleton;
    luxium::page::ParsedPage page = luxium::page::parse_page(skeleton);

    std::string why;
    plan->doc = DocTemplate::build(*page.document, skeleton, &why);
    if (!plan->doc) {
        if (env_on("LUX_LUXP_VERBOSE")) lux::log().info("luxp: no plan for a template (", why, "): its HTML is translated per request");
        return;
    }

    {   // the template itself, for a client that takes the document as template + trace
        const std::string plain = plan->doc->serialize();
        plan->doc_blob = luxium::page::blob_deflate(plain);
        plan->doc_blob.id = luxium::page::block_id(plain);
    }

    // The program is what the page's <script>s say, with no data in them (a
    // marker in a script made build() refuse above).
    luxium::page::ScriptsResult scripts = luxium::page::translate_scripts(*page.document);
    const bool script_holes = skeleton.find("\x01S") != std::string::npos;
    if (scripts.uses_javascript || !scripts.errors.empty()) {
        // With {{ }} in the scripts an error may be the marker's (outside a string literal, say): the filled
        // page is what decides, so no plan -- translated per request, as before.
        if (script_holes && !scripts.uses_javascript) return;
        plan->doc.reset();
        plan->html_only = true;
        t.luxp = plan;
        return;
    }
    if (script_holes) {
        if (!scripts.program) return;
        auto layout = std::make_shared<luxium::script::Layout>(luxium::script::serialize_layout(*scripts.program));
        if (!find_patches(*layout, *plan)) return;
        plan->layout = std::move(layout);
    } else {
        const std::string plain = scripts.program ? luxium::script::serialize(*scripts.program) : std::string();
        plan->program = luxium::page::blob_deflate(plain);
        if (!plain.empty()) plan->program.id = luxium::page::block_id(plain);   // the same on every visit
    }

    // The sheet depends on the path the page is served from only when a URL
    // in it is relative; a template is not tied to one route, so those get no plan.
    bool relative = false;
    auto sheet = li::build_sheet(*page.document, "/", &relative);
    if (relative) {
        if (env_on("LUX_LUXP_VERBOSE")) lux::log().info("luxp: no plan for a template (a stylesheet URL is relative to the page)");
        return;
    }
    plan->sheet.store(sheet);
    plan->checked.store(li::now_ms());
    plan->direct = std::make_unique<Direct[]>(t.exprs.size());
    for (size_t i = 0; i < t.exprs.size(); ++i) {
        const Chunk& c = t.exprs[i];
        const auto& code = c.code;
        Direct& d = plan->direct[i];
        if (code.size() < 2 || code.size() > 2 + 4 || code.front().op != Op::LoadLocal || code.back().op != Op::Return) continue;
        bool ok = true;
        for (size_t k = 1; k + 1 < code.size(); ++k) {
            if (code[k].op != Op::GetMember || !c.constants[code[k].operand].is_str()) { ok = false; break; }
            d.keys[k - 1] = c.constants[code[k].operand].as_str();
        }
        if (!ok) continue;
        d.slot = uint32_t(code.front().operand);
        d.nkeys = uint8_t(code.size() - 2);
        d.ok = true;
    }
    t.luxp = plan;
}

LuxpOutcome luxp_respond(NativeCtx& ctx, const Template& t, std::vector<Value>& slots) {
    Plan* plan = const_cast<Plan*>(static_cast<const Plan*>(t.luxp.get()));
    if (!plan) return LuxpOutcome::Translate;
    if (plan->html_only) return LuxpOutcome::HtmlOnly;

    auto sheet = current_sheet(*plan);
    TplHost host(t, slots, ctx, plan->direct.get());
    static const bool check = env_on("LUX_LUXP_CHECK");
    // LUX_LUXP_ZLEVEL=1..9: deflate the document (or its trace) on every request.  Off by default: it
    // halves the bytes of a plain document but costs ~17 us of CPU a page, +50% on a request that
    // otherwise takes 34 us -- worth it when the link is the bottleneck, not when the CPU is.
    static const int zlevel = [] { const char* v = std::getenv("LUX_LUXP_ZLEVEL"); return v && *v ? std::atoi(v) : 0; }();
    luxium::page::HaveSet have;
    if (const std::string* h = ctx.req.header("x-luxp-have")) have = luxium::page::parse_have(*h);

    // A client that says it takes the document as the template (a block it keeps) plus the trace of
    // the values gets only the values once it holds the template.
    const std::string* caps = ctx.req.header("x-luxp-caps");
    if (caps && caps->find("trace") != std::string::npos) {
        std::string tr;
        tr.reserve(plan->trace_hint.load(std::memory_order_relaxed));
        if (!plan->doc->trace(host, tr)) return LuxpOutcome::Translate;
        plan->trace_hint.store(tr.size(), std::memory_order_relaxed);
        if (check) {   // the trace, run over the template, must give the document run() writes
            TplHost again(t, slots, ctx, plan->direct.get());
            std::string want, got, why;
            if (!plan->doc->run(again, want)) return LuxpOutcome::Translate;
            if (!plan->doc->expand(tr, got, &why) || got != want) {
                lux::log().error("luxp: a trace does not expand to the document the template writes (", why, ")");
                return LuxpOutcome::Translate;
            }
            if (!plan_matches_translation(ctx, t, slots, want, *sheet)) return LuxpOutcome::Translate;
        }
        luxium::page::Blob patched;
        if (plan->layout && !patched_program(*plan, host, patched)) return LuxpOutcome::Translate;
        ctx.res.header("Vary", "Accept, X-Luxp-Have, X-Luxp-Caps");
        // A client that does not hold the template yet is on its first visit to this page: that answer
        // is the big one (template, stylesheet, program, values), and it happens once per client, so
        // deflating the values then costs nothing that matters; the later ones go out plain.
        const int level = zlevel ? zlevel : (have.has(plan->doc_blob.id) ? 0 : 6);
        li::send_luxp(ctx.req, ctx.res,
                      luxium::page::encode_traced_page("index.html", plan->doc_blob, tr, level, sheet->blobs,
                                                       plan->layout ? patched : plan->program, &have),
                      luxium::page::kLuxpMime);
        return LuxpOutcome::Sent;
    }

    // The document is rendered straight into the response, after the container's header.
    std::string body;
    body.reserve(plan->size_hint.load(std::memory_order_relaxed) + sheet->blobs.size() * 128 + plan->program.data.size() + 96);
    const size_t doc_at = luxium::page::begin_single_page(body, "index.html");
    if (!plan->doc->run(host, body)) return LuxpOutcome::Translate;   // the HTML path reports why
    plan->size_hint.store(body.size() - doc_at, std::memory_order_relaxed);

    if (check && !plan_matches_translation(ctx, t, slots, body.substr(doc_at), *sheet)) return LuxpOutcome::Translate;

    luxium::page::Blob patched;
    if (plan->layout && !patched_program(*plan, host, patched)) return LuxpOutcome::Translate;
    luxium::page::finish_single_page(body, doc_at, sheet->blobs, plan->layout ? patched : plan->program, zlevel, &have);
    ctx.res.header("Vary", "Accept, X-Luxp-Have");
    li::send_luxp(ctx.req, ctx.res, std::move(body), luxium::page::kLuxpMime);
    return LuxpOutcome::Sent;
}

LuxpOutcome luxp_respond_slots(NativeCtx& ctx, size_t template_index, const Value* slots, size_t n) {
    if (!ctx.templates || template_index >= ctx.templates->size()) return LuxpOutcome::Translate;
    std::vector<Value> copy(slots, slots + n);   // the caller's array is still read if this falls back
    return luxp_respond(ctx, (*ctx.templates)[template_index], copy);
}

} // namespace lux_script
