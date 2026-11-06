// Internal region geometry functions (spec/sketch.md §3.5).
#pragma once

#include "sketch/sketch.h"

#include <functional>
#include <vector>

namespace construe::sketch {

Point2 computeInteriorPoint(const Region& region);  // §3.5.1
// Contact parameters τ ∈ [0, 1] of g with tolerance tol (§3.2 item 0а, ellipseContacts; S4-SKT-007): samples with |g| ≤ tol,
// roots of sign changes (bisection) and refined minima of |g| ≤ tol.
std::vector<double> contactTaus(const std::function<double(double)>& g, double tol);

}  // namespace construe::sketch
