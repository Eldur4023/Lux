#include <lux_script/db.hpp>
#include <lux/logger.hpp>

#include <sys/socket.h>
#include <cerrno>
#include <algorithm>
#include <chrono>

namespace lux_script {

// ─── DbPool ──────────────────────────────────────────────────────────────────

DbPool::~DbPool() { stop(); }

// Stopping, a worker whose transaction is open waits this long for the rest
// of it (its handler is still running on an event loop) before giving up;
// closing its connection then rolls it back.
constexpr auto kDrainTransaction = std::chrono::seconds(5);

void DbPool::start(size_t workers, std::function<bool(size_t)> in_transaction, Batch batch) {
    if (!threads_.empty()) return;
    workers_.assign(workers, 0);
    pinned_.resize(workers);
    held_.assign(workers, 0);
    batch_ = std::move(batch);

    // The writer: everything queued while the previous batch ran goes in the
    // next one, so the busier it is, the more each commit carries.
    if (batch_) threads_.emplace_back([this, writer = workers] {
        pthread_setname_np(pthread_self(), "lux-dbwriter");
        std::vector<Write> writes;
        std::vector<GroupDone> group;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                writer_cv_.wait(lock, [&] { return turn_ == Turn::Writer || (stopping_ && (writes_.empty() || abandoned_)); });
                if (turn_ != Turn::Writer) return;   // stopping, nothing left to write
                writes.swap(writes_);
                group.swap(group_);   // their savepoints are in the transaction this batch commits
            }
            std::string error;
            try { error = batch_(writer, writes); } catch (...) { error = "database writer failed"; }
            Wake w;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                turn_ = Turn::None;
                dispatch_turn();
                w = wake_after_turn();
            }
            wake(w);
            for (auto& w : writes) w.done(error);
            for (auto& g : group) g(error);
            writes.clear();
            group.clear();
        }
    });

    for (size_t i = 0; i < workers; ++i) {
        threads_.emplace_back([this, i, in_transaction] {
            pthread_setname_np(pthread_self(), "lux-dbpool");
            bool held = false;
            for (;;) {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    held_[i] = held;
                    // A worker whose connection has a transaction open takes
                    // only what is pinned to it: a shared job would run INSIDE
                    // someone else's transaction -- rolled back with it, or,
                    // on MySQL, a BEGIN there silently commits it.
                    auto ready = [&] {
                        return !pinned_[i].empty() || (!held_[i] && !jobs_.empty());
                    };
                    cv_.wait(lock, [&] { return stopping_ || ready(); });
                    // Stopping with nothing queued: done -- unless a
                    // transaction is still open here; its handler may yet
                    // send the rest (see kDrainTransaction).
                    if (!ready() && (!held_[i] ||
                                     !cv_.wait_for(lock, kDrainTransaction, [&] { return !pinned_[i].empty(); })))
                        return;

                    // What is pinned to this worker goes first: it is the
                    // continuation of a transaction whose connection is open.
                    if (!pinned_[i].empty()) {
                        job = std::move(pinned_[i].front());
                        pinned_[i].pop();
                    } else {
                        job = std::move(jobs_.front());
                        jobs_.pop();
                    }
                }
                // A job that throws cannot take the worker down with it: with
                // no live connection, the module would stop answering everyone.
                try { job.work(i); } catch (...) {}
                held = in_transaction(i);
                if (job.done) job.done();   // after: the handler may use this connection next
            }
        });
    }
}

void DbPool::submit(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        jobs_.push(std::move(job));
    }
    // notify_all: the one notify_one wakes may be holding a transaction and
    // unable to take it.
    cv_.notify_all();
}

void DbPool::submit_to(size_t worker, Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Accepted while stopping: a pinned job is the rest of a transaction
        // already open, and refusing it left that transaction -- and
        // SQLite's write lock -- held for good; every job queued behind it
        // then waited out its busy timeout, one after another, and shutdown
        // with them (seen on a slow server: a hang after heavy load).
        if (worker >= pinned_.size()) return;
        pinned_[worker].push(std::move(job));
    }
    // notify_all and not notify_one: the worker that should take it may not be
    // the one that wakes, and the others will go back to sleep.
    cv_.notify_all();
}

// Under mutex_. Who writes next, once nobody does: a checkpoint first (rare,
// and the WAL grows until it runs), then transactions and the writer's batch
// taking turns -- up to kTxStreak transactions in a row, then the batch: a
// batch commits everything queued for it at once, so waiting a few turns
// costs its writes little, while one batch between every two transactions
// capped them at a turn and a half each (measured: ~500/s on a slow server).
constexpr int kTxStreak = 8;
constexpr size_t kMaxGroup = 128;
constexpr int kSameOrigin = 32;   // transaction turns in a row to one loop   // transactions in one COMMIT

void DbPool::dispatch_turn() {
    if (turn_ != Turn::None) return;
    const bool batch = !writes_.empty(), tx = !begins_.empty();
    if (external_waiting_ > 0) {
        turn_ = Turn::External;
        --external_waiting_;
        return;
    }
    if (batch && (!tx || tx_streak_ >= kTxStreak)) {
        turn_ = last_turn_ = Turn::Writer;
        tx_streak_ = 0;
    } else if (tx) {
        turn_ = last_turn_ = Turn::Tx;
        ++tx_streak_;
        // The loop that just finished goes on with its own waiters: no wakeup,
        // no waiting for another loop to get to the grant. kSameOrigin keeps
        // the others from starving.
        auto it = begins_.begin();
        if (last_origin_ && same_origin_ < kSameOrigin)
            if (auto f = std::find_if(begins_.begin(), begins_.end(),
                                      [&](const Begin& b) { return b.origin == last_origin_; });
                f != begins_.end())
                it = f;
        same_origin_ = it->origin == last_origin_ ? same_origin_ + 1 : 0;
        auto grant = std::move(it->grant);
        begins_.erase(it);
        grant();
    }
}

void DbPool::submit_begin(std::function<void()> grant, const void* origin) {
    Wake w;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        begins_.push_back({std::move(grant), origin});   // waits here, not in a busy handler
        dispatch_turn();
        w = wake_after_turn();
    }
    wake(w);
}

void DbPool::release_tx_turn() {
    Wake w;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (turn_ != Turn::Tx) return;
        turn_ = Turn::None;
        dispatch_turn();
        w = wake_after_turn();
    }
    wake(w);
}

bool DbPool::tx_release(GroupDone done, const void* origin) {
    Wake w;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done) group_.push_back(std::move(done));
        last_origin_ = origin;
        // An external turn (checkpoint, snapshot) needs the group committed
        // first; so does a stop, and a group this big has waited long enough.
        const bool more = !stopping_ && external_waiting_ == 0 && group_.size() < kMaxGroup &&
                          (!writes_.empty() || !begins_.empty());
        if (!more) {
            tx_streak_ = 0;
            return true;   // turn_ stays Tx: the caller commits
        }
        turn_ = Turn::None;
        dispatch_turn();
        w = wake_after_turn();
    }
    wake(w);
    return false;
}

void DbPool::group_committed(const std::string& error) {
    std::vector<GroupDone> waiting;
    Wake w;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        waiting.swap(group_);
        if (turn_ == Turn::Tx) {
            turn_ = Turn::None;
            dispatch_turn();
        }
        w = wake_after_turn();
    }
    wake(w);
    for (auto& g : waiting) g(error);
}

void DbPool::acquire_external_turn() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++external_waiting_;
    dispatch_turn();
    wake(wake_after_turn());
    turn_cv_.wait(lock, [&] { return turn_ == Turn::External || abandoned_; });
}

void DbPool::release_external_turn() {
    Wake w;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (turn_ != Turn::External) return;   // abandoned_: never had it
        turn_ = Turn::None;
        dispatch_turn();
        w = wake_after_turn();
    }
    wake(w);
}

void DbPool::submit_write(Write w) {
    Wake wk;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        writes_.push_back(std::move(w));
        dispatch_turn();
        wk = wake_after_turn();
    }
    wake(wk);
}

void DbPool::stop() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        begins_.clear();   // never started: their loops stop right after this
        // A transaction in progress runs on an event loop, which is still
        // running: it may finish (see kDrainTransaction). Given up on, its
        // connection is the writer's -- nothing else may touch it (the
        // writer quits; closing the connection rolls it back).
        if (!turn_cv_.wait_for(lock, kDrainTransaction, [&] { return turn_ != Turn::Tx; })) {
            abandoned_ = true;
        }
    }
    cv_.notify_all(); writer_cv_.notify_all(); turn_cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
}

// ─── DbRegistry ──────────────────────────────────────────────────────────────

// Every compiled driver is declared here.  The functions exist only if their
// cmake option is enabled.
#ifdef LUX_SQLITE
std::unique_ptr<DbDriver> make_sqlite_driver();
#endif
#ifdef LUX_POSTGRES
std::unique_ptr<DbDriver> make_postgres_driver();
#endif
#ifdef LUX_MYSQL
std::unique_ptr<DbDriver> make_mysql_driver();
#endif

#ifdef LUX_SQLITE
int sqlite_restore_main(const std::vector<std::string>& args);
#endif

int restore_main(const std::vector<std::string>& args) {
#ifdef LUX_SQLITE
    return sqlite_restore_main(args);
#else
    (void)args;
    lux::log().error("restore: this lux was built without the sqlite module");
    return 1;
#endif
}

DbRegistry::DbRegistry() {
#ifdef LUX_SQLITE
    { Slot s; s.driver = make_sqlite_driver();   slots_["sqlite"]   = std::move(s); }
#endif
#ifdef LUX_POSTGRES
    { Slot s; s.driver = make_postgres_driver(); slots_["postgres"] = std::move(s); }
#endif
#ifdef LUX_MYSQL
    { Slot s; s.driver = make_mysql_driver();    slots_["mysql"]    = std::move(s); }
#endif
}

DbRegistry& DbRegistry::instance() {
    static DbRegistry r;
    return r;
}

std::vector<std::string> DbRegistry::available() const {
    std::vector<std::string> out;
    for (const auto& [name, _] : slots_) out.push_back(name);
    return out;
}

bool DbRegistry::has(const std::string& name) const {
    return slots_.count(name) > 0;
}

bool DbRegistry::activate(const std::string& name,
                          const std::map<std::string, std::string>& options,
                          std::string& error) {
    auto it = slots_.find(name);
    if (it == slots_.end()) {
        error = "module '" + name + "' is not compiled into this binary";
        return false;
    }
    Slot& slot = it->second;
    if (slot.activated) return true;

    if (!slot.driver->configure(options, error)) return false;

    slot.pool = std::make_unique<DbPool>();
    DbDriver* d = slot.driver.get();
    slot.pool->start(d->pool_size(), [d](size_t w) { return d->in_transaction(w); }, d->batch());
    d->attach(slot.pool.get());
    slot.activated = true;
    return true;
}

DbDriver* DbRegistry::active(const std::string& name) const {
    auto it = slots_.find(name);
    if (it == slots_.end() || !it->second.activated) return nullptr;
    return it->second.driver.get();
}

DbPool* DbRegistry::pool(const std::string& name) const {
    auto it = slots_.find(name);
    if (it == slots_.end() || !it->second.activated) return nullptr;
    return it->second.pool.get();
}

void DbRegistry::shutdown() {
    for (auto& [_, slot] : slots_)
        if (slot.pool) { slot.pool->stop(); slot.driver->shutdown(); }
}

// ─── Bridge shared by bytecode and --native ──────────────────────────────────

SocketState peek_socket(int fd) {
    if (fd < 0) return SocketState::Dead;
    char c;
    const ssize_t n = ::recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return SocketState::Dead;                 // orderly close
    if (n > 0)  return SocketState::Unknown;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return SocketState::Alive;
    if (errno == EINTR) return SocketState::Unknown;
    return SocketState::Dead;                             // reset and friends
}

void GroupCommitAwaitable::await_suspend(std::coroutine_handle<> h) {
    auto* l = loop; auto* e = error;
    if (pool->tx_release([l, h, e](const std::string& err) {
            *e = err;
            l->post([h]() mutable { h.resume(); });
        }, l))
        commit_group(pool, driver, slot);
}

void commit_group(DbPool* pool, DbDriver* driver, size_t slot) {
    std::string err;
    if (driver->in_transaction(slot)) {
        long long n = 0; std::string e;
        if (!driver->exec(slot, "COMMIT", {}, n, e)) {
            err = "commit failed: " + e;
            driver->exec(slot, "ROLLBACK", {}, n, e);
        }
    }
    pool->group_committed(err);
}

Value db_error(const std::string& msg) {
    Value::Dict d;
    d["error"] = Value::str(msg);
    return Value::dict(std::move(d));
}

// A List given for one `?` becomes one `?` per item: `where id in (?)` with
// [1, 2, 3] runs `where id in (?, ?, ?)` (an empty list gives NULL, which
// matches nothing). Quoted text and comments are skipped when counting the `?`s.
void expand_list_params(std::string& sql, std::vector<Value>& params) {
    bool any = false;
    for (const auto& p : params) any |= p.is_list();
    if (!any) return;
    std::string out;
    std::vector<Value> flat;
    size_t next = 0;
    for (size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];
        if (c == '\'' || c == '"' || c == '`') {
            const size_t start = i;
            for (++i; i < sql.size() && !(sql[i] == c && (i + 1 >= sql.size() || sql[i + 1] != c)); ++i)
                if (sql[i] == c) ++i;   // a doubled quote is an escaped one
            out.append(sql, start, std::min(i + 1, sql.size()) - start);
        } else if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            const size_t end = sql.find('\n', i);
            out.append(sql, i, end == std::string::npos ? std::string::npos : end - i);
            i = end == std::string::npos ? sql.size() : end - 1;
        } else if (c == '?' && next < params.size()) {
            const Value& v = params[next++];
            if (v.is_list()) {
                const auto& items = v.as_list();
                for (size_t k = 0; k < items.size(); ++k) { out += k ? ", ?" : "?"; flat.push_back(items[k]); }
                if (items.empty()) out += "NULL";
            } else {
                out += '?';
                flat.push_back(v);
            }
        } else {
            out += c;
        }
    }
    sql = std::move(out);
    for (; next < params.size(); ++next) flat.push_back(params[next]);   // leave a count mismatch to the driver
    params = std::move(flat);
}

lux::Task<Value> await_db(DbOp op, const std::string& module, lux::core::EventLoop* loop,
                            const std::string& sql_in, std::vector<Value> params,
                            std::map<std::string, int>& pinned_workers,
                            std::map<std::string, long long>& last_insert_ids,
                            std::set<std::string>& poisoned) {
    auto& reg    = DbRegistry::instance();
    auto* driver = reg.active(module);
    auto* pool   = reg.pool(module);
    if (!driver || !pool)
        co_return db_error("module '" + module + "' is not configured: "
                           "its block is missing under app:");
    std::string sql = sql_in;
    expand_list_params(sql, params);

    // Inside a transaction, everything goes through the connection that opened it.
    int  pin   = -1;
    auto pinit = pinned_workers.find(module);
    bool in_tx = pinit != pinned_workers.end();
    if (in_tx) pin = pinit->second;

    // last_id() is the id the last exec read on its own connection, right
    // after running. Without an exec yet, or on an engine that has no such
    // id, the driver answers (0, or its error).
    if (op == DbOp::LastId) {
        auto le = last_insert_ids.find(module);
        if (le != last_insert_ids.end()) co_return Value::integer(le->second);
    }

    // An earlier statement of THIS transaction already failed: anything
    // other than commit()/rollback() is rejected without touching the
    // driver, the same way a real SQL engine would reject a command inside
    // an already-aborted transaction. See the `poisoned` comment in db.hpp.
    if (in_tx && op != DbOp::Commit && op != DbOp::Rollback && poisoned.count(module)) {
        co_return db_error("transaction aborted by an earlier failed statement "
                           "(call rollback(), or commit() will roll it back and "
                           "report the abort)");
    }

    // A commit() on a poisoned transaction is rewritten as ROLLBACK before
    // touching the driver: confirming what DID work and papering over the
    // gap left by what failed is exactly the bug this mechanism exists to
    // close. The result is replaced with an error further below, AFTER the
    // common cleanup block -- for that it is enough to run the right
    // statement here and let the rest of the code (which already treats
    // Commit and Rollback the same for pinned_workers) stay unchanged.
    // A read outside a transaction may be served right here, with no thread
    // handoff at all -- see DbDriver::query_inline.
    if (op == DbOp::Query && pin < 0) {
        Value       rows;
        std::string err;
        switch (driver->query_inline(sql, params, rows, err)) {
            case DbDriver::Inline::Done:       co_return rows;
            case DbDriver::Inline::Failed:     co_return db_error(err);
            case DbDriver::Inline::NotHandled: break;
        }
    }

    const bool aborting_commit = (op == DbOp::Commit) && in_tx && poisoned.count(module);
    const DbOp stmt_op         = aborting_commit ? DbOp::Rollback : op;

    // Frame locals, written by the pool thread: the frame stays suspended
    // (and alive) until the job posts the resume, so references are enough.
    Value       result;
    std::string errmsg;
    int         used = -1;
    long long   insert_id = 0;
    bool        has_insert_id = false;

    // A plain write outside a transaction: to the writer, committed with
    // whatever else is queued there.
    if (op == DbOp::Exec && !in_tx && pool->has_writer() && driver->batchable(sql)) {
        std::string commit_error;
        co_await DbWriteAwaitable{pool, loop,
            [&, driver](size_t worker) {
                std::string err;
                long long   n = 0;
                if (!driver->open(worker, err) || !driver->exec(worker, sql, params, n, err)) {
                    errmsg = err;
                    return false;
                }
                result        = Value::integer(n);
                has_insert_id = driver->last_insert_id(worker, insert_id, err);
                return true;
            },
            &commit_error};
        if (errmsg.empty()) errmsg = commit_error;
        if (!errmsg.empty()) co_return db_error(errmsg);
        if (has_insert_id) last_insert_ids[module] = insert_id;
        else               last_insert_ids.erase(module);
        co_return result;
    }

    const bool tx_turn = pool->has_writer();
    std::function<void(size_t)> work =
        [&, driver, op = stmt_op](size_t worker) {
            used = static_cast<int>(worker);
            std::string err;
            if (!driver->open(worker, err)) { errmsg = err; return; }

            long long n = 0;
            if (op == DbOp::Query) {
                Value rows;
                if (!driver->query(worker, sql, params, rows, err)) { errmsg = err; return; }
                result = std::move(rows);
            } else if (op == DbOp::Exec) {
                if (!driver->exec(worker, sql, params, n, err)) { errmsg = err; return; }
                result = Value::integer(n);
                has_insert_id = driver->last_insert_id(worker, insert_id, err);
            } else if (op == DbOp::LastId) {
                if (!driver->last_insert_id(worker, n, err)) { errmsg = err; return; }
                result = Value::integer(n);
            } else {
                // SQLite: IMMEDIATE takes the write lock up front. A plain
                // (deferred) BEGIN that reads and then writes fails outright
                // with SQLITE_BUSY when another one did the same -- no
                // busy_timeout wait can resolve two readers both wanting to
                // write -- so read-then-update transactions broke under load.
                if (op == DbOp::Begin && tx_turn) {
                    // With the writer: a savepoint in the group's transaction,
                    // opened by the first (see DbPool::tx_release).
                    if (!driver->in_transaction(worker) &&
                        !driver->exec(worker, "BEGIN IMMEDIATE", {}, n, err)) { errmsg = err; return; }
                    if (!driver->exec(worker, "SAVEPOINT lux_tx", {}, n, err)) { errmsg = err; return; }
                    result = Value::boolean(true);
                    return;
                }
                const char* stmt = (op == DbOp::Begin)  ? (module == "sqlite" ? "BEGIN IMMEDIATE" : "BEGIN")
                                  : (op == DbOp::Commit) ? "COMMIT" : "ROLLBACK";
                if (!driver->exec(worker, stmt, {}, n, err)) { errmsg = err; return; }
                result = Value::boolean(true);
            }
        };

    // Inside a transaction (SQLite): everything runs right here, on the
    // writer's connection, which is the transaction's while it has the
    // write turn (DbPool::submit_begin) -- COMMIT included: in WAL with
    // synchronous=NORMAL it appends to the WAL in the page cache, no fsync
    // (checkpoints run on their own thread). A round trip per statement held
    // the write turn -- everyone's writes -- across that many trips.
    // ponytail: no CPU budget here (interrupting a write inside a transaction
    // rolls all of it back); a heavy statement in a transaction runs on the
    // loop -- send those to a worker if one ever shows up in latency.
    if (in_tx && tx_turn) {
        const size_t slot = static_cast<size_t>(pin);
        if (op == DbOp::Commit || op == DbOp::Rollback) {
            auto run = [&](const char* q, bool keep_error) {
                long long n = 0; std::string e;
                const bool ok = driver->exec(slot, q, {}, n, e);
                if (!ok && keep_error && errmsg.empty()) errmsg = e;
                return ok;
            };
            if (stmt_op == DbOp::Commit && run("RELEASE lux_tx", true)) {
                std::string group_error;
                co_await GroupCommitAwaitable{pool, loop, driver, slot, &group_error};
                if (!group_error.empty()) errmsg = group_error;
                else                      result = Value::boolean(true);
            } else {   // a rollback, an aborted commit, or a RELEASE that failed
                run("ROLLBACK TO lux_tx", false);
                run("RELEASE lux_tx", false);
                if (pool->tx_release(nullptr, loop)) commit_group(pool, driver, slot);
                if (errmsg.empty()) result = Value::boolean(true);
            }
        } else {
            work(slot);
        }
    } else {
        co_await DbAwaitable{pool, loop, work, pin, op == DbOp::Begin && tx_turn};
        if (op == DbOp::Begin && tx_turn && !errmsg.empty() && pool->tx_release(nullptr, loop))
            commit_group(pool, driver, pool->size());
    }

    if (!errmsg.empty()) {
        // A driver failure INSIDE a transaction poisons it for everything
        // that comes after -- see the `poisoned` comment in db.hpp.
        // begin()/commit()/rollback() failing (uncommon, but possible: the
        // connection dropped) does not count: there is no live transaction
        // to poison.
        if (in_tx && op != DbOp::Begin && op != DbOp::Commit && op != DbOp::Rollback)
            poisoned.insert(module);
        // With the writer the turn is already given back (commit() or
        // rollback() failed after that): the transaction is over, and
        // nothing may release the turn a second time.
        if (in_tx && tx_turn && (op == DbOp::Commit || op == DbOp::Rollback)) {
            pinned_workers.erase(module);
            poisoned.erase(module);
        }
        co_return db_error(errmsg);
    }

    if (op == DbOp::Exec) {
        if (has_insert_id) last_insert_ids[module] = insert_id;
        else               last_insert_ids.erase(module);
    }

    // A transaction pins its connection when it opens and releases it when
    // it closes.
    if (op == DbOp::Begin) {
        pinned_workers[module] = used;
        poisoned.erase(module);   // new transaction, clean
    } else if (op == DbOp::Commit || op == DbOp::Rollback) {
        pinned_workers.erase(module);
        poisoned.erase(module);
    }

    if (aborting_commit)
        co_return db_error("transaction aborted by an earlier failed statement: "
                           "rolled back instead of committing");

    co_return result;
}

lux::Task<void> rollback_pending_db(std::map<std::string, int>& pinned_workers,
                                         lux::core::EventLoop* loop) {
    if (pinned_workers.empty()) co_return;

    auto pending = pinned_workers;
    for (const auto& [mod, worker] : pending) {
        auto& reg    = DbRegistry::instance();
        auto* driver = reg.active(mod);
        auto* pool   = reg.pool(mod);
        if (!driver || !pool) continue;

        lux::log().warn("transaction on '" + mod + "' left without commit or "
                           "rollback: rolling it back");
        if (pool->has_writer()) {   // it runs on this loop (see await_db)
            long long n = 0;
            std::string err;
            driver->exec(static_cast<size_t>(worker), "ROLLBACK TO lux_tx", {}, n, err);
            driver->exec(static_cast<size_t>(worker), "RELEASE lux_tx", {}, n, err);
            if (pool->tx_release(nullptr, loop)) commit_group(pool, driver, static_cast<size_t>(worker));
            continue;
        }
        co_await DbAwaitable{pool, loop,
            [driver](size_t w) {
                long long n = 0;
                std::string err;
                driver->exec(w, "ROLLBACK", {}, n, err);
            },
            worker};
    }
    pinned_workers.clear();
}

} // namespace lux_script
