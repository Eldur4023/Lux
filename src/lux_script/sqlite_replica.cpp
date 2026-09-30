// Continuous replication of a SQLite database, by shipping its WAL.
//
// A write in WAL mode appends its pages to <db>-wal as frames; a commit ends
// with a commit frame, and every frame carries a checksum chained to the one
// before. So the replica is a snapshot of the file plus, in order, every
// committed frame written after it -- restoring is copying the snapshot and
// writing each frame's page at its offset. That is what Litestream does; here
// it lives inside Lux, which already owned checkpointing.
//
// What makes it safe is who may reset the WAL. SQLite starts the WAL over
// (new salts, frames overwritten from the top) only when a checkpoint has
// copied ALL of it into the database file. While replicating, every Lux
// connection has auto-checkpoint off, and the replicator keeps a read
// transaction open (the "pin"): a checkpoint cannot copy past a reader's
// snapshot, and a reader that started on a fully checkpointed WAL blocks
// every checkpoint -- this process's or another's (the sqlite3 shell, a
// backup script). The pin is let go only inside the replicator's own
// checkpoint, which runs holding the write lock right after shipping
// everything: a reset can only drop frames already shipped. Should a reset
// still come from elsewhere, it shows as new salts, and starts a new
// generation -- a fresh snapshot -- instead of a replica with a hole.
//
// Layout on every target:
//   <root>/<generation>/snapshot.db        the file, page for page
//   <root>/<generation>/<seq>.wal          "LUXWAL01" page_size frames, then
//                                          the frames (24-byte header + page)
// generation = 16 hex digits of the ms it began at; seq = 16 hex digits from 0.
// Objects are never rewritten, and each one says how long it must be, so a
// half-uploaded one is recognizable (restore stops there) without renames.
#include "sqlite_replica.hpp"

#include <lux/logger.hpp>
#include <sqlite3.h>

#ifdef LUX_HTTP
#include <curl/curl.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace lux_script {

namespace {

constexpr char     kSegMagic[8]      = {'L', 'U', 'X', 'W', 'A', 'L', '0', '1'};
constexpr size_t   kSegHeader        = 16;
constexpr size_t   kFrameHeader      = 24;
constexpr auto     kInterval         = std::chrono::seconds(1);
// ponytail: 1s between shipments (what a crash can lose), fixed; make it a
// `replicate_every` key if someone needs another trade-off.
// The WAL is read (and checkpointed past kCheckpointFrames) far more often
// than it is shipped: at 1s, a heavy write load grew it to gigabytes
// between checkpoints; frames wait in memory for the next shipment.
constexpr auto     kCaptureEvery     = std::chrono::milliseconds(100);
constexpr size_t   kFlushBytes       = 64u << 20;         // ship sooner than kInterval past this
constexpr uint32_t kCheckpointFrames = 4000;              // as the driver's wal_hook
constexpr size_t   kMaxPendingBytes  = 512u << 20;        // per target, then a new generation
// Segments shipped before a new snapshot: past max(this, 4x the database).
// Replaying segments on restore is cheap; a snapshot is the whole database
// uploaded again (at the forum bench's write rate, 1x meant one every 2s).
constexpr uint64_t kMinGenerationBytes = 1ull << 30;

// A snapshot is the whole database copied (7 GB on the 1M-user forum bench):
// at full speed through the page cache it wrote gigabytes of dirty pages
// and pushed the app's hot pages out, and the server collapsed until it was
// done. Copied instead in slices, each flushed and dropped from the cache,
// and paced: slower, and the app does not notice it.
// ponytail: a fixed rate; make it a `replicate_rate` key if a disk needs another.
constexpr uint64_t kColdSlice       = 8u << 20;
constexpr uint64_t kColdBytesPerSec = 100u << 20;

// Paces a big sequential read or write of `fd` and keeps it out of the page
// cache: every kColdSlice, what was written goes to disk and what was read
// or written is dropped. fd -1 (a segment in memory): only paced. Not
// paced once the server is stopping (g_cold_hurry): what is left of a
// snapshot or an upload then goes as fast as it can.
std::atomic<bool> g_cold_hurry{false};

class ColdIo {
public:
    ColdIo(int fd, bool writes) : fd_(fd), writes_(writes) {}
    ~ColdIo() { drop(); }
    void advance(uint64_t n) {
        done_ += n;
        if ((since_ += n) < kColdSlice) return;
        drop();
        if (!g_cold_hurry.load(std::memory_order_relaxed))
            std::this_thread::sleep_until(start_ + std::chrono::microseconds(done_ * 1'000'000 / kColdBytesPerSec));
    }

private:
    void drop() {
        since_ = 0;
        if (fd_ < 0) return;
        if (writes_) ::sync_file_range(fd_, 0, 0, SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                                                  SYNC_FILE_RANGE_WAIT_AFTER);
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
    }
    int      fd_;
    bool     writes_;
    uint64_t done_ = 0, since_ = 0;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

uint32_t be32(const unsigned char* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t le32(const unsigned char* p) {
    return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}
void put_be32(std::string& s, uint32_t v) {
    for (int i = 3; i >= 0; --i) s += char((v >> (i * 8)) & 0xff);
}
std::string hex16(uint64_t v) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}
uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
bool is_hex16(const std::string& s) {
    return s.size() == 16 && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

// ─── The WAL ─────────────────────────────────────────────────────────────────
// Format: https://www.sqlite.org/fileformat.html#the_write_ahead_log

struct WalHeader { uint32_t page_size = 0, salt1 = 0, salt2 = 0, s1 = 0, s2 = 0; bool big = false; };
struct WalPos    { uint64_t frame = 0; uint32_t s1 = 0, s2 = 0; };   // next frame, checksum so far

void wal_checksum(bool big, const unsigned char* p, size_t n, uint32_t& s1, uint32_t& s2) {
    for (size_t i = 0; i + 8 <= n; i += 8) {
        const uint32_t x0 = big ? be32(p + i) : le32(p + i);
        const uint32_t x1 = big ? be32(p + i + 4) : le32(p + i + 4);
        s1 += x0 + s2;
        s2 += x1 + s1;
    }
}

bool read_wal_header(int fd, WalHeader& h) {
    unsigned char b[32];
    if (pread(fd, b, sizeof b, 0) != static_cast<ssize_t>(sizeof b)) return false;
    const uint32_t magic = be32(b);
    if (magic != 0x377f0682 && magic != 0x377f0683) return false;
    h.big = magic & 1;
    h.page_size = be32(b + 8);
    if (h.page_size == 1) h.page_size = 65536;
    if (h.page_size < 512 || h.page_size > 65536 || (h.page_size & (h.page_size - 1))) return false;
    h.salt1 = be32(b + 16);
    h.salt2 = be32(b + 20);
    uint32_t s1 = 0, s2 = 0;
    wal_checksum(h.big, b, 24, s1, s2);
    if (s1 != be32(b + 24) || s2 != be32(b + 28)) return false;
    h.s1 = s1; h.s2 = s2;
    return true;
}

// Walks the valid frames from `pos` and returns the position after the last
// COMMIT frame among them; the frames up to it are appended to `out`. Frames
// past the last commit (a transaction still writing, or rolled back) are
// left for later, or for good.
WalPos scan_wal(int fd, const WalHeader& h, WalPos pos, std::string* out) {
    const size_t  fsz = kFrameHeader + h.page_size;
    std::string   frame(fsz, '\0');
    auto*         f = reinterpret_cast<unsigned char*>(frame.data());
    WalPos        cur = pos, committed = pos;
    std::string   pending;
    for (;;) {
        const off_t off = 32 + static_cast<off_t>(cur.frame) * static_cast<off_t>(fsz);
        if (pread(fd, f, fsz, off) != static_cast<ssize_t>(fsz)) break;
        if (be32(f + 8) != h.salt1 || be32(f + 12) != h.salt2) break;
        uint32_t s1 = cur.s1, s2 = cur.s2;
        wal_checksum(h.big, f, 8, s1, s2);
        wal_checksum(h.big, f + kFrameHeader, h.page_size, s1, s2);
        if (s1 != be32(f + 16) || s2 != be32(f + 20)) break;
        cur = {cur.frame + 1, s1, s2};
        if (out) pending += frame;
        if (be32(f + 4) != 0) {                 // commit frame: size of the db after it
            committed = cur;
            if (out) { *out += pending; pending.clear(); if (out->size() >= kFlushBytes) break; }
        }
    }
    return committed;
}

// A segment: header + frames. Its length follows from the header, so a
// truncated upload is caught.
bool segment_ok(const std::string& seg, uint32_t& page_size, uint32_t& frames) {
    if (seg.size() < kSegHeader || std::memcmp(seg.data(), kSegMagic, 8) != 0) return false;
    auto* p = reinterpret_cast<const unsigned char*>(seg.data());
    page_size = be32(p + 8);
    frames    = be32(p + 12);
    return page_size >= 512 && seg.size() == kSegHeader + uint64_t(frames) * (kFrameHeader + page_size);
}

// A database file is whole if it is as long as its header says.
bool snapshot_ok(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char h[100];
    if (!in.read(reinterpret_cast<char*>(h), sizeof h)) return false;
    if (std::memcmp(h, "SQLite format 3", 16) != 0) return false;
    uint32_t ps = uint32_t(h[16]) << 8 | h[17];
    if (ps == 1) ps = 65536;
    const uint32_t pages = be32(h + 28);
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || ps < 512 || size % ps) return false;
    // The header's page count is only valid if its version counters agree.
    if (pages && be32(h + 24) == be32(h + 92)) return size == uint64_t(pages) * ps;
    return true;
}

// ─── Targets ─────────────────────────────────────────────────────────────────

// Keys are "<generation>/<name>". Used from one thread at a time.
class Target {
public:
    explicit Target(std::string url) : url_(std::move(url)) {}
    virtual ~Target() = default;
    const std::string& url() const { return url_; }

    virtual bool put(const std::string& key, FILE* in, uint64_t size, std::string& err) = 0;
    virtual bool get(const std::string& key, const std::string& to_file, std::string& err) = 0;
    // Names right under `dir` ("" = the root): generations, or a generation's files.
    virtual bool list(const std::string& dir, std::vector<std::string>& names, std::string& err) = 0;
    // A generation and everything in it.
    virtual bool remove_generation(const std::string& gen, std::string& err) = 0;

protected:
    std::string url_;
};

class DirTarget final : public Target {
public:
    explicit DirTarget(const std::string& url)
        : Target(url), root_(url.rfind("file://", 0) == 0 ? url.substr(7) : url) {}

    bool put(const std::string& key, FILE* in, uint64_t, std::string& err) override {
        const fs::path to = root_ / key, tmp = to.string() + ".tmp";
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        FILE* out = std::fopen(tmp.c_str(), "wb");
        if (!out) { err = "cannot write " + tmp.string() + ": " + std::strerror(errno); return false; }
        char   buf[1 << 16];
        size_t n;
        bool   ok = true;
        {
            ColdIo rd(fileno(in), false), wr(fileno(out), true);
            while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) {
                if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
                rd.advance(n);
                if (std::fflush(out) != 0) { ok = false; break; }
                wr.advance(n);
            }
        }
        ok = ok && std::fflush(out) == 0 && fsync(fileno(out)) == 0;
        ok = (std::fclose(out) == 0) && ok;
        if (ok) fs::rename(tmp, to, ec);
        if (!ok || ec) { err = "cannot write " + to.string(); fs::remove(tmp, ec); return false; }
        return true;
    }
    bool get(const std::string& key, const std::string& to_file, std::string& err) override {
        std::error_code ec;
        fs::copy_file(root_ / key, to_file, fs::copy_options::overwrite_existing, ec);
        if (ec) { err = (root_ / key).string() + ": " + ec.message(); return false; }
        return true;
    }
    bool list(const std::string& dir, std::vector<std::string>& names, std::string& err) override {
        std::error_code ec;
        fs::directory_iterator it(root_ / dir, ec);
        if (ec) { err = (root_ / dir).string() + ": " + ec.message(); return false; }
        for (const auto& e : it) names.push_back(e.path().filename().string());
        return true;
    }
    bool remove_generation(const std::string& gen, std::string& err) override {
        std::error_code ec;
        fs::remove_all(root_ / gen, ec);
        if (ec) { err = ec.message(); return false; }
        return true;
    }

private:
    fs::path root_;
};

#ifdef LUX_HTTP

std::string percent_encode(const std::string& s, bool keep_slash) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

size_t to_string_cb(char* p, size_t s, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, s * n);
    return s * n;
}

// One easy handle per target, reset between requests: its connection (and
// TLS or SSH session) carries over to the next upload.
class CurlTarget : public Target {
public:
    explicit CurlTarget(const std::string& url) : Target(url) {
        static std::once_flag once;
        std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        h_ = curl_easy_init();
    }
    ~CurlTarget() override { if (h_) curl_easy_cleanup(h_); }

protected:
    // Common options, then the target's own (auth), then perform.
    bool run(const std::string& url, std::string& err, long* status = nullptr,
             const std::function<void(CURL*)>& setup = {}) {
        CURL* h = h_;
        curl_easy_reset(h);
        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // slower than 1 KB/s
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);      // for 30s: give up
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
        auth(h);
        if (setup) setup(h);
        const CURLcode rc = curl_easy_perform(h);
        if (rc != CURLE_OK) {
            err = url_ + ": " + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
            return false;
        }
        long code = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        if (status) *status = code;
        return true;
    }
    virtual void auth(CURL*) {}

    struct ColdSource { FILE* in; ColdIo io; };
    static size_t cold_read(char* p, size_t s, size_t n, void* src) {
        auto* c = static_cast<ColdSource*>(src);
        const size_t got = std::fread(p, 1, s * n, c->in);
        c->io.advance(got);
        return got;
    }

    bool upload(const std::string& url, FILE* in, uint64_t size, std::string& err,
                const std::function<void(CURL*)>& extra = {}) {
        ColdSource  src{in, ColdIo(fileno(in), false)};
        long        code = 0;
        std::string body;
        if (!run(url, err, &code, [&](CURL* h) {
                curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
                curl_easy_setopt(h, CURLOPT_READFUNCTION, cold_read);
                curl_easy_setopt(h, CURLOPT_READDATA, &src);
                curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));
                curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, to_string_cb);
                curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
                if (extra) extra(h);
            }))
            return false;
        if (code >= 300) { err = url_ + ": HTTP " + std::to_string(code) + " " + body.substr(0, 300); return false; }
        return true;
    }
    bool download(const std::string& url, const std::string& to_file, std::string& err) {
        FILE* out = std::fopen(to_file.c_str(), "wb");
        if (!out) { err = "cannot write " + to_file; return false; }
        long code = 0;
        bool ok = run(url, err, &code, [&](CURL* h) { curl_easy_setopt(h, CURLOPT_WRITEDATA, out); });
        std::fclose(out);
        if (ok && code >= 300) { err = url_ + ": HTTP " + std::to_string(code) + " for " + url; ok = false; }
        return ok;
    }

    CURL* h_ = nullptr;
};

// s3://bucket/prefix. Credentials and endpoint from the usual AWS variables;
// AWS_ENDPOINT_URL (or AWS_ENDPOINT_URL_S3) points it at R2, B2, MinIO...
// with path-style URLs. curl signs every request (SigV4).
class S3Target final : public CurlTarget {
public:
    static const char* env(const char* a, const char* b = nullptr) {
        const char* v = std::getenv(a);
        if ((!v || !*v) && b) v = std::getenv(b);
        return v && *v ? v : nullptr;
    }

    S3Target(const std::string& url, std::string& error) : CurlTarget(url) {
        const std::string rest = url.substr(5);
        const size_t      slash = rest.find('/');
        bucket_ = rest.substr(0, slash);
        prefix_ = slash == std::string::npos ? "" : rest.substr(slash + 1);
        while (!prefix_.empty() && prefix_.back() == '/') prefix_.pop_back();
        if (!prefix_.empty()) prefix_ += '/';
        if (bucket_.empty()) { error = "replicate: '" + url + "' has no bucket"; return; }
        const char* id = env("AWS_ACCESS_KEY_ID");
        const char* secret = env("AWS_SECRET_ACCESS_KEY");
        if (!id || !secret) {
            error = "replicate: '" + url + "' needs AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY";
            return;
        }
        userpwd_ = std::string(id) + ":" + secret;
        const char* region = env("AWS_REGION", "AWS_DEFAULT_REGION");
        region_ = region ? region : "us-east-1";
        if (const char* token = env("AWS_SESSION_TOKEN")) token_ = std::string("x-amz-security-token: ") + token;
        if (const char* ep = env("AWS_ENDPOINT_URL_S3", "AWS_ENDPOINT_URL")) {
            std::string e = ep;
            while (!e.empty() && e.back() == '/') e.pop_back();
            bucket_url_ = e + "/" + bucket_;
        } else {
            bucket_url_ = "https://" + bucket_ + ".s3." + region_ + ".amazonaws.com";
        }
    }

    bool put(const std::string& key, FILE* in, uint64_t size, std::string& err) override {
        return upload(object_url(key), in, size, err);
    }
    bool get(const std::string& key, const std::string& to_file, std::string& err) override {
        return download(object_url(key), to_file, err);
    }
    bool list(const std::string& dir, std::vector<std::string>& names, std::string& err) override {
        const std::string prefix = prefix_ + (dir.empty() ? "" : dir + "/");
        std::string token;
        for (;;) {
            std::string q = bucket_url_ + "?delimiter=%2F&list-type=2&prefix=" + percent_encode(prefix, false);
            if (!token.empty()) q = bucket_url_ + "?continuation-token=" + percent_encode(token, false) +
                                    "&delimiter=%2F&list-type=2&prefix=" + percent_encode(prefix, false);
            std::string body;
            long        code = 0;
            if (!run(q, err, &code, [&](CURL* h) {
                    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, to_string_cb);
                    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
                }))
                return false;
            if (code >= 300) { err = url_ + ": HTTP " + std::to_string(code) + " " + body.substr(0, 300); return false; }
            // Objects right under the prefix, and "directories" below it.
            for (const std::string& k : tags(body, "Key"))
                if (k.size() > prefix.size()) names.push_back(k.substr(prefix.size()));
            size_t at = 0;
            while ((at = body.find("<CommonPrefixes>", at)) != std::string::npos) {
                const size_t end = body.find("</CommonPrefixes>", at);
                for (std::string p : tags(body.substr(at, end - at), "Prefix")) {
                    if (!p.empty() && p.back() == '/') p.pop_back();
                    if (p.size() > prefix.size()) names.push_back(p.substr(prefix.size()));
                }
                at = end;
            }
            const auto more = tags(body, "NextContinuationToken");
            if (body.find("<IsTruncated>true</IsTruncated>") == std::string::npos || more.empty()) return true;
            token = more[0];
        }
    }
    bool remove_generation(const std::string& gen, std::string& err) override {
        std::vector<std::string> files;
        if (!list(gen, files, err)) return false;
        for (const auto& f : files) {
            long code = 0;
            if (!run(object_url(gen + "/" + f), err, &code,
                     [](CURL* h) { curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE"); }))
                return false;
            if (code >= 300) { err = url_ + ": HTTP " + std::to_string(code) + " deleting " + f; return false; }
        }
        return true;
    }

private:
    std::string object_url(const std::string& key) const {
        return bucket_url_ + "/" + percent_encode(prefix_ + key, true);
    }
    void auth(CURL* h) override {
        sigv4_ = "aws:amz:" + region_ + ":s3";
        curl_easy_setopt(h, CURLOPT_AWS_SIGV4, sigv4_.c_str());
        curl_easy_setopt(h, CURLOPT_USERPWD, userpwd_.c_str());
        curl_slist_free_all(headers_);
        headers_ = nullptr;
        if (!token_.empty()) headers_ = curl_slist_append(headers_, token_.c_str());
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers_);
    }
    // ponytail: keys here are hex names under a prefix the user chose, so
    // XML entities in them are not decoded; decode if prefixes with & or <
    // ever need to work.
    static std::vector<std::string> tags(const std::string& xml, const std::string& tag) {
        std::vector<std::string> out;
        const std::string open = "<" + tag + ">", close = "</" + tag + ">";
        size_t at = 0;
        while ((at = xml.find(open, at)) != std::string::npos) {
            const size_t start = at + open.size(), end = xml.find(close, start);
            if (end == std::string::npos) break;
            out.push_back(xml.substr(start, end - start));
            at = end;
        }
        return out;
    }

    std::string bucket_, prefix_, region_, userpwd_, token_, bucket_url_, sigv4_;
    curl_slist* headers_ = nullptr;

public:
    ~S3Target() override { curl_slist_free_all(headers_); }
};

// sftp://user@host[:port]/absolute/path -- another machine, over SSH, with
// the keys ssh itself uses (agent, ~/.ssh/id_*) and its known_hosts.
// Files go over SCP: libcurl's SFTP waits for every write to be acknowledged,
// measured at 0.9 MB/s over a 30ms link where SCP did 5.2 (and downloads 3x
// faster). SFTP is kept for what SCP cannot do: mkdir, list, rm.
class SftpTarget final : public CurlTarget {
public:
    SftpTarget(const std::string& url, std::string& error) : CurlTarget(url) {
        const size_t host_start = 7, path_start = url.find('/', host_start);
        if (path_start == std::string::npos || path_start + 1 >= url.size()) {
            error = "replicate: '" + url + "' needs a path: sftp://user@host/absolute/path";
            return;
        }
        base_ = url;
        while (base_.size() > path_start + 1 && base_.back() == '/') base_.pop_back();
        path_     = base_.substr(path_start);
        host_url_ = base_.substr(0, path_start) + "/";
        scp_base_ = "scp" + base_.substr(4);
        if (path_.rfind("/~", 0) == 0) {
            error = "replicate: '" + url + "': give the absolute path (no ~)";
            return;
        }
    }

    bool put(const std::string& key, FILE* in, uint64_t size, std::string& err) override {
        const std::string dir = key.substr(0, key.find('/'));
        if (dir != made_dir_) {
            // "*": fine if it exists. The root's parent must exist already.
            if (!quote({"*mkdir \"" + path_ + "\"", "*mkdir \"" + path_ + "/" + dir + "\""}, err)) return false;
            made_dir_ = dir;
        }
        return upload(scp_base_ + "/" + key, in, size, err);
    }
    bool get(const std::string& key, const std::string& to_file, std::string& err) override {
        return download(scp_base_ + "/" + key, to_file, err);
    }
    bool list(const std::string& dir, std::vector<std::string>& names, std::string& err) override {
        std::string body;
        if (!run(base_ + "/" + (dir.empty() ? "" : dir + "/"), err, nullptr, [&](CURL* h) {
                curl_easy_setopt(h, CURLOPT_DIRLISTONLY, 1L);
                curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, to_string_cb);
                curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
            }))
            return false;
        size_t at = 0;
        while (at < body.size()) {
            size_t      nl = body.find('\n', at);
            std::string n  = body.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            if (!n.empty() && n.back() == '\r') n.pop_back();
            if (!n.empty() && n != "." && n != "..") names.push_back(n);
            if (nl == std::string::npos) break;
            at = nl + 1;
        }
        return true;
    }
    bool remove_generation(const std::string& gen, std::string& err) override {
        std::vector<std::string> files;
        if (!list(gen, files, err)) return false;
        std::vector<std::string> cmds;
        for (const auto& f : files) cmds.push_back("rm \"" + path_ + "/" + gen + "/" + f + "\"");
        cmds.push_back("rmdir \"" + path_ + "/" + gen + "\"");
        return quote(cmds, err);
    }

private:
    bool quote(const std::vector<std::string>& cmds, std::string& err) {
        curl_slist* list = nullptr;
        for (const auto& c : cmds) list = curl_slist_append(list, c.c_str());
        const bool ok = run(host_url_, err, nullptr, [&](CURL* h) {
            curl_easy_setopt(h, CURLOPT_QUOTE, list);
            curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
        });
        curl_slist_free_all(list);
        return ok;
    }

    void auth(CURL* h) override {
        curl_easy_setopt(h, CURLOPT_SSH_AUTH_TYPES, static_cast<long>(CURLSSH_AUTH_PUBLICKEY | CURLSSH_AUTH_AGENT));
        if (const char* home = std::getenv("HOME")) {
            known_hosts_ = std::string(home) + "/.ssh/known_hosts";
            curl_easy_setopt(h, CURLOPT_SSH_KNOWNHOSTS, known_hosts_.c_str());
        }
    }

    std::string base_, path_, host_url_, scp_base_, known_hosts_, made_dir_;
};

#endif // LUX_HTTP

std::unique_ptr<Target> make_target(const std::string& url, std::string& error) {
    std::unique_ptr<Target> t;
    if (url.rfind("s3://", 0) == 0 || url.rfind("sftp://", 0) == 0) {
#ifdef LUX_HTTP
        if (url[1] == '3') t = std::make_unique<S3Target>(url, error);
        else               t = std::make_unique<SftpTarget>(url, error);
#else
        error = "replicate: '" + url + "' needs libcurl, and this lux was built without it";
#endif
    } else if (url.find("://") != std::string::npos && url.rfind("file://", 0) != 0) {
        error = "replicate: '" + url + "': expected s3://bucket/prefix, "
                "sftp://user@host/path or a directory";
    } else if (url.empty() || url == "file://") {
        error = "replicate: empty target";
    } else {
        t = std::make_unique<DirTarget>(url);
    }
    if (!error.empty()) t.reset();
    return t;
}

// ─── The replicator ──────────────────────────────────────────────────────────

// A file that deletes itself when the last upload holding it is done.
struct SpoolFile {
    std::string path;
    ~SpoolFile() { std::error_code ec; fs::remove(path, ec); }
};

struct Item {
    std::string                        key;
    std::shared_ptr<const std::string> data;       // a segment in memory, or
    std::shared_ptr<SpoolFile>         file;       // one on disk: a snapshot, or a segment queued behind one
    bool                               snapshot = false;
    uint64_t                           bytes = 0;  // of a segment
    uint64_t size() const { return data ? bytes : 0; }         // held in memory
    uint64_t spilled() const { return !data && !snapshot ? bytes : 0; }   // held on disk
};

struct Replica {
    std::unique_ptr<Target> target;
    std::deque<Item>        queue;
    size_t                  pending = 0, spilled = 0;
    bool                    failing = false;
    std::chrono::steady_clock::time_point retry_at{};
};

class Replicator final : public SqliteReplicator {
public:
    Replicator(std::string db, std::vector<std::unique_ptr<Target>> targets, std::function<void(bool)> gate)
        : db_(std::move(db)), wal_(db_ + "-wal"), gate_(std::move(gate)) {
        for (auto& t : targets) replicas_.push_back(Replica{std::move(t), {}, 0, false, {}});
    }

    bool open(std::string& error) {
        for (sqlite3** c : {&lock_, &ckpt_, &pins_[0], &pins_[1]}) {
            if (sqlite3_open_v2(db_.c_str(), c, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
                error = "replicate: cannot open '" + db_ + "': " + sqlite3_errmsg(*c);
                return false;
            }
            sqlite3_wal_autocheckpoint(*c, 0);
            sqlite3_busy_timeout(*c, 5000);   // a background thread: it may wait
            // Also what opens the WAL on this connection: until it has, a
            // checkpoint from it does nothing (and reports no WAL at all).
            char* msg = nullptr;
            sqlite3_exec(*c, "PRAGMA journal_mode=WAL", nullptr, nullptr, &msg);
            if (msg) sqlite3_free(msg);
        }
        // Only ever closed after every connection above: closing any
        // descriptor of the file drops the process's POSIX locks.
        dbfd_    = ::open(db_.c_str(), O_RDONLY | O_CLOEXEC);
        capture_ = std::thread([this] { capture_loop(); });
        upload_  = std::thread([this] { upload_loop(); });
        writeback_ = std::thread([this] { writeback_loop(); });
        return true;
    }

    ~Replicator() override {
        g_cold_hurry = true;
        {
            std::lock_guard<std::mutex> l(m_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (capture_.joinable()) capture_.join();   // its last tick ships what is left
        {
            std::lock_guard<std::mutex> l(m_);
            capture_done_ = true;
        }
        cv_.notify_all();
        if (upload_.joinable()) upload_.join();
        unpin();
        writeback_stop_ = true;
        if (writeback_.joinable()) writeback_.join();
        for (sqlite3* c : {lock_, ckpt_, pins_[0], pins_[1]}) if (c) sqlite3_close(c);
        if (dbfd_ >= 0) ::close(dbfd_);
    }

private:
    // ── capture: the WAL into segments ──

    void capture_loop() {
        pthread_setname_np(pthread_self(), "lux-rcapture");
        std::unique_lock<std::mutex> l(m_);
        for (;;) {
            const bool last = stopping_;
            l.unlock();
            tick();
            l.lock();
            if (last) return;
            cv_.wait_for(l, kCaptureEvery, [&] { return stopping_; });
        }
    }

    void tick() {
        if (need_snapshot_) { flush(); snapshot(); return; }
        uint64_t frames;   // a backlog (a snapshot just ended) goes out a segment at a time
        while ((frames = capture()), seg_frames_.size() >= kFlushBytes) flush();
        if (need_snapshot_) { flush(); snapshot(); return; }
        if (frames >= kCheckpointFrames) checkpoint();
        if (stopping() || seg_frames_.size() >= kFlushBytes ||
            std::chrono::steady_clock::now() - last_flush_ >= kInterval)
            flush();
    }
    bool stopping() { std::lock_guard<std::mutex> l(m_); return stopping_; }

    // The frames captured since the last shipment, as one segment.
    void flush() {
        last_flush_ = std::chrono::steady_clock::now();
        if (gen_.empty()) { seg_frames_.clear(); seg_count_ = 0; }   // see snapshot()'s failure
        if (seg_count_ == 0) return;
        std::string seg(kSegMagic, 8);
        put_be32(seg, seg_page_size_);
        put_be32(seg, seg_count_);
        seg += seg_frames_;
        seg_frames_.clear();
        seg_count_ = 0;
        since_snapshot_ += seg.size();
        Item item{gen_ + "/" + hex16(seq_) + ".wal", nullptr, nullptr, false, seg.size()};
        // Behind a snapshot still uploading (70 s per 7 GB) segments wait on
        // disk, not in memory: a memory cap has the page cache to feed too.
        const std::string path = db_ + "-replica-seg-" + hex16(seq_++);
        if (behind_snapshot() && write_file(path, seg)) {
            item.file = std::make_shared<SpoolFile>();   // not a copy of a temporary: its destructor deletes the file
            item.file->path = path;
        } else item.data = std::make_shared<const std::string>(std::move(seg));
        enqueue(std::move(item));
        if (since_snapshot_ > std::max(kMinGenerationBytes, 4 * snapshot_bytes_)) need_snapshot_ = true;
    }

    // While a checkpoint copies frames into the database, what it dirties is
    // pushed to disk as it goes: after a snapshot the WAL is ~1 GB, and left
    // to the kernel that lands as one burst of writeback that stalled the
    // whole server (reads too) under a memory cap.
    void writeback_loop() {
        pthread_setname_np(pthread_self(), "lux-rwriteback");
        while (!writeback_stop_) {
            if (checkpointing_ && dbfd_ >= 0) ::sync_file_range(dbfd_, 0, 0, SYNC_FILE_RANGE_WRITE);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    bool behind_snapshot() {
        std::lock_guard<std::mutex> l(m_);
        for (auto& r : replicas_) if (!r.queue.empty() && r.queue.front().snapshot) return true;
        return false;
    }
    static bool write_file(const std::string& path, const std::string& d) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return false;
        bool ok = true;
        {
            ColdIo io(fd, true);   // out of the page cache, like the snapshot
            for (size_t at = 0; ok && at < d.size(); at += kColdSlice) {
                const size_t n = std::min<size_t>(kColdSlice, d.size() - at);
                ok = ::write(fd, d.data() + at, n) == static_cast<ssize_t>(n);
                io.advance(n);
            }
        }
        return ::close(fd) == 0 && ok;
    }

    // New committed frames, kept for the next flush(). Returns how many
    // frames the WAL holds past its last reset (the checkpoint's trigger).
    uint64_t capture() {
        const int fd = ::open(wal_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        WalHeader h;
        if (!read_wal_header(fd, h)) { ::close(fd); return 0; }
        if (!have_header_ || h.salt1 != header_.salt1 || h.salt2 != header_.salt2) {
            if (!expect_reset_) {
                lux::log().warn("replicate: the WAL of ", db_, " was reset by someone else; "
                                "starting a new generation");
                need_snapshot_ = true;
                ::close(fd);
                return 0;
            }
            header_ = h; have_header_ = true; expect_reset_ = false;
            pos_ = {0, h.s1, h.s2};
        }
        std::string  frames;
        const WalPos next = scan_wal(fd, header_, pos_, &frames);
        ::close(fd);
        if (next.frame > pos_.frame) {
            if (seg_count_ && seg_page_size_ != header_.page_size) flush();   // a segment has one page size
            seg_page_size_ = header_.page_size;
            seg_frames_ += frames;
            seg_count_ += static_cast<uint32_t>(next.frame - pos_.frame);
            pos_ = next;
        }
        return pos_.frame;
    }

    // Copies every frame into the database file, holding the write lock
    // after shipping them all -- so the reset this allows only drops
    // shipped frames. With writers held off, readers move to the newest
    // snapshot in microseconds, which is what lets PASSIVE finish.
    bool checkpoint_all(std::chrono::milliseconds budget) {
        const auto until = std::chrono::steady_clock::now() + budget;
        for (;;) {
            int log = -1, done = -1;
            sqlite3_wal_checkpoint_v2(ckpt_, nullptr, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
            if (log >= 0 && log == done) return true;
            if (std::chrono::steady_clock::now() >= until) return false;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    bool lock() {
        gate_(true);
        if (sqlite3_exec(lock_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK) return true;
        gate_(false);
        return false;
    }
    void unlock() {
        sqlite3_exec(lock_, "ROLLBACK", nullptr, nullptr, nullptr);
        gate_(false);
    }
    // Pins the newest snapshot. Two connections take turns: the new pin is
    // in place before the old one goes, so the WAL is never left unpinned.
    // Safe at any time, shipped or not: what the pin guards against is a
    // reset, and any pin blocks that (a reader on a checkpointed WAL blocks
    // checkpoints outright; one on frames blocks the reset); a checkpoint
    // copying frames not yet shipped loses nothing, they stay in the WAL.
    void pin() {
        const int next = pinned_ == 0 ? 1 : 0;
        sqlite3_exec(pins_[next], "BEGIN; SELECT 1 FROM sqlite_master LIMIT 1", nullptr, nullptr, nullptr);
        unpin();
        pinned_ = next;
    }
    void unpin() {
        if (pinned_ >= 0) sqlite3_exec(pins_[pinned_], "COMMIT", nullptr, nullptr, nullptr);
        pinned_ = -1;
    }

    // As the driver's checkpointer: the bulk is copied and fsynced (16-25ms
    // on the bench's disk) while the app writes on, up to the refreshed pin;
    // the writer is held only for the rest, a few pages.
    void checkpoint() {
        checkpointing_ = true;
        struct Off { std::atomic<bool>& f; ~Off() { f = false; } } off{checkpointing_};
        pin();
        for (int i = 0; i < 4; ++i) {
            int log = -1, done = -1;
            sqlite3_wal_checkpoint_v2(ckpt_, nullptr, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
            if (log < 0 || log - done < 100) break;
            pin();   // the next round may copy what came in during this one
        }
        if (!lock()) return;
        capture();
        if (!need_snapshot_) {
            unpin();
            expect_reset_ = checkpoint_all(std::chrono::milliseconds(20));
            pin();
        }
        unlock();
    }

    // A new generation: the whole database, then segments from where it
    // stands. Writes are held only while a read transaction opens on ckpt_
    // and the WAL position is taken, so the two agree exactly; the copy
    // itself then reads that transaction's snapshot while the app writes on
    // (holding writes for the whole copy stalled them ~50ms per 65 MB,
    // every couple of seconds under the forum bench).
    //
    // Copied through SQLite (the backup API, page for page), never by
    // opening the file: POSIX locks belong to the process, and close() on
    // ANY descriptor of the file drops all of them -- every connection's.
    // Another process then believes it is alone, and deletes the -shm under
    // us (measured: the app's next read failed with "disk I/O error").
    void snapshot() {
        if (!lock()) return;
        sqlite3_exec(ckpt_, "BEGIN; SELECT 1 FROM sqlite_master LIMIT 1", nullptr, nullptr, nullptr);
        // Everything in the WAL is in that snapshot: continue after its last commit.
        have_header_ = false;
        if (const int fd = ::open(wal_.c_str(), O_RDONLY | O_CLOEXEC); fd >= 0) {
            WalHeader h;
            if (read_wal_header(fd, h)) {
                header_ = h; have_header_ = true;
                pos_ = scan_wal(fd, h, {0, h.s1, h.s2}, nullptr);
            }
            ::close(fd);
        }
        expect_reset_ = true;
        pin();
        unlock();

        const std::string gen = hex16(now_ms());
        auto spool = std::make_shared<SpoolFile>(SpoolFile{db_ + "-replica-" + gen});
        std::error_code ec;
        fs::remove(spool->path, ec);
        sqlite3*    dest = nullptr;
        std::string err;
        if (sqlite3_open_v2(spool->path.c_str(), &dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK) {
            // A scratch copy: no journal, no syncs (ColdIo flushes it).
            sqlite3_exec(dest, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF", nullptr, nullptr, nullptr);
            // A backup whose source has a read transaction open copies
            // from that transaction's snapshot -- slice after slice, since
            // that transaction stays open between steps.
            int page_size = 4096;
            if (sqlite3_stmt* st = nullptr;
                sqlite3_prepare_v2(ckpt_, "PRAGMA page_size", -1, &st, nullptr) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW) page_size = sqlite3_column_int(st, 0);
                sqlite3_finalize(st);
            }
            const int slice = static_cast<int>(kColdSlice / static_cast<uint64_t>(std::max(page_size, 512)));
            const int fd = ::open(spool->path.c_str(), O_RDONLY | O_CLOEXEC);
            sqlite3_backup* b = sqlite3_backup_init(dest, "main", ckpt_, "main");
            if (!b) err = sqlite3_errmsg(dest);
            {
                ColdIo io(fd, true);
                for (int rc = SQLITE_OK; b && rc != SQLITE_DONE; io.advance(uint64_t(slice) * page_size)) {
                    rc = sqlite3_backup_step(b, slice);
                    if (rc != SQLITE_OK && rc != SQLITE_DONE) { err = sqlite3_errmsg(dest); break; }
                }
            }
            if (b && sqlite3_backup_finish(b) != SQLITE_OK && err.empty()) err = sqlite3_errmsg(dest);
            sqlite3_close(dest);
            dest = nullptr;
            if (fd >= 0) ::close(fd);   // after dest: closing it earlier dropped dest's locks
        } else {
            err = sqlite3_errmsg(dest);
        }
        sqlite3_close(dest);
        sqlite3_exec(ckpt_, "COMMIT", nullptr, nullptr, nullptr);
        if (!err.empty()) {
            lux::log().error("replicate: cannot snapshot ", db_, ": ", err);
            // The position moved past the previous generation's last
            // segment: what is captured until the next snapshot belongs to
            // no generation, and flush() drops it instead of leaving that
            // one with a hole.
            gen_.clear();
            need_snapshot_ = true;
            return;
        }

        gen_ = gen; seq_ = 0; since_snapshot_ = 0; need_snapshot_ = false;
        snapshot_bytes_ = fs::file_size(spool->path, ec);
        enqueue(Item{gen_ + "/snapshot.db", nullptr, spool, true, 0});
    }

    // ── upload: segments to every target, in order ──

    void enqueue(Item item) {
        {
            std::lock_guard<std::mutex> l(m_);
            for (auto& r : replicas_) {
                // Segments behind a snapshot still uploading wait on disk
                // (flush()) and count apart: else a busy database drops the
                // snapshot it is sending, takes a new one, and never
                // finishes either.
                if (r.pending > kMaxPendingBytes || r.spilled > 8 * kMaxPendingBytes) {
                    // ponytail: in memory; a target down this long loses its
                    // queue and gets a new generation once it is back.
                    lux::log().error("replicate: ", r.target->url(), " is too far behind; "
                                     "it will get a new generation");
                    r.queue.clear();
                    r.pending = r.spilled = 0;
                    if (!item.snapshot) need_snapshot_ = true;   // unless this is that new snapshot
                }
                r.pending += item.size();
                r.spilled += item.spilled();
                r.queue.push_back(item);
            }
        }
        cv_.notify_all();
    }

    void upload_loop() {
        pthread_setname_np(pthread_self(), "lux-rupload");
        std::unique_lock<std::mutex> l(m_);
        for (;;) {
            bool all_empty = true, any_ready = false;
            const auto now = std::chrono::steady_clock::now();
            for (auto& r : replicas_) {
                if (r.queue.empty()) continue;
                all_empty = false;
                if (now >= r.retry_at || capture_done_) any_ready = true;
            }
            if (capture_done_ && (all_empty || final_attempts_ >= 3)) return;
            if (!any_ready) { cv_.wait_for(l, std::chrono::seconds(1)); continue; }
            if (capture_done_) ++final_attempts_;

            for (auto& r : replicas_) {
                if (r.queue.empty() || (now < r.retry_at && !capture_done_)) continue;
                Item item = r.queue.front();
                l.unlock();
                std::string err;
                const bool  ok = put(*r.target, item, err);
                if (ok && item.snapshot) prune(*r.target, item.key.substr(0, 16));
                l.lock();
                if (ok) {
                    if (!r.queue.empty() && r.queue.front().key == item.key) {
                        r.pending -= std::min(r.pending, size_t(item.size()));
                        r.spilled -= std::min(r.spilled, size_t(item.spilled()));
                        r.queue.pop_front();
                    }
                    if (r.failing) lux::log().info("replicate: ", r.target->url(), " is back");
                    r.failing = false;
                } else {
                    if (!r.failing) lux::log().error("replicate: ", err, " (retrying)");
                    r.failing  = true;
                    r.retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                }
            }
        }
    }

    static bool put(Target& t, const Item& item, std::string& err) {
        FILE*    in   = nullptr;
        uint64_t size = 0;
        if (item.data) {
            size = item.data->size();
            in   = fmemopen(const_cast<char*>(item.data->data()), size, "rb");
        } else {
            std::error_code ec;
            size = fs::file_size(item.file->path, ec);
            in   = std::fopen(item.file->path.c_str(), "rb");
        }
        if (!in) { err = std::string("replicate: ") + std::strerror(errno); return false; }
        const bool ok = t.put(item.key, in, size, err);
        std::fclose(in);
        return ok;
    }

    // With a new generation's snapshot in place, keep it and the one before.
    static void prune(Target& t, const std::string& current) {
        std::vector<std::string> gens;
        std::string              err;
        if (!t.list("", gens, err)) return;
        gens.erase(std::remove_if(gens.begin(), gens.end(),
                                  [&](const std::string& g) { return !is_hex16(g) || g > current; }),
                   gens.end());
        std::sort(gens.begin(), gens.end());
        for (size_t i = 0; i + 2 < gens.size(); ++i)
            if (!t.remove_generation(gens[i], err))
                lux::log().warn("replicate: cannot remove old generation ", gens[i], ": ", err);
    }

    std::string db_, wal_;
    std::function<void(bool)> gate_;
    sqlite3*    lock_ = nullptr;   // holds the write lock while checkpointing
    sqlite3*    ckpt_ = nullptr;   // checkpoints
    sqlite3*    pins_[2] = {nullptr, nullptr};   // the read transaction that holds off everyone else's
    int         pinned_  = -1;                   // which of pins_ holds it

    // capture thread only
    WalHeader header_;
    bool      have_header_ = false, expect_reset_ = false;
    WalPos    pos_;
    std::string gen_;
    uint64_t  seq_ = 0, since_snapshot_ = 0, snapshot_bytes_ = 0;
    std::string seg_frames_;
    uint32_t    seg_count_ = 0, seg_page_size_ = 0;
    std::chrono::steady_clock::time_point last_flush_{};
    std::atomic<bool> need_snapshot_{true};

    std::mutex              m_;
    std::condition_variable cv_;
    std::vector<Replica>    replicas_;
    bool                    stopping_ = false, capture_done_ = false;
    int                     final_attempts_ = 0;
    std::thread             capture_, upload_, writeback_;
    std::atomic<bool>       checkpointing_{false}, writeback_stop_{false};
    int                     dbfd_ = -1;
};

} // namespace

bool SqliteReplicator::validate(const std::vector<std::string>& targets, std::string& error) {
    for (const auto& url : targets)
        if (!make_target(url, error)) return false;
    return true;
}

std::unique_ptr<SqliteReplicator> SqliteReplicator::start(const std::string& db_file,
                                                          const std::vector<std::string>& targets,
                                                          std::function<void(bool)> gate,
                                                          std::string& error) {
    std::vector<std::unique_ptr<Target>> ts;
    for (const auto& url : targets) {
        auto t = make_target(url, error);
        if (!t) return nullptr;
        ts.push_back(std::move(t));
    }
    auto r = std::make_unique<Replicator>(db_file, std::move(ts), std::move(gate));
    if (!r->open(error)) return nullptr;
    return r;
}

// ─── lux restore ─────────────────────────────────────────────────────────────

namespace {

bool apply_segment(int fd, const std::string& seg, std::string& err) {
    uint32_t ps = 0, n = 0;
    if (!segment_ok(seg, ps, n)) { err = "incomplete"; return false; }
    const auto* p = reinterpret_cast<const unsigned char*>(seg.data()) + kSegHeader;
    for (uint32_t i = 0; i < n; ++i, p += kFrameHeader + ps) {
        const uint32_t pgno = be32(p), commit_size = be32(p + 4);
        if (pgno == 0) { err = "bad frame"; return false; }
        if (pwrite(fd, p + kFrameHeader, ps, off_t(pgno - 1) * ps) != ssize_t(ps)) {
            err = std::strerror(errno);
            return false;
        }
        if (commit_size && ftruncate(fd, off_t(commit_size) * ps) != 0) { err = std::strerror(errno); return false; }
    }
    return true;
}

} // namespace

int sqlite_restore_main(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cerr << "usage: lux restore <replica> <out.db>\n"
                     "  <replica>: s3://bucket/prefix, sftp://user@host/path or a directory,\n"
                     "  as given to `replicate` in the sqlite: block\n";
        return 2;
    }
    const std::string& url = args[0];
    const std::string& out = args[1];
    std::error_code    ec;
    if (fs::exists(out, ec)) { std::cerr << out << " already exists; restore into a new file\n"; return 1; }

    std::string err;
    auto        target = make_target(url, err);
    if (!target) { std::cerr << err << "\n"; return 1; }

    std::vector<std::string> gens;
    if (!target->list("", gens, err)) { std::cerr << err << "\n"; return 1; }
    gens.erase(std::remove_if(gens.begin(), gens.end(), [](const std::string& g) { return !is_hex16(g); }),
               gens.end());
    std::sort(gens.rbegin(), gens.rend());

    const std::string tmp = out + ".restoring";
    for (const auto& gen : gens) {
        std::vector<std::string> files;
        if (!target->list(gen, files, err)) { std::cerr << err << "\n"; return 1; }
        if (std::find(files.begin(), files.end(), "snapshot.db") == files.end()) continue;
        if (!target->get(gen + "/snapshot.db", tmp, err) || !snapshot_ok(tmp)) {
            std::cerr << "generation " << gen << ": snapshot unusable" << (err.empty() ? "" : ": " + err)
                      << "; trying an older one\n";
            err.clear();
            continue;
        }

        std::vector<std::string> segs;
        for (const auto& f : files)
            if (f.size() == 20 && f.compare(16, 4, ".wal") == 0 && is_hex16(f.substr(0, 16))) segs.push_back(f);
        std::sort(segs.begin(), segs.end());

        const int fd = ::open(tmp.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) { std::cerr << tmp << ": " << std::strerror(errno) << "\n"; return 1; }
        size_t applied = 0;
        for (const auto& s : segs) {
            if (s.substr(0, 16) != hex16(applied)) break;   // a gap: stop at the last whole point
            const std::string seg_file = tmp + ".seg";
            std::string       data;
            if (target->get(gen + "/" + s, seg_file, err)) {
                std::ifstream in(seg_file, std::ios::binary);
                data.assign(std::istreambuf_iterator<char>(in), {});
            }
            fs::remove(seg_file, ec);
            if (!err.empty() || !apply_segment(fd, data, err)) {
                std::cerr << "segment " << s << ": " << err << "; stopping at the one before\n";
                break;
            }
            ++applied;
        }
        fsync(fd);
        ::close(fd);

        sqlite3* db = nullptr;
        std::string check = "not run";
        if (sqlite3_open_v2(tmp.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, nullptr) == SQLITE_OK &&
                sqlite3_step(st) == SQLITE_ROW)
                check = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
        for (const char* suffix : {"-wal", "-shm"}) fs::remove(tmp + suffix, ec);
        if (check != "ok") {
            std::cerr << "integrity_check: " << check << "\n";
            fs::remove(tmp, ec);
            return 1;
        }
        fs::rename(tmp, out, ec);
        if (ec) { std::cerr << out << ": " << ec.message() << "\n"; return 1; }
        std::cout << "restored " << out << " from generation " << gen << " (snapshot + " << applied
                  << " of " << segs.size() << " segments), integrity ok\n";
        return 0;
    }
    std::cerr << "no usable generation at " << url << "\n";
    return 1;
}

} // namespace lux_script
