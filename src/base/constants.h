// Mathematical constants shared by all modules (spec/base.md §2а).
#pragma once

#include <numbers>

namespace construe {

inline constexpr double PI = std::numbers::pi;
inline constexpr double TWO_PI = 2 * std::numbers::pi;
// One degree in radians and one radian in degrees (exact products, not reformulations of `x * 180 / PI`).
inline constexpr double DEG = std::numbers::pi / 180.0;
inline constexpr double RAD_TO_DEG = 180.0 / std::numbers::pi;

}  // namespace construe
