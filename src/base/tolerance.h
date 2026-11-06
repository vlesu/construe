// Global tolerances for geometry comparison (spec/base.md §3).
#pragma once

#include <algorithm>

namespace construe {

inline constexpr double LINEAR_TOL = 1e-7;    // mm
inline constexpr double ANGULAR_TOL = 1e-12;  // rad

// Guards "away from zero" (spec/base.md §3): a vector shorter than DEGENERATE_EPS has no direction; a positive
// divisor (zoom, scale) is clamped from below to SAFE_DIV_MIN.
inline constexpr double DEGENERATE_EPS = 1e-12;
inline constexpr double SAFE_DIV_MIN = 1e-9;

// num / den with den clamped from below to SAFE_DIV_MIN (den is expected to be positive).
inline double safeDiv(double num, double den) { return num / std::max(SAFE_DIV_MIN, den); }

}  // namespace construe
