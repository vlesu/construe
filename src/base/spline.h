// Planar B-spline / NURBS curves: evaluation, knot insertion, sub-curves, interpolation, flattening
// (spec/sketch.md §2.7, S4A-SPL-001…003). Shared by sketch, solver, regen and kernel conversion.
#pragma once

#include "base/math.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace construe::spline {

inline constexpr int SPLINE_MAX_DEGREE = 25;        // highest supported degree (§2.7.1, as in common B-rep kernels)
inline constexpr double SPLINE_KNOT_TOL = 1e-12;    // relative knot coincidence tolerance (× domain length)
inline constexpr int SPLINE_SPAN_SAMPLES = 16;      // samples per knot span for closest point and contacts (§2.7.5)
inline constexpr int SPLINE_NEWTON_ITER = 30;       // Newton iterations of the closest point (§2.7.5)
inline constexpr int SPLINE_MAX_SEGMENTS = 256;     // flattening: chords per knot span at most (§2.7.6)
inline constexpr int SPLINE_GAUSS_POINTS = 16;      // Gauss–Legendre points per span for area and length (§2.7.6)

// Curve in document form (§2.7.1): poles, weights, distinct knots with multiplicities, degree, periodicity.
struct Curve {
    int degree = 3;
    std::vector<Point2> poles;
    std::vector<double> weights;  // empty — all 1
    std::vector<double> knots;    // distinct, ascending
    std::vector<int> mults;       // multiplicity of each knot
    bool periodic = false;
};

// Expanded form (§2.7.2): flat knot vector U, poles P with weights w; the curve is defined on [a, b].
// Periodic curves are stored unwrapped (poles repeated, knots extended by the period).
struct Flat {
    int p = 3;
    std::vector<Point2> P;
    std::vector<double> w;
    std::vector<double> U;
    bool periodic = false;
    double a = 0, b = 1;
    double period() const { return b - a; }
};

// Bezier curve of degree n−1 through the poles (weights — optional) in document form.
Curve bezier(const std::vector<Point2>& poles, const std::vector<double>& weights = {});
// Validation of the document form (§2.7.1): empty string — valid, otherwise the violated rule.
std::string validate(const Curve& c);
// Expanded form; the curve must be valid.
Flat expand(const Curve& c);
// Compression of an open clamped flat form back to the document form (knots merged within SPLINE_KNOT_TOL).
Curve compress(const Flat& f);

// Parameter normalized into [a, b) for periodic curves; unchanged for open ones (extrapolation allowed, §2.7.3).
double wrap(const Flat& f, double t);
// Index k of the span [U[k], U[k+1]) used for t (first/last non-empty span outside the domain).
int findSpan(const Flat& f, double t);
// Basis functions N[0..p] of span k and their derivatives up to order nd (≤ 2): ders[d][j] for pole k − p + j.
void basisDers(const Flat& f, int k, double t, int nd, double ders[3][SPLINE_MAX_DEGREE + 1]);
// Point and derivatives C, C', C'' at t (rational: quotient rule, §2.7.3).
void derivs(const Flat& f, double t, Point2 out[3]);
Point2 eval(const Flat& f, double t);
// Rational basis R_j(t) = N_j·w_j/W for poles k − p + j, j = 0..p (∂C/∂P = R, §2.7.3).
int rationalBasis(const Flat& f, double t, double R[SPLINE_MAX_DEGREE + 1]);
// Signed curvature cross(C', C'')/|C'|³ (0 where |C'| = 0).
double curvature(const Flat& f, double t);
// Distinct knot values in [a, b] (span boundaries), ascending.
std::vector<double> breaks(const Flat& f);

// Closest parameter to X over [a, b] (periodic: over one period): samples + Newton (§2.7.5).
double closestParam(const Flat& f, const Point2& X);
// Knot insertion (Boehm) of t once.
Flat insertKnot(const Flat& f, double t);
// Open clamped sub-curve over [s, e], a ≤ s < e ≤ b (periodic: s < e ≤ s + period; may wrap) — §2.7.4.
Flat segment(const Flat& f, double s, double e);
// Reversed direction: C_rev(t) = C(a + b − t).
Flat reversed(const Flat& f);
// Interpolating open curve through the points (≥ 2): degree min(3, n − 1), chord-length parameters, averaged knots
// (§2.7.7). Coincident consecutive points are an error (empty result).
Curve interpolate(const std::vector<Point2>& points);

// Flattening (§2.7.6): parameters of polyline vertices over [a, b] (periodic: one period), chord error ≈ ≤ tol.
std::vector<double> flattenParams(const Flat& f, double tol);
// Signed area contribution ∫(x dy − y dx)/2 over [s, e] (Gauss–Legendre per span, §2.7.6).
double greenArea(const Flat& f, double s, double e);
// Length over [s, e] (Gauss–Legendre per span).
double length(const Flat& f, double s, double e);
// Parameters t ∈ [a, b] (periodic: [a, b)) where g(C(t)) = 0: sign changes and tangential minima (§2.7.5).
std::vector<double> roots(const Flat& f, const std::function<double(const Point2&)>& g, double tol);
// Intersections of two curves (§2.7.5): parameter pairs (s on A, t on B) — crossings of their flattened polylines
// refined by Newton on A(s) − B(t) = 0 and ends of either lying on the other within tol; points closer than 10·tol
// are merged. Tangential touching without crossing (away from ends) is not detected.
std::vector<std::pair<double, double>> intersections(const Flat& A, const Flat& B, double tol);
// Whether the curve crosses itself (§2.7.5): non-adjacent chords of its flattening (tolerance tol) intersect; for a
// periodic curve the first and last chords are adjacent.
bool selfIntersects(const Flat& f, double tol);

}  // namespace construe::spline
