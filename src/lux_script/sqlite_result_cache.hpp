#pragma once
// A cache for the rows of SELECTs, valid while the tables they read have not been written.
//
// How a stale row is ruled out:
//   * every table has a generation, bumped AFTER each commit that touched it (the driver feeds it from
//     sqlite3_update_hook, which names the tables, and sqlite3_wal_hook, which runs once the commit is
//     visible); DDL bumps an epoch that invalidates everything;
//   * a result is stored with the generations read BEFORE the statement ran, and is served only while all of
//     them are unchanged: a commit racing with the read can only make the entry look older than it is;
//   * a hard time limit bounds the staleness caused by what the hooks cannot see (another process writing the file).
//
// A hit hands out a fresh copy of the rows (lists and dictionaries are mutable in the VM, so the stored ones
// must never be shared); the strings inside are immutable boxes and stay shared.
#include <lux_script/value.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lux_script {

class TableGens {
public:
    static constexpr int kMax = 256;

    // The id of a table (case-insensitive), -1 when there are too many to track.
    int id(std::string_view name) {
        std::string key(name);
        for (char& c : key) if (c >= 'A' && c <= 'Z') c = char(c + 32);
        std::lock_guard<std::mutex> l(m_);
        if (auto it = ids_.find(key); it != ids_.end()) return it->second;
        if (next_ >= kMax) return -1;
        return ids_.emplace(std::move(key), next_++).first->second;
    }
    uint64_t gen(int id) const { return gens_[size_t(id)].load(std::memory_order_acquire); }
    void     bump(int id) { gens_[size_t(id)].fetch_add(1, std::memory_order_acq_rel); }
    uint64_t epoch() const { return epoch_.load(std::memory_order_acquire); }
    void     bump_all() { epoch_.fetch_add(1, std::memory_order_acq_rel); }

private:
    std::mutex                           m_;
    std::unordered_map<std::string, int> ids_;
    int                                  next_ = 0;
    std::array<std::atomic<uint64_t>, kMax> gens_{};
    std::atomic<uint64_t>                epoch_{0};
};

class ResultCache {
public:
    struct Tag { int table; uint64_t gen; };

    struct Config {
        bool          enabled = true;
        int64_t       ttl_ms = 1000;
        size_t        max_bytes = 64u << 20;   // all shards together
        size_t        max_entry_bytes = 1u << 20;
    };

    explicit ResultCache(Config c) : cfg_(c) {}
    ~ResultCache() = default;

    const Config& config() const { return cfg_; }
    TableGens&    gens() { return gens_; }

    // The key of (sql, args): false when an argument cannot be part of it (a list, a long string).
    static bool make_key(const std::string& sql, const std::vector<Value>& args, std::string& key) {
        key.assign(sql);
        for (const Value& v : args) {
            key += '\x1f';
            if (v.is_null()) key += 'n';
            else if (v.is_bool()) { key += 'b'; key += v.as_bool() ? '1' : '0'; }
            else if (v.is_int()) { key += 'i'; const long long i = v.as_int(); key.append(reinterpret_cast<const char*>(&i), sizeof i); }
            else if (v.is_float()) { key += 'f'; const double d = v.as_float(); key.append(reinterpret_cast<const char*>(&d), sizeof d); }
            else if (v.is_str()) {
                const std::string& s = v.as_str();
                if (s.size() > 256) return false;
                key += 's';
                const uint32_t n = uint32_t(s.size());
                key.append(reinterpret_cast<const char*>(&n), sizeof n);
                key += s;
            } else return false;
        }
        return true;
    }

    // A fresh copy of the rows when the entry is still valid.
    bool get(const std::string& key, Value& out) {
        Shard& s = shard(key);
        std::lock_guard<std::mutex> l(s.m);
        auto it = s.map.find(key);
        if (it == s.map.end()) { misses_.fetch_add(1, std::memory_order_relaxed); return false; }
        const Entry& e = it->second;
        bool ok = e.epoch == gens_.epoch() && now_ns() < e.expires_ns;
        for (size_t i = 0; ok && i < e.tags.size(); ++i) ok = gens_.gen(e.tags[i].table) == e.tags[i].gen;
        if (!ok) {
            s.bytes -= e.bytes;
            s.map.erase(it);
            stale_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        out = copy_rows(e.rows);
        hits_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void put(std::string key, std::vector<Tag> tags, uint64_t epoch, const Value& rows) {
        const size_t bytes = estimate(rows) + key.size() + 96;
        if (bytes > cfg_.max_entry_bytes) return;
        Entry e{std::move(tags), epoch, copy_rows(rows), now_ns() + cfg_.ttl_ms * 1'000'000, bytes};
        Shard& s = shard(key);
        std::lock_guard<std::mutex> l(s.m);
        if (s.bytes + bytes > cfg_.max_bytes / kShards) { s.map.clear(); s.bytes = 0; }   // full: start the shard over
        auto [it, fresh] = s.map.try_emplace(std::move(key));
        if (!fresh) s.bytes -= it->second.bytes;
        it->second = std::move(e);
        s.bytes += bytes;
        stores_.fetch_add(1, std::memory_order_relaxed);
    }

    // Admission: a result is stored the second time the same key misses, not the first. Most keys of a busy site are
    // asked for once (a deep page, a rare search) and storing them would be a copy for nothing. A direct-mapped table
    // of key hashes: no lock, and a collision only delays an admission.
    bool admit(const std::string& key) {
        const uint64_t h = std::hash<std::string>{}(key) | 1;   // never 0
        std::atomic<uint64_t>& slot = seen_[h >> 8 & (kSeen - 1)];
        if (slot.load(std::memory_order_relaxed) == h) return true;
        slot.store(h, std::memory_order_relaxed);
        return false;
    }

    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    uint64_t stale() const { return stale_; }
    uint64_t stores() const { return stores_; }

    // Rows are lists of dictionaries of scalars and strings: new boxes for the containers, the same string boxes.
    static Value copy_rows(const Value& rows) {
        if (!rows.is_list()) return rows;
        const auto& src = rows.as_list();
        Value::List out;
        out.reserve(src.size());
        for (const Value& r : src) out.push_back(r.is_dict() ? Value::dict(Value::Dict(r.as_dict())) : r);
        return Value::list(std::move(out));
    }

private:
    static constexpr size_t kShards = 32;
    static constexpr size_t kSeen = 1u << 16;
    struct Entry {
        std::vector<Tag> tags;
        uint64_t         epoch;
        Value            rows;
        int64_t          expires_ns;
        size_t           bytes;
    };
    struct Shard {
        std::mutex                           m;
        std::unordered_map<std::string, Entry> map;
        size_t                               bytes = 0;
    };

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static size_t estimate(const Value& rows) {
        if (!rows.is_list()) return 64;
        size_t n = 64;
        for (const Value& r : rows.as_list()) {
            n += 80;
            if (!r.is_dict()) continue;
            for (const auto& p : r.as_dict()) {
                n += 48 + p.first.size();
                if (p.second.is_str()) n += 32 + p.second.as_str().size();
            }
        }
        return n;
    }
    Shard& shard(const std::string& key) { return shards_[std::hash<std::string>{}(key) % kShards]; }

    Config                       cfg_;
    TableGens                    gens_;
    std::array<Shard, kShards>   shards_;
    std::atomic<uint64_t>        hits_{0}, misses_{0}, stale_{0}, stores_{0};
    std::array<std::atomic<uint64_t>, kSeen> seen_{};
};

}  // namespace lux_script
