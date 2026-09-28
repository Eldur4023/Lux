#include <lux_script/db.hpp>
#include <lux/logger.hpp>

#include <sys/socket.h>
#include <cerrno>
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
    tx_owner_.assign(workers, 0);
    batch_ = std::move(batch);

    // The writer: everything queued while the previous batch ran goes in the
    // next one, so the busier it is, the more each commit carries.
    if (batch_) threads_.emplace_back([this, writer = workers] {
        std::vector<Write> writes;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return turn_ == Turn::Writer || (stopping_ && writes_.empty()); });
                if (turn_ != Turn::Writer) return;   // stopping, nothing left to write
                writes.swap(writes_);
            }
            std::string error;
            try { error = batch_(writer, writes); } catch (...) { error = "database writer failed"; }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                turn_ = Turn::None;
                dispatch_turn();
            }
            cv_.notify_all();
            for (auto& w : writes) w.done(error);
            writes.clear();
        }
    });

    for (size_t i = 0; i < workers; ++i) {
        threads_.emplace_back([this, i, in_transaction] {
            bool held = false;
            for (;;) {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    held_[i] = held;
                    // This worker's transaction had the write turn and it is
                    // over (committed, rolled back, or its BEGIN failed): the
                    // turn goes to whoever is next (dispatch_turn).
                    if (tx_owner_[i] && !held) {
                        tx_owner_[i] = 0;
                        turn_        = Turn::None;
                        dispatch_turn();
                        cv_.notify_all();
                    }
                    // A worker whose connection has a transaction open takes
                    // only what is pinned to it: a shared job would run INSIDE
                    // someone else's transaction -- rolled back with it, or,
                    // on MySQL, a BEGIN there silently commits it.
                    auto ready = [&] {
                        return !pinned_[i].empty() || (!held_[i] && (!jobs_.empty() || !begin_ready_.empty()));
                    };
                    cv_.wait(lock, [&] { return stopping_ || ready(); });
                    // Stopping with nothing queued: done -- unless a
                    // transaction is still open here; its handler may yet
                    // send the rest (see kDrainTransaction).
                    if (!ready() && (!held_[i] ||
                                     !cv_.wait_for(lock, kDrainTransaction, [&] { return !pinned_[i].empty(); }))) {
                        if (tx_owner_[i]) {   // given up on: its turn goes on
                            turn_ = Turn::None;
                            dispatch_turn();
                            cv_.notify_all();
                        }
                        return;
                    }

                    // What is pinned to this worker goes first: it is the
                    // continuation of a transaction whose connection is open.
                    if (!pinned_[i].empty()) {
                        job = std::move(pinned_[i].front());
                        pinned_[i].pop();
                    } else if (!begin_ready_.empty()) {
                        job = std::move(begin_ready_.front());
                        begin_ready_.pop();
                        tx_owner_[i] = 1;
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
        begin_ready_.push(std::move(begins_.front()));
        begins_.pop();
    }
}

void DbPool::submit_begin(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        begins_.push(std::move(job));   // waits here, not in a busy handler
        dispatch_turn();
    }
    cv_.notify_all();
}

void DbPool::acquire_external_turn() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++external_waiting_;
    dispatch_turn();
    cv_.notify_all();
    cv_.wait(lock, [&] { return turn_ == Turn::External; });
}

void DbPool::release_external_turn() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        turn_ = Turn::None;
        dispatch_turn();
    }
    cv_.notify_all();
}

void DbPool::submit_write(Write w) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        writes_.push_back(std::move(w));
        dispatch_turn();
    }
    cv_.notify_all();
}

void DbPool::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
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

Value db_error(const std::string& msg) {
    Value::Dict d;
    d["error"] = Value::str(msg);
    return Value::dict(std::move(d));
}

lux::Task<Value> await_db(DbOp op, const std::string& module, lux::core::EventLoop* loop,
                            const std::string& sql, std::vector<Value> params,
                            std::map<std::string, int>& pinned_workers,
                            std::map<std::string, long long>& last_insert_ids,
                            std::set<std::string>& poisoned) {
    auto& reg    = DbRegistry::instance();
    auto* driver = reg.active(module);
    auto* pool   = reg.pool(module);
    if (!driver || !pool)
        co_return db_error("module '" + module + "' is not configured: "
                           "its block is missing under app:");

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
                const char* stmt = (op == DbOp::Begin)  ? (module == "sqlite" ? "BEGIN IMMEDIATE" : "BEGIN")
                                  : (op == DbOp::Commit) ? "COMMIT" : "ROLLBACK";
                if (!driver->exec(worker, stmt, {}, n, err)) { errmsg = err; return; }
                result = Value::boolean(true);
            }
        };

    // Inside a transaction (SQLite): the statement runs right here, with the
    // transaction's connection -- its worker is parked until the commit, so
    // nothing else touches it. Statements in a transaction only change pages
    // in memory; the disk write is COMMIT's, and that one goes to the worker
    // (never disk I/O on an event loop). A round trip per statement held the
    // write turn -- everyone's writes -- across that many trips through a
    // busy loop: a comment's transaction capped the whole forum's writes.
    // ponytail: no CPU budget here (interrupting a write inside a transaction
    // rolls all of it back); a heavy statement in a transaction runs on the
    // loop -- send those to the worker if one ever shows up in latency.
    const bool run_inline = in_tx && pool->has_writer() &&
                            (op == DbOp::Query || op == DbOp::Exec || op == DbOp::LastId);
    if (run_inline) {
        work(static_cast<size_t>(pin));
    } else {
        co_await DbAwaitable{pool, loop, work,
            pin, op == DbOp::Begin && !in_tx && pool->has_writer(),
            op == DbOp::Begin};   // its resume holds the write turn: ahead of the loop's queue
    }

    if (!errmsg.empty()) {
        // A driver failure INSIDE a transaction poisons it for everything
        // that comes after -- see the `poisoned` comment in db.hpp.
        // begin()/commit()/rollback() failing (uncommon, but possible: the
        // connection dropped) does not count: there is no live transaction
        // to poison.
        if (in_tx && op != DbOp::Begin && op != DbOp::Commit && op != DbOp::Rollback)
            poisoned.insert(module);
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
