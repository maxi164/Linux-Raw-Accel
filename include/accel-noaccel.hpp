#pragma once
#include "rawaccel-base.hpp"

namespace rawaccel {

struct accel_noaccel {
    accel_noaccel() = default;
    accel_noaccel(const accel_args&) {}

    double operator()(double, const accel_args&) const {
        return 1.0;
    }
};

} // namespace rawaccel
