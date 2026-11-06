// Spur involute gears (spec/gear.md): geometry by the basic rack, generated flank and fillet, cutting tool section.
#include "gear/gear.h"

#include "base/geom2.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>

namespace construe::gear {

namespace {

Point2 polar(double R, double a) { return Point2(R * std::cos(a), R * std::sin(a)); }
Point2 rotate(const Point2& p, double a) {
    const double c = std::cos(a), s = std::sin(a);
    return Point2(c * p.x() - s * p.y(), s * p.x() + c * p.y());
}
using geom2::angleOf;
using geom2::cross2;

Error badValue(const char* name) { return Error("GEAR_BAD_VALUE").with("name", std::string(name)); }

// Space side in the template frame (§4.1): space centred on +x, the plus side at positive polar angles; segments
// from the root end to the tip end, IDs without the space number ("f", "u", "i", "e").
struct Side {
    std::vector<Seg> segs;
    double rootHalf = 0;  // half-angle of the root land
    double tipHalf = 0;   // polar angle of the flank end on the tip circle
    double rRoot = 0, rTip = 0;
};

Seg lineSeg(const std::string& id, const Point2& a, const Point2& b) {
    Seg s;
    s.kind = Seg::Kind::Line;
    s.id = id;
    s.a = a, s.b = b;
    return s;
}
// Arc about c from a to b; ccw — counterclockwise.
Seg arcSeg(const std::string& id, const Point2& c, double r, const Point2& a, const Point2& b, bool ccw) {
    Seg s;
    s.kind = Seg::Kind::Arc;
    s.id = id;
    s.center = c, s.radius = r, s.a = a, s.b = b, s.ccw = ccw;
    return s;
}
// Single Bezier curve fitted to an analytic curve f(t), t ∈ [t0, t1] (§4.4): ends — a, b exactly; interior poles — least
// squares over Chebyshev nodes with the parameter u = (t − t0)/(t1 − t0); the degree grows from GEAR_BEZIER_DEGREE
// to GEAR_BEZIER_MAX_DEGREE until the deviation at GEAR_FIT_SAMPLES uniform parameters is below GEAR_FIT_TOL·m.
template <class F>
Seg bezierSeg(const std::string& id, F&& f, double t0, double t1, const Point2& a, const Point2& b, double m) {
    auto at = [&](double u) { return Point2(f(t0 + (t1 - t0) * u)); };
    std::vector<Point2> best;
    double bestDev = 1e300;
    for (int n = GEAR_BEZIER_DEGREE; n <= GEAR_BEZIER_MAX_DEGREE; ++n) {
        const int N = 4 * n;
        Eigen::MatrixXd A(N, n - 1);
        Eigen::MatrixXd rhs(N, 2);
        auto bern = [n](int i, double u) {
            double c = 1;
            for (int k = 1; k <= i; ++k) c = c * double(n - i + k) / double(k);
            return c * std::pow(u, i) * std::pow(1 - u, n - i);
        };
        for (int r = 0; r < N; ++r) {
            const double u = 0.5 - 0.5 * std::cos(PI * (r + 0.5) / N);
            for (int i = 1; i < n; ++i) A(r, i - 1) = bern(i, u);
            const Point2 q = at(u) - bern(0, u) * a - bern(n, u) * b;
            rhs(r, 0) = q.x(), rhs(r, 1) = q.y();
        }
        const Eigen::MatrixXd X = A.colPivHouseholderQr().solve(rhs);
        std::vector<Point2> poles{a};
        for (int i = 0; i < n - 1; ++i) poles.push_back(Point2(X(i, 0), X(i, 1)));
        poles.push_back(b);
        const spline::Flat fl = spline::expand(spline::bezier(poles));
        double dev = 0;
        for (int k = 1; k < GEAR_FIT_SAMPLES; ++k) {
            const double u = double(k) / GEAR_FIT_SAMPLES;
            dev = std::max(dev, (spline::eval(fl, fl.a + (fl.b - fl.a) * u) - at(u)).norm());
        }
        if (dev < bestDev) bestDev = dev, best = poles;
        if (dev <= GEAR_FIT_TOL * m) break;
    }
    Seg s;
    s.kind = Seg::Kind::Spline;
    s.id = id;
    s.curve = spline::bezier(best);
    s.a = a, s.b = b;
    return s;
}

// Involute side point at roll angle ξ (§3.2, §4.4): R = rb·√(1 + ξ²).
Point2 involuteAt(const Params& p, double rb, double xi) {
    const double R = rb * std::sqrt(1 + xi * xi);
    return polar(R, flankAngle(p, R));
}
double rollAngle(double R, double rb) { return std::sqrt(std::max(0.0, (R / rb) * (R / rb) - 1)); }

Seg transformed(const Seg& s, double angle, bool mirror) {
    auto t = [&](const Point2& p) { return rotate(mirror ? Point2(p.x(), -p.y()) : p, angle); };
    Seg o = s;
    o.a = t(s.a), o.b = t(s.b), o.center = t(s.center);
    if (mirror) o.ccw = !s.ccw;
    for (auto& q : o.curve.poles) q = t(q);
    return o;
}
Seg reversedSeg(const Seg& s) {
    Seg o = s;
    std::swap(o.a, o.b);
    o.ccw = !s.ccw;
    if (s.kind == Seg::Kind::Spline) o.curve = spline::compress(spline::reversed(spline::expand(s.curve)));
    return o;
}

// Generating rack of an external gear (§4.2): rounding envelope and the corner trajectory in the template frame.
struct Rack {
    double r, b, uc, rho, alpha;
    // Rounding envelope at rolling angle φ.
    Point2 rounding(double phi) const {
        const Point2 C(uc - r * phi, r + b);
        const Point2 d = Point2(0, r) - C;
        return toTemplate(rotateBack(C - rho * d / d.norm(), phi));
    }
    // Trajectory of the corner where the rounding meets the straight flank.
    Point2 corner(double phi) const {
        return toTemplate(rotateBack(Point2(uc + rho * std::cos(alpha) - r * phi, r + b - rho * std::sin(alpha)), phi));
    }
    static Point2 rotateBack(const Point2& w, double phi) {
        const double c = std::cos(phi), s = std::sin(phi);
        return Point2(w.x() * c + w.y() * s, -w.x() * s + w.y() * c);
    }
    // Derivation frame (space centred on +y, plus side toward +x) → template frame (space on +x).
    static Point2 toTemplate(const Point2& g) { return Point2(g.y(), g.x()); }
};

struct ExternalSide {
    Side side;
    double rForm = 0;
    bool undercut = false;
};

// tipShift ≥ 0 (§4.5): the flank continues past the tip circle to ra + tipShift (tool section, the tips stay off the
// blank surface).
Result<ExternalSide> externalSide(const Params& p, const Geometry& g, double tipShift = 0) {
    const BasicRack& k = basicRack();
    const double m = p.m, r = g.d / 2, rb = g.db / 2, ra = g.da / 2, rf = g.df / 2;
    const double rho = k.rho * m;
    const double vc = -(k.ha + k.c) * m + rho;
    const double uc = PI * m / 4 + vc * std::tan(k.alpha) - rho / std::cos(k.alpha);
    const double b = p.x * m + vc;
    if (uc < 0 || b >= 0) return Error("GEAR_PROFILE_FAILED").with("detail", std::string("rack"));
    const Rack rack{r, b, uc, rho, k.alpha};
    const double phi0 = uc / r;
    const double phi1 = (uc + b / std::tan(k.alpha)) / r;
    auto gap = [&](const Point2& q) {  // > 0 — the generating curve is outside the involute space (§4.2)
        const double R = q.norm();
        return R < rb ? -1.0 : angleOf(q) - flankAngle(p, R);
    };
    ExternalSide out;
    // The generating curve L (§4.2 item 3): the rounding envelope φ0 → φ1, then the corner trajectory φ < φ1. Normal
    // case: L never leaves the involute space and the envelope ends on the involute at φ1. Undercut (item 4): L goes
    // outside the involute space; the junction is the first crossing back inside after that.
    auto L = [&](double phi) { return phi >= phi1 ? rack.rounding(phi) : rack.corner(phi); };
    const double step = (phi0 - phi1) / GEAR_SCAN_POINTS;
    double phiJ = phi1;
    bool outside = false, found = false;
    double prev = phi0;
    for (int i = 1; i <= 20 * GEAR_SCAN_POINTS && !found; ++i) {
        const double phi = phi0 - step * i;
        const Point2 q = L(phi);
        const double gq = gap(q);
        if (gq > GEAR_JOIN_TOL) {
            outside = true;
        } else if (outside && q.norm() >= rb) {
            double lo = phi, hi = prev;  // gap(L(lo)) ≤ 0, gap(L(hi)) > 0
            for (int it = 0; it < 80; ++it) {
                const double mid = 0.5 * (lo + hi);
                (gap(L(mid)) > 0 ? hi : lo) = mid;
            }
            phiJ = 0.5 * (lo + hi);
            found = true;
        }
        if (q.norm() > ra) break;
        prev = phi;
    }
    if (outside && !found) return Error("GEAR_PROFILE_FAILED").with("detail", std::string("undercut"));
    if (!outside && rack.rounding(phi1).norm() < rb) return Error("GEAR_PROFILE_FAILED").with("detail", std::string("form"));
    out.undercut = outside;
    const bool onCorner = phiJ < phi1;
    const Point2 J = onCorner ? rack.corner(phiJ) : rack.rounding(phiJ);
    out.rForm = J.norm();
    if (out.rForm >= ra) return Error("GEAR_PROFILE_FAILED").with("detail", std::string("no involute"));
    // Pieces (§4.4): rounding φ0 → max(φJ, φ1), corner φ1 → φJ, involute rForm → ra (+ tip shift).
    const double phiF = onCorner ? phi1 : phiJ;
    auto rounding = [&](double t) { return rack.rounding(t); };
    out.side.segs.push_back(bezierSeg("f", rounding, phi0, phiF, rack.rounding(phi0), onCorner ? rack.rounding(phi1) : J, p.m));
    if (onCorner) {
        auto corner = [&](double t) { return rack.corner(t); };
        out.side.segs.push_back(bezierSeg("u", corner, phi1, phiJ, rack.rounding(phi1), J, p.m));
    }
    const double rTip = ra + tipShift;
    const double x0 = rollAngle(out.rForm, rb), x1 = rollAngle(rTip, rb);
    auto inv = [&](double xi) { return involuteAt(p, rb, xi); };
    out.side.segs.push_back(bezierSeg("i", inv, x0, x1, J, polar(rTip, flankAngle(p, rTip)), p.m));
    out.side.rootHalf = phi0;
    out.side.tipHalf = flankAngle(p, rTip);
    out.side.rRoot = rf, out.side.rTip = rTip;
    return out;
}

struct InternalSide {
    Side side;
    double rForm = 0;
    double filletRadius = 0;
    bool tipBelowBase = false;
};

// tipShift ≥ 0 (§4.5): the tooth side ends at Ra − tipShift instead of Ra.
Result<InternalSide> internalSide(const Params& p, const Geometry& g, double tipShift = 0) {
    const BasicRack& k = basicRack();
    const double rb = g.db / 2, Ra = g.da / 2, Rf = g.df / 2;
    InternalSide out;
    const double rLow = std::max(Ra, rb), rTip = Ra - tipShift;
    out.tipBelowBase = Ra < rb;
    // Fillet arc (§4.3): radius ρ, tangent to the involute and to the root circle; centre C(R) = P(R) + ρ·n. A narrow
    // root takes a smaller radius: ρ·GEAR_FILLET_SHRINK^k, k ≤ GEAR_FILLET_STEPS.
    double rho = k.rho * p.m;
    auto center = [&](double R) {
        const double beta = flankAngle(p, R);
        const double dBeta = -std::sqrt(std::max(0.0, R * R - rb * rb)) / (R * rb);
        const Point2 radial(std::cos(beta), std::sin(beta)), tang(-std::sin(beta), std::cos(beta));
        const Point2 T = (radial + R * dBeta * tang).normalized();
        Point2 n(-T.y(), T.x());
        if (n.dot(-tang) < 0) n = -n;  // into the space: toward smaller polar angles
        return Point2(polar(R, beta) + rho * n);
    };
    auto h = [&](double R) { return center(R).norm() - (Rf - rho); };
    double Rt = 0, gamma = 0;
    Point2 C = Point2::Zero();
    bool fits = false;
    for (int step = 0; step <= GEAR_FILLET_STEPS && !fits; ++step, rho *= GEAR_FILLET_SHRINK) {
        if (!(h(rLow) < 0 && h(Rf) > 0)) continue;
        double lo = rLow, hi = Rf;
        for (int it = 0; it < 100; ++it) {
            const double mid = 0.5 * (lo + hi);
            (h(mid) > 0 ? hi : lo) = mid;
        }
        Rt = 0.5 * (lo + hi);
        C = center(Rt);
        gamma = angleOf(C);
        fits = gamma > 0;
        if (fits) break;
    }
    if (!fits) return Error("GEAR_FILLET");
    out.filletRadius = rho;
    const Point2 Pt = polar(Rt, flankAngle(p, Rt)), Q = polar(Rf, gamma);
    out.rForm = Rt;
    // Segments from the root end: fillet arc Q → Pt, involute Pt → rLow, radial extension rLow → Ra.
    const bool ccw = cross2(Q - C, Pt - C) > 0;
    out.side.segs.push_back(arcSeg("f", C, rho, Q, Pt, ccw));
    const double rInv = std::max(rTip, rb);  // the involute ends here, below — the radial extension
    auto inv = [&](double xi) { return involuteAt(p, rb, xi); };
    out.side.segs.push_back(bezierSeg("i", inv, rollAngle(Rt, rb), rollAngle(rInv, rb), Pt, polar(rInv, flankAngle(p, rInv)), p.m));
    const double tipAngle = flankAngle(p, rInv);
    if (rTip < rb) out.side.segs.push_back(lineSeg("e", polar(rb, tipAngle), polar(rTip, tipAngle)));
    out.side.rootHalf = gamma;
    out.side.tipHalf = tipAngle;
    out.side.rRoot = Rf, out.side.rTip = rTip;
    return out;
}

struct Built {
    Geometry geo;
    Side side;
};

Result<Built> build(const Params& p, double tipShift = 0) {
    auto g = geometry(p);
    if (!g) return g.error();
    Built b;
    b.geo = *g;
    const double tau = 2 * PI / p.z;
    // The shifted tip must leave a tooth (§4.5): halve the shift until it does (0 — the tip circle itself).
    for (int halving = 0;; ++halving) {
        const double shift = halving > GEAR_TIP_SHIFT_HALVINGS ? 0.0 : tipShift / double(1 << halving);
        if (p.internal) {
            auto s = internalSide(p, *g, shift);
            if (!s) return s.error();
            b.side = s->side;
        } else {
            auto s = externalSide(p, *g, shift);
            if (!s) return s.error();
            b.side = s->side;
        }
        if (shift == 0 || tau - 2 * b.side.tipHalf > GEAR_ANGLE_TOL) break;
    }
    return b;
}

// Space k (number for IDs) centred at angle θ: its minus side (tip → root) and plus side (root → tip).
void appendMinus(const Side& s, double theta, int k, Loop& out) {
    for (auto it = s.segs.rbegin(); it != s.segs.rend(); ++it) {
        Seg q = reversedSeg(transformed(*it, theta, true));
        q.id = it->id + "m" + std::to_string(k);
        out.push_back(std::move(q));
    }
}
void appendPlus(const Side& s, double theta, int k, Loop& out) {
    for (const auto& seg : s.segs) {
        Seg q = transformed(seg, theta, false);
        q.id = seg.id + "p" + std::to_string(k);
        out.push_back(std::move(q));
    }
}
Seg circleArc(const std::string& id, double R, double from, double to) {  // about the origin, counterclockwise if to > from
    return arcSeg(id, Point2::Zero(), R, polar(R, from), polar(R, to), to > from);
}

}  // namespace

const BasicRack& basicRack() {
    static const BasicRack r{20.0 * PI / 180.0, 1.0, 0.25, 0.38, "ГОСТ 13755-2015"};
    return r;
}

double involute(double a) { return std::tan(a) - a; }

double flankAngle(const Params& p, double R) {
    const BasicRack& k = basicRack();
    const double d = p.m * p.z, rb = d / 2 * std::cos(k.alpha);
    const double t = std::acos(std::clamp(rb / R, -1.0, 1.0));
    const double w = p.m * (PI / 2 + 2 * p.x * std::tan(k.alpha));  // external: tooth s; internal: space e
    if (p.internal) return w / d + involute(k.alpha) - involute(t);
    return PI / p.z - (w / d + involute(k.alpha) - involute(t));
}

Result<Geometry> geometry(const Params& p) {
    const BasicRack& k = basicRack();
    if (!(p.m > 0) || !std::isfinite(p.m)) return badValue("module");
    if (p.z < (p.internal ? GEAR_MIN_TEETH_INTERNAL : GEAR_MIN_TEETH) || p.z > GEAR_MAX_TEETH) return badValue("teeth");
    if (!std::isfinite(p.x) || std::abs(p.x) > GEAR_MAX_SHIFT) return badValue("shift");
    Geometry g;
    const double m = p.m;
    g.d = m * p.z;
    g.db = g.d * std::cos(k.alpha);
    g.p = PI * m;
    if (p.internal) {
        g.da = g.d - 2 * m * (k.ha - p.x);
        g.df = g.d + 2 * m * (k.ha + k.c + p.x);
        g.e = m * (PI / 2 + 2 * p.x * std::tan(k.alpha));
        g.s = g.p - g.e;
        const double Rlow = std::max(g.da / 2, g.db / 2);
        g.sa = g.da / 2 * (2 * PI / p.z - 2 * flankAngle(p, Rlow));
        if (g.da / 2 < g.db / 2) g.warnings.push_back(Error("GEAR_TIP_BELOW_BASE"));
    } else {
        g.da = g.d + 2 * m * (k.ha + p.x);
        g.df = g.d - 2 * m * (k.ha + k.c - p.x);
        g.s = m * (PI / 2 + 2 * p.x * std::tan(k.alpha));
        g.e = g.p - g.s;
        g.sa = g.da * (PI / p.z - flankAngle(p, g.da / 2));
        g.zMin = 2 * (k.ha - p.x) / (std::sin(k.alpha) * std::sin(k.alpha));
    }
    if (g.sa <= 0) return Error("GEAR_POINTED");
    if (g.sa < GEAR_MIN_TIP_FACTOR * m) g.warnings.push_back(Error("GEAR_THIN_TIP").with("actual", g.sa));
    if (p.internal) {
        auto s = internalSide(p, g);
        if (!s) return s.error();
        g.rForm = s->rForm;
        if (s->filletRadius < basicRack().rho * m * (1 - 1e-12)) g.warnings.push_back(Error("GEAR_FILLET_REDUCED").with("actual", s->filletRadius));
    } else {
        auto s = externalSide(p, g);
        if (!s) return s.error();
        g.rForm = s->rForm;
        // Undercut (§4.2 item 5): the generating curve leaves the involute space, or the classical criterion z < zMin
        // (the flank end generates past the interference point; the loop may be thinner than GEAR_JOIN_TOL).
        g.undercut = s->undercut || p.z < g.zMin * (1 - 1e-12);
        if (g.undercut) g.warnings.insert(g.warnings.begin(), Error("GEAR_UNDERCUT").with("teeth", double(p.z)).with("expected", g.zMin));
    }
    return g;
}

int sectorTeeth(int z, double angle) { return int(std::floor(angle * z / (2 * PI) + GEAR_ANGLE_TOL)); }

Result<Loop> outline(const Params& p, double phase, double tipShift) {
    auto b = build(p, tipShift);
    if (!b) return b.error();
    const double tau = 2 * PI / p.z;
    const Side& s = b->side;
    Loop out;
    for (int k = 1; k <= p.z; ++k) {
        const double theta = phase + (k - 0.5) * tau;  // space k; tooth k centred at phase + (k − 1)·τ
        out.push_back(circleArc("t" + std::to_string(k), s.rTip, theta - tau + s.tipHalf, theta - s.tipHalf));
        appendMinus(s, theta, k, out);
        out.push_back(circleArc("r" + std::to_string(k), s.rRoot, theta - s.rootHalf, theta + s.rootHalf));
        appendPlus(s, theta, k, out);
    }
    return out;
}

Result<Section> toolSection(const Params& p, const Layout& layout, double aux, double tipShift) {
    auto b = build(p, tipShift);
    if (!b) return b.error();
    const Side& s = b->side;
    if (!(p.internal ? aux > 0 && aux < s.rTip : aux > s.rTip)) return badValue("aux");
    Section out;
    if (!layout.sector) {
        auto o = outline(p, layout.phase, tipShift);
        if (!o) return o.error();
        Seg c;
        c.kind = Seg::Kind::Circle;
        c.id = "w1";
        c.radius = aux;
        c.a = c.b = Point2(aux, 0);
        if (p.internal) out.outer = std::move(*o), out.holes.push_back({c});
        else out.outer = {c}, out.holes.push_back(std::move(*o));
        out.teeth = p.z;
        return out;
    }
    if (!(layout.angle > 0) || layout.angle >= 2 * PI - GEAR_ANGLE_TOL) return badValue("sector");
    const int n = sectorTeeth(p.z, layout.angle);
    if (n < 1) return Error("GEAR_SECTOR_EMPTY").with("teeth", double(p.z));
    const double tau = 2 * PI / p.z, t0 = layout.start, t1 = layout.start + n * tau;
    Loop& L = out.outer;
    L.push_back(circleArc("r1", s.rRoot, t0, t0 + s.rootHalf));
    appendPlus(s, t0, 1, L);
    for (int k = 1; k <= n; ++k) {
        const double theta = t0 + k * tau;  // space k + 1
        L.push_back(circleArc("t" + std::to_string(k), s.rTip, theta - tau + s.tipHalf, theta - s.tipHalf));
        appendMinus(s, theta, k + 1, L);
        if (k < n) {
            L.push_back(circleArc("r" + std::to_string(k + 1), s.rRoot, theta - s.rootHalf, theta + s.rootHalf));
            appendPlus(s, theta, k + 1, L);
        }
    }
    L.push_back(circleArc("r" + std::to_string(n + 1), s.rRoot, t1 - s.rootHalf, t1));
    L.push_back(lineSeg("w2", polar(s.rRoot, t1), polar(aux, t1)));
    L.push_back(circleArc("w1", aux, t1, t0));
    L.push_back(lineSeg("w3", polar(aux, t0), polar(s.rRoot, t0)));
    out.teeth = n;
    return out;
}

double loopArea(const Loop& loop) {
    double A = 0;
    for (const auto& s : loop) {
        switch (s.kind) {
            case Seg::Kind::Line: A += 0.5 * cross2(s.a, s.b); break;
            case Seg::Kind::Circle: A += PI * s.radius * s.radius; break;
            case Seg::Kind::Arc: {
                const double ta = angleOf(s.a - s.center), tb = angleOf(s.b - s.center);
                const double sweep = s.ccw ? geom2::ccwSweepPositive(ta, tb) : -geom2::ccwSweepPositive(tb, ta);
                const double r = s.radius, cx = s.center.x(), cy = s.center.y();
                A += 0.5 * (r * r * sweep + cx * r * (std::sin(ta + sweep) - std::sin(ta)) - cy * r * (std::cos(ta + sweep) - std::cos(ta)));
                break;
            }
            case Seg::Kind::Spline: {
                const spline::Flat f = spline::expand(s.curve);
                A += spline::greenArea(f, f.a, f.b);
                break;
            }
        }
    }
    return A;
}

Point2 segPoint(const Seg& s, double u) {
    switch (s.kind) {
        case Seg::Kind::Line: return s.a + u * (s.b - s.a);
        case Seg::Kind::Circle: return s.center + polar(s.radius, 2 * PI * u);
        case Seg::Kind::Arc: {
            const double ta = angleOf(s.a - s.center), tb = angleOf(s.b - s.center);
            const double sweep = s.ccw ? geom2::ccwSweepPositive(ta, tb) : -geom2::ccwSweepPositive(tb, ta);
            return s.center + polar(s.radius, ta + u * sweep);
        }
        case Seg::Kind::Spline: {
            const spline::Flat f = spline::expand(s.curve);
            return spline::eval(f, f.a + u * (f.b - f.a));
        }
    }
    return s.a;
}

}  // namespace construe::gear
