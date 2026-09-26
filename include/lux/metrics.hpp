#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <chrono>
#include <string>
#include <sstream>
#include <iomanip>

// ── Metrics ────────────────────────────────────────────────────────────────────
//
// Process-wide singleton that tracks basic server counters.
// Written from HttpConnection::finish_dispatch (one call per request),
// read from the /metrics route. All fields are atomics — lock-free.
//
// Wired up by App::run():
//   Metrics::instance().active_connections_ = shared_conn_count.get();
//
// Usage:
//   app.enable_health();    // GET /health  → {"status":"ok","uptime":…}
//   app.enable_metrics();   // GET /metrics → Prometheus text format

namespace lux {

class Metrics {
public:
    static Metrics& instance() noexcept {
        static Metrics m;
        return m;
    }

    // Called from finish_dispatch() for every completed HTTP request.
    void record(int status) noexcept {
        Counters& c = mine();
        bump(c.total);
        if      (status >= 500) bump(c.r5xx);
        else if (status >= 400) bump(c.r4xx);
        else if (status >= 200) bump(c.r2xx);
    }

    // Prometheus exposition format (text/plain; version=0.0.4).
    std::string to_prometheus() const {
        using Clock = std::chrono::steady_clock;
        double uptime = std::chrono::duration<double>(Clock::now() - started_at_).count();
        int    conns  = active_connections_
                        ? active_connections_->load(std::memory_order_relaxed)
                        : 0;

        uint64_t total = sum(&Counters::total);
        uint64_t r2xx  = sum(&Counters::r2xx);
        uint64_t r4xx  = sum(&Counters::r4xx);
        uint64_t r5xx  = sum(&Counters::r5xx);

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3);

        ss << "# HELP lux_requests_total Total HTTP requests handled\n"
              "# TYPE lux_requests_total counter\n"
           << "lux_requests_total " << total << "\n\n"

              "# HELP lux_requests_by_class HTTP requests grouped by status class\n"
              "# TYPE lux_requests_by_class counter\n"
           << "lux_requests_by_class{class=\"2xx\"} " << r2xx << "\n"
           << "lux_requests_by_class{class=\"4xx\"} " << r4xx << "\n"
           << "lux_requests_by_class{class=\"5xx\"} " << r5xx << "\n\n"

              "# HELP lux_active_connections Currently open TCP connections\n"
              "# TYPE lux_active_connections gauge\n"
           << "lux_active_connections " << conns << "\n\n"

              "# HELP lux_uptime_seconds Seconds since server start\n"
              "# TYPE lux_uptime_seconds gauge\n"
           << "lux_uptime_seconds " << uptime << "\n";

        return ss.str();
    }

    // Summary of /health, already as text.  It is four fields of known types:
    // building a JSON tree for this was going through a middleman that
    // contributed nothing.
    std::string to_health_text() const {
        using Clock = std::chrono::steady_clock;
        double uptime = std::chrono::duration<double>(Clock::now() - started_at_).count();
        int    conns  = active_connections_
                        ? active_connections_->load(std::memory_order_relaxed)
                        : 0;
        std::ostringstream ss;
        ss << "{\"status\":\"ok\",\"uptime_seconds\":" << uptime
           << ",\"active_connections\":" << conns
           << ",\"requests_total\":" << sum(&Counters::total)
           << "}";
        return ss.str();
    }

    // ── Framework-internal ──────────────────────────────────────────────────────
    // Pointer to the shared conn counter owned by App::run().
    // Written once before threads start; never written again until run() returns.
    std::atomic<int>* active_connections_ = nullptr;

private:
    Metrics() = default;

    // One set per thread, summed when read: a single set written by every
    // event loop on every request kept its cache line bouncing between cores.
    // Only the owning thread writes, so a plain load+store is enough.
    struct Counters { std::atomic<uint64_t> total{0}, r2xx{0}, r4xx{0}, r5xx{0}; };
    static void bump(std::atomic<uint64_t>& a) noexcept {
        a.store(a.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    Counters& mine() {
        thread_local Counters* c = [this] {
            std::lock_guard<std::mutex> lk(all_mutex_);
            return all_.emplace_back(std::make_unique<Counters>()).get();   // kept after the thread ends
        }();
        return *c;
    }
    uint64_t sum(std::atomic<uint64_t> Counters::* f) const {
        std::lock_guard<std::mutex> lk(all_mutex_);
        uint64_t n = 0;
        for (const auto& c : all_) n += ((*c).*f).load(std::memory_order_relaxed);
        return n;
    }
    mutable std::mutex                     all_mutex_;
    std::vector<std::unique_ptr<Counters>> all_;
    std::chrono::steady_clock::time_point  started_at_ = std::chrono::steady_clock::now();
};

} // namespace lux
