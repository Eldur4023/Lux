# Considerations

Known costs and limits of how Lux works today: not bugs, but things to know before
deploying, and candidates for improvement. Each entry says what happens, when, how
much it hurts, and what would fix it.

---

## A replica's full snapshot floods the disk for about a minute

**What:** `sqlite: replicate` ships every commit, but each generation of a replica
starts from a full copy of the database (`sqlite3_backup` into a spool file next to
the database, then uploaded to each target). On a large database that is a lot of
I/O at once: for a 7 GB database, ~7 GB read and ~14 GB written (spool plus a
directory target's copy), all of it through the page cache.

**When:** once when the server starts, and again each time a new generation begins:
after ~1 GB of WAL, or 4× the database size if that is larger (~28 GB of WAL for a
7 GB database).

**How much:** measured on the 1M-user forum bench (7 GB SQLite, 30 GB of RAM,
NVMe), load arriving while the first snapshot ran: throughput collapsed and the
slowest requests took several seconds until the snapshot was done. The snapshot
evicts the hot part of the database from the page cache, so reads that were memory
hits start waiting on the disk. The same load after the snapshot: 125k users with
the slowest 1% of requests at ~1 ms. The snapshot itself does not hold writes (it
reads from its own read transaction); it hurts through I/O and cache pressure.

**Workaround:** start the server, let the first snapshot finish, then send
traffic. The bench does this (`run.sh` waits for the `forum.db-replica-*` spool
file to go away).

**Fix:** throttle the snapshot's I/O (copy in slices with pauses, or a fixed
MB/s budget), read and write it with `posix_fadvise(DONTNEED)` so it does not
evict the cache, and upload straight from the backup instead of spooling a full
copy first. A snapshot would then take longer and cost almost nothing while it
runs.
