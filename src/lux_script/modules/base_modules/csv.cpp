// CSV in and out. csv.read() turns text into a List of Dicts (one per row,
// each cell typed: int, float, bool or string) and csv.write() does the
// reverse; filtering, sorting and aggregating are the List methods
// (filter, sort_by, map, reduce). RFC 4180 quoting, any one-character
// delimiter (Excel in many locales writes ';'), and a UTF-8 BOM is dropped.
//
// parse()/rows()/columns()/row_count()/to_csv()/close() are the older
// handle-based form, kept as a thin layer over the same code.
#include <lux_script/builtin_module.hpp>

#include <cctype>
#include <cstdlib>

namespace lux_script {

namespace {

using Row = std::vector<std::string>;

struct Table {
    Row              columns;
    std::vector<Row> rows;
};

HandleTable<Table>& handles() {
    static HandleTable<Table> h;
    return h;
}

std::vector<Row> split(const std::string& text, char delim) {
    std::vector<Row> rows;
    Row row;
    std::string field;
    bool quoted = false, pending = false;
    size_t i = text.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;   // Excel's UTF-8 BOM
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c != '"') field += c;
            else if (i + 1 < text.size() && text[i + 1] == '"') field += text[++i];
            else quoted = false;
            continue;
        }
        pending = true;
        if (c == '"' && field.empty()) quoted = true;
        else if (c == delim) { row.push_back(std::move(field)); field.clear(); }
        else if (c == '\n') {
            row.push_back(std::move(field));
            field.clear();
            rows.push_back(std::move(row));
            row.clear();
            pending = false;
        } else if (c != '\r') field += c;
    }
    if (pending || !field.empty() || !row.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

Table make_table(const std::string& text, bool header, char delim) {
    Table t;
    t.rows = split(text, delim);
    if (t.rows.empty()) return t;
    if (header) {
        t.columns = std::move(t.rows.front());
        t.rows.erase(t.rows.begin());
    } else {
        for (size_t j = 0; j < t.rows.front().size(); ++j) t.columns.push_back(std::to_string(j));
    }
    return t;
}

// A cell becomes a number only when nothing is lost: "007", "+34 600...",
// " 5" and a 25-digit account number stay text (a zip code or a phone is
// not a quantity), as do floats past what a double holds exactly.
bool canonical_number(const std::string& s, bool& is_int) {
    size_t i = s[0] == '-', digits = 0;
    const size_t int_start = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i, ++digits;
    if (i == int_start || (s[int_start] == '0' && i - int_start > 1)) return false;
    is_int = i == s.size();
    if (is_int) return digits <= 18;
    if (s[i] == '.') {
        const size_t f = ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i, ++digits;
        if (i == f) return false;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        const size_t e = i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i == e) return false;
    }
    return i == s.size() && digits <= 15;
}

Value typed(const std::string& s, bool infer) {
    if (!infer) return Value::str(s);
    if (s == "true" || s == "false") return Value::boolean(s == "true");
    bool is_int = false;
    if (s.empty() || !canonical_number(s, is_int)) return Value::str(s);
    return is_int ? Value::integer(std::strtoll(s.c_str(), nullptr, 10)) : Value::real(std::strtod(s.c_str(), nullptr));
}

Value to_rows(const Table& t, bool infer = true) {
    Value::List out;
    for (const Row& r : t.rows) {
        Value::Dict d;
        d.reserve(t.columns.size());
        for (size_t j = 0; j < t.columns.size(); ++j) d[t.columns[j]] = typed(j < r.size() ? r[j] : "", infer);
        out.push_back(Value::dict(std::move(d)));
    }
    return Value::list(std::move(out));
}

// CSV injection (OWASP): a cell starting with = + - @ TAB or CR is run as a
// formula when the file is opened in a spreadsheet. A leading ' makes it
// text -- visible if the file is parsed back, the accepted cost.
void cell(std::string& out, const std::string& s, char delim) {
    const std::string f = !s.empty() && std::string_view("=+-@\t\r").find(s[0]) != std::string_view::npos ? "'" + s : s;
    if (f.find_first_of(std::string{delim, '"', '\n', '\r'}) == std::string::npos) { out += f; return; }
    out += '"';
    for (char c : f) out += c == '"' ? "\"\"" : std::string(1, c);
    out += '"';
}

void line(std::string& out, const Row& r, char delim) {
    for (size_t j = 0; j < r.size(); ++j) {
        if (j) out += delim;
        cell(out, r[j], delim);
    }
    out += "\r\n";
}

std::string to_text(const Table& t, char delim) {
    std::string out;
    if (!t.columns.empty()) line(out, t.columns, delim);
    for (const Row& r : t.rows) line(out, r, delim);
    return out;
}

bool delimiter(const std::vector<Value>& a, size_t i, char& out, std::string& error) {
    out = ',';
    if (a.size() <= i) return true;
    if (a[i].as_str().size() != 1) { error = "csv: the delimiter must be one character"; return false; }
    out = a[i].as_str()[0];
    return true;
}

// csv.read(text[, header = true[, delimiter = ","[, typed = true]]]):
// typed false leaves every cell a string.
Value fn_read(NativeCtx&, std::vector<Value>& a, std::string& error) {
    char d;
    if (!delimiter(a, 2, d, error)) return Value::null();
    return to_rows(make_table(a[0].as_str(), a.size() < 2 || a[1].as_bool(), d), a.size() < 4 || a[3].as_bool());
}

// csv.write(rows[, columns[, delimiter]]): rows are Dicts (columns default
// to the first row's keys) or Lists (written as they are).
Value fn_write(NativeCtx&, std::vector<Value>& a, std::string& error) {
    char d;
    if (!delimiter(a, 2, d, error)) return Value::null();
    const auto& rows = a[0].as_list();
    Table t;
    if (a.size() > 1 && !a[1].is_null())
        for (const Value& c : a[1].as_list()) t.columns.push_back(c.to_string());
    else if (!rows.empty() && rows[0].is_dict())
        for (const auto& [k, _] : rows[0].as_dict()) t.columns.push_back(k);
    for (const Value& r : rows) {
        Row out;
        if (r.is_dict()) {
            for (const auto& c : t.columns) {
                auto it = r.as_dict().find(c);
                out.push_back(it == r.as_dict().end() || it->second.is_null() ? "" : it->second.to_string());
            }
        } else if (r.is_list()) {
            for (const Value& v : r.as_list()) out.push_back(v.is_null() ? "" : v.to_string());
        } else {
            error = "csv.write(): every row must be a Dict or a List";
            return Value::null();
        }
        t.rows.push_back(std::move(out));
    }
    return Value::str(to_text(t, d));
}

// ─── Handle-based form ───────────────────────────────────────────────────────

std::shared_ptr<Table> table(const std::vector<Value>& a, std::string& error) {
    auto t = handles().get(a[0].as_int());
    if (!t) error = "csv: unknown handle";
    return t;
}

Value fn_parse(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::integer(handles().put(make_table(a[0].as_str(), a.size() < 2 || a[1].as_bool(), ',')));
}

Value fn_rows(NativeCtx&, std::vector<Value>& a, std::string& e) {
    const auto t = table(a, e);
    return t ? to_rows(*t) : Value::null();
}

Value fn_columns(NativeCtx&, std::vector<Value>& a, std::string& e) {
    const auto t = table(a, e);
    if (!t) return Value::null();
    Value::List out;
    for (const auto& c : t->columns) out.push_back(Value::str(c));
    return Value::list(std::move(out));
}

Value fn_row_count(NativeCtx&, std::vector<Value>& a, std::string& e) {
    const auto t = table(a, e);
    return t ? Value::integer(static_cast<long long>(t->rows.size())) : Value::null();
}

Value fn_to_csv(NativeCtx&, std::vector<Value>& a, std::string& e) {
    const auto t = table(a, e);
    return t ? Value::str(to_text(*t, ',')) : Value::null();
}

Value fn_close(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::boolean(handles().close(a[0].as_int()));
}

} // namespace

LUX_MODULE(csv, {
    {"read",      "s|bsb", fn_read},
    {"write",     "l|Ls",  fn_write},
    {"parse",     "s|b",   fn_parse},
    {"rows",      "i",     fn_rows},
    {"columns",   "i",     fn_columns},
    {"row_count", "i",     fn_row_count},
    {"to_csv",    "i",     fn_to_csv},
    {"close",     "i",     fn_close},
})

} // namespace lux_script
