#pragma once
#include "rawaccel-base.hpp"
#include <cmath>

namespace rawaccel {

/// Natural (vanishing difference) acceleration.
/// Smoothly approaches a limit asymptote.
struct natural {
    double offset    = 0;
    double accel     = 0;
    double limit     = 0;
    bool   gain_mode = false;

    natural() = default;

    natural(const accel_args& args) : offset(args.input_offset),
                                       limit(std::max(0.0, args.limit - 1.0)) {
        // N8: clamp limit to 0 — same guard as jump mode.
        // args.limit < 1 → limit < 0 → non-gain output goes below 1 (deceleration/inversion).
        // GUI enforces limit>=0.1 but JSON/CLI may pass arbitrary values.
        // O1: avoid operator precedence trap — fabs before ternary, not after.
        // Prevent division by zero when limit ≈ 0 (args.limit ≈ 1).
        double abs_limit = std::fabs(limit);
        accel     = args.decay_rate / (abs_limit < 1e-9 ? 1.0 : abs_limit);
        gain_mode = args.gain;
    }

    double operator()(double x, const accel_args&) const {
        if (x <= offset) return 1.0;
        double t     = x - offset;   // positive distance past offset
        double decay = std::exp(-accel * t);

        if (!gain_mode) {
            // Legacy mode: gain approaches (limit+1) asymptotically.
            // f(t) = limit * (1 - exp(-accel*t)) + 1
            return limit * (1.0 - decay) + 1.0;
        } else {
            // Gain mode: integral form so output speed is smooth.
            // output_speed = integral of gain * dv
            // gain(t) = limit*(1 - exp(-a*t)) + 1
            // output = t + limit*(t + exp(-a*t)/a - 1/a) + constant
            if (x < 1e-9) return 1.0; // guard: prevent output/x blow-up when x ≈ 0
            // Guard: when accel ≈ 0 (decay_rate ≈ 0), the integral term
            // (1 - decay)/accel is 0/0 → NaN.  In this regime the gain curve
            // is flat at 1.0 (no acceleration), so return 1.0 directly.
            if (accel < 1e-12) return 1.0;
            double output = t + limit * (t - (1.0 - decay) / accel);
            return output / x + 1.0;
        }
    }
};

} // namespace rawaccel
