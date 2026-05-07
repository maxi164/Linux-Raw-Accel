#pragma once
#include "rawaccel-base.hpp"
#include <cmath>
#include <cfloat>

namespace rawaccel {

/// Power acceleration: output = (scale * x)^n + C/x
/// Smooth curve that naturally approaches a cap.
struct power {
    vec2d  offset   = {};
    double scale    = 1;
    double constant = 0;
    double cap_x    = DBL_MAX;
    double cap_y    = DBL_MAX;
    double constant_b = 0;

    power() = default;

    power(const accel_args& args) {
        // N11: exponent_power == 0 makes gain_inverse/scale_from_gain_point divide by zero.
        // Clamp to a small positive value; gain_inverse uses 1/n as exponent.
        auto n = args.exponent_power > 0 ? args.exponent_power : 1e-4;

        if (args.cap_mode_val != cap_mode::io) {
            scale = args.scale;
        } else {
            scale = scale_from_gain_point(args.cap.x, args.cap.y, n);
        }

        offset.x = gain_inverse(args.output_offset, n, scale);
        offset.y = args.output_offset;
        constant = offset.x * offset.y * n / (n + 1);

        switch (args.cap_mode_val) {
        case cap_mode::io:
            cap_x = args.cap.x;
            cap_y = args.cap.y;
            break;
        case cap_mode::in:
            if (args.cap.x > 0) {
                if (args.cap.x <= offset.x) { cap_x = 0; cap_y = offset.y; return; }
                cap_x = args.cap.x;
                cap_y = gain_fn(args.cap.x, n, scale);
            }
            break;
        case cap_mode::out:
        default:
            if (args.cap.y > 0) {
                cap_x = gain_inverse(args.cap.y, n, scale);
                cap_y = args.cap.y;
            }
            break;
        }

        constant_b = integration_constant(cap_x, cap_y, base_fn_impl(cap_x, args));
    }

    double operator()(double speed, const accel_args& args) const {
        // D8: also check input_offset — consistent guard with other accelerators.
        // offset.x is computed in the constructor via gain_inverse(output_offset,...).
        if (speed <= 0 || speed <= offset.x) return (offset.x > 0 ? offset.y : 1.0);
        if (speed < cap_x) {
            return base_fn_impl(speed, args);
        } else {
            return cap_y + constant_b / speed;
        }
    }

private:
    double base_fn_impl(double x, const accel_args& args) const {
        if (x <= offset.x) return offset.y;
        return std::pow(scale * x, args.exponent_power) + constant / x;
    }

    static double gain_fn(double input, double power, double sc) {
        return (power + 1) * std::pow(input * sc, power);
    }

    static double gain_inverse(double g, double power, double sc) {
        if (sc <= 0) return 0;
        return std::pow(g / (power + 1), 1.0 / power) / sc;
    }

    static double scale_from_gain_point(double input, double gain, double power) {
        if (input <= 0) return 0; // guard: prevent NaN in io mode when cap.x=0
        return std::pow(gain / (power + 1), 1.0 / power) / input;
    }

    static double integration_constant(double input, double gain, double output) {
        return (output - gain) * input;
    }
};

} // namespace rawaccel
