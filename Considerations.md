# Considerations

Known costs and limits of how Lux works today: not bugs, but things to know before
deploying, and candidates for improvement. Each entry says what happens, when, how
much it hurts, and what would fix it.

---

## A replica's full snapshot is slow, and slows commits while it runs

**What:** `sqlite: replicate` ships every commit, but each generation of a replica
starts from a full copy of the database (`sqlite3_backup` into a spool file next to
the database, then uploaded to each target). The copy goes out in 8 MB slices, each
flushed to disk and dropped from the page cache, at 100 MB/s (`kColdBytesPerSec`
in `sqlite_replica.cpp`); uploads read the spool the same way. A 7 GB database
takes ~70 s to spool, plus ~70 s per directory target.

**Why it is paced:** unpaced, the copy wrote gigabytes of dirty pages and pushed
the app's hot pages out of the cache. Measured on the 1M-user forum bench (7 GB
SQLite, 100k users arriving during the first snapshot): with 30 GB of RAM it did
not matter, but on a 12 GB machine (a cgroup cap) the slowest 1% of requests took
13.4 s. Paced and kept out of the cache: 0.83 ms, the same as with no snapshot.

**What it still costs:** the snapshot holds a read transaction for as long as it
runs, so the WAL cannot restart from the beginning until it ends: it grows, and
commits get slower meanwhile. Measured in the same run: comment (a transaction)
p99 of 409 ms during the snapshot, against 3.4 ms unpaced.

**When:** once when the server starts, and again each time a new generation begins:
after ~1 GB of WAL, or 4× the database size if that is larger (~28 GB of WAL for a
7 GB database).

**Could be better:** snapshot from the WAL's point of view instead of holding one
read transaction (copy the database file's pages, then the WAL frames that
changed them), which would let checkpoints run during the copy; or a rate that
adapts to the disk instead of a fixed one.

---

## An S3 snapshot larger than 5 GB fails to upload

**What:** a replica's snapshot goes to S3 as one `PUT`, and S3 refuses a single
`PUT` over 5 GiB (`EntityTooLarge`); R2, B2 and MinIO have the same limit or
close to it. Segments are far smaller (at most 64 MB), so only the snapshot is
affected.

**When:** a database over 5 GB replicating to `s3://`. The upload fails and is
retried every 5 s, forever; the replica keeps the previous generation, and
nothing captured since the new snapshot reaches it. Directory and `sftp://`
replicas are not affected.

**Workaround:** keep S3-replicated databases under 5 GB, or replicate a large
one to a directory or `sftp://` target.

**Fix:** multipart upload for objects past a threshold (e.g. 100 MB):
`CreateMultipartUpload`, parts of 64-512 MB (`UploadPart`, each retried on its
own), `CompleteMultipartUpload`, and `AbortMultipartUpload` when giving up. curl
already signs every request (SigV4); the new code is the three calls and the
XML of the completion. Needs an S3-compatible server to test against (MinIO).
