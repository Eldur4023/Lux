#include <string_view>
#include <unordered_map>
#include <lux_script/db.hpp>
#include <lux_script/crypto.hpp>
#include <lux/logger.hpp>

#include "sqlite_replica.hpp"
#include "sqlite_result_cache.hpp"
#include <bitset>

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <ctime>
#include <unordered_set>
#include <cctype>
#include <cstring>
#include <lux_script/platform_os.hpp>
#include <chrono>

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
        // SQLite counts every allocation under one process-wide mutex by default
        // (memory statistics nobody here reads): with a connection per loop
        // thread, that mutex was ~12% of the CPU of the forum crowd, in futex
        // waits inside sqlite3Malloc. Must come before SQLite initializes; if
        // something already did, it is refused and nothing changes.
        sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0);
        auto it = options.find("file");
        if (it == options.end() || it->second.empty()) {
            error = "sqlite: missing 'file' in the configuration block";
            return false;
        }
        file_ = it->second;

        if (!read_pool(options, error)) return false;

        // Result cache (sqlite_result_cache.hpp): `result_cache off` turns it off, `result_cache_ms` bounds how long a
        // result may be served (the cover for writers the hooks cannot see: another process), `result_cache_mb` its size.
        {
            ResultCache::Config rc;
            auto opt = [&](const char* k) -> const std::string* { auto f = options.find(k); return f == options.end() ? nullptr : &f->second; };
            if (const auto* v = opt("result_cache")) rc.enabled = !(*v == "off" || *v == "false" || *v == "0" || *v == "no");
            if (const auto* v = opt("result_cache_ms")) rc.ttl_ms = std::max(1, std::atoi(v->c_str()));
            if (const auto* v = opt("result_cache_mb")) rc.max_bytes = size_t(std::max(1, std::atoi(v->c_str()))) << 20;
            if (std::getenv("LUX_NO_RESULT_CACHE")) rc.enabled = false;
            rcache_ = std::make_unique<ResultCache>(rc);
        }

        auto t = options.find("timeout_ms");
        if (t != options.end()) busy_timeout_ = std::atoi(t->second.c_str());

        // Past the pool's workers, the writer's slot, then one per
        // event-loop thread for query_inline(). A :memory: database is
        // private to each connection, so an inline read -- or a write on
        // the writer -- there would see a different, empty database.
        shared_file_ = file_.find(":memory:") == std::string::npos &&
                       file_.find("mode=memory") == std::string::npos;

        // `replicate` may be given more than once (the parser joins the
        // values with newlines): every target gets every commit.
        if (auto r = options.find("replicate"); r != options.end()) {
            for (size_t at = 0; at <= r->second.size();) {
                const size_t nl = r->second.find('\n', at);
                std::string  target = r->second.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
                if (!target.empty()) replicate_.push_back(std::move(target));
                if (nl == std::string::npos) break;
                at = nl + 1;
            }
            if (!shared_file_) { error = "sqlite: an in-memory database cannot be replicated"; return false; }
            if (!SqliteReplicator::validate(replicate_, error)) return false;
        }

        const size_t slots = inline_base() + kInlineSlots;
        conns_.assign(slots, nullptr);
        cache_.assign(slots, {});
        hooks_.resize(slots);
        rc_.assign(slots, SQLITE_OK);
        deadline_ = std::vector<Deadline>(kInlineSlots);
        slow_.assign(kInlineSlots, {});
        return true;
    }

    // Measured on the bench: a primary-key read is ~2us inside SQLite and
    // ~15us of handoff to the pool and back. So reads run on the loop's own
    // connection -- until one takes over kInlineBudgetNs: the progress handler
    // interrupts it (a read, nothing to undo) and that call goes to the pool.
    // A statement interrupted kSlowStrikes times in a row is slow for this
    // thread: its calls go to the pool, except one in kProbeEvery, run inline
    // to find out whether it recovered. One interruption is not enough: the
    // SQL text is the same for every parameter (page 1 and page 900 of a
    // listing), and marking it for good on the first deep page sent every
    // later call, the fast ones too, through the pool -- half the forum
    // crowd's CPU went into that hop.
    Inline query_inline(const std::string& sql, const std::vector<Value>& args,
                        Value& out, std::string& error) override {
        if (!shared_file_) return Inline::NotHandled;
        const long slot = inline_slot();
        if (slot < 0) return Inline::NotHandled;
        auto& slow = slow_[static_cast<size_t>(slot) - inline_base()];
        auto  mark = slow.find(sql);
        if (mark != slow.end() && mark->second.strikes >= kSlowStrikes &&
            (++mark->second.skipped % kProbeEvery) != 0)
            return Inline::NotHandled;

        std::string open_error;
        if (!open(static_cast<size_t>(slot), open_error)) return Inline::NotHandled;

        deadline_[static_cast<size_t>(slot) - inline_base()].at_ns = thread_cpu_ns() + kInlineBudgetNs;
        if (query(static_cast<size_t>(slot), sql, args, out, error)) {
            if (mark != slow.end()) slow.erase(mark);   // finished in budget: strikes start over
            return Inline::Done;
        }

        switch (rc_[static_cast<size_t>(slot)] & 0xff) {
            case SQLITE_INTERRUPT: ++slow[sql].strikes; return Inline::NotHandled;
            case SQLITE_BUSY:
            case SQLITE_LOCKED:    return Inline::NotHandled;   // the pool may wait; the loop may not
            default:               return Inline::Failed;       // the pool would fail the same way
        }
    }

    bool open(size_t worker, std::string& error) override {
        if (worker >= conns_.size()) { error = "sqlite: worker out of range"; return false; }
        if (conns_[worker]) return true;

        // Replication starts with the first connection, not at configure
        // time: `lux --check` configures too, and must not upload anything.
        if (!replicate_.empty()) {
            std::call_once(replicator_once_, [&] {
                replicator_ = SqliteReplicator::start(file_, replicate_,
                                                      [this](bool hold) { gate(hold); },
                                                      replicator_error_);
                if (!replicator_) lux::log().error(replicator_error_);
            });
            if (!replicator_) { error = replicator_error_; return false; }
        }

        // NOMUTEX: every connection here is used by one thread at a time (a
        // pool worker, the writer, or one event loop), so SQLite's own
        // per-call mutex was pure overhead -- 8.6% of a list endpoint.
        sqlite3* db = nullptr;
        int rc = sqlite3_open_v2(file_.c_str(), &db,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                                 nullptr);
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
        // No mmap_size: measured on the forum bench, mapping the file cost
        // 12% of peak throughput (page faults from 8 loop threads contend on
        // the process's mmap_lock); pread() from the page cache is cheaper.
        sqlite3_exec(db, "PRAGMA temp_store=MEMORY", nullptr, nullptr, &msg);
        if (msg) sqlite3_free(msg);

        // Waits instead of failing when another connection holds the file --
        // except on an inline slot: the event loop never sleeps.
        const bool inline_slot = worker >= inline_base();
        if (inline_slot) sqlite3_busy_timeout(db, 0);
        else             sqlite3_busy_handler(db, &busy_wait, &busy_timeout_);
        // Replicating, only the replicator checkpoints (see
        // sqlite_replica.cpp); otherwise the writing connections do.
        auto hook = std::make_unique<ConnHook>();
        hook->self = this;
        hook->checkpoints = !replicator_ && !inline_slot;
        if (replicator_)       sqlite3_wal_autocheckpoint(db, 0);
        else if (!inline_slot) {
            std::call_once(ckpt_once_, [this] { ckpt_thread_ = std::thread([this] { checkpoint_loop(); }); });
        }
        // Every connection tells the result cache which tables a commit changed, once the commit is visible.
        sqlite3_update_hook(db, &update_hook, hook.get());
        sqlite3_rollback_hook(db, &rollback_hook, hook.get());
        sqlite3_wal_hook(db, &wal_hook, hook.get());
        hooks_[worker] = std::move(hook);
        if (inline_slot)
            sqlite3_progress_handler(db, 1000, &past_deadline,
                                     &deadline_[worker - inline_base()]);

        conns_[worker] = db;
        return true;
    }

    bool query(size_t worker, const std::string& sql, const std::vector<Value>& args,
               Value& out, std::string& error) override {
        // A result still valid for this exact (sql, args) is handed out without touching SQLite. Not inside a
        // transaction: it must see its own uncommitted writes.
        thread_local std::string rkey;
        bool use_cache = rcache_->config().enabled && conns_[worker] && sqlite3_get_autocommit(conns_[worker]) &&
                         ResultCache::make_key(sql, args, rkey);
        if (use_cache && rcache_->get(rkey, out)) return true;

        sqlite3_stmt* stmt = nullptr;
        Cached*       entry = nullptr;
        if (!prepare(worker, sql, args, &stmt, &entry, error)) return false;
        const bool cached = entry != nullptr;

        // What to store, decided before the statement runs: the generations of its tables as they are now.
        std::vector<ResultCache::Tag> tags;
        uint64_t                      epoch = 0;
        use_cache = use_cache && entry && entry->cacheable;
        if (use_cache) {
            epoch = rcache_->gens().epoch();
            tags.reserve(entry->tables.size());
            for (int t : entry->tables) tags.push_back({t, rcache_->gens().gen(t)});
        }

        Value::List rows;
        int cols = -1;
        // Names and BOOL-ness are per column, not per cell: kept with the
        // cached statement, rebuilt when SQLite re-prepares it (a schema
        // change can change what `select *` returns). Read after the first
        // step: that step is where a re-prepare happens.
        std::vector<Col>  local;
        std::vector<Col>& meta = entry ? entry->meta : local;
        bool unique = entry && entry->unique_cols;

        for (;;) {
            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                error = std::string("sqlite: ") + sqlite3_errmsg(conns_[worker]);
                rc_[worker] = rc;
                release(stmt, cached);
                    return false;
            }
            if (cols < 0) {
                cols = sqlite3_column_count(stmt);
                const int rp = sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_REPREPARE, 0);
                if (!entry || entry->reprepares != rp || meta.size() != static_cast<size_t>(cols)) {
                    meta.assign(static_cast<size_t>(cols), {});
                    for (int i = 0; i < cols; ++i) {
                        const char* col = sqlite3_column_name(stmt, i);
                        meta[i] = { col ? std::string(col) : std::to_string(i), decltype_is_bool(stmt, i) };
                    }
                    // Distinct names: a row can be built by appending, no lookup per column (a repeated name
                    // keeps the old behaviour: the last value wins).
                    unique = true;
                    for (int i = 0; i < cols && unique; ++i)
                        for (int j = 0; j < i; ++j) if (meta[i].name == meta[j].name) { unique = false; break; }
                    if (entry) { entry->reprepares = rp; entry->unique_cols = unique; }
                }
            }
            Value::Dict row;
            // The column count is known: without this the dictionary grew in
            // steps and it was five reallocations per row.
            row.reserve(static_cast<size_t>(cols));
            if (unique) for (int i = 0; i < cols; ++i) row.append(std::string(meta[i].name), column_value(stmt, i, meta[i].is_bool));
            else        for (int i = 0; i < cols; ++i) row[meta[i].name] = column_value(stmt, i, meta[i].is_bool);
            rows.push_back(Value::dict(std::move(row)));
        }

        release(stmt, cached);
        out = Value::list(std::move(rows));
        if (use_cache && rcache_->admit(rkey)) rcache_->put(rkey, std::move(tags), epoch, out);
        return true;
    }

    bool exec(size_t worker, const std::string& sql, const std::vector<Value>& args,
              long long& affected, std::string& error) override {
        sqlite3_stmt* stmt = nullptr;
        Cached*       entry = nullptr;
        if (!prepare(worker, sql, args, &stmt, &entry, error)) return false;
        const bool cached = entry != nullptr;

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            error = std::string("sqlite: ") + sqlite3_errmsg(conns_[worker]);
            rc_[worker] = rc;
            release(stmt, cached);
            return false;
        }
        release(stmt, cached);
        affected = sqlite3_changes(conns_[worker]);
        if (is_ddl(sql) && hooks_[worker]) hooks_[worker]->all = true;   // the commit that follows invalidates every cached result
        return true;
    }

    bool in_transaction(size_t worker) const override {
        return worker < conns_.size() && conns_[worker] && !sqlite3_get_autocommit(conns_[worker]);
    }

    bool last_insert_id(size_t worker, long long& id, std::string& error) override {
        if (worker >= conns_.size() || !conns_[worker]) {
            error = "sqlite: no connection";
            return false;
        }
        id = sqlite3_last_insert_rowid(conns_[worker]);
        return true;
    }

    // The writer: one connection takes every plain write, so they never
    // fight over SQLite's write lock (the busy_wait sleeps were most of a
    // write's latency), and everything that queued up while the last batch
    // ran commits as one transaction -- one WAL append instead of one each.
    DbPool::Batch batch() override {
        if (!shared_file_) return {};
        return [this](size_t writer, std::vector<DbPool::Write>& writes) { return run_batch(writer, writes); };
    }

    // Statements that cannot run inside the batch's transaction go to the
    // pool as before.
    bool batchable(const std::string& sql) const override {
        size_t i = 0;
        while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
        size_t j = i;
        while (j < sql.size() && std::isalpha(static_cast<unsigned char>(sql[j]))) ++j;
        std::string kw = sql.substr(i, j - i);
        for (auto& c : kw) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        static const std::unordered_set<std::string> alone = {
            "BEGIN", "COMMIT", "END", "ROLLBACK", "SAVEPOINT", "RELEASE",
            "VACUUM", "ATTACH", "DETACH", "PRAGMA"};
        return !kw.empty() && !alone.count(kw);
    }

    // Ship the last commits while the connections still hold the WAL
    // (closing the last one checkpoints and deletes it).
    void shutdown() override { replicator_.reset(); stop_checkpointer(); }

    ~SqliteDriver() override {
        if (rcache_ && std::getenv("LUX_DB_CACHE_STATS"))
            lux::log().info("sqlite result cache: ", rcache_->hits(), " hits, ", rcache_->misses(), " misses, ", rcache_->stale(),
                            " stale, ", rcache_->stores(), " stored");
        replicator_.reset();
        stop_checkpointer();
        // The statements first: sqlite3_close fails if any are still alive.
        for (auto& table : cache_)
            for (auto& [_, c] : table) sqlite3_finalize(c.stmt);
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
    bool                  shared_file_ = false;   // a file every connection sees
    std::vector<std::string>          replicate_;
    std::unique_ptr<SqliteReplicator> replicator_;
    std::once_flag                    replicator_once_;
    std::string                       replicator_error_;

    size_t inline_base() const { return pool_size() + 1; }   // after the writer's slot

    // Each write in its own savepoint: one that fails (a constraint) is
    // undone alone, the rest still commit -- what each would have done on
    // its own. A failure that takes the whole transaction with it (a full
    // disk) fails every write in the batch.
    std::string run_batch(size_t w, std::vector<DbPool::Write>& writes) {
        std::string err;
        if (!open(w, err)) return err;
        return commit_batch(w, writes);   // it has the pool's write turn
    }

    // The replicator's and the checkpointer's gate (SqliteReplicator::start):
    // the pool's external write turn -- no batch or transaction runs while
    // it is held.
    DbPool* pool_ = nullptr;
    void attach(DbPool* pool) override { pool_ = pool; }
    void gate(bool hold) {
        if (!pool_) return;
        if (hold) pool_->acquire_external_turn();
        else      pool_->release_external_turn();
    }

    std::string commit_batch(size_t w, std::vector<DbPool::Write>& writes) {
        // A group of transactions (DbPool::tx_release) may be open: join it
        // and commit it with this batch.
        const bool joined = !sqlite3_get_autocommit(conns_[w]);
        if (!joined) throttle_writer();
        if (writes.size() == 1 && !joined) { writes[0].work(w); return ""; }

        long long n = 0;
        auto run = [&](const char* sql) { std::string e; return exec(w, sql, {}, n, e); };
        if (!joined && !run("BEGIN IMMEDIATE")) {               // the lock never came: each on its own
            for (auto& x : writes) x.work(w);
            return "";
        }
        for (auto& x : writes) {
            run("SAVEPOINT lux_write");
            if (x.work(w)) { run("RELEASE lux_write"); continue; }
            if (sqlite3_get_autocommit(conns_[w]))
                return std::string("sqlite: the batch this write was in was rolled back: ") +
                       sqlite3_errmsg(conns_[w]);
            run("ROLLBACK TO lux_write");
            run("RELEASE lux_write");
        }
        if (!run("COMMIT")) {
            std::string err = std::string("sqlite: commit failed: ") + sqlite3_errmsg(conns_[w]);
            run("ROLLBACK");
            return err;
        }
        return "";
    }

    std::vector<sqlite3*> conns_;
    struct Col    { std::string name; bool is_bool = false; };
    struct Cached { sqlite3_stmt* stmt; std::vector<Col> meta; int reprepares = -1; bool cacheable = false; std::vector<int> tables; bool unique_cols = false; };

    // What the hooks of one connection know about the transaction in flight.
    struct ConnHook {
        SqliteDriver*              self = nullptr;
        std::bitset<TableGens::kMax> pending;     // tables written by the open transaction
        bool                       all = false;   // DDL, an attached database, too many tables: everything
        bool                       checkpoints = false;
    };
    std::vector<std::unique_ptr<ConnHook>> hooks_;
    std::unique_ptr<ResultCache>           rcache_;

    static bool is_ddl(const std::string& sql) {
        size_t i = 0;
        while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
        size_t j = i;
        while (j < sql.size() && std::isalpha(static_cast<unsigned char>(sql[j]))) ++j;
        std::string kw = sql.substr(i, j - i);
        for (auto& c : kw) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return kw == "CREATE" || kw == "DROP" || kw == "ALTER" || kw == "REINDEX" || kw == "VACUUM" || kw == "ANALYZE";
    }
    static void update_hook(void* u, int, const char* db, const char* table, sqlite3_int64) {
        auto* h = static_cast<ConnHook*>(u);
        if (!db || std::strcmp(db, "main") != 0 || !table) { h->all = true; return; }
        const int id = h->self->rcache_->gens().id(table);
        if (id < 0) h->all = true; else h->pending.set(size_t(id));
    }
    static void rollback_hook(void* u) {
        auto* h = static_cast<ConnHook*>(u);
        h->pending.reset();
        h->all = false;
    }
    std::vector<std::unordered_map<std::string, Cached>> cache_;
    std::vector<int>      rc_;   // result code of each slot's last failure

    // ponytail: 64 inline slots, one per event-loop thread; a 65th loop
    // thread just uses the pool. Raise if lux ever runs more loops.
    static constexpr size_t kInlineSlots = 64;
    // CPU time of the loop thread, not wall time: under load the OS preempts
    // the loop mid-query, and a 2us read measured by the wall clock blew any
    // budget and was marked slow for good -- every statement ended up back on
    // the pool.
    static constexpr int64_t kInlineBudgetNs = 100'000;
    struct Deadline { int64_t at_ns = 0; };
    std::atomic<size_t>                           next_inline_{0};
    std::vector<Deadline>                         deadline_;   // per inline slot
    static constexpr unsigned kSlowStrikes = 8, kProbeEvery = 256;
    struct Strikes { unsigned strikes = 0, skipped = 0; };
    std::vector<std::unordered_map<std::string, Strikes>> slow_;   // per inline slot

    static int64_t thread_cpu_ns() {
#ifdef _WIN32
        FILETIME created, exited, kernel, user;   // 100 ns units
        GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
        auto ticks = [](const FILETIME& t) { return (int64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) * 100; };
        return ticks(kernel) + ticks(user);
#else
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
    }
    // sqlite3_busy_timeout() sleeps 1, 2, 5, 10... ms between retries, while
    // a write transaction here holds the lock for tens of microseconds: the
    // waiters overslept by 30x. Same budget, backoff from 20us capped at 1ms.
    static int busy_wait(void* timeout_ms, int count) {
        thread_local std::chrono::steady_clock::time_point start;
        const auto now = std::chrono::steady_clock::now();
        if (count == 0) start = now;
        if (now - start >= std::chrono::milliseconds(*static_cast<int*>(timeout_ms))) return 0;
        std::this_thread::sleep_for(std::chrono::microseconds(std::min(20 << std::min(count, 6), 1000)));
        return 1;
    }

    // SQLite's own auto-checkpoint is PASSIVE: it never waits for readers,
    // and with reads arriving back to back some reader always holds an old
    // snapshot, so the WAL is never reset -- measured: 750MB of WAL in 12s
    // of load on a 3MB database, every read slower as it grows. RESTART
    // waits for the readers in flight (microseconds each) and then starts
    // the WAL over.
    //
    // Not on the committing connection, though: a checkpoint ends in an
    // fsync, 16-25ms on the bench's disk, and every write queued behind it
    // waited that long -- it WAS the writes' p99 (19ms against gin's 5).
    // The checkpointer's thread copies and syncs the bulk while writes go
    // on, then holds the writer (gate) only for the RESTART of what came in
    // meanwhile: a few pages, whose flush is ~2ms.
    static constexpr int kCheckpointPages = 4000;
    // After a commit is in the WAL (so readers can already see it): the written tables move to a new generation,
    // then the checkpoint, if the WAL got long.
    static int wal_hook(void* u, sqlite3*, const char*, int pages) {
        auto* h = static_cast<ConnHook*>(u);
        TableGens& g = h->self->rcache_->gens();
        if (h->all) g.bump_all();
        for (size_t i = 0; i < h->pending.size() && h->pending.any(); ++i)
            if (h->pending.test(i)) { g.bump(int(i)); h->pending.reset(i); }
        h->all = false;
        h->self->wal_pages_.store(pages, std::memory_order_relaxed);
        if (h->checkpoints && pages >= kCheckpointPages) h->self->request_checkpoint();
        return SQLITE_OK;
    }
    // Backpressure for the writer. Past kCheckpointPages the checkpointer is asked to copy the WAL back, but
    // nothing slows the commits while it does: when they outrun the disk the WAL keeps growing during that copy and
    // the RESTART that ends it (which holds every write) has all of it left to do -- seconds. Far past the mark (8x),
    // each batch waits a moment before it commits, which also makes the batches bigger; it is bounded, so a
    // checkpoint that cannot finish (a reader pinning an old snapshot) never stops the writes for good.
    static int wal_high_pages() {
        static const int v = [] { const char* e = std::getenv("LUX_WAL_HIGH_PAGES"); return e && *e ? std::max(100, std::atoi(e)) : kCheckpointPages * 8; }();
        return v;
    }
    void throttle_writer() {
        const int high = wal_high_pages();
        for (int i = 0; i < 20 && wal_pages_.load(std::memory_order_relaxed) > high; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));   // up to 20 ms a batch
    }
    std::atomic<int> wal_pages_{0};
    void request_checkpoint() {
        { std::lock_guard<std::mutex> l(ckpt_m_); ckpt_due_ = true; }
        ckpt_cv_.notify_one();
    }
    void checkpoint_loop() {
        plat::set_thread_name("lux-ckpt");
        sqlite3* c = nullptr;
        if (sqlite3_open_v2(file_.c_str(), &c, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
            lux::log().error("sqlite: the checkpointer cannot open ", file_, ": ", sqlite3_errmsg(c));
            sqlite3_close(c);
            return;
        }
        sqlite3_wal_autocheckpoint(c, 0);
        // Also what opens the WAL on this connection: until then a
        // checkpoint from it does nothing.
        sqlite3_exec(c, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
        static int kBudgetMs = 5;   // for the RESTART: a slow report query must not hold the writer
        sqlite3_busy_handler(c, &busy_wait, &kBudgetMs);

        std::unique_lock<std::mutex> l(ckpt_m_);
        for (;;) {
            ckpt_cv_.wait(l, [&] { return ckpt_due_ || ckpt_stop_; });
            if (ckpt_stop_) break;
            l.unlock();
            for (int i = 0; i < 4; ++i) {   // until what is left is small
                int log = -1, done = -1;
                sqlite3_wal_checkpoint_v2(c, nullptr, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
                if (log < 0 || log - done < 100) break;
            }
            // ponytail: held ~8ms (two fsyncs, ~2ms+ each on the bench's
            // disk, plus pool reads in flight to finish); 4000 pages measured
            // best -- 1000 held as long, 5x as often; 16000 swamped the disk.
            gate(true);
            sqlite3_wal_checkpoint_v2(c, nullptr, SQLITE_CHECKPOINT_RESTART, nullptr, nullptr);
            gate(false);
            l.lock();
            ckpt_due_ = false;   // a commit past the threshold asks again
        }
        l.unlock();
        sqlite3_close(c);
    }
    void stop_checkpointer() {
        { std::lock_guard<std::mutex> l(ckpt_m_); ckpt_stop_ = true; }
        ckpt_cv_.notify_one();
        if (ckpt_thread_.joinable()) ckpt_thread_.join();
    }
    std::thread             ckpt_thread_;
    std::once_flag          ckpt_once_;
    std::mutex              ckpt_m_;
    std::condition_variable ckpt_cv_;
    bool                    ckpt_due_ = false, ckpt_stop_ = false;
    static int past_deadline(void* d) {
        return thread_cpu_ns() > static_cast<Deadline*>(d)->at_ns;
    }

    // This thread's inline slot, claimed on first use; -1 once all are taken.
    long inline_slot() {
        thread_local const SqliteDriver* owner = nullptr;
        thread_local long                slot  = -1;
        if (owner != this) {
            owner = this;
            const size_t n = next_inline_.fetch_add(1);
            slot = n < kInlineSlots ? static_cast<long>(inline_base() + n) : -1;
        }
        return slot;
    }

    struct AuthCtx { std::vector<std::string> tables; bool ok = true; };
    // Observes, never denies: a plain deterministic SELECT over ordinary tables of the main database is cacheable.
    static int auth_cb(void* u, int action, const char* a1, const char* a2, const char* a3, const char*) {
        auto* c = static_cast<AuthCtx*>(u);
        switch (action) {
            case SQLITE_SELECT: case SQLITE_RECURSIVE: break;
            case SQLITE_READ:
                if (!a1 || (a3 && std::strcmp(a3, "main") != 0) || plat::strncasecmp(a1, "sqlite_", 7) == 0) { c->ok = false; break; }
                c->tables.emplace_back(a1);
                break;
            case SQLITE_FUNCTION: {
                static const char* const kVolatile[] = {"random", "randomblob", "changes", "total_changes", "last_insert_rowid", "date", "time",
                                                         "datetime", "julianday", "strftime", "unixepoch", "load_extension", "sqlite_offset"};
                if (!a2) { c->ok = false; break; }
                for (const char* f : kVolatile) if (plat::strncasecmp(a2, f, std::strlen(f) + 1) == 0) c->ok = false;
                if (plat::strncasecmp(a2, "current_", 8) == 0) c->ok = false;
                break;
            }
            default: c->ok = false; break;
        }
        return SQLITE_OK;
    }
    // update_hook does not fire for WITHOUT ROWID tables nor, reliably, for virtual ones: results over them are not cached.
    static bool trackable(sqlite3* db, const std::string& name) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "select sql from sqlite_master where type = 'table' and name = ?1 collate nocase", -1, &st, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_STATIC);
        bool ok = true;
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char* ddl = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            std::string up = ddl ? ddl : "";
            for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            ok = up.find("WITHOUT ROWID") == std::string::npos && up.find("VIRTUAL TABLE") == std::string::npos;
        }
        sqlite3_finalize(st);
        return ok;
    }

    // Parameters ALWAYS go through bind, never concatenated: that is what makes
    // SQL injection impossible from Lux Script.
    bool prepare(size_t worker, const std::string& sql, const std::vector<Value>& args,
                 sqlite3_stmt** out, Cached** entry, std::string& error) {
        sqlite3* db = conns_[worker];
        auto&    table = cache_[worker];

        // A List argument expands the SQL text itself (one `?` becomes N),
        // so the cache below is keyed on the EXPANDED text -- two calls
        // with lists of different lengths are, correctly, different
        // prepared statements. Without a List both stay the caller's.
        std::string              expanded_sql;
        std::vector<Value>       expanded_args;
        const std::string*        q = &sql;
        const std::vector<Value>* a = &args;
        if (std::any_of(args.begin(), args.end(), [](const Value& v) { return v.is_list(); })) {
            if (!expand_list_params(sql, args, expanded_sql, expanded_args, error)) {
                rc_[worker] = SQLITE_MISUSE;
                return false;
            }
            q = &expanded_sql;
            a = &expanded_args;
        }
        const std::string&        eff_sql  = *q;
        const std::vector<Value>& eff_args = *a;

        *entry = nullptr;
        if (auto it = table.find(eff_sql); it != table.end()) {
            *entry = &it->second;
            *out   = it->second.stmt;
            sqlite3_reset(*out);
            sqlite3_clear_bindings(*out);
        } else {
            // What the statement reads, for the result cache: SQLite reports it while compiling.
            AuthCtx auth;
            const bool watch = rcache_->config().enabled && table.size() < kMaxCachedStatements;
            if (watch) sqlite3_set_authorizer(db, &auth_cb, &auth);
            const int prc = sqlite3_prepare_v2(db, eff_sql.c_str(), -1, out, nullptr);
            if (watch) sqlite3_set_authorizer(db, nullptr, nullptr);
            if (prc != SQLITE_OK) {
                error = std::string("sqlite: ") + sqlite3_errmsg(db);
                rc_[worker] = prc;
                return false;
            }
            if (table.size() < kMaxCachedStatements) {
                Cached c{*out, {}, -1, false, {}};
                if (watch && auth.ok && sqlite3_stmt_readonly(*out) && !sqlite3_stmt_isexplain(*out)) {
                    c.cacheable = true;
                    for (const std::string& t : auth.tables) {
                        const int id = trackable(db, t) ? rcache_->gens().id(t) : -1;
                        if (id < 0) { c.cacheable = false; break; }
                        if (std::find(c.tables.begin(), c.tables.end(), id) == c.tables.end()) c.tables.push_back(id);
                    }
                }
                *entry = &table.emplace(eff_sql, std::move(c)).first->second;
            }
        }
        const bool cached = *entry != nullptr;

        int expected = sqlite3_bind_parameter_count(*out);
        if (expected != static_cast<int>(eff_args.size())) {
            error = "sqlite: the query has " + std::to_string(expected) +
                    " parameter(s) but " + std::to_string(eff_args.size()) + " were passed";
            rc_[worker] = SQLITE_MISUSE;
            release(*out, cached);
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
            else if (v.is_str()) {
                // STATIC: the caller's args outlive every step of this
                // statement (an expanded copy shares their string boxes),
                // and release() clears the bindings before they go.
                const std::string& s = v.as_str();
                rc = sqlite3_bind_text(*out, idx, s.data(), static_cast<int>(s.size()), SQLITE_STATIC);
            } else {
                std::string s = v.to_string();
                rc = sqlite3_bind_text(*out, idx, s.c_str(),
                                       static_cast<int>(s.size()), SQLITE_TRANSIENT);
            }
            if (rc != SQLITE_OK) {
                error = std::string("sqlite: while binding parameter ") +
                        std::to_string(idx) + ": " + sqlite3_errmsg(db);
                rc_[worker] = rc;
                release(*out, cached);
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
    // a 1/0 int, which the strict body binder (json_bind.hpp, driven by
    // bind_body()/codigo_bind_cuerpo) correctly refuses for a `bool` field
    // ("expected bool"), because
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
        for (; decl && decl[0]; ++decl)
            if (plat::strncasecmp(decl, "BOOL", 4) == 0) return true;
        return false;
    }

    static Value column_value(sqlite3_stmt* stmt, int i, bool is_bool) {
        switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_NULL:    return Value::null();
            case SQLITE_INTEGER: {
                long long v = sqlite3_column_int64(stmt, i);
                if (is_bool && (v == 0 || v == 1))
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
