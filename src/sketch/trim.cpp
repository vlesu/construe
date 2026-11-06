#include "sketch/trim.h"

#include "base/geom2.h"
#include "base/ids.h"
#include "sketch/region_internal.h"

#include <memory>

#include <algorithm>
#include <cmath>
#include <set>

namespace construe::sketch {

namespace {

using geom2::cross2;
using geom2::ccwAngle;
using geom2::perp;
// Direction angle of v in [0, 2π).
double angle2pi(const Point2& v) {
    double a = geom2::angleOf(v);
    return a < 0 ? a + 2 * PI : a;
}

// Curve geometry from sketch data (§4.3: document coordinates).
struct Geo {
    EntityType type = EntityType::Line;
    Point2 a = Point2::Zero(), b = Point2::Zero();  // segment p1, p2 / arc start, end
    Point2 c = Point2::Zero();
    double r = 0, alpha = 0;                        // arc: sweep; ellipse arc: parameter sweep
    std::string pa, pb;                             // IDs of the ends (segment, arc, ellipse arc)
    double rx = 0, ry = 0, ang = 0, t0 = 0;         // ellipse, ellipse arc (S4-SKT-007): shape, start parameter
    std::shared_ptr<const spline::Flat> sp;         // spline (S4A-SPL-003); parameter — normalized to [0, 1]
    double len = 0;                                 // spline length
    bool ellipse() const { return type == EntityType::Ellipse || type == EntityType::EllipseArc; }
    bool isSpline() const { return isSplineType(type); }
    bool closed() const { return type == EntityType::Circle || type == EntityType::Ellipse || (sp && sp->periodic); }
    double splParam(const Point2& X) const { return (spline::closestParam(*sp, X) - sp->a) / (sp->b - sp->a); }
    double splDistance(const Point2& X) const { return (spline::eval(*sp, spline::closestParam(*sp, X)) - X).norm(); }
    Point2 u() const { return {std::cos(ang), std::sin(ang)}; }
    // Ellipse: parameter of a point in (−π, π], point at a parameter, Sampson distance (§2.1).
    double param(const Point2& X) const {
        const Point2 w = X - c;
        return std::atan2(w.dot(perp(u())) / ry, w.dot(u()) / rx);
    }
    Point2 at(double t) const { return c + rx * std::cos(t) * u() + ry * std::sin(t) * perp(u()); }
    double sampson(const Point2& X) const {
        const Point2 w = X - c;
        const double s = w.dot(u()), t = w.dot(perp(u()));
        const double F = s * s / (rx * rx) + t * t / (ry * ry) - 1;
        return F / std::max(2 * std::hypot(s / (rx * rx), t / (ry * ry)), 1e-300);
    }
};


Point2 pointOf(const SketchData& s, const std::string& id) {
    if (const Entity* e = s.findEntity(id)) return {e->x, e->y};
    return Point2::Zero();
}

std::optional<Geo> geoOf(const SketchData& s, const Entity& e) {
    Geo g;
    g.type = e.type;
    switch (e.type) {
        case EntityType::Line:
            g.a = pointOf(s, e.p1), g.b = pointOf(s, e.p2), g.pa = e.p1, g.pb = e.p2;
            return g;
        case EntityType::Circle:
            g.c = pointOf(s, e.center), g.r = e.r;
            return g;
        case EntityType::Arc:
            g.c = pointOf(s, e.center), g.a = pointOf(s, e.start), g.b = pointOf(s, e.end), g.pa = e.start, g.pb = e.end;
            g.r = (g.a - g.c).norm();
            g.alpha = ccwAngle(g.a - g.c, g.b - g.c);
            if (g.alpha == 0) g.alpha = 2 * PI;
            return g;
        case EntityType::Ellipse:
        case EntityType::EllipseArc:  // S4-SKT-007
            g.c = pointOf(s, e.center), g.rx = e.rx, g.ry = e.ry, g.ang = e.angle;
            if (!(g.rx > 0 && g.ry > 0)) return std::nullopt;
            if (e.type == EntityType::EllipseArc) {
                g.a = pointOf(s, e.start), g.b = pointOf(s, e.end), g.pa = e.start, g.pb = e.end;
                g.t0 = g.param(g.a);
                g.alpha = geom2::normalizeAngle0To2Pi(g.param(g.b) - g.t0);
                if (g.alpha == 0) g.alpha = 2 * PI;
            }
            return g;
        case EntityType::Bezier:
        case EntityType::BSpline: {  // S4A-SPL-003
            const spline::Curve c = splineCurve(e, {}, &s);
            if (!spline::validate(c).empty()) return std::nullopt;
            g.sp = std::make_shared<const spline::Flat>(spline::expand(c));
            g.len = spline::length(*g.sp, g.sp->a, g.sp->b);
            if (!c.periodic) g.a = c.poles.front(), g.b = c.poles.back(), g.pa = e.poles.front(), g.pb = e.poles.back();
            return g;
        }
        default: return std::nullopt;
    }
}

// Geometry of a projected curve (§2.5) — only as a cutter: its ends are not entities (pa/pb empty).
std::optional<Geo> projectedGeo(const SketchData& s, const Entity& e) {
    auto it = s.projections.find(e.id);
    if (e.type != EntityType::Projected || it == s.projections.end()) return std::nullopt;
    const Projection& p = it->second;
    Geo g;
    switch (p.kind) {
        case Projection::Kind::Line:
            g.type = EntityType::Line, g.a = {p.x1, p.y1}, g.b = {p.x2, p.y2};
            return g;
        case Projection::Kind::Circle:
            g.type = EntityType::Circle, g.c = {p.cx, p.cy}, g.r = p.r;
            return g;
        case Projection::Kind::Arc:
            g.type = EntityType::Arc, g.c = {p.cx, p.cy}, g.a = {p.sx, p.sy}, g.b = {p.ex, p.ey};
            g.r = (g.a - g.c).norm();
            g.alpha = ccwAngle(g.a - g.c, g.b - g.c);
            if (g.alpha == 0) g.alpha = 2 * PI;
            return g;
        default: return std::nullopt;
    }
}

// Parameter of a point on the curve (§4.3 item 1), without normalization.
double rawParam(const Geo& g, const Point2& X) {
    switch (g.type) {
        case EntityType::Line: {
            const Point2 d = g.b - g.a;
            const double l2 = d.squaredNorm();
            return l2 > 0 ? (X - g.a).dot(d) / l2 : 0.0;
        }
        case EntityType::Arc: return ccwAngle(g.a - g.c, X - g.c);
        case EntityType::Ellipse: return geom2::normalizeAngle0To2Pi(g.param(X));
        case EntityType::EllipseArc: return geom2::normalizeAngle0To2Pi(g.param(X) - g.t0);
        case EntityType::Bezier:
        case EntityType::BSpline: return g.splParam(X);
        default: return angle2pi(X - g.c);
    }
}

// Length "by parameter" per unit of parameter (ellipse — the mean radius (rx + ry)/2, §4.3 item 1).
double scale(const Geo& g) {
    if (g.ellipse()) return (g.rx + g.ry) / 2;
    if (g.isSpline()) return g.len;
    return g.type == EntityType::Line ? (g.b - g.a).norm() : g.r;
}

bool onExtent(const Geo& g, const Point2& X) {
    if (g.isSpline()) return g.splDistance(X) <= 10 * TRIM_TOL;
    if (g.closed()) return true;
    const double k = scale(g);
    if (k <= 0) return false;
    const double u = rawParam(g, X), eps = TRIM_TOL / k;
    if (g.type == EntityType::Line) return u >= -eps && u <= 1 + eps;
    return u <= g.alpha + eps || u >= 2 * PI - eps;
}

double carrierDistance(const Geo& g, const Point2& Q) {
    if (g.isSpline()) return g.splDistance(Q);
    if (g.type == EntityType::Line) {
        return geom2::distToLine(Q, g.a, g.b);
    }
    if (g.ellipse()) return std::abs(g.sampson(Q));
    return std::abs((Q - g.c).norm() - g.r);
}

// Largest radius of curvature of the carrier (tangency tolerance at an end, §4.3 item 2).
double curvatureRadius(const Geo& g) {
    if (g.type == EntityType::Line) return 0.0;
    if (g.isSpline()) return g.len;
    if (g.ellipse()) return std::max(g.rx, g.ry) * std::max(g.rx, g.ry) / std::min(g.rx, g.ry);
    return g.r;
}

// Line and ellipse (S4-SKT-007): roots of F(p1 + τ·d) = 0 in the ellipse axes; tangency — one point.
std::vector<Point2> lineEllipse(const Geo& l, const Geo& e) {
    const Point2 dv = l.b - l.a;
    if (dv.norm() == 0) return {};
    const Point2 u = e.u(), v = perp(u), w = l.a - e.c;
    const double ps = w.dot(u) / e.rx, pt = w.dot(v) / e.ry, ds = dv.dot(u) / e.rx, dt = dv.dot(v) / e.ry;
    const double A = ds * ds + dt * dt, B = 2 * (ps * ds + pt * dt), C = ps * ps + pt * pt - 1;
    const double disc = B * B - 4 * A * C;
    if (disc <= 0) {
        const Point2 X = l.a + (-B / (2 * A)) * dv;
        if (std::abs(e.sampson(X)) <= TRIM_TOL) return {X};
        return {};
    }
    const double q = std::sqrt(disc);
    const Point2 X1 = l.a + ((-B - q) / (2 * A)) * dv, X2 = l.a + ((-B + q) / (2 * A)) * dv;
    if ((X1 - X2).norm() <= TRIM_TOL) return {(X1 + X2) / 2};
    return {X1, X2};
}

// Circle/arc or ellipse and an ellipse (S4-SKT-007): numeric contacts along the full carrier of k with the ellipse e.
std::vector<Point2> conicEllipse(const Geo& k, const Geo& e) {
    auto at = [&](double tau) {
        const double t = 2 * PI * tau;
        return k.ellipse() ? k.at(t) : Point2(k.c + k.r * Point2(std::cos(t), std::sin(t)));
    };
    std::vector<Point2> out;
    for (double tau : contactTaus([&](double tau) { return e.sampson(at(tau)); }, TRIM_TOL)) {
        const Point2 X = at(tau);
        if (std::none_of(out.begin(), out.end(), [&](const Point2& o) { return (o - X).norm() <= TRIM_TOL; })) out.push_back(X);
    }
    return out;
}

// Carrier intersections (as in §3.2, step 2).
std::vector<Point2> lineLine(const Geo& k, const Geo& m) {
    const auto X = geom2::lineLine(k.a, k.b - k.a, m.a, m.b - m.a, 0.0);
    return X ? std::vector<Point2>{*X} : std::vector<Point2>{};
}

std::vector<Point2> lineCircle(const Geo& l, const Point2& c, double R) {
    const Point2 dv = l.b - l.a;
    if (dv.norm() == 0) return {};
    return geom2::lineCircle(l.a, dv.normalized(), c, R, TRIM_TOL);
}

// Implicit function of a non-spline carrier: zero on it (line — signed distance, circle — ‖X − c‖ − r, ellipse —
// Sampson distance), for contacts with a spline (S4A-SPL-003).
std::function<double(const Point2&)> implicitOf(const Geo& g) {
    if (g.type == EntityType::Line) {
        const Point2 d = (g.b - g.a).normalized();
        return [d, a = g.a](const Point2& X) { return cross2(d, X - a); };
    }
    if (g.ellipse()) return [g](const Point2& X) { return g.sampson(X); };
    return [c = g.c, r = g.r](const Point2& X) { return (X - c).norm() - r; };
}

std::vector<Point2> carrierIntersections(const Geo& k, const Geo& m) {
    if (k.isSpline() || m.isSpline()) {  // numeric contacts, §2.7.5 (S4A-SPL-003)
        std::vector<Point2> out;
        if (k.isSpline() && m.isSpline()) {
            for (const auto& st : spline::intersections(*k.sp, *m.sp, TRIM_TOL)) out.push_back(spline::eval(*k.sp, st.first));
            return out;
        }
        const Geo& S = k.isSpline() ? k : m;
        const Geo& K = k.isSpline() ? m : k;
        for (double t : spline::roots(*S.sp, implicitOf(K), TRIM_TOL)) out.push_back(spline::eval(*S.sp, t));
        return out;
    }
    const bool kl = k.type == EntityType::Line, ml = m.type == EntityType::Line;
    if (m.ellipse()) return kl ? lineEllipse(k, m) : conicEllipse(k, m);
    if (k.ellipse()) return ml ? lineEllipse(m, k) : conicEllipse(m, k);
    if (kl && ml) return lineLine(k, m);
    if (kl) return lineCircle(k, m.c, m.r);
    if (ml) return lineCircle(m, k.c, k.r);
    return geom2::circleCircle(k.c, k.r, m.c, m.r, TRIM_TOL);
}

// Distance along the length between parameters (cyclic for a circle).
double paramDistance(const Geo& g, double u, double v) {
    double d = std::abs(u - v);
    if (g.closed()) d = std::min(d, (g.isSpline() ? 1.0 : 2 * PI) - d);
    return d * scale(g);
}

std::vector<Cut> cutsOf(const SketchData& s, const Entity& ce, const Geo& g) {
    std::vector<const Entity*> others;
    for (const auto& e : s.entities)
        if (e.id != ce.id && (e.type == EntityType::Line || e.type == EntityType::Circle || e.type == EntityType::Arc ||
                              isEllipseType(e.type) || isSplineType(e.type) || e.type == EntityType::Projected))
            others.push_back(&e);
    std::sort(others.begin(), others.end(), [](const Entity* a, const Entity* b) { return compareIds(a->id, b->id) < 0; });
    const double k = scale(g);
    std::vector<Cut> cuts;
    for (const Entity* ke : others) {
        const auto kg = ke->type == EntityType::Projected ? projectedGeo(s, *ke) : geoOf(s, *ke);
        if (!kg) continue;
        std::vector<Point2> cand = carrierIntersections(g, *kg);
        for (const Point2& q : {kg->a, kg->b})
            if (!kg->closed() && carrierDistance(g, q) <= TRIM_TOL) {
                // Carrier intersection at this end is the same end (tangency, §4.3): a point within ε of the circle
                // is shifted along the tangent by √(2·r·ε).
                const double r = std::max(curvatureRadius(g), curvatureRadius(*kg));
                const double tol = r > 0 ? 2 * std::sqrt(2 * r * TRIM_TOL) : TRIM_TOL;
                std::erase_if(cand, [&](const Point2& x) { return (x - q).norm() <= tol; });
                cand.push_back(q);
            }
        for (const Point2& X : cand) {
            if (!onExtent(g, X) || !onExtent(*kg, X)) continue;
            Cut c;
            c.point = X;
            c.cutter = ke->id;
            // Cutter end within tolerance (the smaller by compareIds).
            std::optional<std::string> best;
            if (!kg->closed())
                for (const auto& [pid, pos] : {std::pair{kg->pa, kg->a}, std::pair{kg->pb, kg->b}})
                    if (!pid.empty() && (pos - X).norm() <= TRIM_TOL && (!best || compareIds(pid, *best) < 0)) best = pid;
            c.cutterPoint = best;
            double u = rawParam(g, X);
            if (g.type == EntityType::Line || (g.isSpline() && !g.closed())) {
                u = std::clamp(u, 0.0, 1.0);
            } else if ((g.type == EntityType::Arc || g.type == EntityType::EllipseArc) && u > g.alpha) {
                u = (u >= 2 * PI - TRIM_TOL / std::max(k, TRIM_TOL)) ? 0.0 : g.alpha;
            }
            c.param = u;
            // At the segment's/arc's own ends — not a cut.
            if (!g.closed()) {
                const double end = g.type == EntityType::Line || g.isSpline() ? 1.0 : g.alpha;
                if (u * k <= TRIM_TOL || (end - u) * k <= TRIM_TOL) continue;
            }
            // Merging coincident cuts: a cutter end is preferred; otherwise the smaller cutter stays (already listed).
            auto same = std::find_if(cuts.begin(), cuts.end(), [&](const Cut& o) { return paramDistance(g, o.param, c.param) <= TRIM_TOL; });
            if (same == cuts.end()) cuts.push_back(c);
            else if (!same->cutterPoint && c.cutterPoint) *same = c;
        }
    }
    std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.param < b.param; });
    return cuts;
}

bool refsContain(const Constraint& c, const std::string& id) {
    return std::find(c.refs.begin(), c.refs.end(), id) != c.refs.end();
}

}  // namespace

const char* trimActionName(TrimPlan::Action a) {
    switch (a) {
        case TrimPlan::Action::Delete: return "delete";
        case TrimPlan::Action::TrimStart: return "trimStart";
        case TrimPlan::Action::TrimEnd: return "trimEnd";
        case TrimPlan::Action::Split: return "split";
        case TrimPlan::Action::CircleToArc: return "circleToArc";
        case TrimPlan::Action::EllipseToArc: return "ellipseToArc";
        case TrimPlan::Action::PeriodicToOpen: return "periodicToOpen";
    }
    return "";
}

Result<TrimPlan> trimPlan(const SketchData& s, const std::string& curve, const Point2& pick) {
    const Entity* ce = s.findEntity(curve);
    const auto g = ce ? geoOf(s, *ce) : std::nullopt;
    if (!g) return Error("SKETCH_BAD_REFS").with("id", curve).with("detail", std::string("trim"));
    const std::vector<Cut> cuts = cutsOf(s, *ce, *g);
    TrimPlan plan;
    plan.curve = curve;
    // Click parameter (§4.3 item 3).
    double u = rawParam(*g, pick);
    if (g->type == EntityType::Line || g->isSpline()) u = std::clamp(u, 0.0, 1.0);
    else if ((g->type == EntityType::Arc || g->type == EntityType::EllipseArc) && u > g->alpha)
        u = (u - g->alpha < 2 * PI - u) ? g->alpha : 0.0;
    auto ellipsePiece = [&](TrimPiece::Kind kind) {  // S4-SKT-007
        plan.removed.kind = kind;
        plan.removed.center = g->c;
        plan.removed.rx = g->rx, plan.removed.ry = g->ry, plan.removed.angle = g->ang;
    };
    // Spline piece between normalized parameters (S4A-SPL-003); a periodic one — cyclically from u0 to u1.
    auto splinePiece = [&](double u0, double u1) {
        const spline::Flat& f = *g->sp;
        if (f.periodic && u1 <= u0) u1 += 1.0;
        plan.removed.kind = TrimPiece::Kind::Spline;
        plan.removed.curve = spline::compress(spline::segment(f, f.a + u0 * f.period(), f.a + u1 * f.period()));
    };
    if (g->isSpline() && g->closed()) {
        if (cuts.size() < 2) {
            plan.action = TrimPlan::Action::Delete;
            plan.removed.kind = TrimPiece::Kind::Spline;
            plan.removed.curve = splineCurve(*ce, {}, &s);
            return plan;
        }
        const Cut* lo = nullptr;
        const Cut* hi = nullptr;
        for (const auto& c : cuts) {
            if (c.param <= u) lo = &c;
            if (c.param > u && !hi) hi = &c;
        }
        if (!lo) lo = &cuts.back();
        if (!hi) hi = &cuts.front();
        plan.action = TrimPlan::Action::PeriodicToOpen;
        plan.lo = *lo;
        plan.hi = *hi;
        splinePiece(lo->param, hi->param);
        return plan;
    }
    if (g->closed()) {
        const bool el = g->type == EntityType::Ellipse;
        if (cuts.size() < 2) {
            plan.action = TrimPlan::Action::Delete;
            if (el) {
                ellipsePiece(TrimPiece::Kind::Ellipse);
                return plan;
            }
            plan.removed.kind = TrimPiece::Kind::Circle;
            plan.removed.center = g->c;
            plan.removed.r = g->r;
            return plan;
        }
        const Cut* lo = nullptr;
        const Cut* hi = nullptr;
        for (const auto& c : cuts) {
            if (c.param <= u) lo = &c;
            if (c.param > u && !hi) hi = &c;
        }
        if (!lo) lo = &cuts.back();
        if (!hi) hi = &cuts.front();
        plan.action = el ? TrimPlan::Action::EllipseToArc : TrimPlan::Action::CircleToArc;
        plan.lo = *lo;
        plan.hi = *hi;
        if (el) ellipsePiece(TrimPiece::Kind::EllipseArc);
        else plan.removed.kind = TrimPiece::Kind::Arc;
        plan.removed.center = g->c;
        plan.removed.start = lo->point;
        plan.removed.end = hi->point;
        return plan;
    }
    for (const auto& c : cuts) {
        if (c.param <= u) plan.lo = c;
        if (c.param > u && !plan.hi) plan.hi = c;
    }
    if (!plan.lo && !plan.hi) plan.action = TrimPlan::Action::Delete;
    else if (!plan.lo) plan.action = TrimPlan::Action::TrimStart;
    else if (!plan.hi) plan.action = TrimPlan::Action::TrimEnd;
    else plan.action = TrimPlan::Action::Split;
    const Point2 from = plan.lo ? plan.lo->point : g->a, to = plan.hi ? plan.hi->point : g->b;
    if (g->type == EntityType::Line) {
        plan.removed.kind = TrimPiece::Kind::Line;
        plan.removed.a = from;
        plan.removed.b = to;
    } else if (g->isSpline()) {
        splinePiece(plan.lo ? plan.lo->param : 0.0, plan.hi ? plan.hi->param : 1.0);
    } else {
        if (g->type == EntityType::EllipseArc) ellipsePiece(TrimPiece::Kind::EllipseArc);
        else plan.removed.kind = TrimPiece::Kind::Arc;
        plan.removed.center = g->c;
        plan.removed.start = from;
        plan.removed.end = to;
    }
    return plan;
}

Result<TrimOutcome> trim(const SketchData& s, const std::string& curve, const Point2& pick,
                         const std::function<std::string(bool entity)>& allocate) {
    auto planR = trimPlan(s, curve, pick);
    if (!planR) return planR.error();
    const TrimPlan& plan = *planR;
    TrimOutcome out;
    out.sketch = s;
    SketchData& d = out.sketch;
    std::set<std::string> removed;
    auto removeIf = [&](const auto& pred) {
        std::vector<Constraint> keep;
        for (auto& c : d.constraints) {
            if (pred(c)) removed.insert(c.id);
            else keep.push_back(std::move(c));
        }
        d.constraints = std::move(keep);
    };
    auto entity = [&](const std::string& id) -> Entity* {
        for (auto& e : d.entities)
            if (e.id == id) return &e;
        return nullptr;
    };
    const Entity orig = *s.findEntity(curve);
    const bool isLine = orig.type == EntityType::Line;
    auto movedPoint = [&](const std::string& y) { removeIf([&](const Constraint& c) { return refsContain(c, y); }); };
    auto changedExtent = [&] {
        removeIf([&](const Constraint& c) {
            if (!refsContain(c, curve)) return false;
            return c.type == "length" || c.type == "arcLength" || (c.type == "equal" && isLine) || c.type == "midpoint" ||
                   (c.type == "fix" && (orig.type == EntityType::Circle || orig.type == EntityType::Ellipse));  // ellipse — FX7-SK-009
        });
    };
    std::vector<Constraint> added;
    auto constraint = [&](const std::string& type, std::vector<std::string> refs) {
        Constraint c;
        c.type = type;
        c.refs = std::move(refs);
        added.push_back(std::move(c));
    };
    auto attach = [&](const std::string& y, const Cut& cut) {
        if (cut.cutterPoint) {
            constraint("coincident", {y, *cut.cutterPoint});
            // Cutter point already at the end of the trimmed curve — its pointOnObject to the curve is redundant (§4.3).
            const std::string p = *cut.cutterPoint;
            removeIf([&](const Constraint& c) { return c.type == "pointOnObject" && c.refs.size() == 2 && c.refs[0] == p && c.refs[1] == curve; });
        } else {
            constraint("pointOnObject", {y, cut.cutter});
        }
    };
    auto newPoint = [&](const Point2& p) {
        Entity e;
        e.id = allocate(true);
        e.type = EntityType::Point;
        e.x = p.x(), e.y = p.y();
        out.created.push_back(e.id);
        return e;
    };
    if (isSplineType(orig.type) && plan.action != TrimPlan::Action::Delete) {
        // Spline (§4.3 item 5, S4A-SPL-003): the kept pieces are sub-curves (§2.7.4) with new poles; pole IDs of the
        // curve are reused in order (moved points), missing ones are created, surplus ones are deleted.
        const spline::Flat F = spline::expand(splineCurve(orig, {}, &s));
        auto T = [&](double u) { return F.a + u * (F.b - F.a); };
        removeIf([&](const Constraint& c) { return refsContain(c, curve) && (c.type == "tangent" || c.type == "curvature"); });
        std::vector<std::string> reuse;
        size_t next = 0;
        std::vector<Entity> created;
        auto assign = [&](const spline::Flat& piece, const std::string* first, const std::string* last) {
            std::vector<std::string> ids;
            const size_t m = piece.P.size();
            for (size_t i = 0; i < m; ++i) {
                const Point2& q = piece.P[i];
                const std::string* keep = i == 0 ? first : i + 1 == m ? last : nullptr;
                std::string id;
                if (keep) {
                    id = *keep;
                } else if (next < reuse.size()) {
                    id = reuse[next++];
                    movedPoint(id);
                } else {
                    Entity e = newPoint(q);
                    id = e.id;
                    created.push_back(e);
                }
                if (Entity* pe = entity(id)) pe->x = q.x(), pe->y = q.y();
                ids.push_back(id);
            }
            return ids;
        };
        auto shape = [&](Entity& e, const spline::Flat& piece, const std::vector<std::string>& ids) {
            const spline::Curve c = spline::compress(piece);
            e.poles = ids;
            e.weights = c.weights;
            e.periodic = false;
            if (e.type == EntityType::BSpline) e.degree = c.degree, e.knots = c.knots, e.mults = c.mults;
        };
        const auto& P = orig.poles;
        std::vector<std::pair<std::string, const Cut*>> attachments;
        std::optional<Entity> C2;
        switch (plan.action) {
            case TrimPlan::Action::TrimStart: {
                reuse.assign(P.begin(), P.end() - 1);
                const spline::Flat piece = spline::segment(F, T(plan.hi->param), F.b);
                const auto ids = assign(piece, nullptr, &P.back());
                shape(*entity(curve), piece, ids);
                attachments.push_back({ids.front(), &*plan.hi});
                break;
            }
            case TrimPlan::Action::TrimEnd: {
                reuse.assign(P.begin() + 1, P.end());
                const spline::Flat piece = spline::segment(F, F.a, T(plan.lo->param));
                const auto ids = assign(piece, &P.front(), nullptr);
                shape(*entity(curve), piece, ids);
                attachments.push_back({ids.back(), &*plan.lo});
                break;
            }
            case TrimPlan::Action::Split: {
                reuse.assign(P.begin() + 1, P.end() - 1);
                const spline::Flat p1 = spline::segment(F, F.a, T(plan.lo->param));
                const spline::Flat p2 = spline::segment(F, T(plan.hi->param), F.b);
                const auto ids1 = assign(p1, &P.front(), nullptr);
                const auto ids2 = assign(p2, nullptr, &P.back());
                shape(*entity(curve), p1, ids1);
                C2 = Entity{};
                C2->id = allocate(true);
                out.created.push_back(C2->id);
                C2->type = orig.type;
                C2->construction = orig.construction;
                shape(*C2, p2, ids2);
                attachments.push_back({ids1.back(), &*plan.lo});
                attachments.push_back({ids2.front(), &*plan.hi});
                break;
            }
            default: {  // PeriodicToOpen: from hi to lo along the curve
                reuse.assign(P.begin(), P.end());
                double t0 = T(plan.hi->param), t1 = T(plan.lo->param);
                if (t1 <= t0) t1 += F.period();
                const spline::Flat piece = spline::segment(F, t0, t1);
                const auto ids = assign(piece, nullptr, nullptr);
                Entity& ce = *entity(curve);
                ce.type = EntityType::BSpline;
                shape(ce, piece, ids);
                attachments.push_back({ids.front(), &*plan.hi});
                attachments.push_back({ids.back(), &*plan.lo});
                break;
            }
        }
        // Surplus poles: deleted with their constraints.
        std::set<std::string> gone(reuse.begin() + static_cast<long>(next), reuse.end());
        if (!gone.empty()) {
            std::erase_if(d.entities, [&](const Entity& e) { return gone.count(e.id) > 0; });
            removeIf([&](const Constraint& c) { return std::any_of(c.refs.begin(), c.refs.end(), [&](const std::string& r) { return gone.count(r) > 0; }); });
        }
        for (auto& e : created) d.entities.push_back(e);
        if (C2) d.entities.push_back(*C2);
        for (const auto& [y, cut] : attachments) attach(y, *cut);
        for (auto& c : added) {
            c.id = allocate(false);
            out.created.push_back(c.id);
            d.constraints.push_back(std::move(c));
        }
        out.removedConstraints.assign(removed.begin(), removed.end());
        std::sort(out.removedConstraints.begin(), out.removedConstraints.end(),
                  [](const std::string& a, const std::string& b) { return compareIds(a, b) < 0; });
        return out;
    }
    switch (plan.action) {
        case TrimPlan::Action::Delete: {
            std::set<std::string> gone{curve};
            for (const auto& id : {orig.p1, orig.p2, orig.center, orig.start, orig.end})
                if (!id.empty()) gone.insert(id);
            for (const auto& id : orig.poles) gone.insert(id);
            std::vector<Entity> keep;
            for (auto& e : d.entities)
                if (!gone.count(e.id)) keep.push_back(std::move(e));
            d.entities = std::move(keep);
            removeIf([&](const Constraint& c) {
                return std::any_of(c.refs.begin(), c.refs.end(), [&](const std::string& r) { return gone.count(r) > 0; });
            });
            break;
        }
        case TrimPlan::Action::TrimStart:
        case TrimPlan::Action::TrimEnd: {
            const bool atStart = plan.action == TrimPlan::Action::TrimStart;
            const std::string y = isLine ? (atStart ? orig.p1 : orig.p2) : (atStart ? orig.start : orig.end);
            const Cut& cut = atStart ? *plan.hi : *plan.lo;
            movedPoint(y);
            Entity* pe = entity(y);
            pe->x = cut.point.x(), pe->y = cut.point.y();
            changedExtent();
            attach(y, cut);
            break;
        }
        case TrimPlan::Action::Split: {
            changedExtent();
            Entity E = newPoint(plan.lo->point);
            Entity S = newPoint(plan.hi->point);
            std::optional<Entity> Z;
            if (!isLine) Z = newPoint(pointOf(s, orig.center));
            Entity C2;
            C2.id = allocate(true);
            out.created.push_back(C2.id);
            C2.type = orig.type;
            C2.construction = orig.construction;
            if (orig.type == EntityType::EllipseArc) C2.rx = orig.rx, C2.ry = orig.ry, C2.angle = orig.angle;  // the same ellipse
            if (isLine) {
                C2.p1 = S.id, C2.p2 = orig.p2;
                entity(curve)->p2 = E.id;
            } else {
                C2.center = Z->id, C2.start = S.id, C2.end = orig.end;
                entity(curve)->end = E.id;
            }
            d.entities.push_back(E);
            d.entities.push_back(S);
            if (Z) d.entities.push_back(*Z);
            d.entities.push_back(C2);
            attach(E.id, *plan.lo);
            attach(S.id, *plan.hi);
            if (isLine) {
                constraint("parallel", {curve, C2.id});
                constraint("pointOnObject", {S.id, curve});
            } else {
                constraint("coincident", {Z->id, orig.center});
                constraint("equal", {curve, C2.id});
            }
            break;
        }
        case TrimPlan::Action::CircleToArc:
        case TrimPlan::Action::EllipseToArc: {
            changedExtent();
            Entity A = newPoint(plan.hi->point);
            Entity B = newPoint(plan.lo->point);
            Entity* ce = entity(curve);
            ce->type = plan.action == TrimPlan::Action::EllipseToArc ? EntityType::EllipseArc : EntityType::Arc;
            ce->start = A.id;
            ce->end = B.id;
            ce->r = 0;
            d.entities.push_back(A);
            d.entities.push_back(B);
            attach(A.id, *plan.hi);
            attach(B.id, *plan.lo);
            break;
        }
    }
    for (auto& c : added) {
        c.id = allocate(false);
        out.created.push_back(c.id);
        d.constraints.push_back(std::move(c));
    }
    out.removedConstraints.assign(removed.begin(), removed.end());
    std::sort(out.removedConstraints.begin(), out.removedConstraints.end(),
              [](const std::string& a, const std::string& b) { return compareIds(a, b) < 0; });
    return out;
}

}  // namespace construe::sketch
