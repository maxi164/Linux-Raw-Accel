#pragma once
#include "math-vec2.hpp"
#include <cfloat>
#include <cstddef>

namespace rawaccel {

/// Single source of truth for the version number — read by GUI and daemon.
inline constexpr const char* RAWACCEL_VERSION = "0.2.0";

using milliseconds = double;

inline constexpr int    POLL_RATE_MIN        = 125;
inline constexpr int    POLL_RATE_MAX        = 8000;
inline constexpr milliseconds DEFAULT_TIME_MIN = 1000.0 / POLL_RATE_MAX / 2;
inline constexpr milliseconds DEFAULT_TIME_MAX = 100;
inline constexpr milliseconds WRITE_DELAY     = 1000;
inline constexpr size_t MAX_DEV_ID_LEN       = 200;
inline constexpr size_t MAX_NAME_LEN         = 256;
inline constexpr size_t LUT_RAW_DATA_CAPACITY = 514;
inline constexpr size_t LUT_POINTS_CAPACITY  = LUT_RAW_DATA_CAPACITY / 2;
inline constexpr double MAX_NORM             = 16;
inline constexpr double NORMALIZED_DPI       = 1000;

enum class accel_mode {
    classic,
    jump,
    natural,
    synchronous,
    power,
    lookup,
    noaccel
};

enum class cap_mode {
    io,
    in,
    out
};

struct accel_args {
    accel_mode mode           = accel_mode::noaccel;
    bool       gain           = true;
    double input_offset       = 0;
    double output_offset      = 0;
    double acceleration       = 0.005;
    double decay_rate         = 0.1;
    double gamma              = 1;
    double motivity           = 1.5;
    double exponent_classic   = 2;
    double scale              = 1;
    double exponent_power     = 0.05;
    double limit              = 1.5;
    double sync_speed         = 5;
    double smooth             = 0.5;
    vec2d  cap                = { 15, 1.5 };
    cap_mode cap_mode_val     = cap_mode::out;
    int    length             = 0;
    mutable float data[LUT_RAW_DATA_CAPACITY] = {};

    // Use field-by-field comparison — memcmp is unreliable with mutable/padding members.
    // All double fields use epsilon comparison to survive JSON round-trips where
    // double→string→double may introduce sub-ULP differences (fixes xy_linked false-unlink).
    // LUT float data uses a slightly larger epsilon for the same reason.
    static constexpr double DBL_EPSILON_CMP = 1e-9;
    static constexpr float  LUT_EPSILON     = 1e-5f;

    static bool deq(double a, double b) {
        return std::fabs(a - b) <= DBL_EPSILON_CMP;
    }

    bool operator==(const accel_args& o) const {
        if (mode != o.mode || gain != o.gain || cap_mode_val != o.cap_mode_val) return false;
        if (!deq(input_offset,     o.input_offset)   || !deq(output_offset, o.output_offset)) return false;
        if (!deq(acceleration,     o.acceleration)   || !deq(decay_rate,    o.decay_rate))    return false;
        if (!deq(gamma,            o.gamma)           || !deq(motivity,      o.motivity))      return false;
        if (!deq(exponent_classic, o.exponent_classic)|| !deq(scale,        o.scale))         return false;
        if (!deq(exponent_power,   o.exponent_power)  || !deq(limit,        o.limit))         return false;
        if (!deq(sync_speed,       o.sync_speed)      || !deq(smooth,       o.smooth))        return false;
        if (!deq(cap.x,            o.cap.x)           || !deq(cap.y,        o.cap.y))         return false;
        if (length != o.length) return false;
        for (int i = 0; i < length && i < (int)LUT_RAW_DATA_CAPACITY; ++i)
            if (std::fabs(data[i] - o.data[i]) > LUT_EPSILON) return false;
        return true;
    }
    bool operator!=(const accel_args& o) const { return !(*this == o); }
};

struct speed_args {
    bool   whole                        = true;
    double lp_norm                      = 2;
    double input_speed_smooth_halflife  = 0;
    double scale_smooth_halflife        = 0;
    double output_speed_smooth_halflife = 0;
};

struct profile {
    char   name[MAX_NAME_LEN]       = "default";
    bool   raw_passthrough          = false; // true → bypass entire pipeline, 1:1 raw input
    vec2d  domain_weights           = { 1, 1 };
    vec2d  range_weights            = { 1, 1 };
    accel_args accel_x;
    accel_args accel_y;
    speed_args speed_processor_args;
    double output_dpi               = NORMALIZED_DPI;
    double lr_output_dpi_ratio      = 1;
    double ud_output_dpi_ratio      = 1;
    double degrees_rotation         = 0;
    double degrees_snap             = 0;
    double speed_min                = 0;
    double speed_max                = 0;
};

} // namespace rawaccel
