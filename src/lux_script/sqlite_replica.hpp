#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lux_script {

// Continuous replication of a SQLite database: every committed transaction
// (WAL frames) is shipped, within a second, to each `replicate` target --
// `s3://bucket/prefix`, `sftp://user@host[:port]/abs/path`, or a local
// directory -- and `lux restore <target> <file>` rebuilds the database from
// any of them. See the .cpp for the layout and why it is safe.
class SqliteReplicator {
public:
    // Checks the targets (syntax, credentials) without touching the network.
    static bool validate(const std::vector<std::string>& targets, std::string& error);

    // Starts shipping `db_file`. While it runs, it is the only one that may
    // checkpoint: every other connection must have auto-checkpoint off.
    // `gate(true)` returns once the app's writer is between batches and
    // keeps it there until `gate(false)`: the writer commits back to back,
    // so SQLite's lock alone would never come free for the replicator.
    static std::unique_ptr<SqliteReplicator> start(const std::string& db_file,
                                                   const std::vector<std::string>& targets,
                                                   std::function<void(bool)> gate,
                                                   std::string& error);

    // Ships what is left, then stops.
    virtual ~SqliteReplicator() = default;
};

// `lux restore <target> <out.db>`
int sqlite_restore_main(const std::vector<std::string>& args);

} // namespace lux_script
