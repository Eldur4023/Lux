#include <lux_script/tiny_regex.hpp>

#include <algorithm>

namespace lux_script::tiny_regex {

bool Regex::CharClass::contains(unsigned char c) const {
    bool in = false;
    for (const auto& [lo, hi] : ranges) {
        if (c >= lo && c <= hi) { in = true; break; }
    }
    return negated ? !in : in;
}

namespace {

// ─── Parser + Thompson-construction compiler ──────────────────────────────────
//
// One pass, recursive descent, emitting instructions directly into the
// Regex's program as it parses -- no separate AST. Grammar:
//
//   Alt      := Concat ('|' Concat)*
//   Concat   := Repeat*                          (empty concat matches "")
//   Repeat   := Atom Quantifier?
//   Quantifier := '*' | '+' | '?' | '{' Number (',' Number?)? '}'
//   Atom     := '(' Alt ')' | '[' Class ']' | '.' | '^' | '$'
//             | '\' EscapeChar | AnyOtherChar
//
// Every compile_* function returns a Frag: the entry instruction index plus
// a list of "dangling" (a/b) fields still pointing at -1, to be patched
// once the caller knows what comes next. This is the standard technique
// for building an NFA into a flat instruction array in one pass (see Russ
// Cox's "Regular Expression Matching: the Virtual Machine Approach").
class Compiler {
public:
    Compiler(const std::string& pattern, Regex& re, std::string& error)
        : pat_(pattern), re_(re), err_(error) {}

    bool run() {
        // Group 0 wraps the entire pattern: Save(0) .. body .. Save(1),
        // exactly like any other capturing group, so search() can read the
        // overall match bounds out of slots[0]/slots[1] uniformly.
        int save0 = emit({Regex::Op::Save, 0, -1, -1, -1, 0});
        Frag body;
        if (!parse_alt(body)) return false;
        if (pos_ != pat_.size()) {
            // Something was left over -- an unmatched ')' is the only way
            // parse_alt()/parse_atom() return successfully without
            // consuming the whole pattern.
            return fail("unexpected ')' (unmatched)");
        }
        re_.prog_[save0].a = body.start;
        int save1 = emit({Regex::Op::Save, 0, -1, -1, -1, 1});
        patch(body.out, save1);
        int matchi = emit({Regex::Op::Match, 0, -1, -1, -1, -1});
        re_.prog_[save1].a = matchi;
        re_.num_groups_ = next_group_;
        if (re_.prog_.size() > kMaxProgramSize) {
            return fail("pattern compiles to a program that is too large "
                        "(check for an excessive {n,m} repeat count)");
        }
        return true;
    }

private:
    const std::string& pat_;
    Regex&              re_;
    std::string&        err_;
    size_t               pos_ = 0;
    int                   next_group_ = 1;   // group 0 is the whole match

    static constexpr size_t kMaxProgramSize = 200'000;

    struct PatchTarget { int inst; bool second; };
    using PatchList = std::vector<PatchTarget>;
    struct Frag { int start; PatchList out; };

    bool fail(const std::string& msg) { err_ = msg; return false; }

    bool more() const { return pos_ < pat_.size(); }
    char peekc() const { return pat_[pos_]; }
    char takec() { return pat_[pos_++]; }

    int emit(Regex::Inst i) {
        re_.prog_.push_back(i);
        return static_cast<int>(re_.prog_.size()) - 1;
    }

    void patch(const PatchList& list, int target) {
        for (const auto& p : list) {
            if (p.second) re_.prog_[p.inst].b = target;
            else          re_.prog_[p.inst].a = target;
        }
    }

    static PatchList join(PatchList a, const PatchList& b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }

    Frag frag_empty() {
        // A single Jmp with a dangling `a` -- consumes nothing, just a
        // patch point. Used for an empty alternative branch ("a|" or "()").
        int i = emit({Regex::Op::Jmp, 0, -1, -1, -1, -1});
        return {i, {{i, false}}};
    }

    Frag frag_char(unsigned char c) {
        int i = emit({Regex::Op::Char, c, -1, -1, -1, -1});
        return {i, {{i, false}}};
    }

    Frag frag_any() {
        int i = emit({Regex::Op::Any, 0, -1, -1, -1, -1});
        return {i, {{i, false}}};
    }

    Frag frag_class(int class_id) {
        int i = emit({Regex::Op::Class, 0, class_id, -1, -1, -1});
        return {i, {{i, false}}};
    }

    // ── Character classes ──────────────────────────────────────────────────

    static void add_shorthand(Regex::CharClass& cc, char kind) {
        // Builds the POSITIVE ranges for d/w/s; negation (D/W/S) is applied
        // by the caller via a wrapping class, or -- inside `[...]`, ECMAScript
        // treats \D/\W/\S as "not this", which we approximate by adding a
        // separately-negated sub-class merge (see parse_class()).
        switch (kind) {
            case 'd':
                cc.ranges.push_back({'0', '9'});
                break;
            case 'w':
                cc.ranges.push_back({'a', 'z'});
                cc.ranges.push_back({'A', 'Z'});
                cc.ranges.push_back({'0', '9'});
                cc.ranges.push_back({'_', '_'});
                break;
            case 's':
                cc.ranges.push_back({' ', ' '});
                cc.ranges.push_back({'\t', '\t'});
                cc.ranges.push_back({'\n', '\n'});
                cc.ranges.push_back({'\r', '\r'});
                cc.ranges.push_back({'\f', '\f'});
                cc.ranges.push_back({'\v', '\v'});
                break;
        }
    }

    // Registers a brand-new standalone class (used for \d \D \w \W \s \S
    // appearing OUTSIDE `[...]`) and returns its id.
    int standalone_shorthand_class(char kind) {
        Regex::CharClass cc;
        char lower = static_cast<char>(kind >= 'A' && kind <= 'Z' ? kind - 'A' + 'a' : kind);
        add_shorthand(cc, lower);
        cc.negated = (kind >= 'A' && kind <= 'Z');
        re_.classes_.push_back(std::move(cc));
        return static_cast<int>(re_.classes_.size()) - 1;
    }

    // Parses the inside of `[...]` (cursor already past the opening '[',
    // stops just before the closing ']', which the caller consumes).
    // Negated shorthands (\D \W \S) inside a class are expanded as
    // "everything except X", which cannot be merged directly into a single
    // positive-ranges class alongside other positive members using set
    // union -- ECMAScript's own semantics for e.g. `[\Da]` is "not a digit,
    // OR 'a'", which (since \D already covers 'a') is just \D again; the
    // general union of "not X" with anything else that is itself a subset
    // of "not X" collapses the same way. To keep this small without a full
    // set-algebra implementation, a negated shorthand inside a class is
    // only supported when it is the ONLY member of that class (covers
    // every pattern this project's own tests/GUIDE.md use, and is the
    // overwhelmingly common real-world shape: `[^\d]`-style negation is
    // written as \D directly almost always) -- anything more exotic is a
    // compile error rather than a silent wrong answer.
    bool parse_class(int& class_id) {
        Regex::CharClass cc;
        bool negate = false;
        if (more() && peekc() == '^') { negate = true; takec(); }

        bool saw_negated_shorthand = false;
        bool saw_anything_else     = false;

        while (true) {
            if (!more()) return fail("unterminated '[' character class");
            if (peekc() == ']') { takec(); break; }

            unsigned char lo;
            if (peekc() == '\\') {
                takec();
                if (!more()) return fail("dangling '\\' at end of pattern");
                char e = takec();
                switch (e) {
                    case 'd': case 'w': case 's':
                        add_shorthand(cc, e); saw_anything_else = true; continue;
                    case 'D': case 'W': case 'S': {
                        if (saw_anything_else || saw_negated_shorthand)
                            return fail("a negated \\D/\\W/\\S inside [...] must be the only member of the class");
                        Regex::CharClass neg;
                        add_shorthand(neg, static_cast<char>(e - 'A' + 'a'));
                        neg.negated = true;
                        cc = neg;
                        saw_negated_shorthand = true;
                        continue;
                    }
                    case 'n': lo = '\n'; break;
                    case 't': lo = '\t'; break;
                    case 'r': lo = '\r'; break;
                    case 'f': lo = '\f'; break;
                    case 'v': lo = '\v'; break;
                    case '0': lo = '\0'; break;
                    default:  lo = static_cast<unsigned char>(e); break; // \. \\ \] \- etc: literal
                }
            } else {
                lo = static_cast<unsigned char>(takec());
            }
            if (saw_negated_shorthand) return fail("a negated \\D/\\W/\\S inside [...] must be the only member of the class");
            saw_anything_else = true;

            unsigned char hi = lo;
            if (more() && peekc() == '-' && pos_ + 1 < pat_.size() && pat_[pos_ + 1] != ']') {
                takec(); // '-'
                unsigned char hc;
                if (peekc() == '\\') {
                    takec();
                    if (!more()) return fail("dangling '\\' at end of pattern");
                    char e = takec();
                    switch (e) {
                        case 'n': hc = '\n'; break;
                        case 't': hc = '\t'; break;
                        case 'r': hc = '\r'; break;
                        case 'f': hc = '\f'; break;
                        case 'v': hc = '\v'; break;
                        default:  hc = static_cast<unsigned char>(e); break;
                    }
                } else {
                    hc = static_cast<unsigned char>(takec());
                }
                hi = hc;
                if (hi < lo) return fail("character range out of order in [...] (e.g. [z-a])");
            }
            cc.ranges.push_back({lo, hi});
        }

        if (negate) cc.negated = !cc.negated; // [^...] on top of a lone \D etc: double negation, rare but consistent
        re_.classes_.push_back(std::move(cc));
        class_id = static_cast<int>(re_.classes_.size()) - 1;
        return true;
    }

    bool parse_number(int& out) {
        if (!more() || peekc() < '0' || peekc() > '9') return false;
        long value = 0;
        while (more() && peekc() >= '0' && peekc() <= '9') {
            value = value * 10 + (takec() - '0');
            if (value > 1'000'000) value = 1'000'000; // clamp, see parse_repeat()'s cap
        }
        out = static_cast<int>(value);
        return true;
    }

    // ── Grammar ─────────────────────────────────────────────────────────────

    bool parse_atom(Frag& out) {
        if (!more()) return fail("unexpected end of pattern");
        char c = peekc();

        if (c == '(') {
            takec();
            int group = next_group_++;
            Frag inner;
            if (!parse_alt(inner)) return false;
            if (!more() || peekc() != ')') return fail("missing closing ')'");
            takec();
            int s0 = emit({Regex::Op::Save, 0, -1, -1, -1, 2 * group});
            re_.prog_[s0].a = inner.start;
            int s1 = emit({Regex::Op::Save, 0, -1, -1, -1, 2 * group + 1});
            patch(inner.out, s1);
            out = {s0, {{s1, false}}};
            return true;
        }
        if (c == '[') {
            takec();
            int class_id;
            if (!parse_class(class_id)) return false;
            out = frag_class(class_id);
            return true;
        }
        if (c == '.') { takec(); out = frag_any(); return true; }
        if (c == '^') { takec(); int i = emit({Regex::Op::BeginText, 0, -1, -1, -1, -1}); out = {i, {{i, false}}}; return true; }
        if (c == '$') { takec(); int i = emit({Regex::Op::EndText,   0, -1, -1, -1, -1}); out = {i, {{i, false}}}; return true; }
        if (c == ')' || c == '|') return fail("unexpected atom"); // caller-level constructs, not atoms
        if (c == '*' || c == '+' || c == '?')
            return fail(std::string("nothing to repeat before '") + c + "'");

        if (c == '\\') {
            takec();
            if (!more()) return fail("dangling '\\' at end of pattern");
            char e = takec();
            switch (e) {
                case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
                    out = frag_class(standalone_shorthand_class(e));
                    return true;
                case 'n': out = frag_char('\n'); return true;
                case 't': out = frag_char('\t'); return true;
                case 'r': out = frag_char('\r'); return true;
                case 'f': out = frag_char('\f'); return true;
                case 'v': out = frag_char('\v'); return true;
                case '0': out = frag_char('\0'); return true;
                default:
                    if (e >= '1' && e <= '9')
                        return fail("backreferences (\\1, \\2, ...) inside a pattern are not supported");
                    out = frag_char(static_cast<unsigned char>(e)); // \. \\ \( \) \[ \] \{ \} \+ \* \? \| etc.
                    return true;
            }
        }

        takec();
        out = frag_char(static_cast<unsigned char>(c));
        return true;
    }

    bool parse_repeat(Frag& out) {
        // Captured BEFORE parse_atom() consumes the atom: {n,m}'s expansion
        // needs to re-parse this SAME atom from source multiple times (see
        // its own comment below for why), which means remembering where it
        // STARTED, not where it ended.
        size_t atom_pos = pos_;
        Frag atom;
        if (!parse_atom(atom)) return false;

        if (!more()) { out = atom; return true; }
        char c = peekc();

        if (c == '*') {
            takec();
            int split = emit({Regex::Op::Split, 0, -1, atom.start, -1, -1});
            patch(atom.out, split);
            out = {split, {{split, true}}};
            return true;
        }
        if (c == '+') {
            takec();
            int split = emit({Regex::Op::Split, 0, -1, atom.start, -1, -1});
            patch(atom.out, split);
            out = {atom.start, {{split, true}}};
            return true;
        }
        if (c == '?') {
            takec();
            int split = emit({Regex::Op::Split, 0, -1, atom.start, -1, -1});
            out = {split, join(atom.out, {{split, true}})};
            return true;
        }
        if (c == '{') {
            size_t save_pos = pos_;
            takec(); // '{'
            int lo = 0, hi = -1;
            bool has_lo = parse_number(lo);
            bool has_comma = more() && peekc() == ',';
            if (has_comma) takec();
            bool has_hi = has_comma && parse_number(hi);
            if (!has_comma && has_lo) hi = lo; // {n} exactly
            if (!more() || peekc() != '}' || !has_lo) {
                // Not a valid {..} quantifier after all (e.g. a literal
                // '{' in the pattern) -- back out and treat '{' as a
                // literal character instead, same as most regex flavors.
                pos_ = save_pos;
                out = atom;
                return true;
            }
            takec(); // '}'
            if (has_comma && !has_hi) hi = -1; // {n,} unbounded
            if (hi != -1 && hi < lo) return fail("{n,m} with m < n");
            if (lo > 1000 || hi > 1000)
                return fail("{n,m} repeat count over 1000 -- split the pattern instead");

            // Expand into an explicit sequence: `lo` mandatory copies, then
            // either (hi-lo) optional copies (bounded) or a trailing `*`
            // (unbounded). Re-parsing the SAME atom text would re-run
            // parse_atom() and double-count capturing groups if the atom is
            // a group, so instead the ALREADY-COMPILED `atom` fragment is
            // reused for the first copy and the pattern's own source slice
            // is re-parsed for subsequent copies -- capturing groups inside
            // a repeated atom are meant to end up with ONE group index
            // reused across iterations (the last iteration's match wins),
            // which is exactly what re-invoking parse_atom on the same
            // source text does NOT give (it would allocate NEW group
            // numbers each time). Re-emitting the compiled Frag's
            // instructions verbatim is not straightforward either (their
            // Save slot numbers are baked in, which is fine -- that is
            // exactly the "reused index" behavior wanted -- but their a/b
            // targets are absolute indices that a naive copy would need to
            // relocate). Given this engine's target patterns are flat
            // (GUIDE.md's own examples: character classes and shorthands,
            // not repeated groups), the pragmatic choice is to support
            // {n,m} fully for a NON-group atom (the overwhelmingly common
            // case: `a{2,4}`, `\d{3}`, `[0-9]{2,5}`) and reject it on a
            // group atom rather than risk a subtly wrong group-numbering.
            if (re_.prog_[atom.start].op == Regex::Op::Save)
                return fail("{n,m} on a capturing group '(...)' is not supported -- "
                            "wrap the repeat inside the group instead, e.g. (a{2,4})");

            Frag chain = atom;
            for (int i = 1; i < lo; ++i) {
                Frag next;
                if (!parse_atom_reparse(atom_pos, next)) return false;
                patch(chain.out, next.start);
                chain = {chain.start, next.out};
            }
            if (hi == -1) {
                // lo copies mandatory, then a * on one more copy.
                Frag star_atom;
                if (lo == 0) {
                    star_atom = atom;
                } else if (!parse_atom_reparse(atom_pos, star_atom)) {
                    return false;
                }
                int split = emit({Regex::Op::Split, 0, -1, star_atom.start, -1, -1});
                patch(star_atom.out, split);
                if (lo == 0) { out = {split, {{split, true}}}; return true; }
                patch(chain.out, split);
                out = {chain.start, {{split, true}}};
                return true;
            }
            // (hi - lo) further OPTIONAL copies, each skippable, all
            // skips converging on the same exit point.
            PatchList exits = (lo == 0) ? PatchList{} : chain.out;
            int chain_start = (lo == 0) ? -1 : chain.start;
            for (int i = lo; i < hi; ++i) {
                Frag next;
                if (!parse_atom_reparse(atom_pos, next)) return false;
                int split = emit({Regex::Op::Split, 0, -1, next.start, -1, -1});
                if (chain_start == -1) chain_start = split; else patch(exits, split);
                exits = join(next.out, {{split, true}});
            }
            if (chain_start == -1) { out = frag_empty(); return true; }
            out = {chain_start, exits};
            return true;
        }

        out = atom;
        return true;
    }

    // Re-parses a single atom from a saved source position, for {n,m}
    // expansion -- used only for non-group atoms (checked by the caller),
    // so re-numbering groups is not a concern (there are none to renumber).
    bool parse_atom_reparse(size_t atom_pos, Frag& out) {
        size_t here = pos_;
        pos_ = atom_pos;
        Frag a;
        if (!parse_atom(a)) return false;
        pos_ = here;
        out = a;
        return true;
    }

    bool parse_concat(Frag& out) {
        if (!more() || peekc() == '|' || peekc() == ')') { out = frag_empty(); return true; }
        Frag first;
        if (!parse_repeat(first)) return false;
        Frag chain = first;
        while (more() && peekc() != '|' && peekc() != ')') {
            Frag next;
            if (!parse_repeat(next)) return false;
            patch(chain.out, next.start);
            chain = {chain.start, next.out};
        }
        out = chain;
        return true;
    }

    bool parse_alt(Frag& out) {
        Frag first;
        if (!parse_concat(first)) return false;
        if (!more() || peekc() != '|') { out = first; return true; }

        PatchList all_out = first.out;
        int chain_start = first.start;

        while (more() && peekc() == '|') {
            takec();
            Frag rhs;
            if (!parse_concat(rhs)) return false;
            int split = emit({Regex::Op::Split, 0, -1, chain_start, rhs.start, -1});
            all_out = join(all_out, rhs.out);
            chain_start = split;
        }
        out = {chain_start, all_out};
        return true;
    }
};

} // namespace

bool Regex::compile(const std::string& pattern, std::string& error) {
    prog_.clear();
    classes_.clear();
    num_groups_ = 1;
    Compiler c(pattern, *this, error);
    if (!c.run()) {
        error = "invalid regex pattern: " + error;
        return false;
    }
    return true;
}

// ─── Pike VM: simulate every live NFA thread in lockstep ──────────────────────
//
// See tiny_regex.hpp's class comment for why this shape (rather than
// backtracking) is the entire point. `gen`/`seen` implement the classic
// "has this instruction already been added at this step" check in O(1)
// without clearing an array every step: `seen[pc] == gen` means yes.
namespace {

struct Thread {
    int              pc;
    std::vector<int> slots;
};

class Sim {
public:
    Sim(const std::vector<Regex::Inst>& prog, const std::vector<Regex::CharClass>& classes,
        int num_slots)
        : prog_(prog), classes_(classes), num_slots_(num_slots),
          seen_(prog.size(), 0), gen_(0) {}

    // Adds pc (and everything epsilon-reachable from it) to `list`, honoring
    // priority order and deduplicating so each pc appears in `list` at most
    // once per step -- THIS is what bounds total work to O(prog size) per
    // subject byte instead of growing without bound.
    //
    // Iterative (an explicit stack of pending items), not the recursive
    // formulation this would most naturally read as: the whole POINT of
    // this engine is that nothing in it recurses proportionally to
    // anything an attacker controls, and while this recursion is bounded
    // by the PATTERN's own size (kMaxProgramSize, tiny_regex.cpp) rather
    // than the subject's, a pattern near that cap built mostly out of
    // chained non-consuming instructions (Jmp/Split/Save/anchors) could
    // still recurse tens of thousands of frames deep -- exactly the class
    // of "the pattern is developer-authored so it's fine" assumption this
    // rewrite exists to stop leaning on by hand-checking case by case.
    // Pushing the SPLIT's second (lower-priority) branch before its first
    // preserves the exact same visitation order the recursive version had:
    // the standard "push children right-to-left so the leftmost pops
    // first, and its ENTIRE subtree is fully walked via further pushes
    // before its sibling's turn comes" iterative-DFS equivalence.
    void add(std::vector<Thread>& list, int start_pc, std::vector<int> start_slots,
            size_t pos, const std::string& text) {
        struct Item { int pc; std::vector<int> slots; };
        std::vector<Item> stack;
        stack.push_back({start_pc, std::move(start_slots)});

        while (!stack.empty()) {
            Item cur = std::move(stack.back());
            stack.pop_back();
            const int pc = cur.pc;
            if (pc < 0 || seen_[static_cast<size_t>(pc)] == gen_) continue;
            seen_[static_cast<size_t>(pc)] = gen_;
            const Regex::Inst& in = prog_[static_cast<size_t>(pc)];
            switch (in.op) {
                case Regex::Op::Jmp:
                    stack.push_back({in.a, std::move(cur.slots)});
                    break;
                case Regex::Op::Split:
                    // in.b (lower priority) pushed first so in.a (higher
                    // priority) is popped -- and its whole subtree walked
                    // -- first. Copy for in.b since cur.slots is still
                    // needed for in.a right after this.
                    stack.push_back({in.b, cur.slots});
                    stack.push_back({in.a, std::move(cur.slots)});
                    break;
                case Regex::Op::Save:
                    if (in.slot >= 0 && in.slot < static_cast<int>(cur.slots.size()))
                        cur.slots[static_cast<size_t>(in.slot)] = static_cast<int>(pos);
                    stack.push_back({in.a, std::move(cur.slots)});
                    break;
                case Regex::Op::BeginText:
                    if (pos == 0) stack.push_back({in.a, std::move(cur.slots)});
                    break;
                case Regex::Op::EndText:
                    if (pos == text.size()) stack.push_back({in.a, std::move(cur.slots)});
                    break;
                case Regex::Op::Char:
                case Regex::Op::Class:
                case Regex::Op::Any:
                case Regex::Op::Match:
                    list.push_back({pc, std::move(cur.slots)});
                    break;
            }
        }
    }

    void new_generation() {
        ++gen_;
        if (gen_ == 0) { std::fill(seen_.begin(), seen_.end(), 0); ++gen_; } // wrap-around, practically unreachable
    }

private:
    const std::vector<Regex::Inst>&       prog_;
    const std::vector<Regex::CharClass>&  classes_;
    int                                    num_slots_;
    std::vector<uint32_t>                 seen_;
    uint32_t                              gen_;
};

} // namespace

bool Regex::search(const std::string& text, size_t start, Match& out) const {
    if (start > text.size()) return false;
    const int num_slots = 2 * num_groups_;

    Sim sim(prog_, classes_, num_slots);
    std::vector<Thread> clist, nlist;
    bool matched = false;
    std::vector<int> best;

    sim.new_generation();
    {
        std::vector<int> fresh(static_cast<size_t>(num_slots), -1);
        sim.add(clist, prog_.empty() ? -1 : 0, std::move(fresh), start, text);
    }

    for (size_t pos = start; ; ++pos) {
        if (!matched && pos > start) {
            // Inject a new, lowest-priority "start trying from here"
            // thread at every position, so the search finds the LEFTMOST
            // match rather than only one anchored at `start` -- this is
            // what makes it a search instead of a match. Stops entirely
            // once matched: nothing starting later can ever beat a match
            // already found (see the loop body's priority handling below
            // for why a SURVIVING higher-priority thread still can).
            std::vector<int> fresh(static_cast<size_t>(num_slots), -1);
            sim.add(clist, 0, std::move(fresh), pos, text);
        }
        if (clist.empty()) break;

        const bool have_byte = pos < text.size();
        const unsigned char c = have_byte ? static_cast<unsigned char>(text[pos]) : 0;

        sim.new_generation();
        nlist.clear();
        bool cut_here = false;
        for (auto& t : clist) {
            if (cut_here) break; // a higher-priority Match already decided this step
            const Inst& in = prog_[static_cast<size_t>(t.pc)];
            switch (in.op) {
                case Op::Char:
                    if (have_byte && c == in.ch) sim.add(nlist, in.a, t.slots, pos + 1, text);
                    break;
                case Op::Class:
                    if (have_byte && classes_[static_cast<size_t>(in.class_id)].contains(c))
                        sim.add(nlist, in.a, t.slots, pos + 1, text);
                    break;
                case Op::Any:
                    if (have_byte && c != '\n') sim.add(nlist, in.a, t.slots, pos + 1, text);
                    break;
                case Op::Match:
                    best = t.slots;
                    matched = true;
                    cut_here = true; // discard remaining LOWER-priority threads this step only
                    break;
                default:
                    break; // unreachable: add() never leaves Jmp/Split/Save/anchors in a list
            }
        }
        std::swap(clist, nlist);
        if (pos >= text.size()) break;
    }

    if (!matched) return false;
    out.slots = std::move(best);
    return true;
}

} // namespace lux_script::tiny_regex
