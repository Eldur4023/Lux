#pragma once
// The run loop of a DocTemplate, as a template on the host type -- see DocTemplate::run().
#include "page/doc_template.hpp"

#include <cstring>
#include <deque>

namespace luxium::page {

inline void put_uv(std::string& out, uint64_t v) {
    while (v >= 0x80) { out += char((v & 0x7F) | 0x80); v >>= 7; }
    out += char(v);
}

inline void put_str(std::string& out, const std::string& s) { put_uv(out, s.size()); out += s; }

// A count that is not known yet: one byte now, patched when it is -- a node
// with children is written in place, with no buffer of its own for them.  Most
// counts fit the byte; the rare 128+ child list is lengthened (the reader only
// sees LEB128).
// Appends n bytes.  A short copy is done with a few overlapping fixed-size moves instead of a call to
// memcpy: a page is hundreds of short literals and values.
inline void copy_small(char* d, const char* s, size_t n) {
    if (n >= 16) { std::memcpy(d, s, 16); std::memcpy(d + n - 16, s + n - 16, 16); }
    else if (n >= 8) { std::memcpy(d, s, 8); std::memcpy(d + n - 8, s + n - 8, 8); }
    else if (n >= 4) { std::memcpy(d, s, 4); std::memcpy(d + n - 4, s + n - 4, 4); }
    else if (n >= 2) { std::memcpy(d, s, 2); std::memcpy(d + n - 2, s + n - 2, 2); }
    else if (n) d[0] = s[0];
}

[[gnu::always_inline]] inline void append_fast(std::string& out, const char* p, size_t n) {
    if (n > 32) { out.append(p, n); return; }
    const size_t sz = out.size();
    out.resize_and_overwrite(sz + n, [&](char* b, size_t) {
        copy_small(b + sz, p, n);
        return sz + n;
    });
}

// A count that is not known yet: one byte now, patched when it is -- a node
// with children is written in place, with no buffer of its own for them.  Most
// counts fit the byte; the rare 128+ child list is lengthened (the reader only
// sees LEB128).
[[gnu::always_inline]] inline size_t reserve_count(std::string& out) {
    const size_t at = out.size();
    out.resize_and_overwrite(at + 1, [](char* b, size_t n) { b[n - 1] = '\0'; return n; });
    return at;
}

[[gnu::noinline]] inline void patch_count_long(std::string& out, size_t at, uint32_t n) {
    std::string enc;
    put_uv(enc, n);
    out.replace(at, 1, enc);   // moves what was written after it, once
}

[[gnu::always_inline]] inline void patch_count(std::string& out, size_t at, uint32_t n) {
    if (n < 0x80) out[at] = char(n);
    else patch_count_long(out, at, n);
}

namespace detail {

// The text buffer of an element being rendered: one per nesting level, kept (with its capacity) between
// requests -- a local std::string would allocate on every element whose text is over 15 bytes.
struct TextPool {
    std::deque<std::string> bufs;   // deque: references stay valid as it grows
    size_t top = 0;
    std::string& take() {
        if (top == bufs.size()) bufs.emplace_back();
        std::string& s = bufs[top++];
        s.clear();
        return s;
    }
};
inline thread_local TextPool g_text;

// What is open while the program runs: an element (its child count and the text of its parent are
// waiting) or a loop.
struct Frame {
    bool         loop;
    uint32_t     id;                 // loop
    uint32_t     index_or_count;     // loop: the current item; element: the parent's child count
    size_t       items_or_at;        // loop: how many; element: where its child count is
    std::string* parent_pending;     // element
};
inline thread_local std::vector<Frame> g_frames;

inline void flush_text(std::string& out, uint32_t& count, std::string& pending) {
    if (pending.empty()) return;
    out += char(dom::NodeType::Text);
    put_uv(out, pending.size());
    out += pending;
    ++count;
    pending.clear();
}


} // namespace detail

template <class H>
bool DocTemplate::run(H& h, std::string& out, size_t max_out, uint64_t max_steps) const {
    out += prologue_;
    out += char(dom::NodeType::Document);
    const size_t doc_at = reserve_count(out);
    uint32_t count = 0;

    const size_t base_top = detail::g_text.top;
    std::string* pending = &detail::g_text.take();
    auto& frames = detail::g_frames;
    const size_t base_frames = frames.size();
    const Op* ops = ops_.data();
    const char* lits = lits_.data();
    const Part* parts = parts_.data();
    bool ok = true;
    uint64_t steps = 0;

    for (size_t pc = 0; ok;) {
        const Op& op = ops[pc++];
        if (++steps > max_steps) { ok = false; break; }
        switch (op.code) {
            case Op::End:
                goto done;
            case Op::Static:
                detail::flush_text(out, count, *pending);
                append_fast(out, lits + op.a, op.b);
                count += op.c;
                break;
            case Op::Text:
                for (uint32_t i = 0; i < op.b && ok; ++i) {
                    const Part& p = parts[op.a + i];
                    if (p.hole >= 0) ok = h.hole(uint32_t(p.hole), *pending);
                    else append_fast(*pending, lits + p.off, p.len);
                }
                break;
            case Op::ElemOpen:
                detail::flush_text(out, count, *pending);
                append_fast(out, lits + op.a, op.b);
                break;
            case Op::AttrStatic:
                append_fast(out, lits + op.a, op.b);
                break;
            case Op::AttrDyn: {
                const size_t at = reserve_count(out);
                const size_t start = out.size();
                for (uint32_t i = 0; i < op.b && ok; ++i) {
                    const Part& p = parts[op.a + i];
                    if (p.hole >= 0) ok = h.hole(uint32_t(p.hole), out);
                    else append_fast(out, lits + p.off, p.len);
                }
                patch_count(out, at, uint32_t(out.size() - start));
                break;
            }
            case Op::Leaf: {
                // <el ...>text</el>: the child count, the text node's type and length, then the text,
                // straight into the output; an empty text means no child at all.
                const size_t count_at = reserve_count(out);
                out += char(dom::NodeType::Text);
                const size_t len_at = reserve_count(out);
                const size_t start = out.size();
                for (uint32_t i = 0; i < op.b && ok; ++i) {
                    const Part& p = parts[op.a + i];
                    if (p.hole >= 0) ok = h.hole(uint32_t(p.hole), out);
                    else append_fast(out, lits + p.off, p.len);
                }
                const size_t n = out.size() - start;
                if (n == 0) {
                    out.resize(count_at);
                    out += '\0';
                } else {
                    out[count_at] = '\1';
                    patch_count(out, len_at, uint32_t(n));
                }
                ++count;
                break;
            }
            case Op::ElemBody:
                if (frames.size() - base_frames >= 1024) { ok = false; break; }
                frames.push_back({false, 0, count, reserve_count(out), pending});
                pending = &detail::g_text.take();
                count = 0;
                break;
            case Op::ElemClose: {
                if (frames.size() <= base_frames || frames.back().loop) { ok = false; break; }
                detail::flush_text(out, count, *pending);
                const detail::Frame f = frames.back();
                frames.pop_back();
                patch_count(out, f.items_or_at, count);
                --detail::g_text.top;
                pending = f.parent_pending;
                count = f.index_or_count + 1;
                break;
            }
            case Op::LoopBegin: {
                size_t items = 0;
                if (!h.loop_begin(op.a, items)) { ok = false; break; }
                if (items == 0) { h.loop_end(op.a); pc = op.b; break; }
                if (frames.size() - base_frames >= 1024) { ok = false; break; }
                frames.push_back({true, op.a, 0, items, nullptr});
                h.loop_item(op.a, 0, items);
                break;
            }
            case Op::LoopNext: {
                if (frames.size() <= base_frames || !frames.back().loop || out.size() > max_out) { ok = false; break; }
                detail::Frame& f = frames.back();
                if (++f.index_or_count < f.items_or_at) {
                    h.loop_item(op.a, f.index_or_count, f.items_or_at);
                    pc = op.b;
                } else {
                    h.loop_end(op.a);
                    frames.pop_back();
                }
                break;
            }
            case Op::If: {
                bool v = false;
                if (!h.test(op.a, v)) { ok = false; break; }
                if (!v) pc = op.b;
                break;
            }
            case Op::Jump:
                pc = op.a;
                break;
        }
    }
done:
    if (ok && frames.size() != base_frames) ok = false;   // a program that ends with something open
    if (!ok) {   // the host failed: close what is open
        while (frames.size() > base_frames) {
            if (frames.back().loop) h.loop_end(frames.back().id);
            frames.pop_back();
        }
        detail::g_text.top = base_top;
        return false;
    }
    detail::flush_text(out, count, *pending);
    patch_count(out, doc_at, count);
    detail::g_text.top = base_top;
    return true;
}

template <class H>
bool DocTemplate::trace(H& h, std::string& out) const {
    struct Loop { uint32_t id; size_t i, n; };
    thread_local std::vector<Loop> loops;
    const size_t base = loops.size();
    const Op* ops = ops_.data();
    const Part* parts = parts_.data();
    auto holes = [&](const Op& op) {
        for (uint32_t i = 0; i < op.b; ++i) {
            const Part& p = parts[op.a + i];
            if (p.hole < 0) continue;
            const size_t at = reserve_count(out);
            const size_t start = out.size();
            if (!h.hole(uint32_t(p.hole), out)) return false;
            patch_count(out, at, uint32_t(out.size() - start));
        }
        return true;
    };
    bool ok = true;
    for (size_t pc = 0; ok;) {
        const Op& op = ops[pc++];
        switch (op.code) {
            case Op::End: goto done;
            case Op::Text: case Op::AttrDyn: case Op::Leaf:
                ok = holes(op);
                break;
            case Op::LoopBegin: {
                size_t items = 0;
                if (!h.loop_begin(op.a, items)) { ok = false; break; }
                put_uv(out, items);
                if (items == 0) { h.loop_end(op.a); pc = op.b; break; }
                loops.push_back({op.a, 0, items});
                h.loop_item(op.a, 0, items);
                break;
            }
            case Op::LoopNext: {
                Loop& f = loops.back();
                if (++f.i < f.n) { h.loop_item(op.a, f.i, f.n); pc = op.b; }
                else { h.loop_end(op.a); loops.pop_back(); }
                break;
            }
            case Op::If: {
                bool v = false;
                if (!h.test(op.a, v)) { ok = false; break; }
                out += char(v ? 1 : 0);
                if (!v) pc = op.b;
                break;
            }
            case Op::Jump: pc = op.a; break;
            default: break;   // structure: the reader of the trace has it from the template
        }
    }
done:
    if (!ok) {
        while (loops.size() > base) { h.loop_end(loops.back().id); loops.pop_back(); }
        return false;
    }
    return true;
}

} // namespace luxium::page
