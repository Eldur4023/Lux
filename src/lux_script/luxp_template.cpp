// Templates compiled to .luxp: see luxp_serve.hpp and page/doc_template.hpp.
//
// At compile time a template becomes a PLAN: a DocTemplate (the page's
// document with holes where the data goes), its stylesheet and its program
// (both the same whatever the data is, compressed once).  At request time the
// plan only evaluates the holes, with the template engine's own machinery.

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

#include <atomic>
#include <cstdlib>

namespace lux_script {

namespace {

using luxium::page::DocTemplate;
namespace li = luxp_internal;

struct Plan {
    std::unique_ptr<DocTemplate>                      doc;        // null with html_only
    bool                                              html_only = false;   // JavaScript: never a .luxp
    std::string                                       skeleton;   // to rebuild the sheet when a file changes
    std::atomic<std::shared_ptr<const li::SheetData>> sheet;
    std::atomic<int64_t>                              checked{0};
    luxium::page::Blob                                program;    // compressed once; empty without a script
    std::atomic<size_t>                               size_hint{0};
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
                out += luxium::page::template_marker('H', in.a);
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
    TplHost(const Template& t, const std::vector<Value>& slots, NativeCtx& ctx)
        : t_(t), ctx_(ctx), values_(slots) {
        values_.resize(t.names.size());
    }

    bool hole(uint32_t id, std::string& out) override {
        Value tmp;
        const Value* v = evaluate(id, tmp);
        if (!v) return false;
        write_template_value(*v, false, out);   // raw: the document stores the string, not its HTML escape
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
    const Value* evaluate(uint32_t idx, Value& tmp) {
        static const Value kNull;
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
    std::vector<Value> values_;
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

    // The program is what the page's <script>s say, with no data in them (a
    // marker in a script made build() refuse above).
    luxium::page::ScriptsResult scripts = luxium::page::translate_scripts(*page.document);
    if (scripts.uses_javascript || !scripts.errors.empty()) {
        plan->doc.reset();
        plan->html_only = true;
        t.luxp = plan;
        return;
    }
    plan->program = luxium::page::blob_deflate(scripts.program ? luxium::script::serialize(*scripts.program) : std::string());

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
    t.luxp = plan;
}

LuxpOutcome luxp_respond(NativeCtx& ctx, const Template& t, const std::vector<Value>& slots) {
    Plan* plan = const_cast<Plan*>(static_cast<const Plan*>(t.luxp.get()));
    if (!plan) return LuxpOutcome::Translate;
    if (plan->html_only) return LuxpOutcome::HtmlOnly;

    TplHost host(t, slots, ctx);
    std::string doc;
    doc.reserve(plan->size_hint.load(std::memory_order_relaxed) + 64);
    if (!plan->doc->render(host, doc)) return LuxpOutcome::Translate;   // the HTML path reports why
    plan->size_hint.store(doc.size(), std::memory_order_relaxed);

    auto sheet = current_sheet(*plan);
    static const bool check = env_on("LUX_LUXP_CHECK");
    if (check && !plan_matches_translation(ctx, t, slots, doc, *sheet)) return LuxpOutcome::Translate;

    ctx.res.header("Vary", "Accept")
        .header("Content-Type", luxium::page::kLuxpMime)
        .send(luxium::page::encode_single_page("index.html", luxium::page::blob_plain(std::move(doc)),
                                               sheet->blob, plan->program));
    return LuxpOutcome::Sent;
}

LuxpOutcome luxp_respond_slots(NativeCtx& ctx, size_t template_index, const Value* slots, size_t n) {
    if (!ctx.templates || template_index >= ctx.templates->size()) return LuxpOutcome::Translate;
    return luxp_respond(ctx, (*ctx.templates)[template_index], std::vector<Value>(slots, slots + n));
}

} // namespace lux_script
