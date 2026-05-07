#pragma once
#include "accel-classic.hpp"
#include "accel-jump.hpp"
#include "accel-lookup.hpp"
#include "accel-synchronous.hpp"
#include "accel-natural.hpp"
#include "accel-noaccel.hpp"
#include "accel-power.hpp"

#include <variant>

namespace rawaccel {

using accel_variant = std::variant<
    accel_noaccel,
    classic,
    power,
    natural,
    jump,
    synchronous,
    lookup
>;

/// Holds one concrete accelerator chosen at runtime.
struct accel_union {
    accel_variant impl;

    accel_union() : impl(accel_noaccel{}) {}

    void init(const accel_args& args) {
        switch (args.mode) {
        case accel_mode::classic:     impl = classic(args);     break;
        case accel_mode::power:       impl = power(args);       break;
        case accel_mode::natural:     impl = natural(args);     break;
        case accel_mode::jump:        impl = jump(args);        break;
        case accel_mode::synchronous: impl = synchronous(args); break;
        case accel_mode::lookup:      impl = lookup(args);      break;
        case accel_mode::noaccel:
        default:                      impl = accel_noaccel{};   break;
        }
    }

    /// Returns the gain multiplier for the given input speed.
    double apply(double speed, const accel_args& args) const {
        return std::visit([&](const auto& a) { return a(speed, args); }, impl);
    }
};

} // namespace rawaccel
