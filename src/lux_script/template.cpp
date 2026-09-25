#include <lux_script/template.hpp>

#include <lux_script/emitter.hpp>
#include <lux_script/lexer.hpp>
#include <lux_script/parser.hpp>
#include <lux_script/vm.hpp>

#include <map>
#include <memory>
#include <filesystem>
#include <fstream>

namespace lux_script {

void escape_html(const std::string& in, std::string& out) {
    // What must not be touched is copied in one go, as in the JSON escaping:
    // the text of a page almost never carries metacharacters.
    size_t clean = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const char* rep = nullptr;
        switch (in[i]) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default: continue;
        }
        out.append(in, clean, i - clean);
        out += rep;
        clean = i + 1;
    }
    out.append(in, clean, in.size() - clean);
}

namespace {

// Trims spaces on the left/right according to the {%- and -%} markers.
void trim_before_tag(std::string& t) {
    size_t n = t.size();
    while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\t' ||
                     t[n - 1] == '\n' || t[n - 1] == '\r')) --n;
    t.resize(n);
}
void trim_after_tag(size_t& i, const std::string& src) {
    while (i < src.size() && (src[i] == ' ' || src[i] == '\t' ||
                              src[i] == '\n' || src[i] == '\r')) ++i;
}

std::string trim(std::string s) { return trim_ascii_ws(s); }


// ─── Compiler ────────────────────────────────────────────────────────────────

class Compiler {
public:
    Compiler(const std::string& dir, DiagnosticBag& diags, Template& out)
        : dir_(dir), diags_(diags), out_(out) {}

    bool compile(const std::string& source, const std::string& file,
                  const std::vector<TypedName>& data) {
        out_.names = data;

        // Inheritance: it climbs the {% extends %} chain collecting blocks.  The
        // child wins, so only the block that was not there yet is recorded.  At
        // the end the ROOT is compiled, with the blocks substituted.
        std::string root = source, root_file = file;
        for (int n = 0; n <= 16; ++n) {
            std::string parent;
            if (!parent_of(root, root_file, parent)) break;
            if (n == 16) {
                error(start_loc(root_file),
                      "too many chained {% extends %}: is there a cycle?");
                return false;
            }
            collect_blocks(root, root_file);
            std::string parent_text;
            if (!read_template(parent, parent_text)) {
                error(start_loc(root_file),
                      "base template not found: '" + parent + "'");
                return false;
            }
            root      = std::move(parent_text);
            root_file = parent;
        }

        body(root, root_file, 0);
        if (!open_.empty()) {
            error(open_.back().loc,
                  "missing {% end" + open_.back().kind + " %}");
        }
        return !failed_;
    }

private:
    struct Open {
        std::string          kind;      // "if" or "for"
        SourceLoc            loc;
        std::vector<size_t>  patches;  // jumps to the end of the block
        size_t               start = 0;
        // Names this loop shadows, to give them back when it closes.
        std::vector<std::pair<size_t, std::string>> shadowed;
    };

    // Shadows an already visible name.  Slots cannot be moved —every compiled
    // chunk indexes by position— so the outer one is renamed to something that
    // is not a valid identifier: it stays unreachable for as long as the loop
    // lasts, which is exactly what shadowing means.
    void shadow(const std::string& name, std::vector<std::pair<size_t, std::string>>& shadowed) {
        for (size_t i = out_.names.size(); i-- > 0;) {
            if (out_.names[i].name == name) {
                shadowed.emplace_back(i, out_.names[i].name);
                // The marker carries the slot so two shadowed names cannot clash
                // with each other: the emitter does not allow that either.
                out_.names[i].name = " shadowed" + std::to_string(i);
                break;
            }
        }
    }

    const std::string&   dir_;
    DiagnosticBag&       diags_;
    Template&           out_;
    bool                 failed_ = false;
    std::vector<Open> open_;
    std::vector<std::string> file_stack_;   // to detect include cycles

    // The files are kept alive because SourceLoc points at their path.
    std::vector<std::unique_ptr<SourceFile>> files_;

    void error(SourceLoc loc, std::string msg) {
        failed_ = true;
        diags_.error(loc, std::move(msg));
    }

    const std::string* intern_file_name(const std::string& f) {
        for (const auto& sf : files_)
            if (sf->path == f && sf->text.empty()) return &sf->path;
        files_.push_back(std::make_unique<SourceFile>(SourceFile{f, {}}));
        return &files_.back()->path;
    }

    size_t text(std::string t) {
        if (t.empty()) return SIZE_MAX;
        out_.texts.push_back(std::move(t));
        out_.code.push_back({Template::Op::Text,
                             static_cast<uint32_t>(out_.texts.size() - 1), 0, 0, 0, {}});
        return out_.code.size() - 1;
    }

    // Compiles a Lux Script expression with the names visible right now.
    // Returns its index in out_.exprs, or SIZE_MAX if it does not compile.
    size_t expression(const std::string& expr_text, SourceLoc loc,
                     const std::string& file) {
        // The fragment is lexed as if it were a file of its own.  Its errors
        // carry the position INSIDE the fragment, which is no use to anyone: the
        // template location is reported and the reason attached.
        files_.push_back(std::make_unique<SourceFile>(
            SourceFile{file + " (expression)", expr_text}));
        SourceFile&   sf = *files_.back();
        DiagnosticBag own_diags;

        Lexer   lx(sf, own_diags);
        Parser  ps(lx.tokenize(), own_diags);
        ExprPtr e = ps.parse_single_expression();
        if (!e || !own_diags.empty()) {
            error(loc, "in the template expression: " +
                  (own_diags.empty() ? std::string("invalid expression")
                                   : own_diags.items().front().message));
            return SIZE_MAX;
        }

        Chunk   ch;
        Emitter em(own_diags);
        if (!em.emit_condition(*e, out_.names, ch) || !own_diags.empty()) {
            error(loc, "in the template expression: " +
                  (own_diags.empty() ? std::string("cannot be compiled")
                                   : own_diags.items().front().message));
            return SIZE_MAX;
        }
        out_.exprs.push_back(std::move(ch));
        return out_.exprs.size() - 1;
    }

    void body(const std::string& src, const std::string& file, int depth);

    // Blocks the child (or the grandchild) defines, by name.  They also keep
    // which file they came from, so the error points at the right place.
    std::map<std::string, std::pair<std::string, std::string>> blocks_;

    SourceLoc start_loc(const std::string& file) {
        SourceLoc l;
        l.file = intern_file_name(file);
        l.line = 1;
        l.col  = 1;
        return l;
    }

    bool read_template(const std::string& name, std::string& out) {
        if (name.empty() || name.find("..") != std::string::npos) return false;
        auto text = read_whole_file(std::filesystem::path(dir_) / name);
        if (!text) return false;
        out = std::move(*text);
        return true;
    }

    // If the template starts with {% extends "x" %}, returns x.  It has to be
    // first: a template that inherits contributes no text of its own, only blocks.
    bool parent_of(const std::string& src, const std::string& file,
                  std::string& parent) {
        size_t i = src.find_first_not_of(" \t\r\n");
        if (i == std::string::npos || src.compare(i, 2, "{%") != 0) return false;
        size_t close_pos = src.find("%}", i);
        if (close_pos == std::string::npos) return false;
        std::string inner = trim(src.substr(i + 2, close_pos - i - 2));
        if (inner.rfind("extends", 0) != 0) return false;
        parent = trim(inner.substr(7));
        if (parent.size() >= 2 && (parent.front() == 0x22 || parent.front() == 0x27))
            parent = parent.substr(1, parent.size() - 2);
        if (parent.empty()) {
            error(start_loc(file), "{% extends %} without a template name");
            return false;
        }
        return true;
    }

    // Index right after the {% endblock %} closing the one that starts at
    // `from`, counting the nested ones.
    static size_t end_of_block(const std::string& src, size_t from_, size_t& content_end) {
        int  level = 1;
        size_t i   = from_;
        while (i < src.size()) {
            size_t open_pos = src.find("{%", i);
            if (open_pos == std::string::npos) break;
            size_t close_pos = src.find("%}", open_pos);
            if (close_pos == std::string::npos) break;
            std::string inner = trim(src.substr(open_pos + 2, close_pos - open_pos - 2));
            if (inner.rfind("block", 0) == 0)      ++level;
            else if (inner.rfind("endblock", 0) == 0) {
                if (--level == 0) { content_end = open_pos; return close_pos + 2; }
            }
            i = close_pos + 2;
        }
        content_end = src.size();
        return std::string::npos;
    }

    // Records the blocks of a child template.  The first one seen wins: it
    // climbs from the most derived, so the lower one shadows the upper.
    void collect_blocks(const std::string& src, const std::string& file) {
        size_t i = 0;
        while (i < src.size()) {
            size_t open_pos = src.find("{%", i);
            if (open_pos == std::string::npos) break;
            size_t close_pos = src.find("%}", open_pos);
            if (close_pos == std::string::npos) break;
            std::string inner = trim(src.substr(open_pos + 2, close_pos - open_pos - 2));
            if (inner.rfind("block", 0) != 0) { i = close_pos + 2; continue; }

            std::string name = trim(inner.substr(5));
            size_t      content_end = 0;
            size_t      after       = end_of_block(src, close_pos + 2, content_end);
            if (after == std::string::npos) {
                error(start_loc(file),
                      "missing {% endblock %} for block '" + name + "'");
                return;
            }
            if (!name.empty() && !blocks_.count(name))
                blocks_[name] = {src.substr(close_pos + 2, content_end - close_pos - 2), file};
            i = after;
        }
    }
};

} // namespace

// The body is defined outside so {% include %} can recurse.
void Compiler::body(const std::string& src, const std::string& file,
                        int depth) {
    size_t i = 0, text_from = 0;

    auto loc_at = [&](size_t pos) {
        SourceLoc l;
        l.file = intern_file_name(file);
        int ln = 1, col = 1;
        for (size_t k = 0; k < pos && k < src.size(); ++k) {
            if (src[k] == '\n') { ++ln; col = 1; } else ++col;
        }
        l.line = ln;
        l.col = col;
        return l;
    };

    while (i < src.size()) {
        size_t open_pos = src.find('{', i);
        if (open_pos == std::string::npos || open_pos + 1 >= src.size()) break;
        const char type = src[open_pos + 1];
        if (type != '{' && type != '%' && type != '#') { i = open_pos + 1; continue; }

        const bool trim_before = (open_pos + 2 < src.size() && src[open_pos + 2] == '-');
        const char* close_str = (type == '{') ? "}}" : (type == '%') ? "%}" : "#}";
        size_t close_pos = src.find(close_str, open_pos + 2);
        if (close_pos == std::string::npos) {
            error(loc_at(open_pos), std::string("missing the closing '") +
                  (type == '{' ? "{{" : type == '%' ? "{%" : "{#") + "'");
            return;
        }
        const bool trim_after = (close_pos > open_pos + 2 && src[close_pos - 1] == '-');

        std::string preceding = src.substr(text_from, open_pos - text_from);
        if (trim_before) trim_before_tag(preceding);
        text(std::move(preceding));

        size_t content_begin = open_pos + 2 + (trim_before ? 1 : 0);
        size_t content_stop  = close_pos - (trim_after ? 1 : 0);
        std::string content = trim(src.substr(content_begin, content_stop - content_begin));
        const SourceLoc loc = loc_at(open_pos);

        i = close_pos + 2;
        if (trim_after) trim_after_tag(i, src);
        text_from = i;

        if (type == '#') continue;                       // comment

        if (type == '{') {                               // {{ expression }}
            bool raw = false;
            if (content.size() > 5 &&
                content.compare(content.size() - 5, 5, "|safe") == 0) {
                raw = true;
                content = trim(content.substr(0, content.size() - 5));
            }
            if (content.find('|') != std::string::npos) {
                error(loc,
                      "Jinja2 filters do not exist here: they are Lux Script methods. "
                      "Instead of {{ x|upper }}, write {{ x.upper() }}");
                continue;
            }
            size_t k = expression(content, loc, file);
            if (k == SIZE_MAX) continue;
            out_.code.push_back({raw ? Template::Op::WriteRaw
                                       : Template::Op::Write,
                                 static_cast<uint32_t>(k), 0, 0, 0, loc});
            continue;
        }

        // {% tag ... %}
        std::string tag = content.substr(0, content.find_first_of(" \t"));
        std::string rest    = trim(content.substr(tag.size()));

        if (tag == "if") {
            size_t k = expression(rest, loc, file);
            // Still pushed onto `open_` even when the condition itself
            // failed to compile (`k == SIZE_MAX`, e.g. an undeclared
            // variable) -- with `start = SIZE_MAX`, the same sentinel
            // `else` below already uses for "no pending condition to
            // patch". Before this, a broken `{% if %}` left NOTHING on
            // `open_`, so its own `{% endif %}` (or `{% elif %}`/
            // `{% else %}`) found the stack empty and reported a second,
            // unrelated "{% endif %} without {% if %}" — real template
            // code, misdiagnosed as a structural typo — on top of the one
            // real error that made the condition fail in the first place.
            // The template is never going to run anyway once ANY error is
            // reported (diags already has one), so what codegen happens
            // for the rest of this broken block does not have to be
            // correct, only structurally balanced enough not to cascade.
            Open ob{"if", loc, {}, k == SIZE_MAX ? SIZE_MAX : out_.code.size()};
            if (k != SIZE_MAX)
                out_.code.push_back({Template::Op::JumpIfFalse,
                                     static_cast<uint32_t>(k), 0, 0, 0, loc});
            open_.push_back(std::move(ob));

        } else if (tag == "elif" || tag == "else") {
            if (open_.empty() || open_.back().kind != "if") {
                error(loc, "{% " + tag + " %} without {% if %}");
                continue;
            }
            // The previous block jumps to the end of the whole chain.
            out_.code.push_back({Template::Op::Jump, 0, 0, 0, 0, loc});
            open_.back().patches.push_back(out_.code.size() - 1);
            // And the pending JumpIfFalse lands here -- unless the `if` (or
            // a previous `elif`) never actually emitted one because ITS
            // condition failed to compile (see the `if` branch above).
            if (open_.back().start != SIZE_MAX)
                out_.code[open_.back().start].b =
                    static_cast<uint32_t>(out_.code.size());

            if (tag == "elif") {
                size_t k = expression(rest, loc, file);
                if (k == SIZE_MAX) { open_.back().start = SIZE_MAX; continue; }
                out_.code.push_back({Template::Op::JumpIfFalse,
                                     static_cast<uint32_t>(k), 0, 0, 0, loc});
                open_.back().start = out_.code.size() - 1;
            } else {
                open_.back().start = SIZE_MAX;   // no condition left pending
            }

        } else if (tag == "endif") {
            if (open_.empty() || open_.back().kind != "if") {
                error(loc, "{% endif %} without {% if %}");
                continue;
            }
            Open ob = std::move(open_.back());
            open_.pop_back();
            if (ob.start != SIZE_MAX)
                out_.code[ob.start].b = static_cast<uint32_t>(out_.code.size());
            for (size_t p : ob.patches)
                out_.code[p].b = static_cast<uint32_t>(out_.code.size());

        } else if (tag == "for") {
            // for <name> in <expression>
            size_t in_pos = rest.find(" in ");
            if (in_pos == std::string::npos) {
                error(loc, "expected {% for x in list %}");
                continue;
            }
            std::string var   = trim(rest.substr(0, in_pos));
            std::string list_ = trim(rest.substr(in_pos + 4));
            if (var.empty()) {
                error(loc, "missing the loop variable name");
                continue;
            }
            size_t k = expression(list_, loc, file);
            if (k == SIZE_MAX) continue;

            // Slots only grow, and the names this loop shadows are recorded to
            // give them back at the {% endfor %}.
            std::vector<std::pair<size_t, std::string>> shadowed;
            shadow(var, shadowed);
            shadow("loop", shadowed);

            const uint32_t slot = static_cast<uint32_t>(out_.names.size());
            // The loop variable carries no type: it depends on what is inside
            // the list, and that is not known here.
            out_.names.push_back({var, {}});
            const uint32_t slot_loop = static_cast<uint32_t>(out_.names.size());
            out_.names.push_back({"loop", {}});

            out_.code.push_back({Template::Op::LoopStart,
                                 static_cast<uint32_t>(k), 0, slot, slot_loop, loc});
            Open ob{"for", loc, {}, out_.code.size() - 1, std::move(shadowed)};
            open_.push_back(std::move(ob));

        } else if (tag == "endfor") {
            if (open_.empty() || open_.back().kind != "for") {
                error(loc, "{% endfor %} without {% for %}");
                continue;
            }
            Open ob = std::move(open_.back());
            open_.pop_back();
            for (const auto& [idx, old_name] : ob.shadowed) out_.names[idx].name = old_name;
            const auto& start_instr = out_.code[ob.start];
            out_.code.push_back({Template::Op::LoopNext, 0,
                                 static_cast<uint32_t>(ob.start + 1),
                                 start_instr.slot, start_instr.slot_loop, loc});
            out_.code[ob.start].b = static_cast<uint32_t>(out_.code.size());

        } else if (tag == "include") {
            if (depth > 16) {
                error(loc, "too many nested {% include %}: is there a cycle?");
                continue;
            }
            std::string name = trim(rest);
            if (name.size() >= 2 && (name.front() == '"' || name.front() == '\''))
                name = name.substr(1, name.size() - 2);
            std::filesystem::path path = std::filesystem::path(dir_) / name;
            if (name.empty() || name.find("..") != std::string::npos) {
                error(loc, "invalid template name in {% include %}");
                continue;
            }
            auto included = read_whole_file(path);
            if (!included) {
                error(loc, "template not found: '" + name + "'");
                continue;
            }
            body(*included, name, depth + 1);

        } else if (tag == "block") {
            // At the root: if the child defined this block, ITS text is compiled
            // and the one here is skipped; if not, the one here is compiled,
            // which is the default value.
            const std::string name = trim(rest);
            size_t content_end = 0;
            size_t after = end_of_block(src, i, content_end);
            if (after == std::string::npos) {
                error(loc, "missing {% endblock %} for block '" + name + "'");
                return;
            }
            auto it = blocks_.find(name);
            if (it != blocks_.end()) {
                body(it->second.first, it->second.second, depth + 1);
                i = after;
                text_from = i;
            }
            // With no replacement, reading carries on normally: the default
            // content is compiled as is and the {% endblock %} does nothing.

        } else if (tag == "endblock") {
            // Closes a block whose default content has just been compiled.

        } else if (tag == "extends") {
            error(loc, "{% extends %} must be the first thing in the template");

        } else if (tag == "macro" || tag == "endmacro") {
            error(loc, "{% " + tag + " %} does not exist in Lux Script: "
                                "to reuse, use {% include %} or a function from the .lux");
        } else {
            error(loc, "unknown tag: {% " + tag + " %}");
        }
    }

    if (text_from < src.size()) text(src.substr(text_from));
}

bool compile_template(const std::string& source, const std::string& file,
                        const std::string& dir,
                        const std::vector<TypedName>& data,
                        DiagnosticBag& diags, Template& out) {
    Compiler c(dir, diags, out);
    return c.compile(source, file, data);
}

// ─── Rendering ───────────────────────────────────────────────────────────────

bool render_template(const Template& p, std::vector<Value> values,
                      NativeCtx& ctx, const FunctionTable* fns,
                      std::string& out, std::string& error) {
    // One slot per name: the input data already arrives, the rest are loop
    // variables that get filled in.
    values.resize(p.names.size());

    // Each loop's list is kept alive for as long as it lasts: the item of every
    // pass comes out of it.
    struct Frame { std::shared_ptr<Value> list_; size_t i; };
    std::vector<Frame> loops;

    thread_local VM vm;

    auto evaluate = [&](uint32_t idx, Value& out) -> bool {
        VM::Result r = vm.start(p.exprs[idx], values, ctx, fns);
        if (r.status != VM::Status::Done) {
            error = r.error;
            return false;
        }
        out = std::move(r.value);
        return true;
    };

    auto set_loop = [&](uint32_t slot_loop, size_t i, size_t n) {
        Value::Dict d;
        d.reserve(5);
        d["index"]  = Value::integer(static_cast<long long>(i + 1));
        d["index0"] = Value::integer(static_cast<long long>(i));
        d["first"]  = Value::boolean(i == 0);
        d["last"]   = Value::boolean(i + 1 == n);
        d["length"] = Value::integer(static_cast<long long>(n));
        values[slot_loop] = Value::dict(std::move(d));
    };

    size_t pc = 0;
    while (pc < p.code.size()) {
        const auto& in = p.code[pc];
        switch (in.op) {
            case Template::Op::Text:
                out += p.texts[in.a];
                ++pc;
                break;

            case Template::Op::Write: {
                Value v;
                if (!evaluate(in.a, v)) return false;
                escape_html(v.to_string(), out);
                ++pc;
                break;
            }
            case Template::Op::WriteRaw: {
                Value v;
                if (!evaluate(in.a, v)) return false;
                out += v.to_string();
                ++pc;
                break;
            }
            case Template::Op::JumpIfFalse: {
                Value v;
                if (!evaluate(in.a, v)) return false;
                pc = v.truthy() ? pc + 1 : in.b;
                break;
            }
            case Template::Op::Jump:
                pc = in.b;
                break;

            case Template::Op::LoopStart: {
                Value v;
                if (!evaluate(in.a, v)) return false;
                if (!v.is_list()) {
                    error = std::string("{% for %} needs a list, not ") +
                            v.type_name();
                    return false;
                }
                auto list_ = std::make_shared<Value>(std::move(v));
                if (list_->as_list().empty()) { pc = in.b; break; }
                loops.push_back({list_, 0});
                values[in.slot] = list_->as_list()[0];
                set_loop(in.slot_loop, 0, list_->as_list().size());
                ++pc;
                break;
            }
            case Template::Op::LoopNext: {
                Frame& m = loops.back();
                const auto& l = m.list_->as_list();
                if (++m.i < l.size()) {
                    values[in.slot] = l[m.i];
                    set_loop(in.slot_loop, m.i, l.size());
                    pc = in.b;
                } else {
                    loops.pop_back();
                    ++pc;
                }
                break;
            }
        }
    }
    return true;
}

} // namespace lux_script
