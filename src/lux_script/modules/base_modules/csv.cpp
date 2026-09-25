// A tabular-data module (NATIVE-MODULES.md) -- parse/filter/sort/aggregate
// CSV text, in the spirit of pandas but expressed as explicit verbs instead
// of arbitrary predicates: Lux Script has no function values to pass as a
// callback (no `df[df.age > 18]`), so `filter_gt(handle, "age", 18)` is the
// honest equivalent a language without closures can actually offer.
//
// This is the first STATEFUL native module: `parse()` hands back an opaque
// `int` handle instead of the parsed table itself, and every later call
// takes that handle back. There is no core-mechanism change for this --
// NATIVE-MODULES.md flagged it as an open question, and building this
// module answered it: a module that needs state just keeps its own
// mutex-protected table, exactly like SharedState (natives.hpp) already
// does for `state.*`. No new BuiltinModule capability was needed.
#include <lux_script/builtin_module.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace lux_script {

namespace {

struct CsvTable {
    std::vector<std::string>              columns;
    std::vector<std::vector<std::string>> rows;   // rows[i][j], j indexes columns
};

// ─── Handle table ────────────────────────────────────────────────────────────
//
// Protected by a mutex because, unlike a request-local object, this outlives
// any single request and every event-loop thread can reach it (GUIDE.md
// §22: "N threads: event loop + its own VM").

HandleTable<CsvTable>& handles() {
    static HandleTable<CsvTable> h;
    return h;
}

// ─── Parsing (RFC 4180: quoted fields, "" as an escaped quote inside one,
// commas/newlines allowed inside a quoted field) ───────────────────────────

std::vector<std::vector<std::string>> parse_csv_text(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool in_quotes = false;
    bool row_has_content = false;

    auto end_field = [&] { row.push_back(field); field.clear(); };
    auto end_row = [&] { end_field(); rows.push_back(std::move(row)); row.clear(); row_has_content = false; };

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { field.push_back('"'); ++i; }
                else in_quotes = false;
            } else {
                field.push_back(c);
            }
            continue;
        }
        row_has_content = true;
        if (c == '"' && field.empty()) { in_quotes = true; }
        else if (c == ',') { end_field(); }
        else if (c == '\r') { /* skip; \n (or end) closes the row */ }
        else if (c == '\n') { end_row(); }
        else { field.push_back(c); }
    }
    // A trailing newline leaves nothing pending; a file with no trailing
    // newline still has one last field/row to close.
    if (row_has_content || !field.empty() || !row.empty()) end_row();

    return rows;
}

// OWASP "CSV Injection": a cell whose text starts with '=', '+', '-', '@',
// TAB, or CR is read as a formula by Excel/LibreOffice/Sheets the moment
// someone opens the file, not as literal text -- a name field of
// `=HYPERLINK("http://evil","click")` or one of the old Excel DDE payloads
// (`=cmd|'/c calc'!A1`) runs the instant whoever opens the export looks at
// that cell. csv.to_csv() exists specifically to hand user-controlled rows
// back as a file people open in a spreadsheet app, so this is the module's
// actual threat model, not an edge case worth skipping.
bool looks_like_formula(const std::string& s) {
    if (s.empty()) return false;
    switch (s.front()) {
        case '=': case '+': case '-': case '@': case '\t': case '\r':
            return true;
        default:
            return false;
    }
}

std::string csv_escape(const std::string& s) {
    // Prefixing with a single quote is the standard mitigation (OWASP CSV
    // Injection cheat sheet): every spreadsheet app treats a leading `'` as
    // "force this cell to text" and never displays it, while RFC 4180
    // itself assigns no meaning to one. round-tripping the output back
    // through csv.parse() does surface that `'` (there is no way to hide a
    // defused formula AND stay losslessly parseable), which is the accepted
    // trade-off of this mitigation everywhere it is used.
    const std::string field = looks_like_formula(s) ? "'" + s : s;

    bool needs_quotes = field.find_first_of(",\"\n\r") != std::string::npos;
    if (!needs_quotes) return field;
    std::string out = "\"";
    for (char c : field) { if (c == '"') out += "\"\""; else out += c; }
    out += "\"";
    return out;
}

std::string write_csv_text(const CsvTable& t) {
    std::string out;
    for (size_t j = 0; j < t.columns.size(); ++j) {
        if (j) out += ',';
        out += csv_escape(t.columns[j]);
    }
    out += "\r\n";
    for (const auto& row : t.rows) {
        for (size_t j = 0; j < row.size(); ++j) {
            if (j) out += ',';
            out += csv_escape(row[j]);
        }
        out += "\r\n";
    }
    return out;
}

// ─── Cell helpers ────────────────────────────────────────────────────────────

std::optional<double> parse_num(const std::string& s) {
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return std::nullopt;
    return v;
}

// Per-cell type inference (not a whole-column pass, see NATIVE-MODULES.md):
// a value that parses cleanly as a whole number becomes Int, a decimal
// becomes Float, "true"/"false" becomes Bool, anything else stays String --
// applied independently to every cell, which is simpler than pandas' dtype-
// per-column inference and good enough for `rows()`'s purpose (handing the
// data to Lux Script as Json, the same shape `sqlite.query()` already
// returns).
Value cell_to_value(const std::string& s) {
    if (s == "true")  return Value::boolean(true);
    if (s == "false") return Value::boolean(false);
    if (auto n = parse_num(s)) {
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
            s.find('E') == std::string::npos) {
            try {
                size_t pos = 0;
                long long i = std::stoll(s, &pos);
                if (pos == s.size()) return Value::integer(i);
            } catch (...) {}
        }
        return Value::real(*n);
    }
    return Value::str(s);
}

int column_index(const CsvTable& t, const std::string& name) {
    for (size_t i = 0; i < t.columns.size(); ++i) if (t.columns[i] == name) return static_cast<int>(i);
    return -1;
}

const std::string& cell_at(const std::vector<std::string>& row, int col) {
    static const std::string empty;
    return col >= 0 && static_cast<size_t>(col) < row.size() ? row[static_cast<size_t>(col)] : empty;
}

// ─── Functions ───────────────────────────────────────────────────────────────

Value fn_csv_parse(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "csv.parse() expects a string"; return Value::null(); }
    bool has_header = args.size() < 2 || !args[1].is_bool() || args[1].as_bool();

    auto raw = parse_csv_text(args[0].as_str());
    CsvTable t;
    size_t start = 0;
    if (has_header && !raw.empty()) {
        t.columns = raw[0];
        start = 1;
    } else if (!raw.empty()) {
        for (size_t j = 0; j < raw[0].size(); ++j) t.columns.push_back(std::to_string(j));
    }
    for (size_t i = start; i < raw.size(); ++i) t.rows.push_back(raw[i]);
    return Value::integer(handles().put(std::move(t)));
}

Value fn_csv_close(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) { error = "csv.close() expects a handle"; return Value::null(); }
    return Value::boolean(handles().close(static_cast<int>(args[0].as_int())));
}

Value fn_csv_columns(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    Value::List cols;
    for (const auto& c : t->columns) cols.push_back(Value::str(c));
    return Value::list(std::move(cols));
}

Value fn_csv_row_count(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    return Value::integer(static_cast<long long>(t->rows.size()));
}

Value fn_csv_rows(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    Value::List out;
    for (const auto& row : t->rows) {
        Value::Dict d;
        for (size_t j = 0; j < t->columns.size(); ++j)
            d[t->columns[j]] = cell_to_value(cell_at(row, static_cast<int>(j)));
        out.push_back(Value::dict(std::move(d)));
    }
    return Value::list(std::move(out));
}

Value fn_csv_get(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    if (!args[1].is_int()) { error = "csv.get(): row must be an int"; return Value::null(); }
    if (!args[2].is_str()) { error = "csv.get(): column must be a string"; return Value::null(); }
    long long ri = args[1].as_int();
    if (ri < 0 || static_cast<size_t>(ri) >= t->rows.size()) {
        error = "csv.get(): row out of range";
        return Value::null();
    }
    int ci = column_index(*t, args[2].as_str());
    if (ci < 0) { error = "csv.get(): no such column '" + args[2].as_str() + "'"; return Value::null(); }
    return cell_to_value(cell_at(t->rows[static_cast<size_t>(ri)], ci));
}

// Shared body of every filter_* -- keeps the six comparison verbs a single
// small function apart instead of six near-duplicates.
enum class CmpOp { Eq, Gt, Lt, Ge, Le, Contains };

Value do_filter(std::vector<Value>& args, std::string& error, CmpOp op) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    if (!args[1].is_str()) { error = "csv.filter(): column must be a string"; return Value::null(); }
    int ci = column_index(*t, args[1].as_str());
    if (ci < 0) { error = "csv.filter(): no such column '" + args[1].as_str() + "'"; return Value::null(); }

    CsvTable out;
    out.columns = t->columns;

    if (op == CmpOp::Contains) {
        if (!args[2].is_str()) { error = "csv.filter_contains(): value must be a string"; return Value::null(); }
        const std::string& needle = args[2].as_str();
        for (const auto& row : t->rows)
            if (cell_at(row, ci).find(needle) != std::string::npos) out.rows.push_back(row);
    } else if (op == CmpOp::Eq) {
        std::string needle = args[2].to_string();
        for (const auto& row : t->rows)
            if (cell_at(row, ci) == needle) out.rows.push_back(row);
    } else {
        if (!args[2].is_num()) { error = "csv.filter(): value must be a number"; return Value::null(); }
        double needle = args[2].as_float();
        for (const auto& row : t->rows) {
            auto v = parse_num(cell_at(row, ci));
            if (!v) continue; // non-numeric cells never match a numeric comparison
            bool match = op == CmpOp::Gt ? *v > needle
                       : op == CmpOp::Lt ? *v < needle
                       : op == CmpOp::Ge ? *v >= needle
                                         : *v <= needle;
            if (match) out.rows.push_back(row);
        }
    }
    return Value::integer(handles().put(std::move(out)));
}

Value fn_csv_filter_eq(NativeCtx&, std::vector<Value>& args, std::string& error)       { return do_filter(args, error, CmpOp::Eq); }
Value fn_csv_filter_gt(NativeCtx&, std::vector<Value>& args, std::string& error)       { return do_filter(args, error, CmpOp::Gt); }
Value fn_csv_filter_lt(NativeCtx&, std::vector<Value>& args, std::string& error)       { return do_filter(args, error, CmpOp::Lt); }
Value fn_csv_filter_ge(NativeCtx&, std::vector<Value>& args, std::string& error)       { return do_filter(args, error, CmpOp::Ge); }
Value fn_csv_filter_le(NativeCtx&, std::vector<Value>& args, std::string& error)       { return do_filter(args, error, CmpOp::Le); }
Value fn_csv_filter_contains(NativeCtx&, std::vector<Value>& args, std::string& error) { return do_filter(args, error, CmpOp::Contains); }

Value fn_csv_select(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    if (!args[1].is_list()) { error = "csv.select() expects a List of column names"; return Value::null(); }
    std::vector<int> idx;
    CsvTable out;
    for (const auto& c : args[1].as_list()) {
        if (!c.is_str()) { error = "csv.select(): every column name must be a string"; return Value::null(); }
        int ci = column_index(*t, c.as_str());
        if (ci < 0) { error = "csv.select(): no such column '" + c.as_str() + "'"; return Value::null(); }
        idx.push_back(ci);
        out.columns.push_back(c.as_str());
    }
    for (const auto& row : t->rows) {
        std::vector<std::string> nrow;
        for (int ci : idx) nrow.push_back(cell_at(row, ci));
        out.rows.push_back(std::move(nrow));
    }
    return Value::integer(handles().put(std::move(out)));
}

Value fn_csv_sort_by(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    if (!args[1].is_str()) { error = "csv.sort_by(): column must be a string"; return Value::null(); }
    int ci = column_index(*t, args[1].as_str());
    if (ci < 0) { error = "csv.sort_by(): no such column '" + args[1].as_str() + "'"; return Value::null(); }
    bool desc = args.size() > 2 && args[2].is_bool() && args[2].as_bool();

    CsvTable out = *t;
    std::stable_sort(out.rows.begin(), out.rows.end(),
        [&](const std::vector<std::string>& a, const std::vector<std::string>& b) {
            const std::string& sa = cell_at(a, ci);
            const std::string& sb = cell_at(b, ci);
            auto na = parse_num(sa), nb = parse_num(sb);
            bool less = (na && nb) ? (*na < *nb) : (sa < sb);
            return desc ? !less && sa != sb : less;
        });
    return Value::integer(handles().put(std::move(out)));
}

Value fn_csv_slice(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    if (!args[1].is_int() || !args[2].is_int()) { error = "csv.slice(): start/end must be ints"; return Value::null(); }
    long long start = std::max<long long>(0, args[1].as_int());
    long long end   = std::min<long long>(static_cast<long long>(t->rows.size()), args[2].as_int());
    CsvTable out;
    out.columns = t->columns;
    for (long long i = start; i < end; ++i) out.rows.push_back(t->rows[static_cast<size_t>(i)]);
    return Value::integer(handles().put(std::move(out)));
}

// Shared body of sum/mean/min/max -- non-numeric cells are ignored (as most
// spreadsheet/data tools do for a numeric aggregate), not an error.
enum class AggOp { Sum, Mean, Min, Max, Count };

Value do_aggregate(std::vector<Value>& args, std::string& error, AggOp op) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    if (op == AggOp::Count) return Value::integer(static_cast<long long>(t->rows.size()));
    if (!args[1].is_str()) { error = "csv: column must be a string"; return Value::null(); }
    int ci = column_index(*t, args[1].as_str());
    if (ci < 0) { error = "csv: no such column '" + args[1].as_str() + "'"; return Value::null(); }

    double sum = 0; long long n = 0;
    double lo = 0, hi = 0; bool any = false;
    for (const auto& row : t->rows) {
        auto v = parse_num(cell_at(row, ci));
        if (!v) continue;
        sum += *v; ++n;
        if (!any) { lo = hi = *v; any = true; } else { lo = std::min(lo, *v); hi = std::max(hi, *v); }
    }
    switch (op) {
        case AggOp::Sum:  return Value::real(sum);
        case AggOp::Mean: return Value::real(n > 0 ? sum / static_cast<double>(n) : 0.0);
        case AggOp::Min:  return Value::real(any ? lo : 0.0);
        case AggOp::Max:  return Value::real(any ? hi : 0.0);
        default:          return Value::integer(0);
    }
}

Value fn_csv_sum(NativeCtx&, std::vector<Value>& args, std::string& error)   { return do_aggregate(args, error, AggOp::Sum); }
Value fn_csv_mean(NativeCtx&, std::vector<Value>& args, std::string& error)  { return do_aggregate(args, error, AggOp::Mean); }
Value fn_csv_min(NativeCtx&, std::vector<Value>& args, std::string& error)   { return do_aggregate(args, error, AggOp::Min); }
Value fn_csv_max(NativeCtx&, std::vector<Value>& args, std::string& error)   { return do_aggregate(args, error, AggOp::Max); }
Value fn_csv_count(NativeCtx&, std::vector<Value>& args, std::string& error) { return do_aggregate(args, error, AggOp::Count); }

Value fn_csv_group_sum(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    if (!args[1].is_str() || !args[2].is_str()) {
        error = "csv.group_sum(): group and value column must be strings";
        return Value::null();
    }
    int gi = column_index(*t, args[1].as_str());
    int vi = column_index(*t, args[2].as_str());
    if (gi < 0) { error = "csv.group_sum(): no such column '" + args[1].as_str() + "'"; return Value::null(); }
    if (vi < 0) { error = "csv.group_sum(): no such column '" + args[2].as_str() + "'"; return Value::null(); }

    std::map<std::string, double> sums; // sorted by group key, deterministic
    for (const auto& row : t->rows) {
        auto v = parse_num(cell_at(row, vi));
        if (!v) continue;
        sums[cell_at(row, gi)] += *v;
    }
    Value::List out;
    for (const auto& [group, sum] : sums) {
        Value::Dict d;
        d["group"] = Value::str(group);
        d["sum"]   = Value::real(sum);
        out.push_back(Value::dict(std::move(d)));
    }
    return Value::list(std::move(out));
}

Value fn_csv_to_csv(NativeCtx&, std::vector<Value>& args, std::string& error) {
    auto* t = handles().get(static_cast<int>(args[0].as_int()));
    if (!t) { error = "csv: unknown handle"; return Value::null(); }
    return Value::str(write_csv_text(*t));
}

} // namespace

LUX_MODULE(csv, {
    {"parse",            1, 2, fn_csv_parse},
    {"close",            1, 1, fn_csv_close},
    {"columns",          1, 1, fn_csv_columns},
    {"row_count",        1, 1, fn_csv_row_count},
    {"rows",             1, 1, fn_csv_rows},
    {"get",              3, 3, fn_csv_get},
    {"filter_eq",        3, 3, fn_csv_filter_eq},
    {"filter_gt",        3, 3, fn_csv_filter_gt},
    {"filter_lt",        3, 3, fn_csv_filter_lt},
    {"filter_ge",        3, 3, fn_csv_filter_ge},
    {"filter_le",        3, 3, fn_csv_filter_le},
    {"filter_contains",  3, 3, fn_csv_filter_contains},
    {"select",           2, 2, fn_csv_select},
    {"sort_by",          2, 3, fn_csv_sort_by},
    {"slice",            3, 3, fn_csv_slice},
    {"sum",              2, 2, fn_csv_sum},
    {"mean",             2, 2, fn_csv_mean},
    {"min",              2, 2, fn_csv_min},
    {"max",              2, 2, fn_csv_max},
    {"count",            1, 1, fn_csv_count},
    {"group_sum",        3, 3, fn_csv_group_sum},
    {"to_csv",           1, 1, fn_csv_to_csv},
})

} // namespace lux_script
