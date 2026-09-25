#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>


namespace lux_script {

// Sanitizers have to see every box as an allocation of its own: a recycler
// hides use-after-free from them, which is exactly the bug they are there to
// catch.  With one active, boxes go straight to the allocator.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define LUX_NO_RECYCLER 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define LUX_NO_RECYCLER 1
#  endif
#endif

namespace detail {

// Recycler for boxes of one size, one per thread.
//
// Value boxes are fixed size and die the moment the refcount hits zero, so
// handing them back to the general allocator and asking for them again is work
// that can be skipped: building a string in a loop takes and releases the same
// box thousands of times.
//
// A box can be born on a database pool thread and die on the event loop one, so
// it ends up on the list of whichever thread releases it.  Hence the cap:
// without it, a producer thread and a consumer thread would leave all the
// memory parked on the second one's list.
template <size_t N>
class Recycler {
public:
    static_assert(N >= sizeof(void*), "the box must be able to hold the link");

    void* take() {
        if (!head_) return ::operator new(N);
        void* p = head_;
        head_  = *reinterpret_cast<void**>(p);
        --count_;
        return p;
    }

    void give_back(void* p) noexcept {
        if (count_ >= kCap) { ::operator delete(p); return; }
        *reinterpret_cast<void**>(p) = head_;
        head_ = p;
        ++count_;
    }

    ~Recycler() {
        while (head_) {
            void* p = head_;
            head_  = *reinterpret_cast<void**>(p);
            ::operator delete(p);
        }
    }

private:
    static constexpr size_t kCap = 256;

    void*  head_  = nullptr;
    size_t count_ = 0;
};

template <size_t N>
Recycler<N>& recycler() {
    thread_local Recycler<N> r;
    return r;
}

}  // namespace detail

// Runtime value of the VM.
//
// Takes 16 bytes: 8 of payload and 1 of tag.  A value is of exactly one type at
// a time, so the heap pointer shares room with the scalars in a union.  It used
// to be 72 —three shared_ptr, two of them always null— and every push and pop
// of the VM moved those 72 bytes.
class Value {
public:
    // Func: a reference to a user-defined `fn`, used to pass one to
    // List.map/filter/reduce/for_each (natives.cpp) -- NOT a closure: it
    // carries no captured environment, just which function, the same way a
    // C function pointer does. Stored inline in `i_` (the function's index
    // into the module's FunctionTable, exactly what Op::CallFunction's
    // operand already encodes at compile time for an ordinary call) --
    // never on_heap(), same as Int/Bool/Float: there is nothing to free.
    enum class Type { Null, Bool, Int, Float, Str, List, Dict, Func };

    using List = std::vector<Value>;

    // Dictionary of the VM.
    //
    // It used to be a std::map: a red-black tree, with one allocation per node
    // and a pointer hop per access, for objects that almost always have between
    // 3 and 10 keys.  Measured with string keys, a linear scan over a contiguous
    // vector runs twice as fast below 8 keys and the tree does not win until 64.
    //
    // So this is always a vector, in insertion order, and past a threshold it
    // gains a hash index.  That way lookup is also O(1) in big dictionaries —an
    // accumulator, a word counter— where a linear scan would be a disaster, and
    // insertion is amortized O(1) at every size, which the tree never gave.
    //
    // The key order of the response does not change: it is fixed when the JSON
    // object is built, not by this container.
    class Dict {
    public:
        using Pair           = std::pair<std::string, Value>;
        using iterator       = std::vector<Pair>::iterator;
        using const_iterator = std::vector<Pair>::const_iterator;

        iterator       begin()       { return v_.begin(); }
        iterator       end()         { return v_.end();   }
        const_iterator begin() const { return v_.begin(); }
        const_iterator end()   const { return v_.end();   }

        size_t size()  const { return v_.size();  }
        bool   empty() const { return v_.empty(); }
        void   clear()       { v_.clear(); idx_.clear(); }

        // Whoever knows how many keys they will insert —the VM's MakeDict has
        // them counted— keeps the vector from growing in steps: without this a
        // 3-key dictionary does three allocations, one per reallocation.
        void   reserve(size_t n) { v_.reserve(n); }
        size_t count(std::string_view k) const { return find(k) == end() ? 0 : 1; }

        // Removes a key if present, returns whether it was. `idx_` maps key
        // -> position in v_, so erasing shifts every later position by one --
        // rebuilding it wholesale (build_index()) is the same "do not patch
        // incrementally" choice insertion already makes above the threshold,
        // just for removal instead. Below the threshold there is no index to
        // worry about.
        bool erase(std::string_view k) {
            auto it = find(k);
            if (it == v_.end()) return false;
            v_.erase(it);
            if (!idx_.empty()) build_index();
            return true;
        }

        iterator       find(std::string_view k);
        const_iterator find(std::string_view k) const;
        Value&         operator[](std::string_view k);

    private:
        // Heterogeneous lookup: without it, every find with a const char* or a
        // string_view would build a std::string just to query.
        struct Hash {
            using is_transparent = void;
            size_t operator()(std::string_view s) const noexcept {
                return std::hash<std::string_view>{}(s);
            }
        };
        struct Equal {
            using is_transparent = void;
            bool operator()(std::string_view a, std::string_view b) const noexcept {
                return a == b;
            }
        };

        // Below the threshold there is no index: keeping it would cost more than
        // walking the whole vector.
        static constexpr size_t kIndexThreshold = 16;

        void build_index();

        std::vector<Pair>                                    v_;
        std::unordered_map<std::string, size_t, Hash, Equal> idx_;
    };

    Value() = default;
    ~Value() { release(); }

    Value(const Value& o) : type_(o.type_) { copy_payload(o); retain(); }
    Value(Value&& o) noexcept : type_(o.type_) { copy_payload(o); o.type_ = Type::Null; }

    Value& operator=(const Value& o) {
        if (this != &o) {
            o.retain();            // before release: it may be the same box
            release();
            type_ = o.type_;
            copy_payload(o);
        }
        return *this;
    }
    Value& operator=(Value&& o) noexcept {
        if (this != &o) {
            release();
            type_ = o.type_;
            copy_payload(o);
            o.type_ = Type::Null;
        }
        return *this;
    }

    static Value null()               { return Value(); }
    static Value boolean(bool b)      { Value v; v.type_ = Type::Bool;  v.b_ = b; return v; }
    static Value integer(long long i) { Value v; v.type_ = Type::Int;   v.i_ = i; return v; }
    static Value real(double d)       { Value v; v.type_ = Type::Float; v.d_ = d; return v; }

    static Value str(std::string s) {
        Value v; v.type_ = Type::Str;  v.o_ = new_box<CStr>(std::move(s));  return v;
    }
    static Value list(List l = {}) {
        Value v; v.type_ = Type::List; v.o_ = new_box<CList>(std::move(l)); return v;
    }
    static Value dict(Dict d = {}) {
        Value v; v.type_ = Type::Dict; v.o_ = new_box<CDict>(std::move(d)); return v;
    }
    static Value func(long long index) {
        Value v; v.type_ = Type::Func; v.i_ = index; return v;
    }

    Type type() const { return type_; }

    bool is_null()  const { return type_ == Type::Null; }
    bool is_bool()  const { return type_ == Type::Bool; }
    bool is_int()   const { return type_ == Type::Int; }
    bool is_float() const { return type_ == Type::Float; }
    bool is_num()   const { return is_int() || is_float(); }
    bool is_str()   const { return type_ == Type::Str; }
    bool is_list()  const { return type_ == Type::List; }
    bool is_dict()  const { return type_ == Type::Dict; }
    bool is_func()  const { return type_ == Type::Func; }

    bool               as_bool()  const { return b_; }
    long long          as_int()   const { return i_; }
    double             as_float() const { return is_float() ? d_ : double(i_); }
    const std::string& as_str()   const { return static_cast<CStr*>(o_)->v;  }
    List&              as_list()  const { return static_cast<CList*>(o_)->v; }
    Dict&              as_dict()  const { return static_cast<CDict*>(o_)->v; }
    long long          as_func_index() const { return i_; }

    // Lux Script truthiness, Python style: null, false, 0, 0.0, the empty
    // string and the empty containers are false.
    //
    // This is NOT the JavaScript coercion the README complains about: that one
    // converts types inside '==' behind your back.  There is no conversion here,
    // just a read in boolean context — the same one Python and C++ do, the two
    // languages Lux Script comes from.
    bool truthy() const {
        switch (type_) {
            case Type::Null:  return false;
            case Type::Bool:  return b_;
            case Type::Int:   return i_ != 0;
            case Type::Float: return d_ != 0;
            case Type::Str:   return !as_str().empty();
            case Type::List:  return !as_list().empty();
            case Type::Dict:  return !as_dict().empty();
            case Type::Func:  return true;
        }
        return false;
    }

    const char* type_name() const;
    std::string to_string() const;          // representation for text()/concatenation
    // Serialization: writes the JSON straight into the buffer, in one pass.
    void        write_json(std::string& out) const;
    std::string to_json_text() const { std::string s; s.reserve(256); write_json(s); return s; }

    // Parses JSON into a Value, with no intermediate tree.  Returns false for
    // anything that is not valid and complete JSON, trailing garbage included.
    // See src/lux_script/json_parse.cpp.
    static bool parse_json(std::string_view text, Value& out);

    bool equals(const Value& o) const;

    // Ordering: `this < o`. `ok` is false for anything not both numbers or
    // both strings -- no coercion, same rule equals() already follows.
    // <=, >, >= are all derivable from this plus equals() (a<=b is
    // !(b<a), etc.) for any domain where `ok` is true, since numbers and
    // strings are both total orders -- so this is the ONE place ordering
    // logic lives. It used to be a second, separate implementation inside
    // vm.cpp's Lt/Le/Gt/Ge handling; List.sort() (natives.cpp) needed the
    // exact same comparison and duplicating it would have been exactly the
    // kind of silent-divergence risk this project has already been burned
    // by once (db.hpp's DbOp comment) -- so it moved here instead, and
    // vm.cpp now calls this too.
    bool less_than(const Value& o, bool& ok) const;

private:
    // Box with its own counter.  Atomic because a value can be born on a
    // database pool thread and be consumed on the event loop one: the handover
    // is synchronized, but the box can stay shared between the two.
    struct Box { std::atomic<unsigned> rc{1}; };
    template <typename T>
    struct BoxOf : Box { T v; explicit BoxOf(T x) : v(std::move(x)) {} };

    using CStr  = BoxOf<std::string>;
    using CList = BoxOf<List>;
    using CDict = BoxOf<Dict>;

    // Boxes go through the thread's recycler instead of new/delete.  The
    // reference count still decides when one dies: the only thing that changes
    // is where the memory goes afterwards.
    template <typename C, typename T>
    static Box* new_box(T x) {
#ifdef LUX_NO_RECYCLER
        return new C(std::move(x));
#else
        void* m = detail::recycler<sizeof(C)>().take();
        return new (m) C(std::move(x));
#endif
    }

    template <typename C>
    void release_box() {
#ifdef LUX_NO_RECYCLER
        delete static_cast<C*>(o_);
#else
        C* c = static_cast<C*>(o_);
        c->~C();
        detail::recycler<sizeof(C)>().give_back(c);
#endif
    }

    bool on_heap() const {
        return type_ == Type::Str || type_ == Type::List || type_ == Type::Dict;
    }
    void copy_payload(const Value& o) { std::memcpy(&i_, &o.i_, sizeof(i_)); }
    void retain() const {
        if (on_heap()) o_->rc.fetch_add(1, std::memory_order_relaxed);
    }
    void release() {
        if (!on_heap()) return;
        if (o_->rc.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        switch (type_) {
            case Type::Str:  release_box<CStr>();  break;
            case Type::List: release_box<CList>(); break;
            case Type::Dict: release_box<CDict>(); break;
            default: break;
        }
    }

    union {
        long long i_ = 0;
        bool      b_;
        double    d_;
        Box*     o_;
    };
    Type type_ = Type::Null;
};

// ─── Value::Dict ─────────────────────────────────────────────────────────────
// Outside the class because they need Value to be complete.

inline void Value::Dict::build_index() {
    idx_.clear();
    idx_.reserve(v_.size() * 2);
    for (size_t i = 0; i < v_.size(); ++i) idx_.emplace(v_[i].first, i);
}

inline Value::Dict::iterator Value::Dict::find(std::string_view k) {
    return v_.begin() + (std::as_const(*this).find(k) - v_.cbegin());
}

inline Value::Dict::const_iterator Value::Dict::find(std::string_view k) const {
    if (!idx_.empty()) {
        auto it = idx_.find(k);
        return it == idx_.end() ? v_.end()
                                : v_.begin() + static_cast<std::ptrdiff_t>(it->second);
    }
    for (auto it = v_.begin(); it != v_.end(); ++it)
        if (it->first == k) return it;
    return v_.end();
}

inline Value& Value::Dict::operator[](std::string_view k) {
    if (auto it = find(k); it != v_.end()) return it->second;

    v_.emplace_back(std::string(k), Value());
    if (!idx_.empty())            idx_.emplace(v_.back().first, v_.size() - 1);
    else if (v_.size() > kIndexThreshold) build_index();
    return v_.back().second;
}

// A Dict read by position. Almost always a failed call -- a query() that
// returned its {"error": ...} Dict where the code expected rows -- so the
// message carries that error instead of a lesson on key types. Shared by
// the VM and the --native runtime.
inline std::string dict_int_index_error(const Value& obj) {
    auto it = obj.as_dict().find("error");
    if (it != obj.as_dict().end() && it->second.is_str())
        return "indexed by position, but this is an error result, not a List: " + it->second.as_str();
    return "a Dict key must be a string, not int";
}

// Trims ASCII whitespace (space/tab/CR/LF) off both ends -- the exact same
// four lines used to be reimplemented separately in natives.cpp (string
// .trim()) and template.cpp (its own free-standing trim()).
inline std::string trim_ascii_ws(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ─── UTF-8 helpers ──────────────────────────────────────────────────────────
//
// Lux Script strings are UTF-8 bytes with no separate "codepoint" concept of
// their own -- `len()`, `.upper()`/`.lower()`, and iterating a string with
// `for` used to all work byte-by-byte, which is exactly right for ASCII and
// silently wrong for anything else: `len("ñandú")` counted 7 (the two
// accented letters each take 2 UTF-8 bytes), not the 5 characters a person
// looking at the word would count, and `.upper()`/`.lower()` left every
// accented letter untouched (`toupper()`/`tolower()` are single-byte and
// locale-dependent; a lone continuation byte handed to either is undefined
// behavior on some libc's, not just a no-op). Declared here (not natives.cpp)
// because native_gen.cpp's generated --native code includes this same header
// and needs the identical logic, not a second reimplementation that could
// silently drift from it -- the same divergence class already fixed once for
// route parameter coercion (project.cpp's coerce()) and runtime error
// messages (route_runtime_prelude()) elsewhere in this codebase.
//
// This is NOT a full Unicode implementation: case conversion covers ASCII,
// the Latin-1 Supplement, and the regular pairs of Latin Extended-A -- the
// scripts most non-English-only Lux apps actually need (Spanish, French,
// German, Portuguese, Polish, Czech, Romanian...), not every script Unicode
// defines, and not locale-sensitive special cases (Turkish's dotless ı,
// German ß having no single uppercase letter). A codepoint this does not
// recognize is returned unchanged, so applying it never corrupts text it
// does not know how to case-convert -- correct is a subset of the input,
// never wrong on any of it.

// Counts codepoints, not bytes: a UTF-8 continuation byte always has its top
// two bits as `10`, so skipping those and counting everything else counts
// exactly one unit per codepoint regardless of how many bytes it takes.
inline size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Decodes the codepoint starting at s[i] and advances i past it. Malformed
// input (a truncated multi-byte sequence, a continuation byte with no
// leader, an overlong/invalid leader) decodes as that ONE byte's own value
// and advances by only 1 -- never an exception, and never a desync that
// could skip or duplicate later valid bytes: a resync happens on its own,
// one byte later, since a malformed byte cannot masquerade as a valid
// continuation byte of whatever comes next either.
inline uint32_t utf8_decode(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len; uint32_t cp;
    if      ((c & 0x80) == 0x00) { len = 1; cp = c; }
    else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
    else { ++i; return c; }
    if (i + len > s.size()) { ++i; return c; }
    for (size_t k = 1; k < len; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) { ++i; return c; }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    i += len;
    return cp;
}

inline std::string utf8_encode(uint32_t cp) {
    std::string out;
    if (cp <= 0x7Fu) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FFu) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp <= 0xFFFFu) {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (cp >> 18));
        out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
    return out;
}

inline uint32_t utf8_codepoint_upper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp >= 0xE0u && cp <= 0xFEu && cp != 0xF7u) return cp - 0x20u; // à-þ, not ÷
    if (cp == 0xFFu) return 0x178u;                                   // ÿ -> Ÿ
    if (cp >= 0x100u && cp <= 0x137u && (cp % 2 == 1)) return cp - 1; // Latin Ext-A pairs
    if (cp >= 0x139u && cp <= 0x148u && (cp % 2 == 0)) return cp - 1;
    if (cp >= 0x14Au && cp <= 0x177u && (cp % 2 == 1)) return cp - 1;
    if (cp >= 0x179u && cp <= 0x17Eu && (cp % 2 == 0)) return cp - 1;
    return cp;
}

inline uint32_t utf8_codepoint_lower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0xC0u && cp <= 0xDEu && cp != 0xD7u) return cp + 0x20u; // À-Þ, not ×
    if (cp == 0x178u) return 0xFFu;                                   // Ÿ -> ÿ
    if (cp >= 0x100u && cp <= 0x137u && (cp % 2 == 0)) return cp + 1;
    if (cp >= 0x139u && cp <= 0x148u && (cp % 2 == 1)) return cp + 1;
    if (cp >= 0x14Au && cp <= 0x177u && (cp % 2 == 0)) return cp + 1;
    if (cp >= 0x179u && cp <= 0x17Eu && (cp % 2 == 1)) return cp + 1;
    return cp;
}

inline std::string utf8_map(const std::string& s, uint32_t (*f)(uint32_t)) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) out += utf8_encode(f(utf8_decode(s, i)));
    return out;
}
inline std::string utf8_upper(const std::string& s) { return utf8_map(s, utf8_codepoint_upper); }
inline std::string utf8_lower(const std::string& s) { return utf8_map(s, utf8_codepoint_lower); }

// Splits a string into its individual codepoints, each its own (1-4 byte)
// string -- what `for c in <string>` and `split(s, "")` iterate over,
// instead of either being unsupported (no way to walk a string a character
// at a time at all) or, worse, walking raw bytes and handing back a broken
// half-a-codepoint "character" for anything outside ASCII.
inline std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size(); ) {
        size_t start = i;
        utf8_decode(s, i);
        out.push_back(s.substr(start, i - start));
    }
    return out;
}

} // namespace lux_script
