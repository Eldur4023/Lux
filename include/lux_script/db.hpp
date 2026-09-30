#pragma once
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <deque>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <coroutine>

#include <lux/core/event_loop.hpp>
#include <lux/task.hpp>
#include "value.hpp"

namespace lux_script {

// ─── Thread pool for the database calls ──────────────────────────────────────
//
// The three clients (SQLite, libpq, libmysqlclient) have a blocking API.
// Running them on the event loop thread would pin that whole core: every
// connection SO_REUSEPORT handed it would stop responding for as long as the
// query lasts.
//
// That is why a query suspends the handler —just like `await sleep`— and the
// real work happens here.  Each worker owns ONE connection, so there is no
// need to hand connections between threads or synchronize their use.
class DbPool {
public:
    ~DbPool();

    // A job: `work` runs on a worker with its connection; `done` runs after,
    // once the pool has looked at the connection's state (in a transaction or
    // not) -- `done` resumes the handler, which may then use that very
    // connection (a transaction's statements run inline, see await_db).
    struct Job {
        std::function<void(size_t worker)> work;
        std::function<void()>              done;
    };

    // A write for the writer (see Batch): `work` runs the statement and says
    // whether it succeeded; `done` runs once the batch it went in is
    // committed, with the commit's error ("" if it committed).
    struct Write {
        std::function<bool(size_t)>              work;
        std::function<void(const std::string&)>  done;
    };
    // Runs everything the writer had queued, as one transaction, on the
    // writer's connection (index size()); returns the commit's error.
    using Batch = std::function<std::string(size_t writer, std::vector<Write>& writes)>;

    // in_transaction(worker): whether that worker's connection has a
    // transaction open, asked on the worker itself after every job.
    // With `batch`, one more thread -- the writer -- takes submit_write().
    void start(size_t workers, std::function<bool(size_t)> in_transaction, Batch batch = {});

    // Free assignment: the first available worker takes it.
    void submit(Job job);

    // Pinned to a specific worker.  A transaction needs this: BEGIN, the
    // queries and COMMIT have to go through the SAME connection, and each
    // worker owns one.
    void submit_to(size_t worker, Job job);

    // To the writer; only when has_writer().
    void submit_write(Write w);

    // With a writer (SQLite: one writer at a time), everything that writes
    // takes turns here instead of racing for SQLite's lock: the writer's
    // batches, transactions, and a checkpoint (the external turn). Whoever
    // waits, waits in memory -- not on a pool worker in a busy handler,
    // polling for the lock, which at load tied up most of the pool and left
    // the reads queued behind it.
    //
    // A transaction: `grant` is called (under the pool's lock -- it only
    // posts) once it is the transactions' turn. The whole transaction then
    // runs on its handler's event loop, on the writer's connection (index
    // size(): the writer is idle while anyone else has the turn), and gives
    // the turn back with release_tx_turn() when it ends (commit, rollback,
    // or a BEGIN that failed). No thread hop while it holds everyone's
    // writes: a hop to a pool worker for BEGIN and another for COMMIT each
    // waited for a CPU under load -- measured, comments at 1.3s p50.
    // `origin` (the caller's event loop) lets the pool prefer a waiter on the
    // loop that just finished: handing the turn to another loop costs a wakeup
    // and the time that loop needs to reach it, ~100us under load -- the
    // whole write path idle for that long, once per transaction.
    void submit_begin(std::function<void()> grant, const void* origin = nullptr);
    void release_tx_turn();

    // Group commit of transactions. On the writer's connection a transaction
    // is a SAVEPOINT inside one shared SQLite transaction (the group): when
    // its statements are done it RELEASEs, and the group's one COMMIT covers
    // it -- one WAL append for every transaction that queued up meanwhile,
    // as the batch does for plain writes.
    //
    // tx_release: the holder of a transaction turn has released its savepoint
    // (or rolled it back). `done` (null for a rollback) is called with the
    // COMMIT's error once the group is committed -- a handler must not answer
    // before. Returns true when nobody is waiting for the turn: the caller
    // still holds it, must commit the group and call group_committed(). False
    // when the turn went on to someone who will (another transaction, or the
    // writer's batch).
    using GroupDone = std::function<void(const std::string&)>;
    bool tx_release(GroupDone done, const void* origin = nullptr);
    void group_committed(const std::string& error);
    // For a checkpoint or a replica's snapshot: blocks until nothing else
    // writes, and holds everyone off until released.
    void acquire_external_turn();
    void release_external_turn();

    bool has_writer() const { return static_cast<bool>(batch_); }

    void stop();

    size_t size() const { return workers_.size(); }

private:
    Batch                                           batch_;
    std::vector<Write>                              writes_;
    std::vector<GroupDone>                          group_;   // waiting for the group's COMMIT
    enum class Turn { None, Writer, Tx, External };
    void dispatch_turn();   // under mutex_
    Turn                                            turn_ = Turn::None, last_turn_ = Turn::None;
    size_t                                          external_waiting_ = 0;
    struct Begin { std::function<void()> grant; const void* origin; };
    std::deque<Begin>                               begins_;       // waiting for their turn
    const void*                                     last_origin_ = nullptr;   // the loop of the last transaction turn
    int                                             same_origin_ = 0;         // grants in a row to it
    bool                                            abandoned_ = false;   // stop() gave up on a transaction
    int                                             tx_streak_ = 0;   // transaction turns in a row
    std::vector<std::thread>                        threads_;
    std::vector<int>                                workers_;   // only for the size
    std::queue<Job>                                 jobs_;
    std::vector<std::queue<Job>>                    pinned_;
    std::vector<char>                               held_;   // connection inside a transaction
    std::mutex                                      mutex_;
    std::condition_variable                         cv_;          // the pool workers
    std::condition_variable                         writer_cv_;   // the writer: its turn came
    std::condition_variable                         turn_cv_;     // acquire_external_turn() and stop()
    // Under mutex_, after dispatch_turn(): who the turn's new state is news to.
    // One shared condition variable woke all of the pool's threads on every
    // write, for an event only the writer could act on.
    struct Wake { bool writer, turn; };
    Wake wake_after_turn() const { return {turn_ == Turn::Writer, turn_ == Turn::External || stopping_}; }
    void wake(Wake w) { if (w.writer) writer_cv_.notify_one(); if (w.turn) turn_cv_.notify_all(); }
    bool                                            stopping_ = false;
};

// ─── Idle connection liveness, without a round trip ─────────────────────────
//
// Between statements an idle connection has nothing to read. A server that
// closed it (KILL, restart, idle timeout) leaves an EOF or a reset waiting in
// the kernel, visible to a non-blocking peek -- no packet sent. Pending bytes
// are ambiguous (a timeout notice before the close, or a TLS session ticket on
// a healthy connection), so they answer Unknown and the driver pings.
enum class SocketState { Alive, Dead, Unknown };
SocketState peek_socket(int fd);

// ─── Driver ──────────────────────────────────────────────────────────────────
//
// A driver is the minimum needed to talk to an engine: open a worker's
// connection, query and execute.  Everything is called from the pool, never
// from the event loop thread.
class DbDriver {
public:
    virtual ~DbDriver() = default;

    // Name it is imported and invoked with from Lux Script: `sqlite`,
    // `postgres`, `mysql`.
    virtual const char* name() const = 0;

    // Configuration taken from the `<module>:` block of `app:`.
    virtual bool configure(const std::map<std::string, std::string>& options,
                           std::string& error) = 0;

    // Opens the connection of the given worker.  Called once per worker, the
    // first time work reaches it.
    virtual bool open(size_t worker, std::string& error) = 0;

    // SELECT: returns a List of Dict, one entry per row.
    virtual bool query(size_t worker, const std::string& sql,
                       const std::vector<Value>& args,
                       Value& out, std::string& error) = 0;

    // INSERT/UPDATE/DELETE/DDL: returns the number of affected rows.
    virtual bool exec(size_t worker, const std::string& sql,
                      const std::vector<Value>& args,
                      long long& affected, std::string& error) = 0;

    // Whether the worker's connection is inside a transaction, asked of the
    // connection itself. The pool keeps such a worker for its own
    // transaction; see DbPool::start.
    virtual bool in_transaction(size_t worker) const = 0;

    // Identifier generated by the last INSERT on that connection.  Not every
    // engine offers it reliably; the ones that do not say so.
    virtual bool last_insert_id(size_t worker, long long& id, std::string& error) {
        (void)worker; (void)id;
        error = std::string(name()) + ": last_id() is not available; use "
                "'insert ... returning id' with query()";
        return false;
    }

    // A read outside a transaction, run on the CALLING thread (the event
    // loop) instead of the pool, for an engine where that can be cheaper
    // than the thread handoff. NotHandled sends it to the pool as usual --
    // the default, and the answer for anything that would block or run long.
    enum class Inline { NotHandled, Done, Failed };
    virtual Inline query_inline(const std::string& sql, const std::vector<Value>& args,
                                Value& out, std::string& error) {
        (void)sql; (void)args; (void)out; (void)error;
        return Inline::NotHandled;
    }

    // Group commit, for an engine with a single writer: an exec() outside a
    // transaction for which batchable() says yes goes to the pool's writer,
    // and run_batch() commits everything queued there at once.
    // After the pool has stopped, on a clean exit: last chance to flush.
    virtual void shutdown() {}

    virtual DbPool::Batch batch() { return {}; }
    // The pool it runs on, once started (for the write turn, see DbPool).
    virtual void attach(DbPool* pool) { (void)pool; }

    virtual bool batchable(const std::string& sql) const { (void)sql; return false; }

    size_t pool_size() const { return pool_size_; }
    void   set_pool_size(size_t n) { pool_size_ = n; }

protected:
    // The optional `pool N` key every driver accepts (1..64 workers).
    bool read_pool(const std::map<std::string, std::string>& options, std::string& error) {
        auto p = options.find("pool");
        if (p == options.end()) return true;
        long n = std::strtol(p->second.c_str(), nullptr, 10);
        if (n < 1 || n > 64) {
            error = std::string(name()) + ": 'pool' must be between 1 and 64";
            return false;
        }
        set_pool_size(static_cast<size_t>(n));
        return true;
    }

    size_t pool_size_ = 4;
};

// ─── Module registry ─────────────────────────────────────────────────────────
//
// The compiled drivers register here.  Whether a module exists depends on the
// cmake options, so an `import` of one that is not compiled has to give a
// clear error and not a strange failure later on.
class DbRegistry {
public:
    static DbRegistry& instance();

    // Names of the drivers available in this binary.
    std::vector<std::string> available() const;
    bool                     has(const std::string& name) const;

    // Activates a module with its configuration and starts its pool.
    bool activate(const std::string& name,
                  const std::map<std::string, std::string>& options,
                  std::string& error);

    // Active driver, or nullptr if that module was not imported.
    DbDriver* active(const std::string& name) const;
    DbPool*   pool(const std::string& name) const;

    void shutdown();

private:
    DbRegistry();

    struct Slot {
        std::unique_ptr<DbDriver> driver;
        std::unique_ptr<DbPool>   pool;
        bool                      activated = false;
    };
    std::map<std::string, Slot> slots_;
};

// A database call that failed comes back from await_db() as {"error": msg}
// -- never ambiguous, since a successful one is a List (query), an int
// (exec, last_id) or a bool (begin/commit/rollback), never a Dict. The
// drivers turn it into a raised error (catchable with try) right away.
inline bool db_failed(const Value& v, std::string& message) {
    if (!v.is_dict()) return false;
    auto it = v.as_dict().find("error");
    if (it == v.as_dict().end()) return false;
    message = it->second.to_string();
    return true;
}

// ─── Bridge with the engine coroutines ───────────────────────────────────────
//
// Suspends the handler, does the work in the pool and resumes it ON ITS OWN
// EVENT LOOP THREAD.  Resuming from the pool thread would touch loop
// structures from outside, which are not safe for that.
struct DbAwaitable {
    DbPool*                        pool;
    lux::core::EventLoop*       loop;
    std::function<void(size_t)>    work;   // receives the worker: it picks the connection
    int                            pinned = -1;   // >= 0 inside a transaction
    bool                           begin  = false;   // a BEGIN with a writer: DbPool::submit_begin

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        // `this` lives until the co_await finishes, and the handle is resumed
        // exactly once, so capturing them by value is safe.
        auto* p = pool; auto* l = loop; int pin = pinned;
        if (begin) {   // on the loop, ahead of its queue: it holds everyone's writes
            p->submit_begin([l, h, w = std::move(work), slot = p->size()] {
                l->post_urgent([h, w, slot]() mutable { w(slot); h.resume(); });
            }, l);
            return;
        }
        DbPool::Job job{std::move(work), [l, h] { l->post([h]() mutable { h.resume(); }); }};
        if (pin >= 0) p->submit_to(static_cast<size_t>(pin), std::move(job));
        else          p->submit(std::move(job));
    }

    void await_resume() const noexcept {}
};

// The same, for the writer: resumed once the write's batch has committed --
// never before, or the handler could answer for a write that then rolls back,
// or read on another connection without seeing its own write.
struct DbWriteAwaitable {
    DbPool*                        pool;
    lux::core::EventLoop*          loop;
    std::function<bool(size_t)>    work;
    std::string*                   commit_error;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        auto* l = loop; auto* ce = commit_error;
        pool->submit_write({std::move(work), [l, h, ce](const std::string& e) {
            *ce = e;
            l->post([h]() mutable { h.resume(); });
        }});
    }

    void await_resume() const noexcept {}
};

// Commits the transaction group the caller's savepoint is in (see
// DbPool::tx_release) and resumes the handler once it is committed.
struct GroupCommitAwaitable {
    DbPool*                 pool;
    lux::core::EventLoop*   loop;
    DbDriver*               driver;
    size_t                  slot;   // the writer's connection
    std::string*            error;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h);
    void await_resume() const noexcept {}
};

// The caller holds the transaction turn and nobody is waiting for it: COMMIT
// the group (if a transaction is open) and hand the turn on.
void commit_group(DbPool* pool, DbDriver* driver, size_t slot);

// ─── Bridge shared between bytecode and --native ─────────────────────────────
//
// --native phase 5.5: the logic of a database suspension used to live only
// inside run_db() (project.cpp), tied to VM::Result/NativeCtx. Extracted
// here, parameterized by DbOp instead of whichever native_id, so the code
// generated for a native route can invoke EXACTLY the same path bytecode
// already uses -- not an "almost the same" reimplementation (the same class
// of silent divergence that already caused two critical fixes in this
// phase). run_db() is now a thin adapter over this.
enum class DbOp { Query, Exec, LastId, Begin, Commit, Rollback };

// {"error": msg} -- how every engine failure reaches the .lux, as a normal value.
Value db_error(const std::string& msg);

// `pinned_workers` (module -> worker) and `last_insert_ids` (module -> id): the same maps
// NativeCtx already carries for a bytecode request today -- the caller
// (run_db(), or a route's generated native code) owns these maps and
// passes them by reference, alive for the whole duration of the request.
// `sql`/`params` are ignored for LastId/Begin/Commit/Rollback (they do not
// need them).
//
// `poisoned`: modules whose in-progress transaction has already seen a
// statement fail. A real SQL engine aborts the ENTIRE transaction the
// moment a statement inside it fails -- everything that comes after,
// including a final commit(), is rejected until a rollback() -- but the
// drivers at this layer (sqlite3/libpq/libmysqlclient) do not expose that
// state uniformly, and an engine error here is always a plain Value, never
// an exception (see the paragraph below): without this tracking, a `.lux`
// that does not check the result of EVERY exec()/query() (90% of the ones
// people write, GUIDE.md included: its own transactions example does not)
// keeps chaining statements over a connection whose first INSERT already
// failed, and the final commit() confirms all of them -- the ones that DID
// work AND the gap left by the one that did not. With this, any statement
// inside an already-marked transaction returns an immediate error without
// touching the driver, and commit() on it does a ROLLBACK instead and
// reports it as an error rather than a success. Lives in the same NativeCtx
// as pinned_workers/last_insert_ids, same lifetime.
//
// Never throws or raises an error beyond this function: an engine failure
// (module not configured, invalid SQL, driver failure) produces a
// Value::Dict {"error": message} as a normal result, exactly as before --
// the `.lux` (or the generated native code) decides what to do with it,
// the function never blows up the handler.
lux::Task<Value> await_db(DbOp op, const std::string& module, lux::core::EventLoop* loop,
                            const std::string& sql, std::vector<Value> params,
                            std::map<std::string, int>& pinned_workers,
                            std::map<std::string, long long>& last_insert_ids,
                            std::set<std::string>& poisoned);

// Closes, with ROLLBACK, any transaction the handler left open (begin()
// with no commit() or rollback() by the time the route ends). Without
// this, the connection that opened it would stay pinned inside a
// transaction forever, and the next request to reuse it would inherit that
// half-finished state. Called once, right before building the response --
// see the call site in project.cpp (bytecode) and in the code that
// generates an async native route (native_gen.cpp). Empties
// `pinned_workers` when done.
// `lux restore <replica> <out.db>`: rebuilds a SQLite database from one of
// its `replicate` targets. Returns the exit code.
int restore_main(const std::vector<std::string>& args);

lux::Task<void> rollback_pending_db(std::map<std::string, int>& pinned_workers,
                                         lux::core::EventLoop* loop);

} // namespace lux_script
