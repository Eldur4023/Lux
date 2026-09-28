#include <lux_script/db.hpp>

#include <libpq-fe.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace lux_script {

namespace {

// ─── Parameter placeholders ──────────────────────────────────────────────────
//
// In Lux Script the placeholder is `?`, sqlite's and mysql's, so the SAME
// query works on all three engines.  Postgres is the only one that numbers
// them, so the translation lives here: the driver is the one that knows its
// dialect, and that way switching engines does not force rewriting every query.
//
// The rules, each with its reason:
//
//   • It only translates if the query carries parameters.  Without them there
//     is nothing to substitute, and a bare `?` in postgres is the JSONB
//     operator —`data ? 'key'`— which must be left alone.
//   • A query written with $1 has no `?` at all, so it comes out untouched.
//     Code written before this keeps working without changes.
//   • What is inside a string, a quoted identifier, a comment or a $tag$ block
//     is not a placeholder.
//   • `??` is a literal `?`, so the JSONB operator can be used in a query that
//     also carries parameters.
//
// Returns false —leaving the reason in `error`— if the query mixes both styles
// or if the placeholders do not match the arguments.
bool traducir_marcadores(const std::string& sql, size_t nargs,
                         std::string& out, std::string& error) {
    // With no parameters there is nothing to substitute, and any `?` in the
    // query is the JSONB operator.  It leaves before even looking.
    if (nargs == 0) { out = sql; return true; }

    auto ident = [](unsigned char c) { return std::isalnum(c) || c == '_'; };

    std::string res;
    res.reserve(sql.size() + 8);
    size_t i = 0;
    int    n = 0;              // marcadores `?` encontrados
    bool   numerado = false;   // a $1, $2... was seen

    while (i < sql.size()) {
        const char c = sql[i];

        // Line comment.
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') res += sql[i++];
            continue;
        }

        // Block comment.  In postgres they nest, so they are counted.
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            int prof = 0;
            while (i < sql.size()) {
                if (sql[i] == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
                    ++prof; res += sql[i++]; res += sql[i++]; continue;
                }
                if (sql[i] == '*' && i + 1 < sql.size() && sql[i + 1] == '/') {
                    --prof; res += sql[i++]; res += sql[i++];
                    if (prof == 0) break;
                    continue;
                }
                res += sql[i++];
            }
            continue;
        }

        // String.  Inside, '' is an escaped quote; if the string is preceded by
        // an E, the backslash escapes too.
        if (c == '\'') {
            const bool con_barra = !res.empty() && (res.back() == 'E' || res.back() == 'e');
            res += sql[i++];
            while (i < sql.size()) {
                if (con_barra && sql[i] == '\\' && i + 1 < sql.size()) {
                    res += sql[i++]; res += sql[i++]; continue;
                }
                if (sql[i] == '\'') {
                    if (i + 1 < sql.size() && sql[i + 1] == '\'') {
                        res += sql[i++]; res += sql[i++]; continue;
                    }
                    res += sql[i++];
                    break;
                }
                res += sql[i++];
            }
            continue;
        }

        // Quoted identifier.
        if (c == '"') {
            res += sql[i++];
            while (i < sql.size()) {
                if (sql[i] == '"') {
                    if (i + 1 < sql.size() && sql[i + 1] == '"') {
                        res += sql[i++]; res += sql[i++]; continue;
                    }
                    res += sql[i++];
                    break;
                }
                res += sql[i++];
            }
            continue;
        }

        if (c == '$') {
            // $1, $2...: the query already comes numbered.
            if (i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
                numerado = true;
                res += sql[i++];
                continue;
            }
            // $tag$ ... $tag$: nothing inside is a placeholder.
            size_t j = i + 1;
            while (j < sql.size() && ident(static_cast<unsigned char>(sql[j]))) ++j;
            if (j < sql.size() && sql[j] == '$') {
                const std::string tag = sql.substr(i, j - i + 1);
                const size_t fin = sql.find(tag, j + 1);
                const size_t up_to = (fin == std::string::npos) ? sql.size() : fin + tag.size();
                res.append(sql, i, up_to - i);
                i = up_to;
                continue;
            }
            res += sql[i++];
            continue;
        }

        if (c == '?') {
            if (i + 1 < sql.size() && sql[i + 1] == '?') {   // `??` -> `?`
                res += '?';
                i += 2;
                continue;
            }
            res += '$';
            res += std::to_string(++n);
            ++i;
            continue;
        }

        res += sql[i++];
    }

    // Not a single `?`: the query is written postgres style and is sent as is,
    // without even touching any `??` it might carry.
    if (n == 0) { out = sql; return true; }

    if (numerado) {
        error = "postgres: the query mixes '?' and '$1' placeholders; use only one "
                "of the two styles";
        return false;
    }
    if (static_cast<size_t>(n) != nargs) {
        error = "postgres: the query has " + std::to_string(n) +
                " '?' placeholder(s) but " + std::to_string(nargs) +
                " argument(s) were passed";
        return false;
    }
    out = std::move(res);
    return true;
}

// PostgreSQL driver on top of libpq.
//
// libpq has an asynchronous API, but its waiting model does not fit someone
// else's event loop without reimplementing the connection loop.  The blocking
// one is used inside the pool: it is simpler and the effect for whoever writes
// Lux Script is the same, because the handler suspends either way.
class PostgresDriver : public DbDriver {
public:
    const char* name() const override { return "postgres"; }

    bool configure(const std::map<std::string, std::string>& options,
                   std::string& error) override {
        auto url = options.find("url");
        if (url != options.end() && !url->second.empty()) {
            conninfo_ = url->second;
        } else {
            // Without a url, it is composed from the separate pieces.
            auto get = [&](const char* k, const char* def) {
                auto it = options.find(k);
                return it == options.end() ? std::string(def) : it->second;
            };
            std::string host = get("host", "localhost");
            std::string port = get("port", "5432");
            std::string db   = get("database", "");
            std::string user = get("user", "");
            std::string pass = get("password", "");
            if (db.empty()) {
                error = "postgres: missing 'url' or 'database' in the configuration block";
                return false;
            }
            conninfo_ = "host=" + host + " port=" + port + " dbname=" + db;
            if (!user.empty()) conninfo_ += " user=" + user;
            if (!pass.empty()) conninfo_ += " password=" + pass;
        }

        // Named prepared statements save the server a parse and a plan per
        // query. PgBouncer in transaction mode (before 1.21) cannot follow
        // them across backends: `statement_cache false` goes back to one
        // unnamed statement per query.
        auto sc = options.find("statement_cache");
        if (sc != options.end()) {
            if (sc->second != "true" && sc->second != "false") {
                error = "postgres: 'statement_cache' must be true or false";
                return false;
            }
            statement_cache_ = sc->second == "true";
        }

        if (!read_pool(options, error)) return false;
        conns_ = std::vector<Conn>(pool_size());
        return true;
    }

    bool open(size_t worker, std::string& error) override {
        if (worker >= conns_.size()) { error = "postgres: worker out of range"; return false; }

        // A dropped connection reopens itself on the next query, without the
        // .lux having to know. PQstatus() only learns of a drop from a failed
        // query, so the socket is peeked first (see peek_socket in db.hpp);
        // pending bytes -- postgres sends an error before closing a
        // terminated backend -- get a real check with an empty query.
        Conn& k = conns_[worker];
        if (k.db && PQstatus(k.db) == CONNECTION_OK) {
            const SocketState st = peek_socket(PQsocket(k.db));
            if (st == SocketState::Dead) drop(worker);
            else if (st == SocketState::Unknown) PQclear(PQexec(k.db, ""));
        }
        if (k.db && PQstatus(k.db) != CONNECTION_OK) drop(worker);
        if (k.db) return true;

        PGconn* c = PQconnectdb(conninfo_.c_str());
        if (!c || PQstatus(c) != CONNECTION_OK) {
            error = std::string("postgres: cannot connect: ") +
                    (c ? PQerrorMessage(c) : "out of memory");
            if (c) PQfinish(c);
            return false;
        }
        k.db = c;
        return true;
    }

    bool query(size_t worker, const std::string& sql, const std::vector<Value>& args,
               Value& out, std::string& error) override {
        PGresult* res = run(worker, sql, args, error);
        if (!res) return false;

        int rows = PQntuples(res), cols = PQnfields(res);
        Value::List list;
        list.reserve(static_cast<size_t>(rows));

        for (int r = 0; r < rows; ++r) {
            Value::Dict row;
            for (int c = 0; c < cols; ++c) {
                const char* col = PQfname(res, c);
                row[col ? col : std::to_string(c)] =
                    PQgetisnull(res, r, c) ? Value::null()
                                           : typed(PQftype(res, c), PQgetvalue(res, r, c));
            }
            list.push_back(Value::dict(std::move(row)));
        }
        PQclear(res);
        out = Value::list(std::move(list));
        return true;
    }

    // last_id() with no extra round trip: an INSERT runs with `RETURNING`
    // its table's id column appended, and the id comes back with the insert
    // itself (the column is looked up once per statement and connection).
    // An INSERT that already says RETURNING gives the first column of its
    // last row. Nothing inserted (ON CONFLICT DO NOTHING), or no single
    // serial/identity column: last_id() says so rather than guess.
    bool exec(size_t worker, const std::string& sql, const std::vector<Value>& args,
              long long& affected, std::string& error) override {
        Conn& k = conns_[worker];
        k.has_last_id = false;
        const bool insert = starts_with_word(sql, "insert");
        PGresult* res = run(worker, sql, args, error, insert);
        if (!res) return false;
        const char* n = PQcmdTuples(res);
        affected = (n && *n) ? std::strtoll(n, nullptr, 10) : 0;
        const int rows = PQntuples(res);
        if (insert && rows > 0 && PQnfields(res) > 0 && !PQgetisnull(res, rows - 1, 0)) {
            char* end = nullptr;
            const char* v = PQgetvalue(res, rows - 1, 0);
            const long long id = std::strtoll(v, &end, 10);
            if (end != v && *end == '\0') { k.last_id = id; k.has_last_id = true; }
        }
        PQclear(res);
        return true;
    }

    // Read once: await_db() takes it right after the exec, on the same
    // connection. A later last_id() that reaches the pool can land on any
    // connection, whose id would be someone else's.
    bool last_insert_id(size_t worker, long long& id, std::string& error) override {
        if (worker < conns_.size() && conns_[worker].has_last_id) {
            id = conns_[worker].last_id;
            conns_[worker].has_last_id = false;
            return true;
        }
        error = "postgres: last_id(): the last exec() inserted no row with an id "
                "(a table needs one serial or identity column; or use "
                "'insert ... returning id' with query())";
        return false;
    }

    bool in_transaction(size_t worker) const override {
        PGconn* c = worker < conns_.size() ? conns_[worker].db : nullptr;
        if (!c) return false;
        const auto st = PQtransactionStatus(c);
        return st == PQTRANS_INTRANS || st == PQTRANS_INERROR;
    }

    ~PostgresDriver() override {
        for (size_t w = 0; w < conns_.size(); ++w) drop(w);
    }

private:
    // One per worker. `stmts` maps the translated SQL to the name of its
    // server-side prepared statement on THIS connection.
    struct Conn {
        PGconn*                                      db = nullptr;
        std::unordered_map<std::string, std::string> stmts;
        unsigned                                     next_id = 0;
        // An INSERT's SQL -> the same with `RETURNING <its id column>`
        // ("" when the table has no single serial/identity column).
        std::unordered_map<std::string, std::string> returning;
        long long                                    last_id = 0;
        bool                                         has_last_id = false;
    };
    static constexpr size_t kMaxCachedStatements = 128;

    std::string       conninfo_;
    std::vector<Conn> conns_;
    bool              statement_cache_ = true;

    void drop(size_t worker) {
        Conn& k = conns_[worker];
        if (k.db) PQfinish(k.db);
        k.db = nullptr;
        k.stmts.clear();   // prepared statements die with their connection
        k.returning.clear();
    }

    static bool starts_with_word(const std::string& sql, const char* word) {
        size_t i = 0;
        while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
        const size_t n = std::strlen(word);
        if (sql.size() - i < n) return false;
        for (size_t j = 0; j < n; ++j)
            if (std::tolower(static_cast<unsigned char>(sql[i + j])) != word[j]) return false;
        return i + n == sql.size() || !std::isalnum(static_cast<unsigned char>(sql[i + n]));
    }

    // `sql` (an INSERT, placeholders translated) with its table's id column
    // returned; `sql` itself when it already returns something, when its
    // end or table is not plain to read, or when the table has no single
    // serial/identity column.
    const std::string& with_returning(size_t worker, const std::string& sql) {
        Conn& k = conns_[worker];
        if (auto it = k.returning.find(sql); it != k.returning.end())
            return it->second.empty() ? sql : it->second;

        std::string lower(sql);
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // A comment could swallow what is appended; RETURNING is the user's.
        if (lower.find("returning") != std::string::npos || lower.find("--") != std::string::npos ||
            lower.find("/*") != std::string::npos)
            return remember(k, sql, "");

        // insert into <table>: bare, schema-qualified, or "quoted".
        size_t i = lower.find("into");
        if (i == std::string::npos) return remember(k, sql, "");
        i += 4;
        while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
        size_t j = i;
        while (j < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[j])) ||
                                  sql[j] == '_' || sql[j] == '.' || sql[j] == '"'))
            ++j;
        const std::string table = sql.substr(i, j - i);

        // Never an error, so it cannot abort a transaction it runs inside:
        // to_regclass() is NULL for a table that is not there.
        const char* lookup =
            "select quote_ident(a.attname) from pg_attribute a "
            "left join pg_attrdef d on d.adrelid = a.attrelid and d.adnum = a.attnum "
            "where a.attrelid = to_regclass($1) and a.attnum > 0 and not a.attisdropped "
            "and (a.attidentity <> '' or pg_get_expr(d.adbin, d.adrelid) like 'nextval(%')";
        const char* param = table.c_str();
        PGresult* r = PQexecParams(k.db, lookup, 1, nullptr, &param, nullptr, nullptr, 0);
        if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {   // not cached: try again next time
            if (r) PQclear(r);
            return sql;
        }
        std::string out;
        if (PQntuples(r) == 1) {
            size_t end = sql.size();
            while (end > 0 && (std::isspace(static_cast<unsigned char>(sql[end - 1])) || sql[end - 1] == ';')) --end;
            out = sql.substr(0, end) + " RETURNING " + PQgetvalue(r, 0, 0);
        }
        PQclear(r);
        return remember(k, sql, std::move(out));
    }

    // Past the cap (SQL built by hand), each such INSERT looks its table up
    // again: a round trip more, never a wrong id.
    static const std::string& remember(Conn& k, const std::string& sql, std::string out) {
        if (k.returning.size() >= kMaxCachedStatements) {
            thread_local std::string last;
            last = out.empty() ? sql : std::move(out);
            return last;
        }
        const std::string& cached = k.returning.emplace(sql, std::move(out)).first->second;
        return cached.empty() ? sql : cached;
    }

    // Named statement for `sql` on this connection, preparing it on first
    // use. Empty when caching is off or full: the caller then sends an
    // unnamed one, as before.
    std::string prepared(size_t worker, const std::string& sql, int nargs, std::string& error,
                         bool& failed) {
        failed = false;
        Conn& k = conns_[worker];
        if (!statement_cache_) return {};
        auto it = k.stmts.find(sql);
        if (it != k.stmts.end()) return it->second;
        if (k.stmts.size() >= kMaxCachedStatements) return {};

        std::string name = "lux_" + std::to_string(++k.next_id);
        PGresult* r = PQprepare(k.db, name.c_str(), sql.c_str(), nargs, nullptr);
        if (!r || PQresultStatus(r) != PGRES_COMMAND_OK) {
            error = std::string("postgres: ") + (r ? PQresultErrorMessage(r) : PQerrorMessage(k.db));
            if (r) PQclear(r);
            failed = true;
            return {};
        }
        PQclear(r);
        k.stmts.emplace(sql, name);
        return name;
    }

    // SQLSTATEs that mean a cached statement is unusable: 26000 it no longer
    // exists (DEALLOCATE ALL, a pooler switched backend), 0A000 "cached plan
    // must not change result type" after an ALTER TABLE. Re-running is safe:
    // a failed statement is rolled back whole, and inside an explicit
    // transaction the retry just fails with the transaction already aborted.
    static bool stale_statement(const PGresult* r) {
        const char* st = r ? PQresultErrorField(r, PG_DIAG_SQLSTATE) : nullptr;
        return st && (std::strcmp(st, "26000") == 0 || std::strcmp(st, "0A000") == 0);
    }

    // The parameters go through PQexecParams, never concatenated: that is what
    // makes SQL injection impossible from Lux Script.  They are sent as text
    // and the server converts them to the column type.
    PGresult* run(size_t worker, const std::string& sql, const std::vector<Value>& args,
                  std::string& error, bool insert = false) {
        PGconn* c = conns_[worker].db;

        std::string sql_pg;
        if (!traducir_marcadores(sql, args.size(), sql_pg, error)) return nullptr;
        if (insert) sql_pg = with_returning(worker, sql_pg);

        std::vector<std::string> store;
        std::vector<const char*> ptrs;
        store.reserve(args.size());
        ptrs.reserve(args.size());
        for (const auto& v : args) {
            if (v.is_null()) { store.emplace_back(); ptrs.push_back(nullptr); continue; }
            store.push_back(v.is_bool() ? (v.as_bool() ? "true" : "false") : v.to_string());
            ptrs.push_back(store.back().c_str());
        }
        // No pointer fix-up needed: `store` was reserved up front, so it never
        // reallocates and every c_str() taken above stays valid.

        const int nargs = static_cast<int>(args.size());
        PGresult* res   = nullptr;
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool failed = false;
            const std::string name = prepared(worker, sql_pg, nargs, error, failed);
            if (failed) return nullptr;
            res = name.empty()
                ? PQexecParams(c, sql_pg.c_str(), nargs, nullptr, ptrs.data(), nullptr, nullptr, 0)
                : PQexecPrepared(c, name.c_str(), nargs, ptrs.data(), nullptr, nullptr, 0);
            if (name.empty() || !stale_statement(res) || attempt == 1) break;
            conns_[worker].stmts.erase(sql_pg);   // re-prepare once; it never ran
            PQclear(res);
            res = nullptr;
        }
        auto status = res ? PQresultStatus(res) : PGRES_FATAL_ERROR;
        if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
            error = std::string("postgres: ") +
                    (res ? PQresultErrorMessage(res) : PQerrorMessage(c));
            const char* st = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : nullptr;
            if (st && std::strcmp(st, "26000") == 0)
                error += " (behind PgBouncer in transaction mode? set `statement_cache false` "
                         "in the postgres: block)";
            if (res) PQclear(res);
            return nullptr;
        }
        return res;
    }

    // OIDs of the types that deserve not to arrive as a string.
    static Value typed(unsigned oid, const char* text) {
        switch (oid) {
            case 16:   return Value::boolean(text && (*text == 't' || *text == 'T'));
            case 20: case 21: case 23:            // int8, int2, int4
                return Value::integer(std::strtoll(text, nullptr, 10));
            case 700: case 701: case 1700:        // float4, float8, numeric
                return Value::real(std::strtod(text, nullptr));
            default:
                return Value::str(text ? text : "");
        }
    }
};

} // namespace

std::unique_ptr<DbDriver> make_postgres_driver() {
    return std::make_unique<PostgresDriver>();
}

} // namespace lux_script
