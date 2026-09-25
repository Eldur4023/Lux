#include <string_view>
#include <unordered_map>
#include <lux_script/db.hpp>
#include <lux_script/crypto.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace lux_script {

namespace {

// Expands a List argument into as many `?` placeholders as it has elements,
// e.g. `where id in (?)` with args = [["a","b","c"]] becomes
// `where id in (?,?,?)` bound to "a","b","c" individually. Lux Script has no
// spread/variadic syntax (GUIDE.md), so a query like "ids in (...)" over a
// list whose length is only known at RUN time could not otherwise be
// expressed with real bind parameters — the only alternative was building
// the `in (...)` literal by hand and sanitizing it with a character
// whitelist instead of parameterizing it for real. An empty list becomes
// `(NULL)`: valid SQL, and `x IN (NULL)` is never true for any `x`, the same
// "matches nothing" behavior an empty `in (...)` is meant to have.
//
// A hand-rolled scanner, not a real SQL tokenizer: it only needs to walk
// past single/double-quoted string literals and `--`/`/* */` comments so a
// literal `?` inside one of those is not mistaken for a placeholder — the
// same minimal amount of SQL awareness sqlite3_prepare_v2 itself needs to
// count real parameters correctly. Every `?` still maps 1:1, in order, to
// one element of `args`, exactly like before this function existed; only a
// List argument now consumes more than one placeholder in the rewritten
// text.
bool expand_list_params(const std::string& sql, const std::vector<Value>& args,
                        std::string& out_sql, std::vector<Value>& out_args,
                        std::string& error) {
    bool any_list = false;
    for (const auto& a : args) if (a.is_list()) { any_list = true; break; }
    if (!any_list) { out_sql = sql; out_args = args; return true; }

    out_sql.clear();
    out_sql.reserve(sql.size());
    out_args.clear();
    out_args.reserve(args.size());

    size_t arg_i = 0;
    bool in_squote = false, in_dquote = false;
    for (size_t i = 0; i < sql.size(); ++i) {
        char c = sql[i];
        if (in_squote || in_dquote) {
            char quote = in_squote ? '\'' : '"';
            out_sql += c;
            if (c == quote) {
                if (i + 1 < sql.size() && sql[i + 1] == quote) out_sql += sql[++i]; // escaped quote
                else { in_squote = false; in_dquote = false; }
            }
            continue;
        }
        if (c == '\'') { in_squote = true; out_sql += c; continue; }
        if (c == '"')  { in_dquote = true; out_sql += c; continue; }
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') out_sql += sql[i++];
            if (i < sql.size()) out_sql += sql[i]; // the newline itself
            continue;
        }
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            out_sql += c;
            out_sql += sql[++i];
            while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) out_sql += sql[++i];
            if (i + 1 < sql.size()) out_sql += sql[++i]; // the closing '/'
            continue;
        }
        if (c == '?') {
            if (arg_i >= args.size()) { out_sql += c; continue; } // let the count check below report it
            const Value& a = args[arg_i++];
            if (!a.is_list()) { out_sql += c; out_args.push_back(a); continue; }
            const auto& l = a.as_list();

            // Accept both the idiomatic `in (?)` and a bare `in ?` as the
            // list-expanding placeholder. If it is already sitting inside
            // its own parens, expand INSIDE them instead of adding a
            // second layer: `in ((?,?,?))` is a parenthesized row-value,
            // not a plain expr-list, and SQLite rejects it ("row value
            // misused") where `in (?,?,?)` is exactly what IN expects.
            size_t back = out_sql.size();
            while (back > 0 && std::isspace(static_cast<unsigned char>(out_sql[back - 1]))) --back;
            size_t fwd = i + 1;
            while (fwd < sql.size() && std::isspace(static_cast<unsigned char>(sql[fwd]))) ++fwd;
            bool already_wrapped = back > 0 && out_sql[back - 1] == '(' &&
                                   fwd < sql.size() && sql[fwd] == ')';

            if (l.empty()) { out_sql += already_wrapped ? "NULL" : "(NULL)"; continue; }
            if (!already_wrapped) out_sql += '(';
            for (size_t k = 0; k < l.size(); ++k) {
                if (k) out_sql += ',';
                if (l[k].is_list()) { error = "sqlite: a List argument cannot contain another List"; return false; }
                out_sql += '?';
                out_args.push_back(l[k]);
            }
            if (!already_wrapped) out_sql += ')';
            continue;
        }
        out_sql += c;
    }
    for (; arg_i < args.size(); ++arg_i) out_args.push_back(args[arg_i]);
    return true;
}

// SQLite driver.
//
// Each worker opens its own connection to the same file.  SQLite serializes
// writes internally, so several concurrent connections are safe; what is
// enabled is WAL, which allows reading while another writes instead of
// blocking everyone.
class SqliteDriver : public DbDriver {
public:
    const char* name() const override { return "sqlite"; }

    bool configure(const std::map<std::string, std::string>& options,
                   std::string& error) override {
        auto it = options.find("file");
        if (it == options.end() || it->second.empty()) {
            error = "sqlite: missing 'file' in the configuration block";
            return false;
        }
        file_ = it->second;

        if (!read_pool(options, error)) return false;

        auto t = options.find("timeout_ms");
        if (t != options.end()) busy_timeout_ = std::atoi(t->second.c_str());

        conns_.assign(pool_size(), nullptr);
        cache_.assign(pool_size(), {});
        return true;
    }

    bool open(size_t worker, std::string& error) override {
        if (worker >= conns_.size()) { error = "sqlite: worker out of range"; return false; }
        if (conns_[worker]) return true;

        sqlite3* db = nullptr;
        int rc = sqlite3_open_v2(file_.c_str(), &db,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (rc != SQLITE_OK) {
            error = std::string("sqlite: cannot open '") + file_ + "': " +
                    (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
            if (db) sqlite3_close(db);
            return false;
        }

        // WAL: concurrent reads with a write in progress.  Without this, with
        // several workers any write would block every read.
        char* msg = nullptr;
        sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, &msg);
        if (msg) sqlite3_free(msg);
        // synchronous=FULL (the default) fsyncs on every commit -- measured:
        // 2.78ms/commit, versus 0.04ms/commit with NORMAL. In WAL, NORMAL is
        // still safe against any crash of the process or the application
        // (the WAL stays consistent); only the last few commits can be lost
        // in the event of a power loss or a kernel panic -- this is SQLite's
        // own recommendation for journal_mode=WAL, not an improvised
        // relaxation.
        sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, &msg);
        if (msg) sqlite3_free(msg);
        sqlite3_exec(db, "PRAGMA foreign_keys=ON", nullptr, nullptr, &msg);
        if (msg) sqlite3_free(msg);

        // Waits instead of failing when another connection holds the file.
        sqlite3_busy_timeout(db, busy_timeout_);

        conns_[worker] = db;
        return true;
    }

    bool query(size_t worker, const std::string& sql, const std::vector<Value>& args,
               Value& out, std::string& error) override {
        sqlite3_stmt* stmt = nullptr;
        bool          cached = false;
        if (!prepare(worker, sql, args, &stmt, &cached, error)) return false;

        Value::List rows;
        int cols = sqlite3_column_count(stmt);

        for (;;) {
            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                error = std::string("sqlite: ") + sqlite3_errmsg(conns_[worker]);
                release(stmt, cached);
                return false;
            }
            Value::Dict row;
            // The column count is known: without this the dictionary grew in
            // steps and it was five reallocations per row.
            row.reserve(static_cast<size_t>(cols));
            for (int i = 0; i < cols; ++i) {
                const char* col = sqlite3_column_name(stmt, i);
                // No ternary: mixing it with std::to_string forced building a
                // temporary std::string on EVERY column of EVERY row just to
                // read it back as a string_view.
                if (col) row[std::string_view(col)]  = column_value(stmt, i);
                else     row[std::to_string(i)]      = column_value(stmt, i);
            }
            rows.push_back(Value::dict(std::move(row)));
        }

        release(stmt, cached);
        out = Value::list(std::move(rows));
        return true;
    }

    bool exec(size_t worker, const std::string& sql, const std::vector<Value>& args,
              long long& affected, std::string& error) override {
        sqlite3_stmt* stmt = nullptr;
        bool          cached = false;
        if (!prepare(worker, sql, args, &stmt, &cached, error)) return false;

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            error = std::string("sqlite: ") + sqlite3_errmsg(conns_[worker]);
            release(stmt, cached);
            return false;
        }
        release(stmt, cached);
        affected = sqlite3_changes(conns_[worker]);
        return true;
    }

    bool last_insert_id(size_t worker, long long& id, std::string& error) override {
        if (worker >= conns_.size() || !conns_[worker]) {
            error = "sqlite: no connection";
            return false;
        }
        id = sqlite3_last_insert_rowid(conns_[worker]);
        return true;
    }

    ~SqliteDriver() override {
        // The statements first: sqlite3_close fails if any are still alive.
        for (auto& table : cache_)
            for (auto& [_, stmt] : table) sqlite3_finalize(stmt);
        for (auto* db : conns_) if (db) sqlite3_close(db);
    }

    // Returns the statement to where it came from.  A cached one is reset —it
    // has to be: in WAL mode a half-walked statement keeps its read snapshot
    // open— and a one-off one is destroyed.
    static void release(sqlite3_stmt* stmt, bool cached) {
        if (!stmt) return;
        if (cached) { sqlite3_reset(stmt); sqlite3_clear_bindings(stmt); }
        else          sqlite3_finalize(stmt);
    }

private:
    // Prepared statements, per connection.
    //
    // The queries of a .lux are source literals, so the set is closed and
    // small.  Without this SQLite parsed and planned the same SELECT tens of
    // thousands of times per second.
    //
    // With the cap full, a new query is prepared and destroyed as before: one
    // already inside is never evicted.  That way, whoever builds SQL by hand
    // cannot blow up the memory or evict the good ones.
    static constexpr size_t kMaxCachedStatements = 128;

    std::string           file_;
    int                   busy_timeout_ = 5000;
    std::vector<sqlite3*> conns_;
    std::vector<std::unordered_map<std::string, sqlite3_stmt*>> cache_;

    // Parameters ALWAYS go through bind, never concatenated: that is what makes
    // SQL injection impossible from Lux Script.
    bool prepare(size_t worker, const std::string& sql, const std::vector<Value>& args,
                 sqlite3_stmt** out, bool* cached, std::string& error) {
        sqlite3* db = conns_[worker];
        auto&    table = cache_[worker];

        // A List argument expands the SQL text itself (one `?` becomes N),
        // so the cache below is keyed on the EXPANDED text -- two calls
        // with lists of different lengths are, correctly, different
        // prepared statements. Everything after this point works on
        // eff_sql/eff_args exactly as it always worked on sql/args.
        std::string eff_sql;
        std::vector<Value> eff_args;
        if (!expand_list_params(sql, args, eff_sql, eff_args, error)) return false;

        if (auto it = table.find(eff_sql); it != table.end()) {
            *out      = it->second;
            *cached = true;
            sqlite3_reset(*out);
            sqlite3_clear_bindings(*out);
        } else {
            if (sqlite3_prepare_v2(db, eff_sql.c_str(), -1, out, nullptr) != SQLITE_OK) {
                error = std::string("sqlite: ") + sqlite3_errmsg(db);
                return false;
            }
            *cached = table.size() < kMaxCachedStatements;
            if (*cached) table.emplace(eff_sql, *out);
        }

        int expected = sqlite3_bind_parameter_count(*out);
        if (expected != static_cast<int>(eff_args.size())) {
            error = "sqlite: the query has " + std::to_string(expected) +
                    " parameter(s) but " + std::to_string(eff_args.size()) + " were passed";
            release(*out, *cached);
            *out = nullptr;
            return false;
        }

        for (size_t i = 0; i < eff_args.size(); ++i) {
            const Value& v = eff_args[i];
            int idx = static_cast<int>(i) + 1;
            int rc;
            if      (v.is_null())  rc = sqlite3_bind_null(*out, idx);
            else if (v.is_bool())  rc = sqlite3_bind_int(*out, idx, v.as_bool() ? 1 : 0);
            else if (v.is_int())   rc = sqlite3_bind_int64(*out, idx, v.as_int());
            else if (v.is_float()) rc = sqlite3_bind_double(*out, idx, v.as_float());
            else {
                std::string s = v.to_string();
                rc = sqlite3_bind_text(*out, idx, s.c_str(),
                                       static_cast<int>(s.size()), SQLITE_TRANSIENT);
            }
            if (rc != SQLITE_OK) {
                error = std::string("sqlite: while binding parameter ") +
                        std::to_string(idx) + ": " + sqlite3_errmsg(db);
                release(*out, *cached);
                *out = nullptr;
                return false;
            }
        }
        return true;
    }

    // SQLite has no boolean storage class of its own: a column declared
    // `BOOLEAN` (or `BOOL`) is really INTEGER affinity underneath, and a
    // plain sqlite3_column_type() can never tell the two apart -- 0/1 comes
    // back as Value::integer() either way. That silently broke the most
    // ordinary CRUD loop: read a row into a class with a `bool done` field
    // (fine, the ORM coerces it), send that exact object back on a later
    // PUT/POST (also fine, it is real Value::boolean() JSON on the wire by
    // then) -- but read the row again with a raw, schemaless query() and
    // hand THAT dict back as a request body, and the once-real bool is now
    // a 1/0 int, which value_matches() (project.cpp/native_gen.cpp)
    // correctly refuses for a `bool` field ("expected bool"), because
    // letting an int through there silently would blur every other
    // int/bool mismatch the strict body validator exists to catch.
    // sqlite3_column_decltype() gives the column's declared type from the
    // CREATE TABLE statement (not available for an expression column, in
    // which case it returns nullptr and this falls through to the plain
    // int case below, same as before) -- checking it for "BOOL" is the
    // same affinity-detection convention SQLite's own documentation
    // recommends for telling a real boolean apart from an ordinary integer
    // column, and what most sqlite wrappers that DO expose a bool type do
    // under the hood.
    static bool decltype_is_bool(sqlite3_stmt* stmt, int i) {
        const char* decl = sqlite3_column_decltype(stmt, i);
        if (!decl) return false;
        std::string d(decl);
        std::transform(d.begin(), d.end(), d.begin(), ::toupper);
        return d.find("BOOL") != std::string::npos;
    }

    static Value column_value(sqlite3_stmt* stmt, int i) {
        switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_NULL:    return Value::null();
            case SQLITE_INTEGER: {
                long long v = sqlite3_column_int64(stmt, i);
                if (decltype_is_bool(stmt, i) && (v == 0 || v == 1))
                    return Value::boolean(v != 0);
                return Value::integer(v);
            }
            case SQLITE_FLOAT:   return Value::real(sqlite3_column_double(stmt, i));

            // A BLOB is not text: it is arbitrary bytes.  Returning them as a
            // string left the response not valid UTF-8 —an x'FF' slipped through
            // raw— and then the failure is not the request's but the client's
            // that receives it, which is worse because it shows up far away.
            // Base64 is how a binary goes into a JSON.
            case SQLITE_BLOB: {
                const void* p = sqlite3_column_blob(stmt, i);
                int         n = sqlite3_column_bytes(stmt, i);
                if (!p || n <= 0) return Value::str("");
                return Value::str(crypto::base64_encode(
                    std::string_view(static_cast<const char*>(p), static_cast<size_t>(n))));
            }

            default: {
                const auto* txt = sqlite3_column_text(stmt, i);
                int         len = sqlite3_column_bytes(stmt, i);
                return Value::str(txt ? std::string(reinterpret_cast<const char*>(txt),
                                                    static_cast<size_t>(len))
                                      : std::string());
            }
        }
    }
};

} // namespace

std::unique_ptr<DbDriver> make_sqlite_driver() {
    return std::make_unique<SqliteDriver>();
}

} // namespace lux_script
