// 2D catalog: stage 1 entities and constraints → equations (spec/solver.md §4.2–§4.3, §4.10, §4.14).
#include "solver/model.h"
#include "solver/dual.h"
#include "base/geom2.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <limits>
#include <type_traits>

namespace construe::solver {

namespace {

template <class T>
struct V2 {
    T x, y;
};

template <class T>
V2<T> sub(const V2<T>& a, const V2<T>& b) {
    return {a.x - b.x, a.y - b.y};
}
template <class T>
T cross(const V2<T>& a, const V2<T>& b) {
    return a.x * b.y - a.y * b.x;
}
template <class T>
T dot(const V2<T>& a, const V2<T>& b) {
    return a.x * b.x + a.y * b.y;
}
template <class T>
T norm(const V2<T>& a) {
    return dsqrt(a.x * a.x + a.y * a.y);
}
template <class T>
T normEta(const V2<T>& a, double L) {
    const double e = ETA_REG * L;
    return dsqrt(a.x * a.x + a.y * a.y + T(e * e));
}
// Signed distance from P to the line (a, b), positive on the left.
template <class T>
T signedDist(const V2<T>& P, const V2<T>& a, const V2<T>& b, double L) {
    const V2<T> d = sub(b, a);
    return cross(d, sub(P, a)) / normEta(d, L);
}

template <class T>
V2<T> pt(const T* x, int i) {
    return {x[i], x[i + 1]};
}

template <class F>
struct GenImpl final : GenFn {
    F f;
    int n;
    GenImpl(F fn, int count) : f(std::move(fn)), n(count) {}
    double value(const double* x) const override { return f(x); }
    void gradient(const double* x, double* g) const override {
        Dual xs[DUAL_N];
        for (int i = 0; i < n; ++i) xs[i] = Dual::variable(x[i], i);
        const Dual r = f(static_cast<const Dual*>(xs));
        for (int i = 0; i < n; ++i) g[i] = r.d[i];
    }
};

template <class F>
Equation general(uint64_t owner, int k, std::vector<int> params, F f, int group = 1) {
    Equation e;
    e.group = group;
    e.owner = owner;
    e.k = k;
    e.kind = EqKind::General;
    const int n = static_cast<int>(params.size());
    e.params = std::move(params);
    e.fn = std::make_shared<GenImpl<F>>(std::move(f), n);
    return e;
}

Equation subst(uint64_t owner, int k, int i, int j, double c) {
    Equation e;
    e.owner = owner;
    e.k = k;
    e.kind = EqKind::Subst;
    e.i = i;
    e.j = j;
    e.c = c;
    return e;
}

Equation fixation(uint64_t owner, int k, int i, double c) {
    Equation e;
    e.owner = owner;
    e.k = k;
    e.kind = EqKind::Fixation;
    e.i = i;
    e.c = c;
    return e;
}

// Curve parameters for radius formulas: Circle — [ox, oy, r], Arc — [ox, oy, sx, sy].
struct CurveParams {
    bool arc = false;
    std::vector<int> p;  // ox, oy, (r | sx, sy)
};

CurveParams curveParams(const Model& m, const ModelEntity& e) {
    CurveParams cp;
    auto [ox, oy] = m.pointParams(e.center);
    if (e.kind == EntityKind::Circle) {
        cp.p = {ox, oy, e.pr};
    } else {
        cp.arc = true;
        auto [sx, sy] = m.pointParams(e.start);
        cp.p = {ox, oy, sx, sy};
    }
    return cp;
}

// Curve radius from the slice of x starting at offset.
template <class T>
T curveR(const T* x, int off, bool arc) {
    if (!arc) return x[off + 2];
    return norm(sub(pt(x, off + 2), pt(x, off)));
}

std::vector<int> lineParams(const Model& m, const ModelEntity& l) {
    auto [ax, ay] = m.pointParams(l.p1);
    auto [bx, by] = m.pointParams(l.p2);
    return {ax, ay, bx, by};
}

// Ellipse parameters: [ox, oy, ux, uy, b] (§2.3, S4-SKT-004).
std::vector<int> ellipseParams(const Model& m, const ModelEntity& e) {
    auto [ox, oy] = m.pointParams(e.center);
    return {ox, oy, e.pux, e.puy, e.pb};
}

std::vector<int> concat(std::vector<int> a, const std::vector<int>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

constexpr int ELLIPSE_TANGENT_SAMPLES = 64;  // §5: samples of τ for the extremum in tangency with an ellipse
constexpr int ELLIPSE_REFINE_ITER = 60;      // §5: golden-section iterations

// Closed carrier of a curve or ellipse for tangency with an ellipse (§4.2.22, S4-SKT-007): parameters of the slice
// x[off …] — Circle [ox, oy, r], Arc [ox, oy, sx, sy], Ellipse [ox, oy, ux, uy, b].
struct Carrier {
    enum class Kind { Circle, Arc, Ellipse } kind = Kind::Circle;
    int off = 0;
    int size() const { return kind == Kind::Ellipse ? 5 : kind == Kind::Arc ? 4 : 3; }
};

Carrier carrierOf(const ModelEntity& e, int off) {
    Carrier c;
    c.off = off;
    c.kind = isEllipseKind(e.kind) ? Carrier::Kind::Ellipse : e.kind == EntityKind::Arc ? Carrier::Kind::Arc : Carrier::Kind::Circle;
    return c;
}

std::vector<int> carrierParams(const Model& m, const ModelEntity& e) {
    if (isEllipseKind(e.kind)) {
        auto [ox, oy] = m.pointParams(e.center);
        return {ox, oy, e.pux, e.puy, e.pb};
    }
    return curveParams(m, e).p;
}

// Implicit function φ(P) of the carrier: length, positive outside (circle — ‖P − o‖ − R, ellipse — (ρ − 1)·sqrt(a·b)).
template <class T>
T carrierPhi(const T* x, const Carrier& c, const V2<T>& P, double L) {
    const V2<T> o = pt(x, c.off);
    if (c.kind != Carrier::Kind::Ellipse) return norm(sub(P, o)) - curveR(x, c.off, c.kind == Carrier::Kind::Arc);
    const auto w = sub(P, o);
    const V2<T> u{x[c.off + 2], x[c.off + 3]};
    const auto a = normEta(u, L);
    const auto bb = x[c.off + 4];
    const auto s = dot(w, u) / a;
    const auto t = cross(u, w) / a;
    const auto rho = dsqrt((s / a) * (s / a) + (t / bb) * (t / bb));
    return (rho - T(1.0)) * dsqrt(a * bb);
}

// Point of the carrier at parameter τ (circle — angle, ellipse — o + a·cos τ·û + b·sin τ·v̂).
template <class T>
V2<T> carrierAt(const T* x, const Carrier& c, double tau, double L) {
    const V2<T> o = pt(x, c.off);
    const double ct = std::cos(tau), st = std::sin(tau);
    if (c.kind != Carrier::Kind::Ellipse) {
        const auto r = curveR(x, c.off, c.kind == Carrier::Kind::Arc);
        return {o.x + r * ct, o.y + r * st};
    }
    const V2<T> u{x[c.off + 2], x[c.off + 3]};
    const auto a = normEta(u, L);
    const auto bb = x[c.off + 4];
    // a·cos τ·û + b·sin τ·v̂, û = u/a, v̂ = rot90(û)
    return {o.x + u.x * ct - bb * (u.y / a) * st, o.y + u.y * ct + bb * (u.x / a) * st};
}

// τ* = argmin (wantMax: argmax) over τ of φ_target(K(τ)) by values (§4.2.22): samples + golden section.
double extremumTau(const double* x, const Carrier& k, const Carrier& target, bool wantMax, double L) {
    auto f = [&](double tau) {
        const double v = carrierPhi(x, target, carrierAt(x, k, tau, L), L);
        return wantMax ? -v : v;
    };
    const int N = ELLIPSE_TANGENT_SAMPLES;
    int best = 0;
    double bestV = std::numeric_limits<double>::infinity();
    for (int i = 0; i < N; ++i) {
        const double v = f(2 * PI * i / N);
        if (v < bestV) bestV = v, best = i;
    }
    double lo = 2 * PI * (best - 1) / N, hi = 2 * PI * (best + 1) / N;
    const double gr = (std::sqrt(5.0) - 1) / 2;
    double x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
    double f1 = f(x1), f2 = f(x2);
    for (int it = 0; it < ELLIPSE_REFINE_ITER; ++it) {
        if (f1 < f2) {
            hi = x2, x2 = x1, f2 = f1;
            x1 = hi - gr * (hi - lo), f1 = f(x1);
        } else {
            lo = x1, x1 = x2, f1 = f2;
            x2 = lo + gr * (hi - lo), f2 = f(x2);
        }
    }
    return f1 < f2 ? x1 : x2;
}

// Unit normal of the carrier at its point S (circle — radial, ellipse — normalized gradient (s/a²)û + (t/b²)v̂).
template <class T>
V2<T> carrierNormal(const T* x, const Carrier& c, const V2<T>& S, double L) {
    const V2<T> o = pt(x, c.off);
    const auto w = sub(S, o);
    if (c.kind != Carrier::Kind::Ellipse) {
        const auto n = normEta(w, L);
        return {w.x / n, w.y / n};
    }
    const V2<T> u{x[c.off + 2], x[c.off + 3]};
    const auto a = normEta(u, L);
    const auto bb = x[c.off + 4];
    const auto s = dot(w, u) / a, t = cross(u, w) / a;
    const auto gs = s / (a * a), gt = t / (bb * bb);
    const V2<T> g{gs * u.x / a - gt * u.y / a, gs * u.y / a + gt * u.x / a};
    const auto n = dsqrt(g.x * g.x + g.y * g.y) + T(1e-300);
    return {g.x / n, g.y / n};
}

// ---------------------------------------------------------------- splines (§4.2.23, S4A-SPL-002)

// End of an open spline seen from the joint: the first three poles in the direction leaving the end and the constants
// of the end derivatives of the leaving parametrization (§4.2.23): C'(0) = c1·(H1 − H0),
// C''(0) = c2·(q1·(H2 − H1) − q0·(H1 − H0)) in homogeneous coordinates H = (w·P, w).
struct SplineEnd {
    std::vector<EntityId> poles;  // 2 (degree 1) or 3 leaving poles
    double w[3] = {1, 1, 1};
    double c1 = 1, c2 = 0, q0 = 1, q1 = 1;
};

SplineEnd splineEnd(const ModelEntity& e, bool atStart) {
    const spline::Flat f = atStart ? *e.curve : spline::reversed(*e.curve);
    SplineEnd se;
    const int p = f.p;
    const int n = static_cast<int>(e.poles.size());
    const int k = std::min(3, n);
    for (int i = 0; i < k; ++i) {
        se.poles.push_back(atStart ? e.poles[i] : e.poles[n - 1 - i]);
        se.w[i] = f.w[i];
    }
    const auto& U = f.U;
    se.c1 = p / (U[p + 1] - U[1]);
    se.q0 = 1 / (U[p + 1] - U[1]);
    if (p >= 2) {
        se.c2 = p * (p - 1) / (U[p + 1] - U[2]);
        se.q1 = 1 / (U[p + 2] - U[2]);
    }
    if (p < 2) se.poles.resize(2);
    return se;
}

std::vector<int> splineEndParams(const Model& m, const SplineEnd& se) {
    std::vector<int> out;
    for (EntityId q : se.poles) {
        auto [qx, qy] = m.pointParams(q);
        out.push_back(qx);
        out.push_back(qy);
    }
    return out;
}

// Leaving tangent direction H1 − H0 (direction of P1 − P0 for any positive weights).
template <class T>
V2<T> leavingTangent(const T* x, int off) {
    return sub(pt(x, off + 2), pt(x, off));
}

// Signed curvature of the leaving parametrization at the end (§4.2.23); degree 1 — 0.
template <class T>
T leavingCurvature(const T* x, int off, const SplineEnd& se, double L) {
    if (se.poles.size() < 3) return T(0.0);
    T hx[3], hy[3];
    for (int i = 0; i < 3; ++i) hx[i] = x[off + 2 * i] * se.w[i], hy[i] = x[off + 2 * i + 1] * se.w[i];
    const double W1 = se.c1 * (se.w[1] - se.w[0]);
    const double W2 = se.c2 * (se.q1 * (se.w[2] - se.w[1]) - se.q0 * (se.w[1] - se.w[0]));
    const V2<T> C{x[off], x[off + 1]};
    const V2<T> A1{se.c1 * (hx[1] - hx[0]), se.c1 * (hy[1] - hy[0])};
    const V2<T> A2{se.c2 * (se.q1 * (hx[2] - hx[1]) - se.q0 * (hx[1] - hx[0])), se.c2 * (se.q1 * (hy[2] - hy[1]) - se.q0 * (hy[1] - hy[0]))};
    const double W0 = se.w[0];
    const V2<T> C1{(A1.x - W1 * C.x) / W0, (A1.y - W1 * C.y) / W0};
    const V2<T> C2{(A2.x - 2.0 * W1 * C1.x - W2 * C.x) / W0, (A2.y - 2.0 * W1 * C1.y - W2 * C.y) / W0};
    const auto s = normEta(C1, L);
    return cross(C1, C2) / (s * s * s);
}

// Point of the spline at the hidden parameter (§4.2.23): params [px, py, xt, poles of the expanded curve (x, y)…].
struct SplinePointFn final : GenFn {
    std::shared_ptr<const spline::Flat> base;
    int axis = 0;
    double L = 1;
    spline::Flat at(const double* x) const {
        spline::Flat f = *base;
        for (size_t i = 0; i < f.P.size(); ++i) f.P[i] = Point2(x[3 + 2 * i], x[4 + 2 * i]);
        return f;
    }
    double param(const double* x) const { return base->a + x[2] / L * (base->b - base->a); }
    double value(const double* x) const override {
        const Point2 C = spline::eval(at(x), param(x));
        return (C[axis] - x[axis]) / L;
    }
    void gradient(const double* x, double* g) const override {
        const spline::Flat f = at(x);
        const double t = param(x);
        const size_t n = 3 + 2 * f.P.size();
        for (size_t i = 0; i < n; ++i) g[i] = 0.0;
        g[axis] = -1.0 / L;
        Point2 d[3];
        spline::derivs(f, t, d);
        g[2] = d[1][axis] * (base->b - base->a) / (L * L);
        double R[spline::SPLINE_MAX_DEGREE + 1];
        const int i0 = spline::rationalBasis(f, t, R);
        for (int j = 0; j <= f.p; ++j) g[3 + 2 * (i0 + j) + axis] = R[j] / L;
    }
};

}  // namespace

// ---------------------------------------------------------------- Equation

double Equation::residual(const std::vector<double>& x, double L) const {
    switch (kind) {
        case EqKind::Subst: return (x[i] - x[j] - c) / L;
        case EqKind::Fixation: return (x[i] - c) / L;
        case EqKind::General: {
            // More than DUAL_N parameters — only equations with an analytic gradient (point on a spline, §4.2.23).
            double stack[DUAL_N];
            std::vector<double> heap;
            double* buf = params.size() <= DUAL_N ? stack : (heap.resize(params.size()), heap.data());
            for (size_t s = 0; s < params.size(); ++s) buf[s] = x[params[s]];
            return fn->value(buf);
        }
    }
    return 0.0;
}

void Equation::gradient(const std::vector<double>& x, double L, std::vector<std::pair<int, double>>& out) const {
    out.clear();
    switch (kind) {
        case EqKind::Subst:
            out.push_back({i, 1.0 / L});
            out.push_back({j, -1.0 / L});
            return;
        case EqKind::Fixation: out.push_back({i, 1.0 / L}); return;
        case EqKind::General: {
            double bufS[DUAL_N], gS[DUAL_N];
            std::vector<double> bufH, gH;
            const bool big = params.size() > DUAL_N;
            if (big) bufH.resize(params.size()), gH.resize(params.size());
            double* buf = big ? bufH.data() : bufS;
            double* g = big ? gH.data() : gS;
            for (size_t s = 0; s < params.size(); ++s) buf[s] = x[params[s]];
            fn->gradient(buf, g);
            for (size_t s = 0; s < params.size(); ++s) {
                bool merged = false;
                for (auto& kv : out)
                    if (kv.first == params[s]) {
                        kv.second += g[s];
                        merged = true;
                    }
                if (!merged) out.push_back({params[s], g[s]});
            }
            return;
        }
    }
}

std::vector<int> Equation::allParams() const {
    if (kind == EqKind::Subst) return {i, j};
    if (kind == EqKind::Fixation) return {i};
    std::vector<int> v = params;
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// ---------------------------------------------------------------- L

double computeL(const Problem& p) {
    double minx = std::numeric_limits<double>::infinity(), miny = minx;
    double maxx = -minx, maxy = -minx;
    auto add = [&](double x, double y) {
        minx = std::min(minx, x);
        maxx = std::max(maxx, x);
        miny = std::min(miny, y);
        maxy = std::max(maxy, y);
    };
    for (const auto& [id, e] : p.entities()) {
        if (e.kind == EntityKind::Point) add(e.x, e.y);
        if (e.kind == EntityKind::Circle) {
            const auto& c = p.entities().at(e.center);
            add(c.x + e.r, c.y);
            add(c.x - e.r, c.y);
            add(c.x, c.y + e.r);
            add(c.x, c.y - e.r);
        }
        if (isEllipseKind(e.kind)) {  // §4.1: center ± (m, 0), center ± (0, m), m = max(‖u‖, b)
            const auto& c = p.entities().at(e.center);
            const double mr = std::max(std::hypot(e.ux, e.uy), e.b);
            add(c.x + mr, c.y);
            add(c.x - mr, c.y);
            add(c.x, c.y + mr);
            add(c.x, c.y - mr);
        }
    }
    double diag = 0.0;
    if (minx <= maxx) diag = std::hypot(maxx - minx, maxy - miny);
    double M = 0.0;
    for (const auto& [id, c] : p.constraints()) {
        if (!c.driving || !c.value) continue;
        switch (c.type) {
            case ConstraintType::Distance:
            case ConstraintType::Length:
            case ConstraintType::Radius:
            case ConstraintType::Diameter:
            case ConstraintType::ArcLength:
            case ConstraintType::SemiMajor:
            case ConstraintType::SemiMinor:
            case ConstraintType::PathLength: M = std::max(M, *c.value); break;
            case ConstraintType::DistanceX:
            case ConstraintType::DistanceY: M = std::max(M, std::abs(*c.value)); break;
            default: break;
        }
    }
    return std::max({L_MIN, diag, M});
}

// ---------------------------------------------------------------- constraint equations

std::vector<Equation> constraintEquations(const Model& m, const ConstraintSpec& c, const ValueOverrides* ov) {
    std::vector<Equation> eq;
    const double L = m.L;
    const uint64_t id = c.id;
    auto ent = [&](size_t i) -> const ModelEntity& { return m.entities.at(c.refs[i]); };
    auto val = [&]() {
        if (ov) {
            auto it = ov->value.find(id);
            if (it != ov->value.end()) return it->second;
        }
        return c.value.value_or(0.0);
    };
    auto target = [&]() -> const std::vector<double>& {
        if (ov) {
            auto it = ov->target.find(id);
            if (it != ov->target.end()) return it->second;
        }
        return c.target;
    };
    const Branch br = c.branch.value_or(Branch{});

    switch (c.type) {
        case ConstraintType::Coincident: {
            auto [px, py] = m.pointParams(c.refs[0]);
            auto [qx, qy] = m.pointParams(c.refs[1]);
            eq.push_back(subst(id, 0, px, qx, 0.0));
            eq.push_back(subst(id, 1, py, qy, 0.0));
            break;
        }
        case ConstraintType::Horizontal:
        case ConstraintType::Vertical: {
            const int slot = c.type == ConstraintType::Horizontal ? 1 : 0;
            if (c.refs.size() == 2 && ent(1).kind == EntityKind::Line) {
                // Point aligned with the segment midpoint (§4.2.2): 2·P − a − b = 0 along one coordinate.
                const ModelEntity& l = ent(1);
                std::vector<int> params = {m.param(c.refs[0], slot), m.param(l.p1, slot), m.param(l.p2, slot)};
                eq.push_back(general(id, 0, params, [L](const auto* x) { return (2.0 * x[0] - x[1] - x[2]) / (2.0 * L); }));
                break;
            }
            EntityId a, b;
            if (c.refs.size() == 1) {
                a = ent(0).p1;
                b = ent(0).p2;
            } else {
                a = c.refs[0];
                b = c.refs[1];
            }
            eq.push_back(subst(id, 0, m.param(a, slot), m.param(b, slot), 0.0));
            break;
        }
        case ConstraintType::Parallel:
        case ConstraintType::Perpendicular: {
            const bool par = c.type == ConstraintType::Parallel;
            auto params = concat(lineParams(m, ent(0)), lineParams(m, ent(1)));
            eq.push_back(general(id, 0, params, [L, par](const auto* x) {
                const auto d1 = sub(pt(x, 2), pt(x, 0));
                const auto d2 = sub(pt(x, 6), pt(x, 4));
                const auto num = par ? cross(d1, d2) : dot(d1, d2);
                return num / (normEta(d1, L) * normEta(d2, L));
            }));
            break;
        }
        case ConstraintType::Tangent:
        case ConstraintType::Curvature:
            if (ent(1).kind == EntityKind::BSpline) {
                // At a spline end (§4.2.23, S4A-SPL-002): the joint is the shared end (coincident class), otherwise
                // the nearest pair of ends at the current values.
                const ModelEntity& a = ent(0);
                const ModelEntity& b = ent(1);
                auto endsOf = [](const ModelEntity& e) -> std::vector<EntityId> {
                    if (e.kind == EntityKind::Line) return {e.p1, e.p2};
                    if (e.kind == EntityKind::Arc) return {e.start, e.end};
                    return {e.poles.front(), e.poles.back()};
                };
                const auto ea = endsOf(a), eb = endsOf(b);
                int ia = -1, ib = -1;
                for (int i = 0; i < 2 && ia < 0; ++i)
                    for (int j = 0; j < 2 && ia < 0; ++j)
                        if (m.rootOf(ea[i]) == m.rootOf(eb[j])) ia = i, ib = j;
                if (ia < 0) {
                    double best = std::numeric_limits<double>::infinity();
                    for (int i = 0; i < 2; ++i)
                        for (int j = 0; j < 2; ++j) {
                            auto [ax, ay] = m.pointParams(ea[i]);
                            auto [bx, by] = m.pointParams(eb[j]);
                            const double d = std::hypot(m.x0[ax] - m.x0[bx], m.x0[ay] - m.x0[by]);
                            if (d < best) best = d, ia = i, ib = j;
                        }
                }
                const SplineEnd sb = splineEnd(b, ib == 0);
                const auto pb = splineEndParams(m, sb);
                const bool curv = c.type == ConstraintType::Curvature;
                if (a.kind == EntityKind::Line) {
                    if (curv) {
                        eq.push_back(general(id, 0, pb, [L, sb](const auto* x) { return leavingCurvature(x, 0, sb, L) * L; }));
                    } else {
                        auto params = concat(lineParams(m, a), pb);
                        eq.push_back(general(id, 0, params, [L](const auto* x) {
                            const auto d = sub(pt(x, 2), pt(x, 0));
                            const auto t = leavingTangent(x, 4);
                            return cross(d, t) / (normEta(d, L) * normEta(t, L));
                        }));
                    }
                } else if (a.kind == EntityKind::Arc) {
                    auto [ox, oy] = m.pointParams(a.center);
                    auto [sx, sy] = m.pointParams(a.start);
                    auto [jx, jy] = m.pointParams(ea[ia]);
                    if (curv) {
                        // Arc leaving curvature: +1/R from start (counterclockwise), −1/R from end.
                        const double sign = ia == 0 ? 1.0 : -1.0;
                        auto params = concat({ox, oy, sx, sy}, pb);
                        eq.push_back(general(id, 0, params, [L, sb, sign](const auto* x) {
                            const auto R = normEta(sub(pt(x, 2), pt(x, 0)), L);
                            return (sign / R + leavingCurvature(x, 4, sb, L)) * L;
                        }));
                    } else {
                        auto params = concat({ox, oy, jx, jy}, pb);
                        eq.push_back(general(id, 0, params, [L](const auto* x) {
                            const auto r = sub(pt(x, 2), pt(x, 0));
                            const auto t = leavingTangent(x, 4);
                            return dot(r, t) / (normEta(r, L) * normEta(t, L));
                        }));
                    }
                } else {
                    const SplineEnd sa = splineEnd(a, ia == 0);
                    const auto pa = splineEndParams(m, sa);
                    const int off = static_cast<int>(pa.size());
                    auto params = concat(pa, pb);
                    if (curv) {
                        eq.push_back(general(id, 0, params, [L, sa, sb, off](const auto* x) {
                            return (leavingCurvature(x, 0, sa, L) + leavingCurvature(x, off, sb, L)) * L;
                        }));
                    } else {
                        eq.push_back(general(id, 0, params, [L, off](const auto* x) {
                            const auto t1 = leavingTangent(x, 0);
                            const auto t2 = leavingTangent(x, off);
                            return cross(t1, t2) / (normEta(t1, L) * normEta(t2, L));
                        }));
                    }
                }
                break;
            }
            {
            const ModelEntity& a = ent(0);
            const ModelEntity& b = ent(1);
            if (a.kind == EntityKind::Line && b.kind == EntityKind::Arc) {
                // Shared point of a segment end and an arc end → tangency at that point: radius ⟂ segment (§4.2.5).
                EntityId shared = 0;
                for (EntityId q : {b.start, b.end})
                    for (EntityId lp : {a.p1, a.p2})
                        if (!shared && m.rootOf(q) == m.rootOf(lp)) shared = q;
                if (shared) {
                    auto [ox, oy] = m.pointParams(b.center);
                    auto [sx, sy] = m.pointParams(shared);
                    auto params = concat(lineParams(m, a), std::vector<int>{ox, oy, sx, sy});
                    eq.push_back(general(id, 0, params, [L](const auto* x) {
                        const auto d = sub(pt(x, 2), pt(x, 0));
                        const auto len = norm(d);
                        return dot(sub(pt(x, 6), pt(x, 4)), d) / (len * L + 1e-300);
                    }));
                    break;
                }
            }
            // Shared point of the ends (coincident class) of a segment/arc/ellipse arc and an ellipse arc → normals of
            // both carriers are parallel at the joint (§4.2.22, S4-SKT-007; like §4.2.5–4.2.6).
            auto endsOf = [](const ModelEntity& e) -> std::vector<EntityId> {
                if (e.kind == EntityKind::Line) return {e.p1, e.p2};
                if (e.kind == EntityKind::Arc || e.kind == EntityKind::EllipseArc) return {e.start, e.end};
                return {};
            };
            if (a.kind == EntityKind::EllipseArc || b.kind == EntityKind::EllipseArc) {
                EntityId shared = 0;
                for (EntityId q : endsOf(a.kind == EntityKind::Line ? b : a))
                    for (EntityId q2 : endsOf(a.kind == EntityKind::Line ? a : b))
                        if (!shared && m.rootOf(q) == m.rootOf(q2)) shared = q;
                if (shared) {
                    auto [sx, sy] = m.pointParams(shared);
                    if (a.kind == EntityKind::Line) {
                        const Carrier cb = carrierOf(b, 4);
                        auto params = concat(concat(lineParams(m, a), carrierParams(m, b)), std::vector<int>{sx, sy});
                        const int so = 4 + cb.size();
                        eq.push_back(general(id, 0, params, [L, cb, so](const auto* x) {
                            const auto d = sub(pt(x, 2), pt(x, 0));
                            const auto n = carrierNormal(x, cb, pt(x, so), L);
                            return dot(n, d) / normEta(d, L);
                        }));
                    } else {
                        const Carrier c1 = carrierOf(a, 0), c2 = carrierOf(b, carrierOf(a, 0).size());
                        auto params = concat(concat(carrierParams(m, a), carrierParams(m, b)), std::vector<int>{sx, sy});
                        const int so = c1.size() + c2.size();
                        eq.push_back(general(id, 0, params, [L, c1, c2, so](const auto* x) {
                            const auto S = pt(x, so);
                            return cross(carrierNormal(x, c1, S, L), carrierNormal(x, c2, S, L));
                        }));
                    }
                    break;
                }
            }
            if (a.kind != EntityKind::Line && (isEllipseKind(a.kind) || isEllipseKind(b.kind))) {
                // Curve or ellipse and ellipse (§4.2.22, S4-SKT-007): extremum over τ of φ₂(K₁(τ)) / φ₁(K₂(τ)); τ* —
                // by values, then a constant (envelope theorem).
                const Carrier c1 = carrierOf(a, 0), c2 = carrierOf(b, carrierOf(a, 0).size());
                auto params = concat(carrierParams(m, a), carrierParams(m, b));
                const int n = static_cast<int>(params.size());
                const Contact contact = br.contact;
                eq.push_back(general(id, 0, params, [L, c1, c2, n, contact](const auto* x) {
                    double v[DUAL_N];
                    for (int i = 0; i < n; ++i) v[i] = value(x[i]);
                    // External: min over K1 of φ2; Internal1 (K2 inside K1): max over K2 of φ1; Internal2: max over K1 of φ2.
                    const bool overK2 = contact == Contact::Internal1;
                    const Carrier& k = overK2 ? c2 : c1;
                    const Carrier& target = overK2 ? c1 : c2;
                    const double tau = extremumTau(v, k, target, contact != Contact::External, L);
                    return carrierPhi(x, target, carrierAt(x, k, tau, L), L) / L;
                }));
                break;
            }
            if (a.kind == EntityKind::Line && isEllipseKind(b.kind)) {
                // Segment and ellipse (§4.2.21): sd(o, l) − side·h, h — ellipse support function along segment normal.
                auto params = concat(lineParams(m, a), ellipseParams(m, b));
                const double side = br.side;
                eq.push_back(general(id, 0, params, [L, side](const auto* x) {
                    using T = std::remove_cv_t<std::remove_reference_t<decltype(x[0])>>;
                    const auto d = sub(pt(x, 2), pt(x, 0));
                    const auto dl = normEta(d, L);
                    const V2<T> n{-d.y / dl, d.x / dl};
                    const V2<T> u{x[6], x[7]};
                    const auto au = normEta(u, L);
                    const auto nu = dot(n, u);
                    const auto nv = x[8] * cross(u, n) / au;
                    const auto h = dsqrt(nu * nu + nv * nv);
                    return (signedDist(pt(x, 4), pt(x, 0), pt(x, 2), L) - side * h) / L;
                }));
                break;
            }
            if (a.kind == EntityKind::Line) {
                const CurveParams cp = curveParams(m, b);
                auto params = concat(lineParams(m, a), cp.p);
                const double side = br.side;
                const bool arc = cp.arc;
                eq.push_back(general(id, 0, params, [L, side, arc](const auto* x) {
                    const auto sd = signedDist(pt(x, 4), pt(x, 0), pt(x, 2), L);
                    return (sd - side * curveR(x, 4, arc)) / L;
                }));
            } else {
                if (a.kind == EntityKind::Arc && b.kind == EntityKind::Arc) {
                    // Shared point of the ends of two arcs → tangency there: centers and point are collinear (§4.2.6).
                    EntityId shared = 0;
                    for (EntityId q : {a.start, a.end})
                        for (EntityId q2 : {b.start, b.end})
                            if (!shared && m.rootOf(q) == m.rootOf(q2)) shared = q;
                    if (shared) {
                        auto [o1x, o1y] = m.pointParams(a.center);
                        auto [sx, sy] = m.pointParams(shared);
                        auto [o2x, o2y] = m.pointParams(b.center);
                        eq.push_back(general(id, 0, {o1x, o1y, sx, sy, o2x, o2y}, [L](const auto* x) {
                            const auto u = sub(pt(x, 2), pt(x, 0));
                            const auto v = sub(pt(x, 4), pt(x, 2));
                            return cross(u, v) / (norm(u) * L + 1e-300);
                        }));
                        break;
                    }
                }
                const CurveParams c1 = curveParams(m, a), c2 = curveParams(m, b);
                const int off2 = static_cast<int>(c1.p.size());
                auto params = concat(c1.p, c2.p);
                const Contact contact = br.contact;
                const bool arc1 = c1.arc, arc2 = c2.arc;
                eq.push_back(general(id, 0, params, [L, contact, arc1, arc2, off2](const auto* x) {
                    const auto delta = norm(sub(pt(x, off2), pt(x, 0)));
                    const auto r1 = curveR(x, 0, arc1);
                    const auto r2 = curveR(x, off2, arc2);
                    if (contact == Contact::External) return (delta - (r1 + r2)) / L;
                    if (contact == Contact::Internal1) return (delta - (r1 - r2)) / L;
                    return (delta - (r2 - r1)) / L;
                }));
            }
            break;
        }
        case ConstraintType::Equal: {
            const ModelEntity& a = ent(0);
            const ModelEntity& b = ent(1);
            if (a.kind == EntityKind::Line) {
                auto params = concat(lineParams(m, a), lineParams(m, b));
                eq.push_back(general(id, 0, params, [L](const auto* x) {
                    return (norm(sub(pt(x, 2), pt(x, 0))) - norm(sub(pt(x, 6), pt(x, 4)))) / L;
                }));
            } else if (a.kind == EntityKind::Circle && b.kind == EntityKind::Circle) {
                eq.push_back(subst(id, 0, a.pr, b.pr, 0.0));
            } else if (isEllipseKind(a.kind)) {  // equal ellipses: u and b (§4.2.22, S4-SKT-007)
                eq.push_back(subst(id, 0, a.pux, b.pux, 0.0));
                eq.push_back(subst(id, 1, a.puy, b.puy, 0.0));
                eq.push_back(subst(id, 2, a.pb, b.pb, 0.0));
            } else {
                const CurveParams c1 = curveParams(m, a), c2 = curveParams(m, b);
                const int off2 = static_cast<int>(c1.p.size());
                const bool arc1 = c1.arc, arc2 = c2.arc;
                eq.push_back(general(id, 0, concat(c1.p, c2.p), [L, arc1, arc2, off2](const auto* x) {
                    return (curveR(x, 0, arc1) - curveR(x, off2, arc2)) / L;
                }));
            }
            break;
        }
        case ConstraintType::Concentric: {
            auto [ax, ay] = m.pointParams(ent(0).center);
            auto [bx, by] = m.pointParams(ent(1).center);
            eq.push_back(subst(id, 0, ax, bx, 0.0));
            eq.push_back(subst(id, 1, ay, by, 0.0));
            break;
        }
        case ConstraintType::PointOnObject: {
            auto [px, py] = m.pointParams(c.refs[0]);
            const ModelEntity& o = ent(1);
            if (o.kind == EntityKind::BSpline) {
                // C(t) − P = 0 by x and y with the hidden curve parameter (§4.2.23); outside a model — no equations.
                auto it = m.curveParam.find(id);
                if (it == m.curveParam.end()) break;
                std::vector<int> params{px, py, it->second};
                const int n = static_cast<int>(o.poles.size());
                for (size_t i = 0; i < o.curve->P.size(); ++i) {
                    auto [qx, qy] = m.pointParams(o.poles[i % n]);
                    params.push_back(qx);
                    params.push_back(qy);
                }
                for (int axis = 0; axis < 2; ++axis) {
                    auto fn = std::make_shared<SplinePointFn>();
                    fn->base = o.curve;
                    fn->axis = axis;
                    fn->L = L;
                    Equation e;
                    e.owner = id;
                    e.k = axis;
                    e.kind = EqKind::General;
                    e.params = params;
                    e.fn = fn;
                    eq.push_back(e);
                }
                break;
            }
            if (o.kind == EntityKind::Line) {
                eq.push_back(general(id, 0, concat({px, py}, lineParams(m, o)),
                                     [L](const auto* x) { return signedDist(pt(x, 0), pt(x, 2), pt(x, 4), L) / L; }));
            } else if (isEllipseKind(o.kind)) {
                // (ρ − 1)·sqrt(a·b), ρ² = (s/a)² + (t/b)² (§4.2.21).
                eq.push_back(general(id, 0, concat({px, py}, ellipseParams(m, o)), [L](const auto* x) {
                    using T = std::remove_cv_t<std::remove_reference_t<decltype(x[0])>>;
                    const auto w = sub(pt(x, 0), pt(x, 2));
                    const V2<T> u{x[4], x[5]};
                    const auto a = normEta(u, L);
                    const auto bb = x[6];
                    const auto s = dot(w, u) / a;
                    const auto t = cross(u, w) / a;
                    const auto rho = dsqrt((s / a) * (s / a) + (t / bb) * (t / bb));
                    return (rho - T(1.0)) * dsqrt(a * bb) / L;
                }));
            } else {
                const CurveParams cp = curveParams(m, o);
                const bool arc = cp.arc;
                eq.push_back(general(id, 0, concat({px, py}, cp.p), [L, arc](const auto* x) {
                    return (norm(sub(pt(x, 0), pt(x, 2))) - curveR(x, 2, arc)) / L;
                }));
            }
            break;
        }
        case ConstraintType::Midpoint: {
            auto [px, py] = m.pointParams(c.refs[0]);
            if (ent(1).kind == EntityKind::Arc) {
                // Arc midpoint (§4.2.11а): on the arc's circle and equidistant from the ends.
                const ModelEntity& a = ent(1);
                auto [cx, cy] = m.pointParams(a.center);
                auto [sx, sy] = m.pointParams(a.start);
                auto [ex, ey] = m.pointParams(a.end);
                const std::vector<int> ap{px, py, cx, cy, sx, sy, ex, ey};
                eq.push_back(general(id, 0, ap, [L](const auto* x) { return (norm(sub(pt(x, 0), pt(x, 2))) - norm(sub(pt(x, 4), pt(x, 2)))) / L; }));
                eq.push_back(general(id, 1, ap, [L](const auto* x) { return (norm(sub(pt(x, 0), pt(x, 4))) - norm(sub(pt(x, 0), pt(x, 6)))) / L; }));
                break;
            }
            auto params = concat({px, py}, lineParams(m, ent(1)));
            eq.push_back(general(id, 0, params, [L](const auto* x) { return (2.0 * x[0] - x[2] - x[4]) / (2.0 * L); }));
            eq.push_back(general(id, 1, params, [L](const auto* x) { return (2.0 * x[1] - x[3] - x[5]) / (2.0 * L); }));
            break;
        }
        case ConstraintType::Symmetric: {
            if (ent(0).kind == EntityKind::Ellipse) {
                // Mirrored ellipse shape (§4.2.12, FX7-SK-012): u' = R(u) — u reflected about the axis direction — and
                // b' = b; the centers are related by a separate point symmetry.
                const ModelEntity& E = ent(0);
                const ModelEntity& F = ent(1);
                auto params = concat({E.pux, E.puy, F.pux, F.puy}, lineParams(m, ent(2)));
                for (int axis = 0; axis < 2; ++axis)
                    eq.push_back(general(id, axis, params, [L, axis](const auto* x) {
                        const auto d = sub(pt(x, 6), pt(x, 4));
                        const auto dd = dot(d, d) + L * L * 1e-12;
                        const auto k = 2.0 * (x[0] * d.x + x[1] * d.y) / dd;  // R(u) = 2(u·d)d/‖d‖² − u
                        return axis == 0 ? (x[2] - (k * d.x - x[0])) / L : (x[3] - (k * d.y - x[1])) / L;
                    }));
                eq.push_back(subst(id, 2, F.pb, E.pb, 0.0));
                break;
            }
            auto [px, py] = m.pointParams(c.refs[0]);
            auto [qx, qy] = m.pointParams(c.refs[1]);
            auto params = concat({px, py, qx, qy}, lineParams(m, ent(2)));
            eq.push_back(general(id, 0, params, [L](const auto* x) {
                using T = std::remove_cv_t<std::remove_reference_t<decltype(x[0])>>;
                const V2<T> M{(x[0] + x[2]) * 0.5, (x[1] + x[3]) * 0.5};
                return signedDist(M, pt(x, 4), pt(x, 6), L) / L;
            }));
            eq.push_back(general(id, 1, params, [L](const auto* x) {
                const auto d = sub(pt(x, 6), pt(x, 4));
                return dot(d, sub(pt(x, 2), pt(x, 0))) / (normEta(d, L) * L);
            }));
            break;
        }
        case ConstraintType::Fix: {
            const auto& t = target();
            const ModelEntity& e = ent(0);
            if (e.kind == EntityKind::Point) {
                eq.push_back(fixation(id, 0, e.px, t[0]));
                eq.push_back(fixation(id, 1, e.py, t[1]));
            } else if (e.kind == EntityKind::Ellipse) {
                // Ellipse (FX7-SK-009): target = [cx, cy, ux, uy, b].
                auto [ox, oy] = m.pointParams(e.center);
                eq.push_back(fixation(id, 0, ox, t[0]));
                eq.push_back(fixation(id, 1, oy, t[1]));
                eq.push_back(fixation(id, 2, e.pux, t[2]));
                eq.push_back(fixation(id, 3, e.puy, t[3]));
                eq.push_back(fixation(id, 4, e.pb, t[4]));
            } else {
                auto [ox, oy] = m.pointParams(e.center);
                eq.push_back(fixation(id, 0, ox, t[0]));
                eq.push_back(fixation(id, 1, oy, t[1]));
                eq.push_back(fixation(id, 2, e.pr, t[2]));
            }
            break;
        }
        case ConstraintType::Distance: {
            const double v = val();
            const ModelEntity& a0 = ent(0);
            const ModelEntity& o = ent(1);
            const bool far = c.far;
            if (o.kind == EntityKind::Circle || o.kind == EntityKind::Arc) {
                // To a circle/arc: nearest or farthest distance (§4.2.15а).
                const CurveParams cc = curveParams(m, o);
                const bool arc = cc.arc;
                const double side = br.side;
                if (a0.kind == EntityKind::Point) {
                    auto [px, py] = m.pointParams(c.refs[0]);
                    eq.push_back(general(id, 0, concat({px, py}, cc.p), [L, v, side, arc, far](const auto* x) {
                        const auto d = norm(sub(pt(x, 2), pt(x, 0)));
                        const auto r = curveR(x, 2, arc);
                        return far ? (d + r - v) / L : (side * (d - r) - v) / L;
                    }));
                } else if (a0.kind == EntityKind::Line) {
                    eq.push_back(general(id, 0, concat(lineParams(m, a0), cc.p), [L, v, side, arc, far](const auto* x) {
                        const auto sd = signedDist(pt(x, 4), pt(x, 0), pt(x, 2), L);
                        const auto r = curveR(x, 4, arc);
                        return far ? (side * sd + r - v) / L : (side * sd - r - v) / L;
                    }));
                } else {
                    const CurveParams c1 = curveParams(m, a0);
                    const int off2 = static_cast<int>(c1.p.size());
                    const bool arc1 = c1.arc;
                    eq.push_back(general(id, 0, concat(c1.p, cc.p), [L, v, arc1, arc, off2, far](const auto* x) {
                        const auto delta = norm(sub(pt(x, off2), pt(x, 0)));
                        const auto r1 = curveR(x, 0, arc1);
                        const auto r2 = curveR(x, off2, arc);
                        return far ? (delta + r1 + r2 - v) / L : (delta - r1 - r2 - v) / L;
                    }));
                }
                break;
            }
            auto [px, py] = m.pointParams(c.refs[0]);
            if (o.kind == EntityKind::Point) {
                auto [qx, qy] = m.pointParams(c.refs[1]);
                eq.push_back(general(id, 0, {px, py, qx, qy},
                                     [L, v](const auto* x) { return (norm(sub(pt(x, 2), pt(x, 0))) - v) / L; }));
            } else {
                const double side = br.side;
                eq.push_back(general(id, 0, concat({px, py}, lineParams(m, o)), [L, v, side](const auto* x) {
                    return (signedDist(pt(x, 0), pt(x, 2), pt(x, 4), L) - side * v) / L;
                }));
            }
            break;
        }
        case ConstraintType::DistanceX:
        case ConstraintType::DistanceY: {
            const int slot = c.type == ConstraintType::DistanceX ? 0 : 1;
            eq.push_back(subst(id, 0, m.param(c.refs[1], slot), m.param(c.refs[0], slot), val()));
            break;
        }
        case ConstraintType::Length: {
            const double v = val();
            eq.push_back(general(id, 0, lineParams(m, ent(0)),
                                 [L, v](const auto* x) { return (norm(sub(pt(x, 2), pt(x, 0))) - v) / L; }));
            break;
        }
        case ConstraintType::Radius:
        case ConstraintType::Diameter: {
            const double v = c.type == ConstraintType::Radius ? val() : val() / 2.0;
            const ModelEntity& e = ent(0);
            if (e.kind == EntityKind::Circle) {
                eq.push_back(fixation(id, 0, e.pr, v));
            } else {
                const CurveParams cp = curveParams(m, e);
                eq.push_back(general(id, 0, cp.p, [L, v](const auto* x) { return (curveR(x, 0, true) - v) / L; }));
            }
            break;
        }
        case ConstraintType::Collinear: {
            // Both ends of the second segment lie on the line of the first (§4.2.19а).
            auto params = concat(lineParams(m, ent(0)), lineParams(m, ent(1)));
            for (int k = 0; k < 2; ++k)
                eq.push_back(general(id, k, params, [L, k](const auto* x) { return signedDist(pt(x, 4 + 2 * k), pt(x, 0), pt(x, 2), L) / L; }));
            break;
        }
        case ConstraintType::ArcLength: {
            // R·α − v, α — sweep from start to end counterclockwise in (0, 2π] (§4.2.19б).
            const ModelEntity& a = ent(0);
            auto [cx, cy] = m.pointParams(a.center);
            auto [sx, sy] = m.pointParams(a.start);
            auto [ex, ey] = m.pointParams(a.end);
            const double v = val();
            eq.push_back(general(id, 0, {cx, cy, sx, sy, ex, ey}, [L, v](const auto* x) {
                const auto s = sub(pt(x, 2), pt(x, 0));
                const auto e = sub(pt(x, 4), pt(x, 0));
                auto alpha = datan2(cross(s, e), dot(s, e));
                if (value(alpha) <= 0) alpha = alpha + 2 * PI;
                return (norm(s) * alpha - v) / L;
            }));
            break;
        }
        case ConstraintType::ArcAngle: {
            // α − v, α — sweep from start to end counterclockwise in (0, 2π] (§4.2.19в).
            const ModelEntity& a = ent(0);
            auto [cx, cy] = m.pointParams(a.center);
            auto [sx, sy] = m.pointParams(a.start);
            auto [ex, ey] = m.pointParams(a.end);
            const double v = val();
            eq.push_back(general(id, 0, {cx, cy, sx, sy, ex, ey}, [v](const auto* x) {
                const auto s = sub(pt(x, 2), pt(x, 0));
                const auto e = sub(pt(x, 4), pt(x, 0));
                auto alpha = datan2(cross(s, e), dot(s, e));
                if (value(alpha) <= 0) alpha = alpha + 2 * PI;
                return alpha - v;
            }));
            break;
        }
        case ConstraintType::Translate:
        case ConstraintType::Rotate: {
            // Copy relations (§4.2.21): copy points are images of source points; for an arc the end follows direction.
            const ModelEntity& S = ent(0);
            const ModelEntity& Tt = ent(1);
            const auto& t = target();
            const bool tr = c.type == ConstraintType::Translate;
            int k = 0;
            const double cphi = tr ? 1.0 : std::cos(t[0]), sphi = tr ? 0.0 : std::sin(t[0]);
            auto image = [&](EntityId P, EntityId Q) {
                auto [px, py] = m.pointParams(P);
                auto [qx, qy] = m.pointParams(Q);
                if (tr && c.refs.size() == 3) {
                    // Along the axis segment (S4-SKT-006): Q − P − (vx·û + vy·v̂) = 0, û = d(l)/‖d(l)‖_η.
                    const auto lp = lineParams(m, m.entities.at(c.refs[2]));
                    const double vx = t[0], vy = t[1];
                    for (int axis = 0; axis < 2; ++axis)
                        eq.push_back(general(id, k++, concat({px, py, qx, qy}, lp), [L, vx, vy, axis](const auto* x) {
                            const auto d = sub(pt(x, 6), pt(x, 4));
                            const auto n = normEta(d, L);
                            const auto ox = (vx * d.x - vy * d.y) / n, oy = (vx * d.y + vy * d.x) / n;
                            return axis == 0 ? (x[2] - x[0] - ox) / L : (x[3] - x[1] - oy) / L;
                        }));
                    return;
                }
                if (tr) {
                    eq.push_back(subst(id, k++, qx, px, t[0]));
                    eq.push_back(subst(id, k++, qy, py, t[1]));
                    return;
                }
                auto [cx, cy] = m.pointParams(c.refs[2]);
                // Q − (C + Rot(φ)(P − C)) = 0 in x and in y.
                eq.push_back(general(id, k++, {px, py, qx, qy, cx, cy}, [L, cphi, sphi](const auto* x) {
                    const auto d = sub(pt(x, 0), pt(x, 4));
                    return (x[2] - x[4] - (d.x * cphi - d.y * sphi)) / L;
                }));
                eq.push_back(general(id, k++, {px, py, qx, qy, cx, cy}, [L, cphi, sphi](const auto* x) {
                    const auto d = sub(pt(x, 0), pt(x, 4));
                    return (x[3] - x[5] - (d.x * sphi + d.y * cphi)) / L;
                }));
            };
            switch (S.kind) {
                case EntityKind::Point: image(c.refs[0], c.refs[1]); break;
                case EntityKind::Line:
                    image(S.p1, Tt.p1);
                    image(S.p2, Tt.p2);
                    break;
                case EntityKind::Circle:
                    image(S.center, Tt.center);
                    eq.push_back(subst(id, k++, Tt.pr, S.pr, 0.0));
                    break;
                case EntityKind::Ellipse: {
                    // Ellipse (FX7-SK-012): center image, u' = Rot(φ)·u (translate — u' = u), b' = b.
                    image(S.center, Tt.center);
                    if (tr) {
                        eq.push_back(subst(id, k++, Tt.pux, S.pux, 0.0));
                        eq.push_back(subst(id, k++, Tt.puy, S.puy, 0.0));
                    } else {
                        eq.push_back(general(id, k++, {S.pux, S.puy, Tt.pux}, [L, cphi, sphi](const auto* x) {
                            return (x[2] - (x[0] * cphi - x[1] * sphi)) / L;
                        }));
                        eq.push_back(general(id, k++, {S.pux, S.puy, Tt.puy}, [L, cphi, sphi](const auto* x) {
                            return (x[2] - (x[0] * sphi + x[1] * cphi)) / L;
                        }));
                    }
                    eq.push_back(subst(id, k++, Tt.pb, S.pb, 0.0));
                    break;
                }
                case EntityKind::Arc: {
                    image(S.center, Tt.center);
                    image(S.start, Tt.start);
                    auto [sox, soy] = m.pointParams(S.center);
                    auto [sex, sey] = m.pointParams(S.end);
                    auto [tox, toy] = m.pointParams(Tt.center);
                    auto [tex, tey] = m.pointParams(Tt.end);
                    eq.push_back(general(id, k++, {sox, soy, sex, sey, tox, toy, tex, tey}, [L, cphi, sphi](const auto* x) {
                        using T = std::remove_cv_t<std::remove_reference_t<decltype(x[0])>>;
                        const auto w = sub(pt(x, 2), pt(x, 0));
                        const V2<T> rw{w.x * cphi - w.y * sphi, w.x * sphi + w.y * cphi};
                        return cross(rw, sub(pt(x, 6), pt(x, 4))) / (normEta(w, L) * L);
                    }));
                    break;
                }
            }
            break;
        }
        case ConstraintType::PathLength: {
            // Σ len(cᵢ) − v (§4.2.19г): segment — ‖d‖, arc — R·α.
            std::vector<int> params;
            std::vector<int> kinds;  // 0 — segment (4 parameters), 1 — arc (6)
            for (size_t i = 0; i < c.refs.size(); ++i) {
                const ModelEntity& e = ent(i);
                if (e.kind == EntityKind::Line) {
                    for (int p : lineParams(m, e)) params.push_back(p);
                    kinds.push_back(0);
                } else {
                    auto [cx, cy] = m.pointParams(e.center);
                    auto [sx, sy] = m.pointParams(e.start);
                    auto [ex, ey] = m.pointParams(e.end);
                    for (int p : {cx, cy, sx, sy, ex, ey}) params.push_back(p);
                    kinds.push_back(1);
                }
            }
            const double v = val();
            eq.push_back(general(id, 0, params, [L, v, kinds](const auto* x) {
                using T = std::remove_cv_t<std::remove_reference_t<decltype(x[0])>>;
                T sum = T(0.0);
                int o = 0;
                for (int k : kinds) {
                    if (k == 0) {
                        sum = sum + norm(sub(pt(x, o + 2), pt(x, o)));
                        o += 4;
                    } else {
                        const auto s = sub(pt(x, o + 2), pt(x, o));
                        const auto e = sub(pt(x, o + 4), pt(x, o));
                        auto alpha = datan2(cross(s, e), dot(s, e));
                        if (value(alpha) <= 0) alpha = alpha + 2 * PI;
                        sum = sum + norm(s) * alpha;
                        o += 6;
                    }
                }
                return (sum - v) / L;
            }));
            break;
        }
        case ConstraintType::SemiMajor: {
            const double v = val();
            const ModelEntity& e = ent(0);
            eq.push_back(general(id, 0, {e.pux, e.puy}, [L, v](const auto* x) { return (norm(pt(x, 0)) - v) / L; }));
            break;
        }
        case ConstraintType::SemiMinor: eq.push_back(fixation(id, 0, ent(0).pb, val())); break;
        case ConstraintType::EllipseAngle: {
            const double v = val();
            const double cs = std::cos(v), sn = std::sin(v);
            const ModelEntity& e = ent(0);
            eq.push_back(general(id, 0, {e.pux, e.puy}, [L, cs, sn](const auto* x) {
                return (cs * x[1] - sn * x[0]) / normEta(pt(x, 0), L);  // sin(φ − v), §4.2.21
            }));
            break;
        }
        case ConstraintType::Angle: {
            double phi;
            if (ov && ov->phi.count(id)) {
                phi = ov->phi.at(id);
            } else {
                const double v = val();
                phi = br.sign * (br.supplementary ? PI - v : v);
            }
            const double cs = std::cos(phi), sn = std::sin(phi);
            eq.push_back(general(id, 0, concat(lineParams(m, ent(0)), lineParams(m, ent(1))), [cs, sn](const auto* x) {
                using T = std::remove_cv_t<std::remove_reference_t<decltype(x[0])>>;
                const auto d1 = sub(pt(x, 2), pt(x, 0));
                const auto d2 = sub(pt(x, 6), pt(x, 4));
                const V2<T> u{cs * d1.x - sn * d1.y, sn * d1.x + cs * d1.y};
                return datan2(cross(u, d2), dot(u, d2));
            }));
            break;
        }
    }
    return eq;
}

// ---------------------------------------------------------------- model

Model buildModel(const Problem& p, const ValueOverrides* overrides, std::optional<double> lengthScale) {
    Model m;
    m.L = lengthScale ? *lengthScale : computeL(p);
    for (const auto& [id, e] : p.entities()) {
        ModelEntity me;
        me.id = id;
        me.kind = e.kind;
        me.p1 = e.p1;
        me.p2 = e.p2;
        me.center = e.center;
        me.start = e.start;
        me.end = e.end;
        if (e.kind == EntityKind::BSpline) {  // §2.3: the expanded curve with placeholder pole coordinates
            me.poles = e.poles;
            spline::Curve sc;
            sc.degree = e.degree;
            sc.poles.assign(e.poles.size(), Point2::Zero());
            sc.knots = e.knots;
            sc.mults = e.mults;
            sc.weights = e.weights;
            sc.periodic = e.periodic;
            me.curve = std::make_shared<const spline::Flat>(spline::expand(sc));
        }
        if (e.kind == EntityKind::Point) {
            m.keys.push_back({id, 0});
            m.x0.push_back(e.x);
            m.fixed.push_back(e.fixed);
            m.keys.push_back({id, 1});
            m.x0.push_back(e.y);
            m.fixed.push_back(e.fixed);
        } else if (e.kind == EntityKind::Circle) {
            m.keys.push_back({id, 0});
            m.x0.push_back(e.r);
            m.fixed.push_back(e.radiusFixed);
        } else if (isEllipseKind(e.kind)) {  // §2.2: ux, uy, b
            for (int s = 0; s < 3; ++s) {
                m.keys.push_back({id, s});
                m.x0.push_back(s == 0 ? e.ux : s == 1 ? e.uy : e.b);
                m.fixed.push_back(e.shapeFixed);
            }
        }
        m.entities[id] = me;
    }
    // Hidden curve parameters of driving point-on-spline constraints (§2.2, §4.2.23): value t̂·L, t̂ — the normalized
    // parameter of the closest point of the curve to the point at the current values.
    {
        int k = 0;
        std::vector<std::pair<ConstraintId, std::pair<ParamKey, double>>> hidden;
        for (const auto& [cid, c] : p.constraints()) {
            if (c.type != ConstraintType::PointOnObject || c.refs.size() != 2) continue;
            const auto it = m.entities.find(c.refs[1]);
            if (it == m.entities.end() || it->second.kind != EntityKind::BSpline) continue;
            spline::Flat f = *it->second.curve;
            const int n = static_cast<int>(it->second.poles.size());
            for (size_t i = 0; i < f.P.size(); ++i) {
                const auto q = p.point(it->second.poles[i % n]);
                f.P[i] = Point2(q.first, q.second);
            }
            const auto P = p.point(c.refs[0]);
            const double t = spline::closestParam(f, Point2(P.first, P.second));
            hidden.push_back({cid, {ParamKey{CURVE_PARAM_ID, k++}, (t - f.a) / (f.b - f.a) * m.L}});
        }
        // Keys stay ascending: CURVE_PARAM_ID is below ORIGIN_ID (the origin point keys are last).
        for (const auto& h : hidden) {
            m.keys.push_back(h.second.first);
            m.x0.push_back(h.second.second);
            m.fixed.push_back(false);
        }
        std::vector<size_t> idx(m.keys.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return m.keys[a] < m.keys[b]; });
        std::vector<ParamKey> keys;
        std::vector<double> x0;
        std::vector<bool> fixed;
        for (size_t i : idx) keys.push_back(m.keys[i]), x0.push_back(m.x0[i]), fixed.push_back(m.fixed[i]);
        m.keys = std::move(keys), m.x0 = std::move(x0), m.fixed = std::move(fixed);
        for (size_t i = 0; i < m.keys.size(); ++i) m.index[m.keys[i]] = static_cast<int>(i);
        for (const auto& h : hidden) m.curveParam[h.first] = m.index.at(h.second.first);
    }
    for (auto& [id, me] : m.entities) {
        if (me.kind == EntityKind::Point) {
            me.px = m.index.at({id, 0});
            me.py = m.index.at({id, 1});
        } else if (me.kind == EntityKind::Circle) {
            me.pr = m.index.at({id, 0});
        } else if (isEllipseKind(me.kind)) {
            me.pux = m.index.at({id, 0});
            me.puy = m.index.at({id, 1});
            me.pb = m.index.at({id, 2});
        }
    }
    // Classes of coincident points — for tangency at a shared point (§4.2.5, §4.2.6).
    {
        std::map<EntityId, EntityId> parent;
        std::function<EntityId(EntityId)> find = [&](EntityId e) -> EntityId {
            auto it = parent.find(e);
            if (it == parent.end() || it->second == e) return e;
            return it->second = find(it->second);
        };
        for (const auto& [cid, c] : p.constraints()) {
            if (c.type != ConstraintType::Coincident || c.refs.size() != 2) continue;
            const EntityId a = find(c.refs[0]), b = find(c.refs[1]);
            if (a == b) continue;
            parent[std::max(a, b)] = std::min(a, b);
            parent.emplace(std::min(a, b), std::min(a, b));
        }
        for (const auto& [e, ignored] : parent) m.coincidentRoot[e] = find(e);
    }
    // Internal arc equations (§4.3), then constraints in ascending ID order.
    const double L = m.L;
    for (const auto& [id, me] : m.entities) {
        if (me.kind == EntityKind::EllipseArc) {
            // Ellipse arc ends on its ellipse (§4.3, S4-SKT-007): (ρ − 1)·sqrt(a·b)/L for start (k = 0) and end (k = 1).
            auto [ox, oy] = m.pointParams(me.center);
            int k = 0;
            for (EntityId q : {me.start, me.end}) {
                auto [qx, qy] = m.pointParams(q);
                m.equations.push_back(general(id, k++, {qx, qy, ox, oy, me.pux, me.puy, me.pb}, [L](const auto* x) {
                    using T = std::remove_cv_t<std::remove_reference_t<decltype(x[0])>>;
                    const auto w = sub(pt(x, 0), pt(x, 2));
                    const V2<T> u{x[4], x[5]};
                    const auto a = normEta(u, L);
                    const auto bb = x[6];
                    const auto s = dot(w, u) / a;
                    const auto t = cross(u, w) / a;
                    const auto rho = dsqrt((s / a) * (s / a) + (t / bb) * (t / bb));
                    return (rho - T(1.0)) * dsqrt(a * bb) / L;
                }, 0));
            }
            continue;
        }
        if (me.kind != EntityKind::Arc) continue;
        auto [ox, oy] = m.pointParams(me.center);
        auto [sx, sy] = m.pointParams(me.start);
        auto [ex, ey] = m.pointParams(me.end);
        m.equations.push_back(general(id, 0, {ox, oy, sx, sy, ex, ey}, [L](const auto* x) {
            return (norm(sub(pt(x, 2), pt(x, 0))) - norm(sub(pt(x, 4), pt(x, 0)))) / L;
        }, 0));
    }
    for (const auto& [id, c] : p.constraints()) {
        if (isDimension(c.type) && !c.driving) continue;
        auto eqs = constraintEquations(m, c, overrides);
        m.equations.insert(m.equations.end(), eqs.begin(), eqs.end());
    }
    return m;
}

// ---------------------------------------------------------------- measure

double measureAt(const Model& m, const ConstraintSpec& c, const std::vector<double>& x) {
    auto P = [&](EntityId e) {
        auto [ix, iy] = m.pointParams(e);
        return V2<double>{x[ix], x[iy]};
    };
    auto R = [&](const ModelEntity& e) {
        if (e.kind == EntityKind::Circle) return x[e.pr];
        return norm(sub(P(e.start), P(e.center)));
    };
    const Branch br = c.branch.value_or(Branch{});
    switch (c.type) {
        case ConstraintType::Distance: {
            const ModelEntity& a0 = m.entities.at(c.refs[0]);
            const ModelEntity& o = m.entities.at(c.refs[1]);
            if (o.kind == EntityKind::Point) return norm(sub(P(c.refs[1]), P(c.refs[0])));
            if (o.kind == EntityKind::Line) return br.side * signedDist(P(c.refs[0]), P(o.p1), P(o.p2), m.L);
            // To a circle/arc (§4.2.15а).
            const double r = R(o);
            if (a0.kind == EntityKind::Point) {
                const double d = norm(sub(P(o.center), P(c.refs[0])));
                return c.far ? d + r : br.side * (d - r);
            }
            if (a0.kind == EntityKind::Line) {
                const double sd = signedDist(P(o.center), P(a0.p1), P(a0.p2), m.L);
                return c.far ? br.side * sd + r : br.side * sd - r;
            }
            const double delta = norm(sub(P(o.center), P(a0.center)));
            return c.far ? delta + R(a0) + r : delta - R(a0) - r;
        }
        case ConstraintType::DistanceX: return P(c.refs[1]).x - P(c.refs[0]).x;
        case ConstraintType::DistanceY: return P(c.refs[1]).y - P(c.refs[0]).y;
        case ConstraintType::Length: {
            const ModelEntity& l = m.entities.at(c.refs[0]);
            return norm(sub(P(l.p2), P(l.p1)));
        }
        case ConstraintType::Radius: return R(m.entities.at(c.refs[0]));
        case ConstraintType::Diameter: return 2.0 * R(m.entities.at(c.refs[0]));
        case ConstraintType::ArcLength:
        case ConstraintType::ArcAngle: {
            const ModelEntity& a = m.entities.at(c.refs[0]);
            const auto o = P(a.center), s = P(a.start), e = P(a.end);
            const V2<double> vs{s.x - o.x, s.y - o.y}, ve{e.x - o.x, e.y - o.y};
            double al = datan2(cross(vs, ve), dot(vs, ve));
            if (al <= 0) al += 2 * PI;
            return c.type == ConstraintType::ArcAngle ? al : norm(vs) * al;
        }
        case ConstraintType::PathLength: {
            double sum = 0;
            for (EntityId r : c.refs) {
                const ModelEntity& e = m.entities.at(r);
                if (e.kind == EntityKind::Line) {
                    sum += norm(sub(P(e.p2), P(e.p1)));
                } else {
                    const auto o = P(e.center), s = P(e.start), en = P(e.end);
                    const V2<double> vs{s.x - o.x, s.y - o.y}, ve{en.x - o.x, en.y - o.y};
                    double al = datan2(cross(vs, ve), dot(vs, ve));
                    if (al <= 0) al += 2 * PI;
                    sum += norm(vs) * al;
                }
            }
            return sum;
        }
        case ConstraintType::SemiMajor: {
            const ModelEntity& e = m.entities.at(c.refs[0]);
            return std::hypot(x[e.pux], x[e.puy]);
        }
        case ConstraintType::SemiMinor: return x[m.entities.at(c.refs[0]).pb];
        case ConstraintType::EllipseAngle: {
            const ModelEntity& e = m.entities.at(c.refs[0]);
            double a = std::atan2(x[e.puy], x[e.pux]);
            while (a < 0) a += PI;
            while (a >= PI) a -= PI;
            return a;
        }
        case ConstraintType::Angle: {
            const ModelEntity& l1 = m.entities.at(c.refs[0]);
            const ModelEntity& l2 = m.entities.at(c.refs[1]);
            const auto d1 = sub(P(l1.p2), P(l1.p1));
            const auto d2 = sub(P(l2.p2), P(l2.p1));
            const double alpha = datan2(cross(d1, d2), dot(d1, d2));
            return br.supplementary ? geom2::normalizeAngleSym(PI - br.sign * alpha) : geom2::normalizeAngleSym(br.sign * alpha);
        }
        default: return 0.0;
    }
}

// ---------------------------------------------------------------- guards

namespace {

double arcSweep(double ox, double oy, double sx, double sy, double ex, double ey) {
    const V2<double> s{sx - ox, sy - oy}, e{ex - ox, ey - oy};
    double a = datan2(cross(s, e), dot(s, e));
    if (a <= 0) a += 2 * PI;
    return a;
}

}  // namespace

bool guardsAccept(const Model& m, const Problem& p, const std::vector<double>& xo, const std::vector<double>& xn) {
    const double L = m.L;
    const double eps = EPS_LEN * L;
    // G0: changed values only; offset from the value at call start — invariant under translation (§4.10).
    for (size_t i = 0; i < xn.size(); ++i) {
        if (xn[i] == xo[i]) continue;
        if (!std::isfinite(xn[i]) || std::abs(xn[i] - m.x0[i]) > SCALED_MAX * L) return false;
    }
    auto P = [&](const std::vector<double>& x, EntityId e) {
        auto [ix, iy] = m.pointParams(e);
        return V2<double>{x[ix], x[iy]};
    };
    for (const auto& [id, e] : m.entities) {
        switch (e.kind) {
            case EntityKind::Circle:  // G1
                if (xo[e.pr] >= eps && xn[e.pr] < eps) return false;
                break;
            case EntityKind::Ellipse:
            case EntityKind::EllipseArc: {  // G1 (ellipse); ellipse arc — also G3 by the parameter sweep (§4.3)
                const double ao = std::hypot(xo[e.pux], xo[e.puy]), an = std::hypot(xn[e.pux], xn[e.puy]);
                if ((ao >= eps && an < eps) || (xo[e.pb] >= eps && xn[e.pb] < eps)) return false;
                if (e.kind == EntityKind::EllipseArc && ao >= eps && xo[e.pb] >= eps) {
                    auto sweepT = [&](const std::vector<double>& x) {
                        const V2<double> o = P(x, e.center);
                        const double a = std::hypot(x[e.pux], x[e.puy]);
                        const V2<double> u{x[e.pux] / a, x[e.puy] / a};
                        auto param = [&](const V2<double>& q) {
                            const auto w = sub(q, o);
                            return std::atan2(cross(u, w) / x[e.pb], dot(w, u) / a);
                        };
                        return geom2::ccwSweepPositive(param(P(x, e.start)), param(P(x, e.end)));
                    };
                    if (an >= eps && xn[e.pb] >= eps && std::abs(sweepT(xn) - sweepT(xo)) > ARC_SWEEP_JUMP) return false;
                }
                break;
            }
            case EntityKind::Line: {  // G2
                const auto dOld = sub(P(xo, e.p2), P(xo, e.p1));
                const auto dNew = sub(P(xn, e.p2), P(xn, e.p1));
                if (norm(dOld) >= eps && (norm(dNew) < eps || dot(dNew, dOld) <= 0)) return false;
                break;
            }
            case EntityKind::Arc: {  // G3
                const auto o0 = P(xo, e.center), s0 = P(xo, e.start), e0 = P(xo, e.end);
                const auto o1 = P(xn, e.center), s1 = P(xn, e.start), e1 = P(xn, e.end);
                const double rs0 = norm(sub(s0, o0)), re0 = norm(sub(e0, o0));
                const double rs1 = norm(sub(s1, o1)), re1 = norm(sub(e1, o1));
                if ((rs0 >= eps && rs1 < eps) || (re0 >= eps && re1 < eps)) return false;
                const double a0 = arcSweep(o0.x, o0.y, s0.x, s0.y, e0.x, e0.y);
                const double a1 = arcSweep(o1.x, o1.y, s1.x, s1.y, e1.x, e1.y);
                if (std::abs(a1 - a0) > ARC_SWEEP_JUMP) return false;
                break;
            }
            default: break;
        }
    }
    // G4: driving distance(P, Q).
    for (const auto& [cid, c] : p.constraints()) {
        if (c.type != ConstraintType::Distance || !c.driving) continue;
        if (m.entities.at(c.refs[1]).kind != EntityKind::Point) continue;
        const auto vOld = sub(P(xo, c.refs[1]), P(xo, c.refs[0]));
        const auto vNew = sub(P(xn, c.refs[1]), P(xn, c.refs[0]));
        if (norm(vOld) >= eps && dot(vNew, vOld) <= 0) return false;
    }
    return true;
}

std::vector<EntityId> degenerateEntities(const Model& m, const std::vector<double>& x) {
    std::vector<EntityId> out;
    const double eps = EPS_LEN * m.L;
    auto P = [&](EntityId e) {
        auto [ix, iy] = m.pointParams(e);
        return V2<double>{x[ix], x[iy]};
    };
    for (const auto& [id, e] : m.entities) {
        if (e.kind == EntityKind::Line && norm(sub(P(e.p2), P(e.p1))) <= eps) out.push_back(id);
        if (e.kind == EntityKind::Circle && x[e.pr] <= eps) out.push_back(id);
        if (e.kind == EntityKind::Arc && norm(sub(P(e.start), P(e.center))) <= eps) out.push_back(id);
        if (isEllipseKind(e.kind) && (std::hypot(x[e.pux], x[e.puy]) <= eps || x[e.pb] <= eps)) out.push_back(id);
    }
    return out;
}

std::vector<EntityId> underconstrained(const Model& m, const std::vector<bool>& free) {
    std::vector<EntityId> out;
    auto pointFree = [&](EntityId e) {
        auto [ix, iy] = m.pointParams(e);
        return free[ix] || free[iy];
    };
    for (const auto& [id, e] : m.entities) {
        bool f = false;
        switch (e.kind) {
            case EntityKind::Point: f = pointFree(id); break;
            case EntityKind::Line: f = pointFree(e.p1) || pointFree(e.p2); break;
            case EntityKind::Circle: f = free[e.pr] || pointFree(e.center); break;
            case EntityKind::Arc: f = pointFree(e.center) || pointFree(e.start) || pointFree(e.end); break;
            case EntityKind::Ellipse: f = pointFree(e.center) || free[e.pux] || free[e.puy] || free[e.pb]; break;
            case EntityKind::EllipseArc:
                f = pointFree(e.center) || pointFree(e.start) || pointFree(e.end) || free[e.pux] || free[e.puy] || free[e.pb];
                break;
            case EntityKind::BSpline:
                for (EntityId q : e.poles) f = f || pointFree(q);
                break;
        }
        if (f) out.push_back(id);
    }
    return out;
}

}  // namespace construe::solver
