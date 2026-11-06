// Sketch profiles: loops, regions, interior point, triangulation (spec/sketch.md §3).
#include "sketch/sketch.h"

#include "base/geom2.h"
#include "base/ids.h"
#include "font/font.h"
#include "sketch/region_internal.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>

namespace construe::sketch {

namespace {

constexpr double TOL = PROFILE_MERGE_TOL;

using geom2::angleOf;
using geom2::ccwAngle;
using geom2::cross2;
using geom2::perp;

bool idLess(const std::string& a, const std::string& b) { return compareIds(a, b) < 0; }

struct Curve {
    std::string id;
    Segment::Kind kind = Segment::Kind::Line;
    Point2 a = Point2::Zero(), b = Point2::Zero();  // line p1,p2 / arc, ellipse arc start,end
    Point2 c = Point2::Zero();
    double r = 0, alpha = 0;
    double rx = 0, ry = 0, ang = 0, t0 = 0;  // ellipse arc (S4-SKT-007): shape, start parameter; alpha — parameter sweep
    std::string pa, pb;  // IDs of the end points (line/arc)
    int na = -1, nb = -1;  // nodes
    std::shared_ptr<const spline::Flat> sp;  // spline (S4A-SPL-003): open clamped piece over [sp->a, sp->b]
};

// Spline (§3.2 item 0б, S4A-SPL-003): parameter of τ ∈ [0, 1], point, unit tangent and curvature at an end.
double splT(const Curve& k, double tau) { return k.sp->a + tau * (k.sp->b - k.sp->a); }
Point2 splTangent(const Curve& k, bool atStart) {
    Point2 d[3];
    spline::derivs(*k.sp, atStart ? k.sp->a : k.sp->b, d);
    return d[1].normalized();
}
double splCurvature(const Curve& k, bool atStart) { return spline::curvature(*k.sp, atStart ? k.sp->a : k.sp->b); }
// Scale of the tangential closeness band at a node: min(length, largest end radius of curvature) (§3.2 item 0б).
double splRho(const Curve& k) {
    const double len = spline::length(*k.sp, k.sp->a, k.sp->b);
    double rho = 0;
    for (bool st : {true, false}) {
        const double kap = std::abs(splCurvature(k, st));
        rho = std::max(rho, kap > 0 ? 1 / kap : len);
    }
    return std::min(rho, len);
}
Curve splinePiece(const std::string& id, const spline::Flat& f, const std::string& pa, const std::string& pb) {
    Curve k;
    k.id = id;
    k.kind = Segment::Kind::Spline;
    k.sp = std::make_shared<const spline::Flat>(f);
    k.a = spline::eval(f, f.a);
    k.b = spline::eval(f, f.b);
    k.pa = pa;
    k.pb = pb;
    return k;
}

// Point X on an arc (by angle, with tolerance).
bool onArcAngle(const Curve& k, const Point2& X) {
    if (k.kind == Segment::Kind::Circle) return true;
    return geom2::onArc(k.c, k.a, k.alpha, X, TOL / std::max(k.r, TOL));
}

bool onSegmentParam(const Curve& k, const Point2& X) {
    const Point2 d = k.b - k.a;
    const double len2 = d.squaredNorm();
    if (len2 == 0) return false;
    const double t = (X - k.a).dot(d) / len2;
    const double eps = TOL / std::sqrt(len2);
    return t >= -eps && t <= 1 + eps;
}

bool onCurveExtent(const Curve& k, const Point2& X) {
    if (k.kind == Segment::Kind::Spline) return (spline::eval(*k.sp, spline::closestParam(*k.sp, X)) - X).norm() <= 10 * TOL;
    return k.kind == Segment::Kind::Line ? onSegmentParam(k, X) : onArcAngle(k, X);
}

// Ellipse arc (§3.2 item 0а, S4-SKT-007): point at parameter t, parameter of a point, tangent, curvature.
Point2 ellUnit(const Curve& k) { return {std::cos(k.ang), std::sin(k.ang)}; }
Point2 ellAt(const Curve& k, double t) {
    const Point2 u = ellUnit(k);
    return k.c + k.rx * std::cos(t) * u + k.ry * std::sin(t) * perp(u);
}
double ellParam(const Curve& k, const Point2& X) {
    const Point2 u = ellUnit(k), w = X - k.c;
    return std::atan2(w.dot(perp(u)) / k.ry, w.dot(u) / k.rx);
}
// Parameter of X from the arc start in [0, 2π).
double ellFromStart(const Curve& k, const Point2& X) {
    return geom2::normalizeAngle0To2Pi(ellParam(k, X) - k.t0);
}
bool onEllArc(const Curve& k, const Point2& X) {
    const double t = ellFromStart(k, X), eps = TOL / std::min(k.rx, k.ry);
    return t <= k.alpha + eps || t >= 2 * PI - eps;
}
Point2 ellTangent(const Curve& k, double t) {
    const Point2 u = ellUnit(k);
    return (-k.rx * std::sin(t) * u + k.ry * std::cos(t) * perp(u)).normalized();
}
double ellCurvature(const Curve& k, double t) {
    const double s = std::sin(t), c = std::cos(t);
    return k.rx * k.ry / std::pow(k.rx * k.rx * s * s + k.ry * k.ry * c * c, 1.5);
}
// Largest radius of curvature of the ellipse: max(rx, ry)²/min(rx, ry).
double ellRho(const Curve& k) { return std::max(k.rx, k.ry) * std::max(k.rx, k.ry) / std::min(k.rx, k.ry); }
double ellSampson(const Curve& k, const Point2& X) {
    const Point2 u = ellUnit(k), w = X - k.c;
    const double s = w.dot(u), t = w.dot(perp(u));
    const double F = s * s / (k.rx * k.rx) + t * t / (k.ry * k.ry) - 1;
    const double g = 2 * std::hypot(s / (k.rx * k.rx), t / (k.ry * k.ry));
    return F / std::max(g, 1e-300);
}

// Point of a curve at τ ∈ [0, 1] over its extent (circle — the whole circle).
Point2 curveAt(const Curve& k, double tau) {
    switch (k.kind) {
        case Segment::Kind::Line: return k.a + tau * (k.b - k.a);
        case Segment::Kind::Circle: return k.c + k.r * Point2(std::cos(2 * PI * tau), std::sin(2 * PI * tau));
        case Segment::Kind::EllipseArc: return ellAt(k, k.t0 + tau * k.alpha);
        case Segment::Kind::Spline: return spline::eval(*k.sp, splT(k, tau));
        default: {
            const double a = angleOf(k.a - k.c) + tau * k.alpha;
            return k.c + k.r * Point2(std::cos(a), std::sin(a));
        }
    }
}

double segSegDistance(const Curve& k, const Curve& m) {
    // Segment intersection?
    const Point2 r = k.b - k.a, s = m.b - m.a;
    const double den = cross2(r, s);
    if (std::abs(den) > 0) {
        const double t = cross2(m.a - k.a, s) / den, u = cross2(m.a - k.a, r) / den;
        if (t >= 0 && t <= 1 && u >= 0 && u <= 1) return 0.0;
    }
    return std::min({geom2::distToSegment(k.a, m.a, m.b), geom2::distToSegment(k.b, m.a, m.b), geom2::distToSegment(m.a, k.a, k.b),
                     geom2::distToSegment(m.b, k.a, k.b)});
}

// Intersection points of the carriers (with tangency within tolerance).
std::vector<Point2> lineCircle(const Curve& l, const Point2& c, double R) {
    return geom2::lineCircle(l.a, (l.b - l.a).normalized(), c, R, TOL);
}

// Length of the common part of two arcs/circles on the same carrier (§3.2 item 2): intersection of angular intervals
// [start, start + sweep] on the circle, accounting for ±2π shifts; multiplied by the radius.
double arcOverlapLength(const Curve& k, const Curve& m) {
    auto interval = [](const Curve& c) {
        if (c.kind == Segment::Kind::Circle) return std::pair{0.0, 2 * PI};
        double a0 = angleOf(c.a - c.c);
        if (a0 < 0) a0 += 2 * PI;
        return std::pair{a0, a0 + c.alpha};
    };
    const auto [a0, a1] = interval(k);
    const auto [b0, b1] = interval(m);
    double common = 0;
    for (double shift : {-2 * PI, 0.0, 2 * PI}) common += std::max(0.0, std::min(a1, b1 + shift) - std::max(a0, b0 + shift));
    return std::min(common, 2 * PI) * k.r;
}

// Sketch ellipse in profiles (§3.1, S4-SKT-004).
struct Ell {
    std::string id;
    Point2 c = Point2::Zero();
    double rx = 0, ry = 0, ang = 0;
    Point2 u() const { return {std::cos(ang), std::sin(ang)}; }
    Point2 v() const { return {-std::sin(ang), std::cos(ang)}; }
    Point2 at(double t) const { return c + rx * std::cos(t) * u() + ry * std::sin(t) * v(); }
    // Sampson distance F/‖∇F‖, F = (s/rx)² + (t/ry)² − 1: sign — outside (+) / inside (−), near the curve ≈ distance.
    double sampson(const Point2& X) const {
        const Point2 w = X - c;
        const double s = w.dot(u()), t = w.dot(v());
        const double F = s * s / (rx * rx) + t * t / (ry * ry) - 1;
        const double g = 2 * std::hypot(s / (rx * rx), t / (ry * ry));
        return F / std::max(g, 1e-300);
    }
    bool inside(const Point2& X) const {
        const Point2 w = X - c;
        const double s = w.dot(u()), t = w.dot(v());
        return s * s / (rx * rx) + t * t / (ry * ry) < 1;
    }
};

// Whether g(τ), τ ∈ [0, 1], has a zero: sign change between samples or min |g| ≤ TOL (tangency), §3.2 item 0.
bool hasContact(const std::function<double(double)>& g) {
    const int N = ELLIPSE_CROSS_SAMPLES;
    std::vector<double> v(N + 1);
    for (int i = 0; i <= N; ++i) v[i] = g(static_cast<double>(i) / N);
    for (int i = 0; i <= N; ++i) {
        if (std::abs(v[i]) <= TOL) return true;
        if (i < N && ((v[i] < 0) != (v[i + 1] < 0))) return true;
    }
    for (int i = 0; i <= N; ++i) {
        const double prev = i > 0 ? std::abs(v[i - 1]) : std::numeric_limits<double>::infinity();
        const double next = i < N ? std::abs(v[i + 1]) : std::numeric_limits<double>::infinity();
        if (std::abs(v[i]) > prev || std::abs(v[i]) > next) continue;
        // Refining the minimum of |g| by golden-section search on [τ(i−1), τ(i+1)].
        double lo = static_cast<double>(std::max(0, i - 1)) / N, hi = static_cast<double>(std::min(N, i + 1)) / N;
        const double gr = (std::sqrt(5.0) - 1) / 2;
        double x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
        double f1 = std::abs(g(x1)), f2 = std::abs(g(x2));
        for (int k = 0; k < ELLIPSE_REFINE_ITER; ++k) {
            if (f1 < f2) {
                hi = x2, x2 = x1, f2 = f1;
                x1 = hi - gr * (hi - lo), f1 = std::abs(g(x1));
            } else {
                lo = x1, x1 = x2, f1 = f2;
                x2 = lo + gr * (hi - lo), f2 = std::abs(g(x2));
            }
        }
        if (std::min(f1, f2) <= TOL) return true;
    }
    return false;
}

// Contacts of two splines (§3.2 item 0б): intersections of their flattened polylines refined by Newton (§2.7.5).
std::vector<Point2> splineSplineContacts(const Curve& A, const Curve& B) {
    std::vector<Point2> out;
    for (const auto& st : spline::intersections(*A.sp, *B.sp, TOL)) out.push_back(spline::eval(*A.sp, st.first));
    return out;
}

}  // namespace

std::vector<double> contactTaus(const std::function<double(double)>& g, double tol) {
    const double TOL = tol;
    const int N = ELLIPSE_CROSS_SAMPLES;
    std::vector<double> v(N + 1), out;
    for (int i = 0; i <= N; ++i) v[i] = g(static_cast<double>(i) / N);
    for (int i = 0; i <= N; ++i) {
        if (std::abs(v[i]) <= TOL) out.push_back(static_cast<double>(i) / N);
        if (i < N && std::abs(v[i]) > TOL && std::abs(v[i + 1]) > TOL && ((v[i] < 0) != (v[i + 1] < 0))) {
            double lo = static_cast<double>(i) / N, hi = static_cast<double>(i + 1) / N, glo = v[i];
            for (int k = 0; k < ELLIPSE_REFINE_ITER; ++k) {
                const double mid = (lo + hi) / 2, gm = g(mid);
                if ((gm < 0) == (glo < 0)) lo = mid, glo = gm;
                else hi = mid;
            }
            out.push_back((lo + hi) / 2);
        }
    }
    for (int i = 0; i <= N; ++i) {
        const double prev = i > 0 ? std::abs(v[i - 1]) : std::numeric_limits<double>::infinity();
        const double next = i < N ? std::abs(v[i + 1]) : std::numeric_limits<double>::infinity();
        if (std::abs(v[i]) <= TOL || std::abs(v[i]) > prev || std::abs(v[i]) > next) continue;
        double lo = static_cast<double>(std::max(0, i - 1)) / N, hi = static_cast<double>(std::min(N, i + 1)) / N;
        const double gr = (std::sqrt(5.0) - 1) / 2;
        double x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
        double f1 = std::abs(g(x1)), f2 = std::abs(g(x2));
        for (int k = 0; k < ELLIPSE_REFINE_ITER; ++k) {
            if (f1 < f2) {
                hi = x2, x2 = x1, f2 = f1;
                x1 = hi - gr * (hi - lo), f1 = std::abs(g(x1));
            } else {
                lo = x1, x1 = x2, f1 = f2;
                x2 = lo + gr * (hi - lo), f2 = std::abs(g(x2));
            }
        }
        if (std::min(f1, f2) <= TOL) out.push_back(f1 < f2 ? x1 : x2);
    }
    return out;
}

namespace {

struct UF {
    std::vector<int> p;
    explicit UF(int n) : p(n) {
        for (int i = 0; i < n; ++i) p[i] = i;
    }
    int find(int x) { return p[x] == x ? x : p[x] = find(p[x]); }
    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) p[std::max(a, b)] = std::min(a, b);
    }
};

struct Contour {
    std::vector<int> halfEdges;   // empty for a circle
    int circle = -1;              // index of the circle curve
    int ellipse = -1;             // ellipse index (S4-SKT-004)
    double area = 0;
    int component = -1;
    std::string regionId;
    int parent = -1;
    int depth = 0;
};

}  // namespace

ProfileSet regions(const SketchData& s, const SolvedState& state) {
    ProfileSet out;
    auto P = [&](const std::string& id) {
        auto it = state.points.find(id);
        if (it != state.points.end()) return it->second;
        const Entity* e = s.findEntity(id);
        return e ? Point2(e->x, e->y) : Point2::Zero();
    };

    // 1. Curves.
    std::vector<Curve> curves;
    std::vector<Ell> ellipses;  // ellipses — separate closed loops (§3.2 item 0, item 3)
    for (const auto& e : s.entities) {
        if (e.construction) continue;
        Curve k;
        k.id = e.id;
        if (e.type == EntityType::Ellipse) {
            Ell el;
            el.id = e.id;
            el.c = P(e.center);
            auto it = state.ellipses.find(e.id);
            const SolvedState::EllipseShape sh = it != state.ellipses.end() ? it->second : SolvedState::EllipseShape{e.rx, e.ry, e.angle};
            el.rx = sh.rx, el.ry = sh.ry, el.ang = sh.angle;
            if (el.rx > 0 && el.ry > 0) ellipses.push_back(el);
            continue;
        }
        if (isSplineType(e.type)) {
            // Spline (§3.2 item 0б): an edge between its end poles; a closed one (periodic, or open with coincident ends)
            // — two pieces <ID>@1, <ID>@2 halved by the parameter, the joint nodes under the spline's own ID.
            const spline::Curve sc = splineCurve(e, state, &s);
            if (!spline::validate(sc).empty()) continue;
            const spline::Flat f = spline::expand(sc);
            const bool closed = sc.periodic || (spline::eval(f, f.a) - spline::eval(f, f.b)).norm() <= TOL;
            if (!closed) {
                curves.push_back(splinePiece(e.id, f, e.poles.front(), e.poles.back()));
            } else {
                const double mid = (f.a + f.b) / 2;
                const std::string first = sc.periodic ? e.id : e.poles.front(), last = sc.periodic ? e.id : e.poles.back();
                curves.push_back(splinePiece(e.id + "@1", spline::segment(f, f.a, mid), first, e.id));
                curves.push_back(splinePiece(e.id + "@2", spline::segment(f, mid, f.b), e.id, last));
            }
            continue;
        }
        if (e.type == EntityType::Line) {
            k.kind = Segment::Kind::Line;
            k.a = P(e.p1);
            k.b = P(e.p2);
            k.pa = e.p1;
            k.pb = e.p2;
        } else if (e.type == EntityType::Arc) {
            k.kind = Segment::Kind::Arc;
            k.c = P(e.center);
            k.a = P(e.start);
            k.b = P(e.end);
            k.r = (k.a - k.c).norm();
            k.alpha = geom2::arcSweep(k.c, k.a, k.b);
            k.pa = e.start;
            k.pb = e.end;
        } else if (e.type == EntityType::EllipseArc) {  // S4-SKT-007: a graph edge, not split (§3.2 item 0а)
            k.kind = Segment::Kind::EllipseArc;
            k.c = P(e.center);
            k.a = P(e.start);
            k.b = P(e.end);
            auto it = state.ellipses.find(e.id);
            const SolvedState::EllipseShape sh = it != state.ellipses.end() ? it->second : SolvedState::EllipseShape{e.rx, e.ry, e.angle};
            k.rx = sh.rx, k.ry = sh.ry, k.ang = sh.angle;
            if (!(k.rx > 0 && k.ry > 0)) continue;
            k.t0 = ellParam(k, k.a);
            k.alpha = ellFromStart(k, k.b);
            if (k.alpha <= 0) k.alpha = 2 * PI;
            k.pa = e.start;
            k.pb = e.end;
        } else if (e.type == EntityType::Circle) {
            k.kind = Segment::Kind::Circle;
            k.c = P(e.center);
            auto it = state.radii.find(e.id);
            k.r = it != state.radii.end() ? it->second : e.r;
        } else if (e.type == EntityType::Projected) {
            // Projected curve (§2.5); its ends are not entities: they go into nodes under the projection's own ID.
            auto it = s.projections.find(e.id);
            if (it == s.projections.end()) continue;
            const Projection& g = it->second;
            k.pa = k.pb = e.id;
            if (g.kind == Projection::Kind::Line) {
                k.kind = Segment::Kind::Line;
                k.a = Point2(g.x1, g.y1);
                k.b = Point2(g.x2, g.y2);
            } else if (g.kind == Projection::Kind::Arc) {
                k.kind = Segment::Kind::Arc;
                k.c = Point2(g.cx, g.cy);
                k.a = Point2(g.sx, g.sy);
                k.b = Point2(g.ex, g.ey);
                k.r = (k.a - k.c).norm();
                k.alpha = geom2::arcSweep(k.c, k.a, k.b);
            } else if (g.kind == Projection::Kind::Circle) {
                k.kind = Segment::Kind::Circle;
                k.c = Point2(g.cx, g.cy);
                k.r = g.r;
            } else {
                continue;  // a point does not take part in profiles
            }
        } else if (e.type == EntityType::Text) {
            // Text outlines (§3.1, S2-SK-013): curves <ID>@k, ends — under the text's ID (as for projections).
            auto f = font::loadFont(e.font);
            if (!f) {
                out.messages.push_back(Error("SKETCH_TEXT_FONT").with("id", e.id).with("font", e.font).with("detail", f.error().code));
                continue;
            }
            const font::Layout lay = font::layoutText(**f, e.text, e.height, P(e.center), e.angle, e.styles);
            for (const auto& w : lay.warnings) {
                const std::string* ch = w.stringParam("char");
                out.messages.push_back(Error("SKETCH_TEXT_MISSING_GLYPH").with("id", e.id).with("char", ch ? *ch : std::string()));
            }
            int n = 0;
            for (const auto& contour : lay.contours)
                for (const auto& c : contour) {
                    Curve t;
                    t.id = e.id + "@" + std::to_string(++n);
                    t.pa = t.pb = e.id;
                    t.a = c.a;
                    t.b = c.b;
                    if (c.kind == font::Curve2::Kind::Line) {
                        t.kind = Segment::Kind::Line;
                    } else {
                        t.kind = Segment::Kind::Arc;
                        t.c = c.center;
                        t.r = (c.a - c.center).norm();
                        t.alpha = geom2::arcSweep(t.c, t.a, t.b);
                    }
                    curves.push_back(t);
                }
            continue;
        } else {
            continue;
        }
        curves.push_back(k);
    }
    std::sort(curves.begin(), curves.end(), [](const Curve& x, const Curve& y) { return idLess(x.id, y.id); });
    // 0. An ellipse crossing or touching any other curve is excluded from profiles (§3.2 item 0).
    std::sort(ellipses.begin(), ellipses.end(), [](const Ell& x, const Ell& y) { return idLess(x.id, y.id); });
    {
        std::vector<std::string> crossing;
        std::vector<bool> bad(ellipses.size(), false);
        for (size_t i = 0; i < ellipses.size(); ++i) {
            const Ell& E = ellipses[i];
            for (const Curve& k : curves) {
                if (bad[i]) break;
                bad[i] = hasContact([&](double tau) { return E.sampson(curveAt(k, tau)); });
            }
            for (size_t j = 0; j < ellipses.size(); ++j) {
                if (i == j) continue;
                const Ell& F = ellipses[j];
                if (hasContact([&](double tau) { return E.sampson(F.at(2 * PI * tau)); })) bad[i] = bad[j] = true;
            }
        }
        std::vector<Ell> kept;
        for (size_t i = 0; i < ellipses.size(); ++i) (bad[i] ? (void)crossing.push_back(ellipses[i].id) : kept.push_back(ellipses[i]));
        ellipses = std::move(kept);
        if (!crossing.empty()) {
            Error w("SKETCH_ELLIPSE_CROSSING");
            w.with("ids", crossing);
            out.messages.insert(out.messages.begin(), w);
        }
    }
    auto isProjection = [&](const std::string& id) {
        const Entity* e = s.findEntity(id);
        return e && e->type == EntityType::Projected;
    };
    auto baseOf = [](const std::string& id) { return id.substr(0, id.find('@')); };
    auto isText = [&](const std::string& id) {
        const Entity* e = s.findEntity(id.substr(0, id.find('@')));
        return e && e->type == EntityType::Text;
    };

    // Steps 1–2 are repeated after splitting the curves at intersection points (§3.2 item 2а, S2-SK-007).
    std::vector<std::string> nodePointIds;
    std::vector<Point2> nodePos;
    UF uf(0);
    std::map<int, int> repOf;
    std::vector<std::string> openIds;
    std::vector<Curve> live;
    std::set<std::string> crossIds, overlapIds;
    std::set<std::string> ellipseBad;  // ellipse arcs with contacts outside shared nodes (§3.2 item 0а)
    int synthetic = 0;
    auto nodeOf = [&](int endpoint) { return uf.find(endpoint); };
    for (int pass = 0;; ++pass) {
    nodePointIds.clear();
    nodePos.clear();
    repOf.clear();
    openIds.clear();
    live.clear();
    crossIds.clear();
    overlapIds.clear();
    ellipseBad.clear();
    std::map<std::string, std::vector<Point2>> splits;  // split points of own curves
    for (auto& k : curves) {
        if (k.kind == Segment::Kind::Circle) continue;
        k.na = static_cast<int>(nodePos.size());
        nodePointIds.push_back(k.pa);
        nodePos.push_back(k.a);
        k.nb = static_cast<int>(nodePos.size());
        nodePointIds.push_back(k.pb);
        nodePos.push_back(k.b);
    }
    uf = UF(static_cast<int>(nodePos.size()));
    for (size_t i = 0; i < nodePos.size(); ++i)
        for (size_t j = i + 1; j < nodePos.size(); ++j)
            if ((nodePos[i] - nodePos[j]).norm() <= TOL) uf.unite(static_cast<int>(i), static_cast<int>(j));
    // Node representative — the point with the smallest ID (synthetic z<N> — after the sketch points).
    for (size_t i = 0; i < nodePos.size(); ++i) {
        const int r = uf.find(static_cast<int>(i));
        auto it = repOf.find(r);
        if (it == repOf.end() || idLess(nodePointIds[i], nodePointIds[it->second])) repOf[r] = static_cast<int>(i);
    }

    for (auto& k : curves) {
        if (k.kind != Segment::Kind::Circle && nodeOf(k.na) == nodeOf(k.nb)) {
            openIds.push_back(k.id);  // degenerate curve
            continue;
        }
        live.push_back(k);
    }

    // 2. Intersections and overlaps.
    auto sharedNodeAt = [&](const Curve& k, const Curve& m, const Point2& X) {
        if (k.kind == Segment::Kind::Circle || m.kind == Segment::Kind::Circle) return false;
        const int kn[2] = {nodeOf(k.na), nodeOf(k.nb)};
        const int mn[2] = {nodeOf(m.na), nodeOf(m.nb)};
        for (int a : kn)
            for (int b : mn)
                if (a == b && (nodePos[repOf[a]] - X).norm() <= 10 * TOL) return true;
        return false;
    };
    auto shareNode = [&](const Curve& k, const Curve& m) {
        if (k.kind == Segment::Kind::Circle || m.kind == Segment::Kind::Circle) return false;
        const int kn[2] = {nodeOf(k.na), nodeOf(k.nb)};
        const int mn[2] = {nodeOf(m.na), nodeOf(m.nb)};
        for (int a : kn)
            for (int b : mn)
                if (a == b) return true;
        return false;
    };
    // A spline crossing itself (§3.2 item 0б) — a crossing that is not split.
    for (const Curve& k : live)
        if (k.kind == Segment::Kind::Spline && spline::selfIntersects(*k.sp, std::max(TOL, 1e-3 * (k.b - k.a).norm())))
            crossIds.insert(k.id);
    for (size_t i = 0; i < live.size(); ++i)
        for (size_t j = i + 1; j < live.size(); ++j) {
            const Curve& k = live[i];
            const Curve& m = live[j];
            // Pairs of curves of the same text are not checked (§3.1): font outlines do not intersect, while adjacent
            // approximation pieces join almost tangentially — carrier intersection would give false points there.
            if (baseOf(k.id) == baseOf(m.id) && isText(k.id)) continue;
            auto markCross = [&]() {
                crossIds.insert(k.id);
                crossIds.insert(m.id);
            };
            // Split point of both curves (§3.2 item 2а); dropped for a curve if it coincides with its end.
            auto splitAt = [&](const Point2& X) {
                splits[k.id].push_back(X);
                splits[m.id].push_back(X);
            };
            auto markOverlap = [&]() {
                overlapIds.insert(k.id);
                overlapIds.insert(m.id);
            };
            if (k.kind == Segment::Kind::EllipseArc || m.kind == Segment::Kind::EllipseArc) {
                // Ellipse arc (§3.2 item 0а): numeric contacts; allowed at a shared node; the end of the ellipse arc
                // inside another curve — a T-junction (that curve is split); otherwise the ellipse arc is excluded.
                double rho = 0;
                for (const Curve* c : {&k, &m}) {
                    if (c->kind == Segment::Kind::EllipseArc) rho = std::max(rho, ellRho(*c));
                    if (c->kind == Segment::Kind::Arc || c->kind == Segment::Kind::Circle) rho = std::max(rho, c->r);
                }
                const double nodeTol = std::max(10 * TOL, 2 * std::sqrt(2 * rho * TOL));
                auto nearSharedNode = [&](const Point2& X) {
                    if (k.kind == Segment::Kind::Circle || m.kind == Segment::Kind::Circle) return false;  // no nodes
                    const int kn[2] = {nodeOf(k.na), nodeOf(k.nb)};
                    const int mn[2] = {nodeOf(m.na), nodeOf(m.nb)};
                    for (int a : kn)
                        for (int b : mn)
                            if (a == b && (nodePos[repOf[a]] - X).norm() <= nodeTol) return true;
                    return false;
                };
                for (const auto& [A, K] : {std::pair{&k, &m}, std::pair{&m, &k}}) {
                    if (A->kind != Segment::Kind::EllipseArc) continue;
                    for (double tau : contactTaus([&](double t) { return ellSampson(*A, curveAt(*K, t)); }, TOL)) {
                        const Point2 X = curveAt(*K, tau);
                        if (!onEllArc(*A, X) || nearSharedNode(X)) continue;
                        const Point2* end = (X - A->a).norm() <= nodeTol ? &A->a : (X - A->b).norm() <= nodeTol ? &A->b : nullptr;
                        if (end && K->kind != Segment::Kind::EllipseArc) {
                            splits[K->id].push_back(*end);  // T-junction: the other curve is split at the end
                            crossIds.insert(K->id);
                            continue;
                        }
                        ellipseBad.insert(A->id);
                        if (K->kind == Segment::Kind::EllipseArc) ellipseBad.insert(K->id);
                    }
                }
                continue;
            }
            if (k.kind == Segment::Kind::Spline || m.kind == Segment::Kind::Spline) {
                // Spline (§3.2 item 0б): numeric contacts; at a shared node (closeness band) — allowed; otherwise both
                // curves are split at the contact point (an end of one on the other — a T-junction).
                double rho = 0;
                for (const Curve* c : {&k, &m}) {
                    if (c->kind == Segment::Kind::Spline) rho = std::max(rho, splRho(*c));
                    if (c->kind == Segment::Kind::Arc || c->kind == Segment::Kind::Circle) rho = std::max(rho, c->r);
                }
                const double nodeTol = std::max(10 * TOL, 2 * std::sqrt(2 * rho * TOL));
                std::vector<Point2> contacts;
                if (k.kind == Segment::Kind::Spline && m.kind == Segment::Kind::Spline) {
                    contacts = splineSplineContacts(k, m);
                } else {
                    const Curve& S = k.kind == Segment::Kind::Spline ? k : m;
                    const Curve& K = k.kind == Segment::Kind::Spline ? m : k;
                    std::function<double(const Point2&)> g;
                    if (K.kind == Segment::Kind::Line) {
                        const Point2 d = (K.b - K.a).normalized();
                        g = [d, a = K.a](const Point2& X) { return cross2(d, X - a); };
                    } else {
                        g = [c = K.c, r = K.r](const Point2& X) { return (X - c).norm() - r; };
                    }
                    for (double t : spline::roots(*S.sp, g, TOL)) {
                        const Point2 X = spline::eval(*S.sp, t);
                        if (onCurveExtent(K, X)) contacts.push_back(X);
                    }
                }
                for (const Point2& X : contacts) {
                    bool nearShared = false;
                    if (k.kind != Segment::Kind::Circle && m.kind != Segment::Kind::Circle)
                        for (int a : {nodeOf(k.na), nodeOf(k.nb)})
                            for (int b : {nodeOf(m.na), nodeOf(m.nb)})
                                nearShared = nearShared || (a == b && (nodePos[repOf[a]] - X).norm() <= nodeTol);
                    if (nearShared) continue;
                    markCross();
                    splitAt(X);
                }
                continue;
            }
            const bool kl = k.kind == Segment::Kind::Line, ml = m.kind == Segment::Kind::Line;
            if (kl && ml) {
                const Point2 d = (k.b - k.a).normalized();
                const double dc = std::abs(cross2(d, m.a - k.a)), dd = std::abs(cross2(d, m.b - k.a));
                if (dc <= TOL && dd <= TOL) {
                    const double len = (k.b - k.a).norm();
                    double t0 = (m.a - k.a).dot(d), t1 = (m.b - k.a).dot(d);
                    if (t0 > t1) std::swap(t0, t1);
                    const double ov = std::min(len, t1) - std::max(0.0, t0);
                    if (ov > TOL) markOverlap();
                    else if (ov >= -TOL && !shareNode(k, m)) markCross();
                    continue;
                }
                if (shareNode(k, m)) continue;
                if (segSegDistance(k, m) <= TOL) {
                    markCross();
                    // Proper intersection or T-junction: intersection point of the lines, or an end on the other curve.
                    const Point2 r = k.b - k.a, sv = m.b - m.a;
                    const double den = cross2(r, sv);
                    bool tee = false;
                    for (const auto& [E, other] : {std::pair{k.a, &m}, {k.b, &m}, {m.a, &k}, {m.b, &k}})
                        if (geom2::distToSegment(E, other->a, other->b) <= TOL) splitAt(E), tee = true;
                    if (!tee && std::abs(den) > 0) splitAt(k.a + (cross2(m.a - k.a, sv) / den) * r);
                }
                continue;
            }
            std::vector<Point2> cand;
            if (kl || ml) {
                const Curve& l = kl ? k : m;
                const Curve& c = kl ? m : k;
                cand = lineCircle(l, c.c, c.r);
            } else {
                if ((k.c - m.c).norm() <= TOL && std::abs(k.r - m.r) <= TOL) {
                    if (k.kind == Segment::Kind::Circle || m.kind == Segment::Kind::Circle ||
                        arcOverlapLength(k, m) > TOL) {
                        markOverlap();
                        continue;
                    }
                    // Arcs on the same carrier without overlap: tangency only at shared nodes.
                    for (const Point2& X : {k.a, k.b})
                        if (onArcAngle(m, X) && !sharedNodeAt(k, m, X)) markCross(), splitAt(X);
                    continue;
                }
                cand = geom2::circleCircle(k.c, k.r, m.c, m.r, TOL);
            }
            for (const Point2& X : cand)
                if (onCurveExtent(k, X) && onCurveExtent(m, X) && !sharedNodeAt(k, m, X)) {
                    markCross();
                    splitAt(X);
                }
        }
    // Splitting own curves (§3.2 item 2а): projections are not split (excluded below); with an overlap — not needed.
    bool anyProjection = false;  // and text curves: they are not split, the text is excluded (§3.1)
    for (const auto* set : {&crossIds, &overlapIds})
        for (const auto& id : *set) anyProjection = anyProjection || isProjection(id) || isText(id);
    if (anyProjection || !overlapIds.empty() || crossIds.empty() || !ellipseBad.empty()) break;
    std::vector<Curve> next;
    bool anySplit = false;
    for (const Curve& k : curves) {
        auto it = splits.find(k.id);
        if (it == splits.end()) {
            next.push_back(k);
            continue;
        }
        if (k.kind == Segment::Kind::Spline) {
            // Spline pieces between split points by the parameter (§3.2 item 2а): sub-curves §2.7.4.
            std::vector<double> ts;
            for (const Point2& X : it->second) {
                if ((X - k.a).norm() <= TOL || (X - k.b).norm() <= TOL) continue;
                ts.push_back(spline::closestParam(*k.sp, X));
            }
            std::sort(ts.begin(), ts.end());
            std::vector<double> uniq;
            for (double t : ts)
                if (uniq.empty() || (spline::eval(*k.sp, t) - spline::eval(*k.sp, uniq.back())).norm() > TOL) uniq.push_back(t);
            if (uniq.empty()) {
                next.push_back(k);
                continue;
            }
            anySplit = true;
            std::vector<std::pair<double, std::string>> nodes{{k.sp->a, k.pa}};
            for (double t : uniq) nodes.push_back({t, "z" + std::to_string(++synthetic)});
            nodes.push_back({k.sp->b, k.pb});
            for (size_t i = 0; i + 1 < nodes.size(); ++i)
                next.push_back(splinePiece(k.id + "@" + std::to_string(i + 1), spline::segment(*k.sp, nodes[i].first, nodes[i + 1].first),
                                           nodes[i].second, nodes[i + 1].second));
            continue;
        }
        // Point parameters: segment — length from a; arc/circle — angle (arc — from start, circle — in [0, 2π)).
        std::vector<double> ts;
        const double len = k.kind == Segment::Kind::Line ? (k.b - k.a).norm() : k.r * (k.kind == Segment::Kind::Arc ? k.alpha : 2 * PI);
        for (const Point2& X : it->second) {
            double t;
            if (k.kind == Segment::Kind::Line) {
                t = (X - k.a).dot((k.b - k.a).normalized());
            } else if (k.kind == Segment::Kind::Arc) {
                t = ccwAngle(k.a - k.c, X - k.c) * k.r;
            } else {
                t = angleOf(X - k.c);
                if (t < 0) t += 2 * PI;
                t *= k.r;
            }
            if (k.kind != Segment::Kind::Circle && (t <= TOL || t >= len - TOL)) continue;
            ts.push_back(t);
        }
        std::sort(ts.begin(), ts.end());
        std::vector<double> uniq;
        for (double t : ts)
            if (uniq.empty() || t - uniq.back() > TOL) uniq.push_back(t);
        if (k.kind == Segment::Kind::Circle && uniq.size() >= 2 && uniq.front() + len - uniq.back() <= TOL) uniq.pop_back();
        if (uniq.empty() || (k.kind == Segment::Kind::Circle && uniq.size() < 2)) {
            next.push_back(k);  // a circle with one tangency point is not split — the intersection remains an error
            continue;
        }
        anySplit = true;
        auto pointAt = [&](double t) -> Point2 {
            if (k.kind == Segment::Kind::Line) return k.a + t * (k.b - k.a).normalized();
            const double a0 = k.kind == Segment::Kind::Arc ? angleOf(k.a - k.c) : 0.0;
            const double a = a0 + t / k.r;
            return k.c + k.r * Point2(std::cos(a), std::sin(a));
        };
        // Nodes of the pieces: curve ends — its points; split points — synthetic z<N>.
        std::vector<std::pair<Point2, std::string>> nodes;
        if (k.kind != Segment::Kind::Circle) nodes.push_back({k.a, k.pa});
        for (double t : uniq) nodes.push_back({pointAt(t), "z" + std::to_string(++synthetic)});
        if (k.kind != Segment::Kind::Circle) nodes.push_back({k.b, k.pb});
        else nodes.push_back(nodes.front());  // closing the circle onto the first point
        for (size_t i = 0; i + 1 < nodes.size(); ++i) {
            Curve piece = k;
            piece.id = k.id + "@" + std::to_string(i + 1);
            piece.kind = k.kind == Segment::Kind::Line ? Segment::Kind::Line : Segment::Kind::Arc;
            piece.a = nodes[i].first, piece.b = nodes[i + 1].first;
            piece.pa = nodes[i].second, piece.pb = nodes[i + 1].second;
            if (piece.kind == Segment::Kind::Arc) piece.alpha = geom2::arcSweep(piece.c, piece.a, piece.b);
            next.push_back(piece);
        }
    }
    if (!anySplit) break;
    curves = std::move(next);
    std::sort(curves.begin(), curves.end(), [](const Curve& x, const Curve& y) { return idLess(x.id, y.id); });
    }
    // Projections and texts in intersections/overlaps are excluded; profiles are rebuilt without them (§2.5, §3.1).
    {
        std::vector<std::string> excluded, texts;
        for (const auto* set : {&crossIds, &overlapIds})
            for (const auto& id : *set) {
                if (const Entity* e = s.findEntity(id); e && e->type == EntityType::Projected &&
                                                        std::find(excluded.begin(), excluded.end(), id) == excluded.end())
                    excluded.push_back(id);
                if (isText(id) && std::find(texts.begin(), texts.end(), baseOf(id)) == texts.end()) texts.push_back(baseOf(id));
            }
        if (!excluded.empty() || !texts.empty() || !ellipseBad.empty()) {
            std::sort(excluded.begin(), excluded.end(), idLess);
            std::sort(texts.begin(), texts.end(), idLess);
            SketchData reduced = s;
            for (auto& e : reduced.entities)
                if (std::find(excluded.begin(), excluded.end(), e.id) != excluded.end() ||
                    std::find(texts.begin(), texts.end(), e.id) != texts.end() || ellipseBad.count(e.id))
                    e.construction = true;
            ProfileSet r = regions(reduced, state);
            if (!ellipseBad.empty()) {  // excluded ellipse arcs — together with the ellipses of item 0 (S4-SKT-007)
                std::vector<std::string> ids(ellipseBad.begin(), ellipseBad.end());
                auto it = std::find_if(r.messages.begin(), r.messages.end(), [](const Error& m) { return m.code == "SKETCH_ELLIPSE_CROSSING"; });
                if (it != r.messages.end()) {
                    const auto& prev = std::get<std::vector<std::string>>(it->params.at("ids"));
                    ids.insert(ids.end(), prev.begin(), prev.end());
                    r.messages.erase(it);
                }
                std::sort(ids.begin(), ids.end(), idLess);
                Error w("SKETCH_ELLIPSE_CROSSING");
                w.with("ids", ids);
                r.messages.insert(r.messages.begin(), w);
            }
            if (!texts.empty()) {
                Error w("SKETCH_TEXT_CROSSING");
                w.with("ids", texts);
                r.messages.insert(r.messages.begin(), w);
            }
            if (!excluded.empty()) {
                Error w("SKETCH_PROJECTION_EXCLUDED");
                w.with("ids", excluded);
                r.messages.insert(r.messages.begin(), w);
            }
            return r;
        }
    }
    if (!crossIds.empty()) {
        Error e("SKETCH_PROFILE_CROSSING");
        std::set<std::string> bases;
        for (const auto& id : crossIds) bases.insert(baseOf(id));
        std::vector<std::string> ids(bases.begin(), bases.end());
        std::sort(ids.begin(), ids.end(), idLess);
        e.with("ids", ids);
        out.messages.push_back(e);
    }
    if (!overlapIds.empty()) {
        Error e("SKETCH_PROFILE_OVERLAP");
        std::vector<std::string> ids(overlapIds.begin(), overlapIds.end());
        std::sort(ids.begin(), ids.end(), idLess);
        e.with("ids", ids);
        out.messages.push_back(e);
    }
    auto finishOpen = [&](const std::set<std::string>& liveBases) {
        // Projections are reference geometry: open ones are silently excluded (§5.1, S1-SK-012). Curve pieces — by curve
        // ID, and only if none of its pieces entered the graph (§3.2 item 2а).
        std::set<std::string> bases;
        for (const auto& id : openIds)
            if (!isProjection(baseOf(id)) && !liveBases.count(baseOf(id))) bases.insert(baseOf(id));
        openIds.assign(bases.begin(), bases.end());
        if (openIds.empty()) return;
        std::sort(openIds.begin(), openIds.end(), idLess);
        Error e("SKETCH_OPEN_CURVES");
        e.with("ids", openIds);
        out.messages.push_back(e);
    };
    if (!crossIds.empty() || !overlapIds.empty()) {
        finishOpen({});
        return out;
    }

    // 3–4. Graph: edges are segments and arcs; removal of dangling edges and bridges.
    std::vector<int> edges;  // indices into live
    std::vector<int> circles;
    for (size_t i = 0; i < live.size(); ++i)
        (live[i].kind == Segment::Kind::Circle ? circles : edges).push_back(static_cast<int>(i));
    std::vector<bool> alive(live.size(), true);
    while (true) {
        bool changed = false;
        std::map<int, int> degree;
        for (int e : edges)
            if (alive[e]) {
                ++degree[nodeOf(live[e].na)];
                ++degree[nodeOf(live[e].nb)];
            }
        for (int e : edges)
            if (alive[e] && (degree[nodeOf(live[e].na)] == 1 || degree[nodeOf(live[e].nb)] == 1)) {
                alive[e] = false;
                openIds.push_back(live[e].id);
                changed = true;
            }
        if (changed) continue;
        // Bridges (Tarjan, multigraph).
        std::map<int, std::vector<std::pair<int, int>>> adj;  // node → (neighbor, edge)
        for (int e : edges)
            if (alive[e]) {
                const int a = nodeOf(live[e].na), b = nodeOf(live[e].nb);
                adj[a].push_back({b, e});
                adj[b].push_back({a, e});
            }
        std::map<int, int> tin, low;
        int timer = 0;
        std::vector<int> bridges;
        std::function<void(int, int)> dfs = [&](int v, int viaEdge) {
            tin[v] = low[v] = timer++;
            for (auto [to, e] : adj[v]) {
                if (e == viaEdge) continue;
                if (tin.count(to)) {
                    low[v] = std::min(low[v], tin[to]);
                } else {
                    dfs(to, e);
                    low[v] = std::min(low[v], low[to]);
                    if (low[to] > tin[v]) bridges.push_back(e);
                }
            }
        };
        for (auto& [v, lst] : adj)
            if (!tin.count(v)) dfs(v, -1);
        for (int e : bridges) {
            alive[e] = false;
            openIds.push_back(live[e].id);
            changed = true;
        }
        if (!changed) break;
    }
    {
        std::set<std::string> liveBases;
        for (size_t i = 0; i < live.size(); ++i)
            if (live[i].kind == Segment::Kind::Circle || alive[i]) liveBases.insert(baseOf(live[i].id));
        finishOpen(liveBases);
    }

    // 5. Half-edges: h = 2·k + dir (k — index in the list of edges sorted by ID; dir 0 — positive).
    std::vector<int> E;
    for (int e : edges)
        if (alive[e]) E.push_back(e);  // live is already in compareIds order
    const int H = static_cast<int>(E.size()) * 2;
    auto curveOf = [&](int h) -> const Curve& { return live[E[h / 2]]; };
    auto fromNode = [&](int h) { const Curve& k = curveOf(h); return h % 2 == 0 ? nodeOf(k.na) : nodeOf(k.nb); };
    auto toNode = [&](int h) { const Curve& k = curveOf(h); return h % 2 == 0 ? nodeOf(k.nb) : nodeOf(k.na); };
    auto tangentAt = [&](const Curve& k, bool atStartOfCurve, bool positive) -> Point2 {
        Point2 t;
        if (k.kind == Segment::Kind::Line) t = (k.b - k.a).normalized();
        else if (k.kind == Segment::Kind::EllipseArc) t = ellTangent(k, atStartOfCurve ? k.t0 : k.t0 + k.alpha);
        else if (k.kind == Segment::Kind::Spline) t = splTangent(k, atStartOfCurve);
        else t = perp((atStartOfCurve ? k.a : k.b) - k.c) / k.r;
        return positive ? t : Point2(-t);
    };
    auto tOut = [&](int h) { return tangentAt(curveOf(h), h % 2 == 0, h % 2 == 0); };
    auto tIn = [&](int h) { return tangentAt(curveOf(h), h % 2 != 0, h % 2 == 0); };
    // Signed curvature at the start of the half-edge (an ellipse arc — at its end where h starts).
    auto kappa = [&](int h) {
        const Curve& k = curveOf(h);
        if (k.kind == Segment::Kind::Line) return 0.0;
        if (k.kind == Segment::Kind::EllipseArc) return (h % 2 == 0 ? 1.0 : -1.0) * ellCurvature(k, h % 2 == 0 ? k.t0 : k.t0 + k.alpha);
        if (k.kind == Segment::Kind::Spline) return (h % 2 == 0 ? 1.0 : -1.0) * splCurvature(k, h % 2 == 0);
        return (h % 2 == 0 ? 1.0 : -1.0) / k.r;
    };
    std::map<int, std::vector<int>> outgoing;
    for (int h = 0; h < H; ++h) outgoing[fromNode(h)].push_back(h);
    bool overlapDuringWalk = false;
    std::set<std::string> walkOverlapIds;  // curves of pairs with coinciding tangent and curvature (§3.2 item 5)
    auto next = [&](int h) {
        const int v = toNode(h);
        const int tw = h ^ 1;
        int best = -1;
        double bestTheta = -1e9, bestK = 0;
        const Point2 ti = tIn(h);
        for (int g : outgoing[v]) {
            if (g == tw) continue;
            const Point2 to = tOut(g);
            double th = std::atan2(cross2(ti, to), ti.dot(to));
            if (std::abs(std::abs(th) - PI) <= PROFILE_ANGLE_TOL) th = kappa(g) < kappa(tw) ? PI : -PI;
            const double kg = kappa(g);
            // Both tangent and curvature coincide — an overlap (after step 2 possible only due to rounding).
            if (best >= 0 && std::abs(th - bestTheta) <= PROFILE_ANGLE_TOL && std::abs(kg - bestK) <= PROFILE_ANGLE_TOL) {
                overlapDuringWalk = true;
                walkOverlapIds.insert(curveOf(g).id);
                walkOverlapIds.insert(curveOf(best).id);
            }
            if (best < 0 || th > bestTheta + PROFILE_ANGLE_TOL ||
                (std::abs(th - bestTheta) <= PROFILE_ANGLE_TOL && kg > bestK)) {
                best = g;
                bestTheta = th;
                bestK = kg;
            }
        }
        return best;
    };
    auto heArea = [&](int h) {
        const Curve& k = curveOf(h);
        double a;
        if (k.kind == Segment::Kind::Line) a = cross2(k.a, k.b) / 2;
        else if (k.kind == Segment::Kind::EllipseArc) a = (cross2(k.c, k.b - k.a) + k.rx * k.ry * k.alpha) / 2;  // §3.2.1
        else if (k.kind == Segment::Kind::Spline) a = spline::greenArea(*k.sp, k.sp->a, k.sp->b);  // §3.2.1, §2.7.6
        else a = cross2(k.a, k.b) / 2 + (k.r * k.r / 2) * (k.alpha - std::sin(k.alpha));
        return h % 2 == 0 ? a : -a;
    };

    std::vector<Contour> contours;
    std::vector<bool> visited(H, false);
    for (int h0 = 0; h0 < H; ++h0) {
        if (visited[h0]) continue;
        Contour c;
        int h = h0;
        int guard = 0;
        while (!visited[h] && guard++ <= H) {
            visited[h] = true;
            c.halfEdges.push_back(h);
            h = next(h);
            if (h < 0) break;
        }
        for (int x : c.halfEdges) c.area += heArea(x);
        if (c.area > 0) contours.push_back(c);
    }
    if (overlapDuringWalk) {
        Error e("SKETCH_PROFILE_OVERLAP");
        std::vector<std::string> ids(walkOverlapIds.begin(), walkOverlapIds.end());
        std::sort(ids.begin(), ids.end(), IdLess{});
        e.with("ids", ids);
        out.messages.insert(out.messages.begin(), e);
        return out;
    }
    // Connected components.
    UF comp(static_cast<int>(nodePos.size()));
    for (int e : E) comp.unite(nodeOf(live[e].na), nodeOf(live[e].nb));
    for (auto& c : contours) c.component = comp.find(fromNode(c.halfEdges[0]));
    int nextComp = static_cast<int>(nodePos.size());
    for (int ci : circles) {
        Contour c;
        c.circle = ci;
        c.area = PI * live[ci].r * live[ci].r;
        c.component = nextComp++;
        contours.push_back(c);
    }
    for (size_t ei = 0; ei < ellipses.size(); ++ei) {  // §3.2 item 3: like a circle
        Contour c;
        c.ellipse = static_cast<int>(ei);
        c.area = PI * ellipses[ei].rx * ellipses[ei].ry;
        c.component = nextComp++;
        contours.push_back(c);
    }

    // Loop segments in traversal order.
    auto segmentsOf = [&](const Contour& c) {
        std::vector<Segment> segs;
        if (c.ellipse >= 0) {
            const Ell& k = ellipses[static_cast<size_t>(c.ellipse)];
            Segment sg;
            sg.id = k.id;
            sg.kind = Segment::Kind::Ellipse;
            sg.center = k.c;
            sg.radius = k.rx;
            sg.minorRadius = k.ry;
            sg.angle = k.ang;
            segs.push_back(sg);
            return segs;
        }
        if (c.circle >= 0) {
            const Curve& k = live[c.circle];
            Segment sg;
            sg.id = k.id;
            sg.kind = Segment::Kind::Circle;
            sg.center = k.c;
            sg.radius = k.r;
            segs.push_back(sg);
            return segs;
        }
        for (int h : c.halfEdges) {
            const Curve& k = curveOf(h);
            Segment sg;
            sg.id = k.id;
            sg.kind = k.kind;
            sg.reversed = h % 2 != 0;
            if (k.kind == Segment::Kind::Line) {
                sg.p1 = k.a;
                sg.p2 = k.b;
            } else if (k.kind == Segment::Kind::Spline) {
                sg.curve = spline::compress(*k.sp);
                sg.start = k.a;
                sg.end = k.b;
            } else if (k.kind == Segment::Kind::EllipseArc) {
                sg.center = k.c;
                sg.radius = k.rx;
                sg.minorRadius = k.ry;
                sg.angle = k.ang;
                sg.start = k.a;
                sg.end = k.b;
            } else {
                sg.center = k.c;
                sg.radius = k.r;
                sg.start = k.a;
                sg.end = k.b;
            }
            segs.push_back(sg);
        }
        return segs;
    };
    for (auto& c : contours) {
        std::vector<std::string> ids;
        for (const auto& sg : segmentsOf(c)) ids.push_back(sg.id);
        std::sort(ids.begin(), ids.end(), idLess);
        c.regionId = "r:";
        for (size_t i = 0; i < ids.size(); ++i) c.regionId += (i ? "," : "") + ids[i];
    }

    // 6. Nesting.
    auto testPoint = [&](const Contour& c) -> Point2 {
        if (c.circle >= 0) return live[c.circle].c + Point2(live[c.circle].r, 0);
        if (c.ellipse >= 0) return ellipses[static_cast<size_t>(c.ellipse)].at(0);  // end of the rx semi-axis
        const Curve* best = nullptr;
        for (int h : c.halfEdges)
            if (!best || idLess(curveOf(h).id, best->id)) best = &curveOf(h);
        if (best->kind == Segment::Kind::Line) return (best->a + best->b) / 2;
        if (best->kind == Segment::Kind::EllipseArc) return ellAt(*best, best->t0 + best->alpha / 2);
        if (best->kind == Segment::Kind::Spline) return spline::eval(*best->sp, splT(*best, 0.5));
        const double a0 = angleOf(best->a - best->c) + best->alpha / 2;
        return best->c + best->r * Point2(std::cos(a0), std::sin(a0));
    };
    auto winding = [&](const Contour& d, const Point2& p) {
        if (d.circle >= 0) return (p - live[d.circle].c).norm() < live[d.circle].r ? 1 : 0;
        if (d.ellipse >= 0) return ellipses[static_cast<size_t>(d.ellipse)].inside(p) ? 1 : 0;
        double sum = 0;
        for (int h : d.halfEdges) {
            const Curve& k = curveOf(h);
            const Point2 s0 = h % 2 == 0 ? k.a : k.b, e0 = h % 2 == 0 ? k.b : k.a;
            if (k.kind == Segment::Kind::Spline) {  // by its flattened polyline (§3.2.1)
                const auto ts = spline::flattenParams(*k.sp, REGION_MIN_CHORD);
                double w = 0;
                for (size_t i = 0; i + 1 < ts.size(); ++i) {
                    const Point2 u = spline::eval(*k.sp, ts[i]) - p, v = spline::eval(*k.sp, ts[i + 1]) - p;
                    w += std::atan2(cross2(u, v), u.dot(v));
                }
                sum += h % 2 == 0 ? w : -w;
                continue;
            }
            double contrib = std::atan2(cross2(s0 - p, e0 - p), (s0 - p).dot(e0 - p));
            if (k.kind == Segment::Kind::Arc || k.kind == Segment::Kind::EllipseArc) {
                const bool inside = k.kind == Segment::Kind::Arc ? (p - k.c).norm() < k.r : ellSampson(k, p) < 0;
                const bool inSeg = inside && cross2(k.b - k.a, p - k.a) < 0;
                double extra = inSeg ? 2 * PI : 0.0;
                contrib += h % 2 == 0 ? extra : -extra;
            }
            sum += contrib;
        }
        return static_cast<int>(std::lround(sum / (2 * PI)));
    };
    for (size_t i = 0; i < contours.size(); ++i) {
        const Point2 tp = testPoint(contours[i]);
        int parent = -1;
        for (size_t j = 0; j < contours.size(); ++j) {
            if (i == j || contours[j].component == contours[i].component) continue;
            if (winding(contours[j], tp) == 0) continue;
            if (parent < 0 || std::abs(contours[j].area) < std::abs(contours[parent].area) ||
                (std::abs(contours[j].area) == std::abs(contours[parent].area) && contours[j].regionId < contours[parent].regionId))
                parent = static_cast<int>(j);
        }
        contours[i].parent = parent;
    }
    for (auto& c : contours) {
        int d = 0;
        for (int p = c.parent; p >= 0; p = contours[static_cast<size_t>(p)].parent) ++d;
        c.depth = d;
    }

    // 7–8. Regions and normalization (§3.4).
    auto rotateToMin = [](std::vector<Segment>& segs) {
        size_t m = 0;
        for (size_t i = 1; i < segs.size(); ++i)
            if (idLess(segs[i].id, segs[m].id)) m = i;
        std::rotate(segs.begin(), segs.begin() + static_cast<long>(m), segs.end());
    };
    for (size_t i = 0; i < contours.size(); ++i) {
        Region r;
        r.regionId = contours[i].regionId;
        r.depth = contours[i].depth;
        r.outer.segments = segmentsOf(contours[i]);
        rotateToMin(r.outer.segments);
        r.area = contours[i].area;
        for (size_t j = 0; j < contours.size(); ++j) {
            if (contours[j].parent != static_cast<int>(i)) continue;
            std::vector<Segment> segs = segmentsOf(contours[j]);
            std::reverse(segs.begin(), segs.end());
            for (auto& sg : segs) sg.reversed = !sg.reversed;
            rotateToMin(segs);
            r.holes.push_back({segs});
            r.area -= std::abs(contours[j].area);
        }
        std::sort(r.holes.begin(), r.holes.end(),
                  [](const Loop& a, const Loop& b) { return idLess(a.segments[0].id, b.segments[0].id); });
        out.regions.push_back(r);
    }
    std::sort(out.regions.begin(), out.regions.end(),
              [](const Region& a, const Region& b) { return a.regionId < b.regionId; });
    for (auto& r : out.regions) {
        r.interiorPoint = computeInteriorPoint(r);  // §3.5.1
    }
    return out;
}

}  // namespace construe::sketch
