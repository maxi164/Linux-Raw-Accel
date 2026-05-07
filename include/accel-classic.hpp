#pragma once
#include "rawaccel-base.hpp"
#include <cmath>
#include <cfloat>

namespace rawaccel {

/// Classic (linear raised to power) acceleration.
/// Formula (legacy/scale mode): scale * x^(exp-1)
/// Returns a multiplier applied to input speed.
struct classic {
    double accel_raised = 0;
    double cap          = DBL_MAX;
    double sign         = 1;

    classic() = default;

    classic(const accel_args& args) {
        // exponent == 1 → linear (acceleration * x / x == acceleration, constant gain)
        // Skip the normal setup; operator() will handle it via the exp==1 path.
        if (args.exponent_classic <= 1.0) {
            // Store acceleration directly; operator() returns args.acceleration + 1
            accel_raised = args.acceleration;
            // cap.y == 0 → no cap (DBL_MAX); cap.y > 0 → cap = cap.y - 1 (in gain units)
            // cap.y < 1 is degenerate, but guard: don't go negative → clamp to 0
            cap = args.cap.y > 0 ? std::max(0.0, args.cap.y - 1.0) : DBL_MAX;
            return;
        }

        switch (args.cap_mode_val) {
        case cap_mode::io:
            cap = args.cap.y - 1;
            if (cap < 0) { cap = -cap; sign = -sign; }
            {
                // O5: if cap.x <= input_offset, base_accel returns 0 → pow(0, exp-1).
                // When exp < 1 this yields Inf. Degenerate config — guard with accel_raised = 0.
                double a = base_accel(args.cap.x, cap, args);
                accel_raised = (std::isfinite(a) && a > 0)
                    ? std::pow(a, args.exponent_classic - 1)
                    : 0.0;
            }
            break;
        case cap_mode::in:
            {
                double ar = std::pow(args.acceleration, args.exponent_classic - 1);
                accel_raised = std::isfinite(ar) ? ar : 0.0; // NaN guard: neg accel + non-int exp
            }
            // BUG-9: when cap.x <= input_offset, base_fn computes
            // pow(negative_base, exp) which yields NaN for non-integer
            // exponents.  That NaN then poisons every operator() call
            // (`min(finite, NaN)` returns NaN per IEEE 754 ordering).
            // Treat the degenerate config as "no input cap" so output
            // remains finite.
            if (args.cap.x > 0 && args.cap.x > args.input_offset) {
                cap = base_fn(args.cap.x, accel_raised, args);
                if (!std::isfinite(cap)) cap = DBL_MAX;
            }
            break;
        case cap_mode::out:
        default:
            {
                double ar = std::pow(args.acceleration, args.exponent_classic - 1);
                accel_raised = std::isfinite(ar) ? ar : 0.0; // NaN guard: neg accel + non-int exp
            }
            if (args.cap.y > 0) {
                cap = args.cap.y - 1;
                if (cap < 0) { cap = -cap; sign = -sign; }
            }
            break;
        }
    }

    double operator()(double x, const accel_args& args) const {
        if (x <= args.input_offset) return 1.0;
        // Linear path (exponent <= 1): constant gain = acceleration, capped at cap_y
        if (args.exponent_classic <= 1.0)
            return 1.0 + minsd(accel_raised, cap);
        return sign * minsd(base_fn(x, accel_raised, args), cap) + 1.0;
    }

private:
    double base_fn(double x, double ar, const accel_args& args) const {
        return ar * std::pow(x - args.input_offset, args.exponent_classic) / x;
    }

    static double base_accel(double x, double y, const accel_args& args) {
        double power = args.exponent_classic;
        if (power <= 1.0 || x <= args.input_offset) return 0; // degenerate
        return std::pow(x * y * std::pow(x - args.input_offset, -power), 1.0 / (power - 1));
    }
};

} // namespace rawaccel
