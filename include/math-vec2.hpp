#pragma once
#include <cmath>

namespace rawaccel {

struct vec2d {
    double x = 0;
    double y = 0;
};

inline double magnitude(vec2d v) {
    return std::hypot(v.x, v.y);
}

inline double lp_distance(vec2d v, double p) {
    // R6: guard against pow(0, negative) = Inf when both components are zero.
    // The correct Lp distance for the zero vector is always 0.
    if (v.x == 0 && v.y == 0) return 0;
    return std::pow(std::pow(std::fabs(v.x), p) + std::pow(std::fabs(v.y), p), 1.0 / p);
}

inline double maxsd(double a, double b) {
    return a > b ? a : b;
}

inline double minsd(double a, double b) {
    return a < b ? a : b;
}

inline double clampsd(double val, double lo, double hi) {
    return val < lo ? lo : (val > hi ? hi : val);
}

inline vec2d rotate(vec2d v, vec2d dir) {
    return { v.x * dir.x - v.y * dir.y, v.x * dir.y + v.y * dir.x };
}

inline vec2d direction(double degrees) {
    double rad = degrees * M_PI / 180.0;
    return { std::cos(rad), std::sin(rad) };
}

} // namespace rawaccel
