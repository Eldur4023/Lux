#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace lux_script::tiny_regex {

// ─── A small, linear-time regex engine ────────────────────────────────────────
//
// Replaces std::regex for lux_script's regex.* module. std::regex's
// ECMAScript grammar (libstdc++'s implementation of it) is a classic
// backtracking engine: it recurses in the C++ call stack once per
// character of the SUBJECT for most patterns, and its running time is not
// bounded by any polynomial in the subject length for a great many
// ordinary-looking patterns -- both properties are unacceptable for a
// function that runs on data an HTTP request controls. Measured against
// the real binary before this replaced it: a subject of ~4000 characters
// against a plausible nested-group pattern SEGV'd the whole process (stack
// overflow), and a 28-BYTE subject against a pattern as ordinary as
// `^(\w+\s?)*$` (a "words separated by single spaces" check anyone might
// write) took 5.7 seconds, doubling roughly every 4 extra bytes --
// catastrophic backtracking, not a contrived worst case. A handful of
// concurrent requests like that pegged every core the process had and
// stayed there for as long as it took the (uninterruptible) backtracking
// search to exhaust itself, which for a slightly longer subject is not a
// number of seconds a request can be reasonably asked to wait for.
//
// This engine instead compiles a pattern to a small NFA (Thompson's
// construction) and executes ALL live NFA states in lockstep, one subject
// byte at a time (the "Pike VM" -- Rob Pike's technique from the Plan 9
// regexp library, popularized by Russ Cox's "Regular Expression Matching:
// the Virtual Machine Approach" and used essentially unchanged inside
// RE2/Go's regexp package). Every reachable program counter is visited AT
// MOST ONCE per subject byte (a generation-tagged array makes checking and
// marking that O(1)), which bounds total work to O(pattern_size *
// subject_size) -- no exponential blowup is possible no matter how the
// pattern nests quantifiers and alternations, and the only recursion is
// over the PATTERN's structure at compile time (bounded by the pattern's
// own length, developer-authored, not attacker input), never over the
// subject.
//
// Deliberately small: the ECMAScript-ish subset actually used by this
// project's own tests/GUIDE.md examples, not a general-purpose library.
// Supported: literals, `.` (any byte but '\n'), `^` `$` (start/end of the
// whole subject, not multiline), character classes `[...]`/`[^...]`
// (ranges, `\d\D\w\W\s\S` and escapes inside), the shorthand classes
// outside `[...]` too, capturing groups `(...)`, alternation `|`, and the
// quantifiers `* + ? {n} {n,} {n,m}` (all greedy -- there is no lazy `*?`
// form). NOT supported, and rejected as a compile error rather than
// silently mismatching: non-capturing groups `(?:...)`, lookaround
// `(?=...)`/`(?!...)`, backreferences INSIDE a pattern (`\1` matching
// "whatever group 1 matched" -- `$1` in a REPLACEMENT string is unrelated
// and still works, that is just text substitution after the fact), and
// named groups.
class Regex {
public:
    // Compiles `pattern`. Returns false and fills `error` (matching this
    // module's existing "invalid regex pattern: ..." wording) on anything
    // the parser does not understand, so callers keep exactly the error
    // shape/behavior they already had with std::regex.
    bool compile(const std::string& pattern, std::string& error);

    // Number of capturing groups INCLUDING group 0 (the whole match).
    int group_count() const { return num_groups_; }

    struct Match {
        // slots[2*i] / slots[2*i+1] = start/end byte offset (end exclusive)
        // of group i, or -1/-1 if that group did not participate in this
        // match (e.g. the losing side of an alternation). Always sized
        // 2*group_count() after a successful search().
        std::vector<int> slots;
    };

    // Finds the leftmost match starting at or after `start` (byte offset
    // into `text`), preferring -- among matches starting at that same
    // leftmost position -- the one the pattern's own alternation/quantifier
    // order would prefer first (the same "leftmost-first" rule
    // PCRE/JavaScript/std::regex's ECMAScript mode all use, so existing
    // patterns keep meaning what they meant). Returns false if no match
    // exists anywhere in text[start..].
    bool search(const std::string& text, size_t start, Match& out) const;

    // ── Compiled program (Thompson NFA, Pike-VM instruction shape) ────────
    // Public only so the .cpp's free-function compiler can fill it in
    // without a friend declaration; not part of the class's real interface
    // (nothing outside tiny_regex.cpp constructs an Inst or CharClass).
    enum class Op : uint8_t { Char, Class, Any, Save, Jmp, Split, BeginText, EndText, Match };

    struct Inst {
        Op            op;
        unsigned char ch        = 0;   // Op::Char
        int           class_id  = -1;  // Op::Class
        int           a         = -1;  // Jmp target / Split branch 1 (higher priority) / next for Char,Class,Any,Save,anchors
        int           b         = -1;  // Split branch 2 (lower priority)
        int           slot      = -1;  // Op::Save
    };

    struct CharClass {
        std::vector<std::pair<unsigned char, unsigned char>> ranges; // sorted, non-overlapping
        bool negated = false;
        bool contains(unsigned char c) const;
    };

    std::vector<Inst>      prog_;
    std::vector<CharClass> classes_;
    int                    num_groups_ = 1;   // group 0 always exists
};

} // namespace lux_script::tiny_regex
