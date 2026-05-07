#pragma once
#include "rawaccel-base.hpp"
#include <cmath>

namespace rawaccel {

/// Lookup table acceleration. User provides (speed, gain) point pairs.
/// Linear interpolation between points.
struct lookup {
    // Points stored as pairs: [x0, y0, x1, y1, ...]
    // x = input speed, y = gain multiplier
    int    point_count = 0;

    lookup() = default;

    lookup(const accel_args& args) {
        // O6: if length is odd (incomplete point pair), count only complete pairs.
        // length=1 → 0 pairs → behaves like noaccel; silent data loss prevented.
        // Guard: point_count must not exceed LUT_POINTS_CAPACITY (buffer boundary).
        int pairs = (args.length >= 2) ? (args.length / 2) : 0;
        point_count = (pairs <= (int)LUT_POINTS_CAPACITY) ? pairs : (int)LUT_POINTS_CAPACITY;
    }

    double operator()(double speed, const accel_args& args) const {
        if (point_count <= 0) return 1.0;

        const float* pts = args.data;
        const size_t n = static_cast<size_t>(point_count);

        // Cast to double explicitly to avoid -Wdouble-promotion warnings
        // Below first point
        if (speed <= static_cast<double>(pts[0])) return static_cast<double>(pts[1]);
        // Above last point
        if (speed >= static_cast<double>(pts[(n - 1) * 2]))
            return static_cast<double>(pts[(n - 1) * 2 + 1]);

        // Binary search for segment
        size_t lo = 0, hi = n - 1;
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (static_cast<double>(pts[mid * 2]) <= speed) lo = mid;
            else                                             hi = mid;
        }

        double x0 = pts[lo * 2],       y0 = pts[lo * 2 + 1];
        double x1 = pts[(lo + 1) * 2], y1 = pts[(lo + 1) * 2 + 1];

        double dx = x1 - x0;
        if (dx <= 0) return y0; // guard: duplicate/misordered points
        double t = (speed - x0) / dx;
        return y0 + t * (y1 - y0);
    }
};

} // namespace rawaccel
