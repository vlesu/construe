// Plane (2D) geometry helpers shared by all modules (spec/base.md §2а). Angles in radians, counterclockwise positive.
#pragma once

#include "base/constants.h"
#include "base/math.h"
#include "base/tolerance.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace construe::geom2 {

// z-component of the cross product a × b (signed parallelogram area; > 0 when b is counterclockwise of a).
inline double cross2(const Vec2& a, const Vec2& b) { return a.x() * b.y() - a.y() * b.x(); }

// v rotated by +90°.
inline Vec2 perp(const Vec2& v) { return {-v.y(), v.x()}; }

// Direction angle of v, in (−π, π] (atan2).
inline double angleOf(const Vec2& v) { return std::atan2(v.y(), v.x()); }
// Direction angle of the vector from c to p, in (−π, π].
inline double angleOf(const Point2& c, const Point2& p) { return std::atan2(p.y() - c.y(), p.x() - c.x()); }

// Angle reduced to [0, 2π). fmod is exact, so for |a| < 4π the result equals repeated ±2π steps bit for bit.
inline double normalizeAngle0To2Pi(double a) {
    a = std::fmod(a, TWO_PI);
    if (a < 0) a += TWO_PI;
    if (a >= TWO_PI) a -= TWO_PI;  // a = −tiny: −tiny + 2π rounds to 2π
    return a;
}

// Angle reduced to (−π, π].
inline double normalizeAngleSym(double a) {
    a = std::fmod(a, TWO_PI);
    if (a <= -PI) a += TWO_PI;
    if (a > PI) a -= TWO_PI;
    return a;
}

// Counterclockwise turn from angle `from` to angle `to`, in [0, 2π).
inline double ccwSweep(double from, double to) { return normalizeAngle0To2Pi(to - from); }
// The same in (0, 2π]: equal angles — a full turn.
inline double ccwSweepPositive(double from, double to) {
    const double s = ccwSweep(from, to);
    return s <= 0 ? TWO_PI : s;
}

// Counterclockwise angle from direction u to direction v, in [0, 2π) (0 for codirectional vectors).
inline double ccwAngle(const Vec2& u, const Vec2& v) {
    const double a = std::atan2(cross2(u, v), u.dot(v));
    return a < 0 ? a + TWO_PI : a;
}

// Sweep of the counterclockwise arc about c from start s to end e, in (0, 2π] (s, e at the same angle — full turn).
inline double arcSweep(const Point2& c, const Point2& s, const Point2& e) {
    const Vec2 a = s - c, b = e - c;
    const double t = std::atan2(cross2(a, b), a.dot(b));
    return t <= 0 ? t + TWO_PI : t;
}

// X (on the arc's circle) lies on the counterclockwise arc about c that starts at s and sweeps `sweep`, with an angular
// tolerance `angTol` at both ends.
inline bool onArc(const Point2& c, const Point2& s, double sweep, const Point2& X, double angTol) {
    const double phi = ccwAngle(s - c, X - c);
    return phi <= sweep + angTol || phi >= TWO_PI - angTol;
}

// Distance from p to the segment a–b (a degenerate segment — distance to a).
inline double distToSegment(const Point2& p, const Point2& a, const Point2& b) {
    const Vec2 d = b - a;
    const double l2 = d.squaredNorm();
    const double t = l2 > 0 ? std::clamp((p - a).dot(d) / l2, 0.0, 1.0) : 0.0;
    return (p - (a + t * d)).norm();
}

// Distance from p to the infinite line through a and b (a = b — distance to a).
inline double distToLine(const Point2& p, const Point2& a, const Point2& b) {
    const Vec2 d = b - a;
    const double l = d.norm();
    if (l <= 0) return (p - a).norm();
    return std::abs(cross2(p - a, d)) / l;
}

// Intersection of the lines p1 + t·d1 and p2 + u·d2; none when |d1 × d2| ≤ parallelTol (parallel or degenerate).
inline std::optional<Point2> lineLine(const Point2& p1, const Vec2& d1, const Point2& p2, const Vec2& d2,
                                      double parallelTol) {
    const double den = cross2(d1, d2);
    if (std::abs(den) <= parallelTol) return std::nullopt;
    return p1 + cross2(p2 - p1, d2) / den * d1;
}

// Intersections of the line a + t·d (d — unit vector) with the circle (c, R): tangency within tol — one point (the foot
// of the perpendicular), otherwise two points (in the direction of d) or none.
inline std::vector<Point2> lineCircle(const Point2& a, const Vec2& d, const Point2& c, double R, double tol) {
    const Point2 f = a + (c - a).dot(d) * d;
    const double dist = (c - f).norm();
    if (std::abs(dist - R) <= tol) return {f};
    if (dist > R) return {};
    const double h = std::sqrt(R * R - dist * dist);
    return {f - h * d, f + h * d};
}

// Intersections of two circles: concentric (centers within tol) — none; external/internal tangency within tol — one
// point; otherwise two points (left of c1→c2 first) or none.
inline std::vector<Point2> circleCircle(const Point2& c1, double r1, const Point2& c2, double r2, double tol) {
    const Vec2 v = c2 - c1;
    const double d = v.norm();
    if (d <= tol) return {};
    if (std::abs(d - (r1 + r2)) <= tol || std::abs(d - std::abs(r1 - r2)) <= tol) {
        const double sgn = std::abs(d - (r1 + r2)) <= tol ? 1.0 : (r1 >= r2 ? 1.0 : -1.0);
        return {c1 + sgn * r1 * v / d};
    }
    if (d > r1 + r2 || d < std::abs(r1 - r2)) return {};
    const double a = (r1 * r1 - r2 * r2 + d * d) / (2 * d);
    const double h = std::sqrt(std::max(0.0, r1 * r1 - a * a));
    const Point2 m = c1 + a * v / d;
    const Vec2 n = perp(v / d);
    return {m + h * n, m - h * n};
}

// Vector too short to have a direction (‖v‖ < DEGENERATE_EPS).
inline bool isZeroVec(const Vec2& v) { return v.norm() < DEGENERATE_EPS; }

}  // namespace construe::geom2
