// Forward-mode dual numbers for exact derivatives (spec/solver.md §4.1, §8.3).
// Conventions for degenerate functions: ‖0‖ = 0 with zero derivatives, atan2(0,0) = 0.
#pragma once

#include <array>
#include <cmath>

namespace construe::solver {

inline constexpr int DUAL_N = 12;  // stage 1: ≤ 12 scalar parameters per equation

struct Dual {
    double v = 0.0;
    std::array<double, DUAL_N> d{};

    Dual() = default;
    Dual(double value) : v(value) {}  // NOLINT: implicit conversion of constants

    static Dual variable(double value, int index) {
        Dual r(value);
        r.d[index] = 1.0;
        return r;
    }
};

inline Dual operator+(const Dual& a, const Dual& b) {
    Dual r(a.v + b.v);
    for (int i = 0; i < DUAL_N; ++i) r.d[i] = a.d[i] + b.d[i];
    return r;
}
inline Dual operator-(const Dual& a, const Dual& b) {
    Dual r(a.v - b.v);
    for (int i = 0; i < DUAL_N; ++i) r.d[i] = a.d[i] - b.d[i];
    return r;
}
inline Dual operator-(const Dual& a) {
    Dual r(-a.v);
    for (int i = 0; i < DUAL_N; ++i) r.d[i] = -a.d[i];
    return r;
}
inline Dual operator*(const Dual& a, const Dual& b) {
    Dual r(a.v * b.v);
    for (int i = 0; i < DUAL_N; ++i) r.d[i] = a.d[i] * b.v + a.v * b.d[i];
    return r;
}
inline Dual operator/(const Dual& a, const Dual& b) {
    Dual r(a.v / b.v);
    const double inv2 = 1.0 / (b.v * b.v);
    for (int i = 0; i < DUAL_N; ++i) r.d[i] = (a.d[i] * b.v - a.v * b.d[i]) * inv2;
    return r;
}
inline Dual operator*(double s, const Dual& a) { return Dual(s) * a; }
inline Dual operator*(const Dual& a, double s) { return a * Dual(s); }

// sqrt with the convention: at zero the value is 0 and all derivatives are 0.
inline Dual dsqrt(const Dual& a) {
    if (a.v <= 0.0) return Dual(0.0);
    const double s = std::sqrt(a.v);
    Dual r(s);
    const double k = 0.5 / s;
    for (int i = 0; i < DUAL_N; ++i) r.d[i] = a.d[i] * k;
    return r;
}
inline double dsqrt(double a) { return a <= 0.0 ? 0.0 : std::sqrt(a); }

inline Dual dsin(const Dual& a) {
    Dual r(std::sin(a.v));
    const double c = std::cos(a.v);
    for (int i = 0; i < DUAL_N; ++i) r.d[i] = a.d[i] * c;
    return r;
}
inline Dual dcos(const Dual& a) {
    Dual r(std::cos(a.v));
    const double s = -std::sin(a.v);
    for (int i = 0; i < DUAL_N; ++i) r.d[i] = a.d[i] * s;
    return r;
}
inline double dsin(double a) { return std::sin(a); }
inline double dcos(double a) { return std::cos(a); }

// atan2 with the convention atan2(0,0) = 0, derivatives 0.
inline Dual datan2(const Dual& y, const Dual& x) {
    if (y.v == 0.0 && x.v == 0.0) return Dual(0.0);
    Dual r(std::atan2(y.v, x.v));
    const double inv = 1.0 / (x.v * x.v + y.v * y.v);
    for (int i = 0; i < DUAL_N; ++i) r.d[i] = (x.v * y.d[i] - y.v * x.d[i]) * inv;
    return r;
}
inline double datan2(double y, double x) { return (y == 0.0 && x == 0.0) ? 0.0 : std::atan2(y, x); }

inline double value(double x) { return x; }
inline double value(const Dual& x) { return x.v; }

}  // namespace construe::solver
