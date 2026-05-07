#pragma once
#include "rawaccel-base.hpp"
#include <cmath>

namespace rawaccel {

/// Synchronous acceleration - maps input speed directly to output speed.
/// sync_speed: the speed where input = output (multiplier = 1)
/// Above sync_speed: faster input -> higher multiplier
/// Below sync_speed: slower input -> lower multiplier (< 1)
struct synchronous {
    double sync_speed = 5;
    double power      = 1;

    synchronous() = default;

    synchronous(const accel_args& args) {
        sync_speed = args.sync_speed > 0 ? args.sync_speed : 5;
        power      = args.exponent_power > 0 ? args.exponent_power : 1;
    }

    double operator()(double x, const accel_args&) const {
        if (x <= 0) return 1.0;
        // Output speed = sync_speed * (x/sync_speed)^power
        // Multiplier = output/input = (x/sync_speed)^(power-1)
        // Guard: when power < 1, multiplier → ∞ as x → 0.
        // Clamp input speed to a small epsilon to avoid infinity/NaN.
        static constexpr double MIN_SPEED = 1e-6;
        double xs = x < MIN_SPEED ? MIN_SPEED : x;
        double gain = std::pow(xs / sync_speed, power - 1.0);
        // Also guard against infinite gain from extreme exponents
        if (!std::isfinite(gain)) return 1.0;
        return gain;
    }
};

} // namespace rawaccel
