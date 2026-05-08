#pragma once
// lat_stats.hpp — Latency histogram for per-device processing-time tracking.
//
// This header has NO libevdev dependency so it can be included in unit tests
// without linking against the evdev/uinput libraries.
//
// Thread safety: flush_motion() (loop thread) calls record(); dump_latency_stats()
// (main thread) calls snapshot_and_reset(). mtx serialises all field access.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace rawaccel {

struct lat_stats {
    // 1000 buckets × 0.5 µs = 0–500 µs range.  Samples above the range go to 'over'.
    static constexpr int    BUCKETS    = 1000;
    static constexpr double BUCKET_US  = 0.5;         // µs per bucket
    static constexpr double RANGE_US   = BUCKETS * BUCKET_US; // 500 µs

    mutable std::mutex mtx;     // guards all fields below

    uint64_t count   = 0;
    double   sum_us  = 0;
    double   min_us  = 1e9;
    double   max_us  = 0;
    uint64_t hist[BUCKETS] = {};
    uint64_t over    = 0;       // samples > RANGE_US

    // std::mutex is neither copyable nor movable; provide explicit move constructor
    // so mouse_device (which contains lat_stats) can be moved into a vector.
    // The mutex is NOT transferred — the new object gets a fresh (unlocked) mutex.
    lat_stats() = default;
    lat_stats(lat_stats&& o) noexcept {
        // R13: lock source before reading — another thread may be calling record().
        // In practice moves only happen under devices_mutex_, but defend in depth.
        std::lock_guard<std::mutex> lk(o.mtx);
        count = o.count; sum_us = o.sum_us;
        min_us = o.min_us; max_us = o.max_us; over = o.over;
        std::copy(o.hist, o.hist + BUCKETS, hist);
        o.count = 0; o.sum_us = 0; o.min_us = 1e9; o.max_us = 0;
        std::fill(o.hist, o.hist + BUCKETS, 0ULL);
        o.over = 0;
    }
    lat_stats& operator=(lat_stats&& o) noexcept {
        if (this != &o) {
            // R13: lock both — prevent concurrent record() during move.
            // Always lock in address order to avoid deadlock.
            std::mutex* first  = &mtx < &o.mtx ? &mtx : &o.mtx;
            std::mutex* second = &mtx < &o.mtx ? &o.mtx : &mtx;
            std::lock_guard<std::mutex> lk1(*first);
            std::lock_guard<std::mutex> lk2(*second);
            count = o.count; sum_us = o.sum_us;
            min_us = o.min_us; max_us = o.max_us; over = o.over;
            std::copy(o.hist, o.hist + BUCKETS, hist);
            o.count = 0; o.sum_us = 0; o.min_us = 1e9; o.max_us = 0;
            std::fill(o.hist, o.hist + BUCKETS, 0ULL);
            o.over = 0;
        }
        return *this;
    }
    lat_stats(const lat_stats&)            = delete;
    lat_stats& operator=(const lat_stats&) = delete;

    // ── Write path (loop thread, called from flush_motion) ──────────────────

    void record(double lat_us) {
        std::lock_guard<std::mutex> lk(mtx);
        count++;
        sum_us += lat_us;
        if (lat_us < min_us) min_us = lat_us;
        if (lat_us > max_us) max_us = lat_us;
        if (lat_us < RANGE_US) {
            int b = static_cast<int>(lat_us / BUCKET_US);
            if (b < 0) b = 0;
            hist[b]++;
        } else {
            over++;
        }
    }

    // ── Read path (caller must hold mtx, or use snapshot_and_reset) ─────────

    double avg_us() const {
        return count > 0 ? sum_us / static_cast<double>(count) : 0.0;
    }

    /// Compute a percentile (0–100) from the histogram.  O(BUCKETS).
    /// Caller must hold mtx.
    double percentile(double pct) const {
        if (count == 0) return 0.0;
        uint64_t target = static_cast<uint64_t>(std::ceil(static_cast<double>(count) * pct / 100.0));
        uint64_t cum = 0;
        for (int i = 0; i < BUCKETS; i++) {
            cum += hist[i];
            if (cum >= target)
                return (i + 1) * BUCKET_US; // upper edge of bucket
        }
        return max_us; // all overflow
    }

    // ── Snapshot (read + reset atomically, caller must NOT hold mtx) ────────

    struct snapshot {
        uint64_t count;
        double   sum_us, min_us, max_us;
        uint64_t hist[BUCKETS];
        uint64_t over;

        double avg_us() const { return count > 0 ? sum_us / static_cast<double>(count) : 0.0; }

        double percentile(double pct) const {
            if (count == 0) return 0.0;
            uint64_t target = static_cast<uint64_t>(std::ceil(static_cast<double>(count) * pct / 100.0));
            uint64_t cum = 0;
            for (int i = 0; i < BUCKETS; i++) {
                cum += hist[i];
                if (cum >= target) return (i + 1) * BUCKET_US;
            }
            return max_us;
        }
    };

    /// Atomically copies current stats into a snapshot and resets all counters.
    snapshot snapshot_and_reset() {
        std::lock_guard<std::mutex> lk(mtx);
        snapshot s;
        s.count  = count;
        s.sum_us = sum_us;
        s.min_us = min_us;
        s.max_us = max_us;
        std::copy(hist, hist + BUCKETS, s.hist);
        s.over   = over;
        // Reset
        count = 0; sum_us = 0; min_us = 1e9; max_us = 0;
        std::fill(hist, hist + BUCKETS, 0ULL);
        over = 0;
        return s;
    }
};

} // namespace rawaccel
