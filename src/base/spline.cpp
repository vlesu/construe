// Planar B-spline / NURBS curves (spec/sketch.md §2.7).
#include "base/spline.h"

#include "base/geom2.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>

namespace construe::spline {

namespace {

using geom2::cross2;

double knotTol(const Flat& f) { return SPLINE_KNOT_TOL * std::max(1.0, std::abs(f.b - f.a)); }

// Gauss–Legendre nodes and weights on [−1, 1] (Newton on P_n).
struct Gauss {
    double x[SPLINE_GAUSS_POINTS], w[SPLINE_GAUSS_POINTS];
    Gauss() {
        const int n = SPLINE_GAUSS_POINTS;
        for (int i = 0; i < n; ++i) {
            double z = std::cos(PI * (i + 0.75) / (n + 0.5)), dp = 0;
            for (int it = 0; it < 100; ++it) {
                double p0 = 1, p1 = z;
                for (int k = 2; k <= n; ++k) {
                    const double p2 = ((2 * k - 1) * z * p1 - (k - 1) * p0) / k;
                    p0 = p1, p1 = p2;
                }
                dp = n * (z * p1 - p0) / (z * z - 1);
                const double dz = p1 / dp;
                z -= dz;
                if (std::abs(dz) < 1e-16) break;
            }
            x[i] = z;
            w[i] = 2 / ((1 - z * z) * dp * dp);
        }
    }
};
const Gauss& gauss() {
    static const Gauss g;
    return g;
}

// Homogeneous pole (w·P, w).
struct HP {
    double x, y, w;
};

std::vector<HP> homog(const Flat& f) {
    std::vector<HP> h(f.P.size());
    for (size_t i = 0; i < f.P.size(); ++i) h[i] = {f.P[i].x() * f.w[i], f.P[i].y() * f.w[i], f.w[i]};
    return h;
}

void dehomog(const std::vector<HP>& h, Flat& f) {
    f.P.resize(h.size());
    f.w.resize(h.size());
    for (size_t i = 0; i < h.size(); ++i) f.P[i] = Point2(h[i].x / h[i].w, h[i].y / h[i].w), f.w[i] = h[i].w;
}

// Periodic flat form extended to cover two periods (for sub-curves that wrap, §2.7.4).
Flat twoPeriods(const Flat& f) {
    const int p = f.p;
    const int n = static_cast<int>(f.P.size()) - p;
    const double T = f.period();
    Flat g = f;
    g.P.clear(), g.w.clear(), g.U.clear();
    for (int i = 0; i < 2 * n + p; ++i) g.P.push_back(f.P[i % n]), g.w.push_back(f.w[i % n]);
    auto u = [&](int j) {  // u_j = U[p + (j mod n)] + floor(j / n)·T
        const int q = (j % n + n) % n;
        const int fl = (j - q) / n;
        return f.U[p + q] + fl * T;
    };
    for (int i = 0; i <= 2 * n + 2 * p; ++i) g.U.push_back(u(i - p));
    g.b = f.a + 2 * T;
    return g;
}

int multiplicityAt(const Flat& f, double t, double tol) {
    int m = 0;
    for (double u : f.U) m += std::abs(u - t) <= tol;
    return m;
}

}  // namespace

Curve bezier(const std::vector<Point2>& poles, const std::vector<double>& weights) {
    Curve c;
    const int n = static_cast<int>(poles.size());
    c.degree = std::max(1, n - 1);
    c.poles = poles;
    c.weights = weights;
    c.knots = {0.0, 1.0};
    c.mults = {n, n};
    return c;
}

std::string validate(const Curve& c) {
    const int n = static_cast<int>(c.poles.size());
    const int p = c.degree;
    if (p < 1 || p > SPLINE_MAX_DEGREE) return "degree";
    if (!c.weights.empty() && static_cast<int>(c.weights.size()) != n) return "weights";
    for (double w : c.weights)
        if (!(std::isfinite(w) && w > 0)) return "weights";
    for (const auto& q : c.poles)
        if (!(std::isfinite(q.x()) && std::isfinite(q.y()))) return "poles";
    if (c.knots.size() < 2 || c.knots.size() != c.mults.size()) return "knots";
    for (size_t i = 0; i < c.knots.size(); ++i) {
        if (!std::isfinite(c.knots[i])) return "knots";
        if (i > 0 && !(c.knots[i] > c.knots[i - 1])) return "knots";
    }
    const size_t K = c.knots.size();
    int sum = 0;
    for (size_t i = 0; i < K; ++i) {
        const int m = c.mults[i];
        const bool end = i == 0 || i + 1 == K;
        if (m < 1) return "mults";
        if (!c.periodic && end && m != p + 1) return "mults";
        if (!end && m > p) return "mults";
        if (c.periodic && end && m > p) return "mults";
        sum += m;
    }
    if (c.periodic) {
        if (c.mults.front() != c.mults.back()) return "mults";
        if (sum - c.mults.back() != n) return "mults";
        if (n < p + 1) return "poles";
    } else {
        if (n < p + 1) return "poles";
        if (sum != n + p + 1) return "mults";
    }
    return {};
}

Flat expand(const Curve& c) {
    Flat f;
    f.p = c.degree;
    f.periodic = c.periodic;
    const int n = static_cast<int>(c.poles.size());
    std::vector<double> w = c.weights.empty() ? std::vector<double>(n, 1.0) : c.weights;
    if (!c.periodic) {
        f.P = c.poles;
        f.w = w;
        for (size_t i = 0; i < c.knots.size(); ++i)
            for (int k = 0; k < c.mults[i]; ++k) f.U.push_back(c.knots[i]);
        f.a = f.U[f.p];
        f.b = f.U[n];
        return f;
    }
    // Periodic (§2.7.2): one period of knots with multiplicity (last knot excluded), unwrapped by ±period.
    std::vector<double> seq;
    for (size_t i = 0; i + 1 < c.knots.size(); ++i)
        for (int k = 0; k < c.mults[i]; ++k) seq.push_back(c.knots[i]);
    const double T = c.knots.back() - c.knots.front();
    const int p = f.p;
    auto u = [&](int j) {
        const int q = (j % n + n) % n;
        const int fl = (j - q) / n;
        return seq[q] + fl * T;
    };
    for (int i = 0; i < n + p; ++i) f.P.push_back(c.poles[i % n]), f.w.push_back(w[i % n]);
    for (int i = 0; i <= n + 2 * p; ++i) f.U.push_back(u(i - p));
    f.a = c.knots.front();
    f.b = f.a + T;
    return f;
}

Curve compress(const Flat& f) {
    Curve c;
    c.degree = f.p;
    c.poles = f.P;
    c.weights = f.w;
    bool unit = true;
    for (double w : f.w) unit = unit && w == 1.0;
    if (unit) c.weights.clear();
    const double tol = knotTol(f);
    for (double u : f.U) {
        if (!c.knots.empty() && std::abs(u - c.knots.back()) <= tol) {
            ++c.mults.back();
        } else {
            c.knots.push_back(u);
            c.mults.push_back(1);
        }
    }
    return c;
}

double wrap(const Flat& f, double t) {
    if (!f.periodic) return t;
    const double T = f.period();
    double r = std::fmod(t - f.a, T);
    if (r < 0) r += T;
    return f.a + r;
}

int findSpan(const Flat& f, double t) {
    const int p = f.p;
    const int n = static_cast<int>(f.P.size());
    int first = p, last = n - 1;
    while (first < last && !(f.U[first] < f.U[first + 1])) ++first;
    while (last > first && !(f.U[last] < f.U[last + 1])) --last;
    if (t <= f.U[first]) return first;
    if (t >= f.U[last + 1]) return last;
    int k = first;
    for (int i = first; i <= last; ++i)
        if (f.U[i] <= t && f.U[i] < f.U[i + 1]) k = i;
    return k;
}

void basisDers(const Flat& f, int k, double t, int nd, double ders[3][SPLINE_MAX_DEGREE + 1]) {
    // The NURBS Book, A2.3.
    const int p = f.p;
    double ndu[SPLINE_MAX_DEGREE + 1][SPLINE_MAX_DEGREE + 1];
    double left[SPLINE_MAX_DEGREE + 1], right[SPLINE_MAX_DEGREE + 1];
    ndu[0][0] = 1.0;
    for (int j = 1; j <= p; ++j) {
        left[j] = t - f.U[k + 1 - j];
        right[j] = f.U[k + j] - t;
        double saved = 0.0;
        for (int r = 0; r < j; ++r) {
            ndu[j][r] = right[r + 1] + left[j - r];
            const double tmp = ndu[r][j - 1] / ndu[j][r];
            ndu[r][j] = saved + right[r + 1] * tmp;
            saved = left[j - r] * tmp;
        }
        ndu[j][j] = saved;
    }
    for (int j = 0; j <= p; ++j) ders[0][j] = ndu[j][p];
    for (int d = 1; d <= nd; ++d)
        for (int j = 0; j <= p; ++j) ders[d][j] = 0.0;
    double a[2][SPLINE_MAX_DEGREE + 1];
    for (int r = 0; r <= p; ++r) {
        int s1 = 0, s2 = 1;
        a[0][0] = 1.0;
        for (int d = 1; d <= std::min(nd, p); ++d) {
            double dd = 0.0;
            const int rk = r - d, pk = p - d;
            if (r >= d) {
                a[s2][0] = a[s1][0] / ndu[pk + 1][rk];
                dd = a[s2][0] * ndu[rk][pk];
            }
            const int j1 = rk >= -1 ? 1 : -rk;
            const int j2 = (r - 1 <= pk) ? d - 1 : p - r;
            for (int j = j1; j <= j2; ++j) {
                a[s2][j] = (a[s1][j] - a[s1][j - 1]) / ndu[pk + 1][rk + j];
                dd += a[s2][j] * ndu[rk + j][pk];
            }
            if (r <= pk) {
                a[s2][d] = -a[s1][d - 1] / ndu[pk + 1][r];
                dd += a[s2][d] * ndu[r][pk];
            }
            ders[d][r] = dd;
            std::swap(s1, s2);
        }
    }
    double fac = p;
    for (int d = 1; d <= std::min(nd, p); ++d) {
        for (int j = 0; j <= p; ++j) ders[d][j] *= fac;
        fac *= p - d;
    }
}

void derivs(const Flat& f, double t, Point2 out[3]) {
    t = wrap(f, t);
    const int k = findSpan(f, t);
    double N[3][SPLINE_MAX_DEGREE + 1];
    basisDers(f, k, t, 2, N);
    Point2 A[3] = {Point2::Zero(), Point2::Zero(), Point2::Zero()};
    double W[3] = {0, 0, 0};
    for (int j = 0; j <= f.p; ++j) {
        const int i = k - f.p + j;
        for (int d = 0; d < 3; ++d) {
            A[d] += N[d][j] * f.w[i] * f.P[i];
            W[d] += N[d][j] * f.w[i];
        }
    }
    out[0] = A[0] / W[0];
    out[1] = (A[1] - W[1] * out[0]) / W[0];
    out[2] = (A[2] - 2 * W[1] * out[1] - W[2] * out[0]) / W[0];
}

Point2 eval(const Flat& f, double t) {
    Point2 d[3];
    derivs(f, t, d);
    return d[0];
}

int rationalBasis(const Flat& f, double t, double R[SPLINE_MAX_DEGREE + 1]) {
    t = wrap(f, t);
    const int k = findSpan(f, t);
    double N[3][SPLINE_MAX_DEGREE + 1];
    basisDers(f, k, t, 0, N);
    double W = 0;
    for (int j = 0; j <= f.p; ++j) W += N[0][j] * f.w[k - f.p + j];
    for (int j = 0; j <= f.p; ++j) R[j] = N[0][j] * f.w[k - f.p + j] / W;
    return k - f.p;
}

double curvature(const Flat& f, double t) {
    Point2 d[3];
    derivs(f, t, d);
    const double s = d[1].norm();
    if (!(s > 0)) return 0.0;
    return cross2(d[1], d[2]) / (s * s * s);
}

std::vector<double> breaks(const Flat& f) {
    std::vector<double> out;
    const double tol = knotTol(f);
    out.push_back(f.a);
    for (double u : f.U)
        if (u > f.a + tol && u < f.b - tol && u - out.back() > tol) out.push_back(u);
    out.push_back(f.b);
    return out;
}

double closestParam(const Flat& f, const Point2& X) {
    const std::vector<double> br = breaks(f);
    double best = f.a, bestD = std::numeric_limits<double>::infinity();
    for (size_t s = 0; s + 1 < br.size(); ++s)
        for (int i = 0; i <= SPLINE_SPAN_SAMPLES; ++i) {
            const double t = br[s] + (br[s + 1] - br[s]) * i / SPLINE_SPAN_SAMPLES;
            const double d = (eval(f, t) - X).squaredNorm();
            if (d < bestD) bestD = d, best = t;
        }
    double t = best;
    for (int it = 0; it < SPLINE_NEWTON_ITER; ++it) {
        Point2 d[3];
        derivs(f, t, d);
        const Point2 r = d[0] - X;
        const double g = r.dot(d[1]), h = d[1].dot(d[1]) + r.dot(d[2]);
        if (!(h > 0)) break;
        double tn = t - g / h;
        if (f.periodic) tn = wrap(f, tn);
        else tn = std::clamp(tn, f.a, f.b);
        if ((eval(f, tn) - X).squaredNorm() > r.squaredNorm()) break;
        const bool done = std::abs(tn - t) <= 1e-15 * std::max(1.0, f.period());
        t = tn;
        if (done) break;
    }
    return t;
}

Flat insertKnot(const Flat& f, double t) {
    // The NURBS Book, A5.1 (one insertion) in homogeneous coordinates.
    const int p = f.p;
    int k = p;
    for (int i = 0; i + 1 < static_cast<int>(f.U.size()); ++i)
        if (f.U[i] <= t && t < f.U[i + 1]) k = i;
    const std::vector<HP> P = homog(f);
    std::vector<HP> Q(P.size() + 1);
    for (int i = 0; i <= k - p; ++i) Q[i] = P[i];
    for (int i = k - p + 1; i <= k; ++i) {
        const double den = f.U[i + p] - f.U[i];
        const double al = den > 0 ? (t - f.U[i]) / den : 0.0;
        Q[i] = {al * P[i].x + (1 - al) * P[i - 1].x, al * P[i].y + (1 - al) * P[i - 1].y, al * P[i].w + (1 - al) * P[i - 1].w};
    }
    for (int i = k + 1; i < static_cast<int>(Q.size()); ++i) Q[i] = P[i - 1];
    Flat g = f;
    g.U.insert(g.U.begin() + k + 1, t);
    dehomog(Q, g);
    return g;
}

Flat segment(const Flat& f, double s, double e) {
    Flat g = f;
    if (f.periodic) {
        const double len = e - s;
        s = wrap(f, s);
        e = s + len;
        g = twoPeriods(f);
    }
    const double tol = knotTol(f);
    auto snap = [&](double t) {
        for (double u : g.U)
            if (std::abs(u - t) <= tol) return u;
        return t;
    };
    s = snap(s);
    e = snap(e);
    while (multiplicityAt(g, s, 0.0) < g.p) g = insertKnot(g, s);
    while (multiplicityAt(g, e, 0.0) < g.p) g = insertKnot(g, e);
    int js = -1, je = -1;
    for (int i = 0; i < static_cast<int>(g.U.size()); ++i) {
        if (g.U[i] == s) js = i;
        if (g.U[i] == e && je < 0) je = i;
    }
    je += g.p - 1;
    Flat r;
    r.p = g.p;
    r.a = s, r.b = e;
    for (int i = js - g.p; i <= je - g.p; ++i) r.P.push_back(g.P[i]), r.w.push_back(g.w[i]);
    for (int i = 0; i <= g.p; ++i) r.U.push_back(s);
    for (int i = js + 1; i < je - g.p + 1; ++i)
        if (g.U[i] > s && g.U[i] < e) r.U.push_back(g.U[i]);
    for (int i = 0; i <= g.p; ++i) r.U.push_back(e);
    return r;
}

Flat reversed(const Flat& f) {
    Flat r = f;
    std::reverse(r.P.begin(), r.P.end());
    std::reverse(r.w.begin(), r.w.end());
    const size_t m = f.U.size();
    for (size_t i = 0; i < m; ++i) r.U[i] = f.a + f.b - f.U[m - 1 - i];
    return r;
}

Curve interpolate(const std::vector<Point2>& Q) {
    const int m = static_cast<int>(Q.size()) - 1;
    if (m < 1) return {};
    const int p = std::min(3, m);
    std::vector<double> d(m + 1, 0.0), ub(m + 1, 0.0);
    double D = 0;
    for (int k = 1; k <= m; ++k) {
        d[k] = (Q[k] - Q[k - 1]).norm();
        if (!(d[k] > 0)) return {};
        D += d[k];
    }
    for (int k = 1; k < m; ++k) ub[k] = ub[k - 1] + d[k] / D;
    ub[m] = 1.0;
    Flat f;
    f.p = p;
    f.P.assign(m + 1, Point2::Zero());
    f.w.assign(m + 1, 1.0);
    for (int i = 0; i <= p; ++i) f.U.push_back(0.0);
    for (int j = 1; j <= m - p; ++j) {
        double s = 0;
        for (int i = j; i <= j + p - 1; ++i) s += ub[i];
        f.U.push_back(s / p);
    }
    for (int i = 0; i <= p; ++i) f.U.push_back(1.0);
    f.a = 0, f.b = 1;
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(m + 1, m + 1);
    Eigen::MatrixXd B(m + 1, 2);
    for (int k = 0; k <= m; ++k) {
        const int sp = findSpan(f, ub[k]);
        double N[3][SPLINE_MAX_DEGREE + 1];
        basisDers(f, sp, ub[k], 0, N);
        for (int j = 0; j <= p; ++j) A(k, sp - p + j) = N[0][j];
        B(k, 0) = Q[k].x(), B(k, 1) = Q[k].y();
    }
    const Eigen::MatrixXd X = A.partialPivLu().solve(B);
    for (int i = 0; i <= m; ++i) f.P[i] = Point2(X(i, 0), X(i, 1));
    Curve c = compress(f);
    c.poles.front() = Q.front();  // exact ends (interpolation conditions at t = 0 and t = 1)
    c.poles.back() = Q.back();
    return c;
}

std::vector<double> flattenParams(const Flat& f, double tol) {
    const int p = f.p;
    double D = 0;
    for (size_t i = 0; i + 2 < f.P.size(); ++i) D = std::max(D, (f.P[i + 2] - 2 * f.P[i + 1] + f.P[i]).norm());
    double wmin = f.w[0], wmax = f.w[0];
    for (double w : f.w) wmin = std::min(wmin, w), wmax = std::max(wmax, w);
    int m = 1;
    if (p >= 2) {
        const double v = std::sqrt(p * (p - 1) * D * (wmax / wmin) / (8 * std::max(tol, 1e-300)));
        m = static_cast<int>(std::clamp(std::ceil(v), 2.0, static_cast<double>(SPLINE_MAX_SEGMENTS)));
    }
    const std::vector<double> br = breaks(f);
    std::vector<double> out{br.front()};
    for (size_t s = 0; s + 1 < br.size(); ++s)
        for (int i = 1; i <= m; ++i) out.push_back(br[s] + (br[s + 1] - br[s]) * i / m);
    return out;
}

double greenArea(const Flat& f, double s, double e) {
    const Gauss& G = gauss();
    double sum = 0;
    std::vector<double> cuts{s};
    for (double u : f.U)
        if (u > s && u < e && u - cuts.back() > knotTol(f)) cuts.push_back(u);
    cuts.push_back(e);
    for (size_t k = 0; k + 1 < cuts.size(); ++k) {
        const double h = (cuts[k + 1] - cuts[k]) / 2, mid = (cuts[k + 1] + cuts[k]) / 2;
        for (int i = 0; i < SPLINE_GAUSS_POINTS; ++i) {
            Point2 d[3];
            derivs(f, mid + h * G.x[i], d);
            sum += G.w[i] * h * cross2(d[0], d[1]) / 2;
        }
    }
    return sum;
}

double length(const Flat& f, double s, double e) {
    const Gauss& G = gauss();
    double sum = 0;
    std::vector<double> cuts{s};
    for (double u : f.U)
        if (u > s && u < e && u - cuts.back() > knotTol(f)) cuts.push_back(u);
    cuts.push_back(e);
    for (size_t k = 0; k + 1 < cuts.size(); ++k) {
        const double h = (cuts[k + 1] - cuts[k]) / 2, mid = (cuts[k + 1] + cuts[k]) / 2;
        for (int i = 0; i < SPLINE_GAUSS_POINTS; ++i) {
            Point2 d[3];
            derivs(f, mid + h * G.x[i], d);
            sum += G.w[i] * h * d[1].norm();
        }
    }
    return sum;
}

std::vector<double> roots(const Flat& f, const std::function<double(const Point2&)>& g, double tol) {
    const std::vector<double> br = breaks(f);
    std::vector<double> ts;
    for (size_t s = 0; s + 1 < br.size(); ++s)
        for (int i = (s == 0 ? 0 : 1); i <= SPLINE_SPAN_SAMPLES; ++i) ts.push_back(br[s] + (br[s + 1] - br[s]) * i / SPLINE_SPAN_SAMPLES);
    if (f.periodic) ts.pop_back();  // b ≡ a
    const int N = static_cast<int>(ts.size());
    std::vector<double> gv(N);
    for (int i = 0; i < N; ++i) gv[i] = g(eval(f, ts[i]));
    std::vector<double> found;
    auto G = [&](double t) { return g(eval(f, t)); };
    const int last = f.periodic ? N : N - 1;
    for (int i = 0; i < N; ++i)
        if (std::abs(gv[i]) <= tol) found.push_back(ts[i]);
    for (int i = 0; i < last; ++i) {
        const int j = (i + 1) % N;
        double lo = ts[i], hi = j == 0 ? f.b : ts[j];
        double glo = gv[i], ghi = gv[j];
        if (std::abs(glo) <= tol || std::abs(ghi) <= tol) continue;
        if ((glo < 0) != (ghi < 0)) {  // root: bisection
            for (int it = 0; it < 60; ++it) {
                const double mid = (lo + hi) / 2, gm = G(mid);
                if ((gm < 0) == (glo < 0)) lo = mid, glo = gm;
                else hi = mid;
            }
            found.push_back(wrap(f, (lo + hi) / 2));
        }
    }
    // Tangential minima of |g| (§2.7.5): golden section around a local sample minimum.
    for (int i = 0; i < N; ++i) {
        const bool interior = f.periodic || (i > 0 && i + 1 < N);
        if (!interior) continue;
        const int ip = (i - 1 + N) % N, in = (i + 1) % N;
        if (!(std::abs(gv[i]) <= std::abs(gv[ip]) && std::abs(gv[i]) <= std::abs(gv[in]))) continue;
        if ((gv[ip] < 0) != (gv[in] < 0)) continue;  // sign change — handled above
        double lo = i == 0 ? ts[N - 1] - f.period() : ts[ip], hi = in == 0 ? f.b : ts[in];
        if (hi < lo) hi += f.period();
        const double gr = (std::sqrt(5.0) - 1) / 2;
        double x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
        double f1 = std::abs(G(x1)), f2 = std::abs(G(x2));
        for (int it = 0; it < 60; ++it) {
            if (f1 < f2) hi = x2, x2 = x1, f2 = f1, x1 = hi - gr * (hi - lo), f1 = std::abs(G(x1));
            else lo = x1, x1 = x2, f1 = f2, x2 = lo + gr * (hi - lo), f2 = std::abs(G(x2));
        }
        const double tm = f1 < f2 ? x1 : x2;
        if (std::min(f1, f2) <= tol) found.push_back(wrap(f, tm));
    }
    std::sort(found.begin(), found.end());
    std::vector<double> out;
    for (double t : found)
        if (out.empty() || (eval(f, t) - eval(f, out.back())).norm() > tol) out.push_back(t);
    if (f.periodic && out.size() >= 2 && (eval(f, out.front()) - eval(f, out.back())).norm() <= tol) out.pop_back();
    return out;
}

std::vector<std::pair<double, double>> intersections(const Flat& A, const Flat& B, double tol) {
    std::vector<std::pair<double, double>> out;
    const Point2 a0 = eval(A, A.a), a1 = eval(A, A.b), b0 = eval(B, B.a), b1 = eval(B, B.b);
    const double ftol = std::max(tol, 1e-3 * std::max((a1 - a0).norm(), (b1 - b0).norm()));
    const auto ta = flattenParams(A, ftol), tb = flattenParams(B, ftol);
    std::vector<Point2> pa, pb;
    for (double t : ta) pa.push_back(eval(A, t));
    for (double t : tb) pb.push_back(eval(B, t));
    auto add = [&](double s, double t) {
        const Point2 X = eval(A, s);
        for (const auto& o : out)
            if ((eval(A, o.first) - X).norm() <= 10 * tol) return;
        out.push_back({s, t});
    };
    auto clampTo = [](const Flat& f, double t) { return f.periodic ? wrap(f, t) : std::clamp(t, f.a, f.b); };
    for (size_t i = 0; i + 1 < pa.size(); ++i)
        for (size_t j = 0; j + 1 < pb.size(); ++j) {
            const Point2 r = pa[i + 1] - pa[i], q = pb[j + 1] - pb[j];
            const double den = cross2(r, q);
            if (std::abs(den) <= 1e-300) continue;
            const double u = cross2(pb[j] - pa[i], q) / den, v = cross2(pb[j] - pa[i], r) / den;
            const double slack = 0.1;
            if (u < -slack || u > 1 + slack || v < -slack || v > 1 + slack) continue;
            double s = ta[i] + std::clamp(u, 0.0, 1.0) * (ta[i + 1] - ta[i]);
            double t = tb[j] + std::clamp(v, 0.0, 1.0) * (tb[j + 1] - tb[j]);
            for (int it = 0; it < SPLINE_NEWTON_ITER; ++it) {
                Point2 da[3], db[3];
                derivs(A, s, da);
                derivs(B, t, db);
                const Point2 F = da[0] - db[0];
                // [A'(s), −B'(t)]·(Δs, Δt) = −F (Cramer).
                const double det = -da[1].x() * db[1].y() + da[1].y() * db[1].x();
                if (std::abs(det) <= 1e-300) break;
                const double ds = (F.x() * db[1].y() - F.y() * db[1].x()) / det;
                const double dt = (-da[1].x() * F.y() + da[1].y() * F.x()) / det;
                s = clampTo(A, s + ds);
                t = clampTo(B, t + dt);
            }
            if ((eval(A, s) - eval(B, t)).norm() <= tol) add(s, t);
        }
    if (!A.periodic)
        for (double s : {A.a, A.b}) {
            const double t = closestParam(B, eval(A, s));
            if ((eval(B, t) - eval(A, s)).norm() <= tol) add(s, t);
        }
    if (!B.periodic)
        for (double t : {B.a, B.b}) {
            const double s = closestParam(A, eval(B, t));
            if ((eval(A, s) - eval(B, t)).norm() <= tol) add(s, t);
        }
    return out;
}

bool selfIntersects(const Flat& f, double tol) {
    const auto ts = flattenParams(f, tol);
    std::vector<Point2> P;
    for (double t : ts) P.push_back(eval(f, t));
    const size_t n = P.size() - 1;  // chords
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 2; j < n; ++j) {
            if (f.periodic && i == 0 && j == n - 1) continue;
            const Point2 r = P[i + 1] - P[i], q = P[j + 1] - P[j];
            const double den = cross2(r, q);
            if (den == 0) continue;
            const double u = cross2(P[j] - P[i], q) / den, v = cross2(P[j] - P[i], r) / den;
            if (u > 0 && u < 1 && v > 0 && v < 1) return true;
        }
    return false;
}

}  // namespace construe::spline
