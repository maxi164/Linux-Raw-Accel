#pragma once
#include "rawaccel-base.hpp"
#include <cmath>

namespace rawaccel {

/// Jump (sigmoid) acceleration.
/// Uses a logistic/sigmoid function to create an S-curve.
/// Smoothly transitions from 1.0 to limit.
struct jump {
    double mid    = 0;
    double k      = 0;
    double limit  = 0;

    jump() = default;

    jump(const accel_args& args) {
        // sync_speed = midpoint of sigmoid, smooth controls steepness
        mid   = args.sync_speed;
        k     = args.smooth * 10.0; // steepness
        // limit = args.limit - 1: gain offset above baseline.
        // args.limit < 1 → limit < 0 → sigmoid produces deceleration (gain < 1).
        // Clamp to 0 to prevent unintended deceleration; GUI enforces limit >= 0.1
        // but guard here for programmatic use.
        limit = std::max(0.0, args.limit - 1.0);
    }

    double operator()(double x, const accel_args&) const {
        if (x <= 0) return 1.0;
        // Sigmoid: maps smoothly from 1.0 (low speed) to 1+limit (high speed)
        double sig = 1.0 / (1.0 + std::exp(-k * (x - mid)));
        return 1.0 + limit * sig;
    }
};

} // namespace rawaccel
