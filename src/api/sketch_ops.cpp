// Sketch command packages (api/sketch_ops.h).
#include "api/sketch_ops.h"
#include "api/sketch_groups.h"
#include "sketch/offset.h"

#include "base/geom2.h"
#include "base/number_format.h"

#include <algorithm>
#include <array>
#include <limits>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <cmath>

namespace construe::api::sketchops {

namespace {

std::string tid(int n) { return "t" + std::to_string(n); }

Json pointItem(const std::string& id, const Point2& p) { return Json{{"id", id}, {"type", "point"}, {"x", p.x()}, {"y", p.y()}}; }

Json constraintItem(const std::string& type, const std::vector<std::string>& refs) {
    Json r = Json::array();
    for (const auto& x : refs) r.push_back(x);
    return Json{{"type", type}, {"refs", r}};
}


using geom2::angleOf;
using geom2::ccwSweep;

// Four sides by corners corners[0..3] with coincident endpoints and H/V; n — the next free ID number.
void sides(Batch& out, const Point2 (&corners)[4], int& n) {
    std::string lines[4], p1[4], p2[4];
    for (int i = 0; i < 4; ++i) {
        p1[i] = tid(n++);
        p2[i] = tid(n++);
        out.items.push_back(pointItem(p1[i], corners[i]));
        out.items.push_back(pointItem(p2[i], corners[(i + 1) % 4]));
    }
    for (int i = 0; i < 4; ++i) {
        lines[i] = tid(n++);
        out.items.push_back(Json{{"id", lines[i]}, {"type", "line"}, {"p1", p1[i]}, {"p2", p2[i]}});
    }
    for (int i = 0; i < 4; ++i) out.items.push_back(constraintItem("coincident", {p2[i], p1[(i + 1) % 4]}));
    out.items.push_back(constraintItem("horizontal", {lines[0]}));
    out.items.push_back(constraintItem("horizontal", {lines[2]}));
    out.items.push_back(constraintItem("vertical", {lines[1]}));
    out.items.push_back(constraintItem("vertical", {lines[3]}));
    for (int i = 0; i < 4; ++i) {
        out.roles["corner" + std::to_string(i)] = p1[i];
        out.roles["side" + std::to_string(i)] = lines[i];
    }
}

bool isCurve(const sketch::Entity* e) {
    return e && (e->type == sketch::EntityType::Circle || e->type == sketch::EntityType::Arc);
}

}  // namespace

std::optional<Point2> circumcenter(const Point2& a, const Point2& b, const Point2& c) {
    const double d = 2 * (a.x() * (b.y() - c.y()) + b.x() * (c.y() - a.y()) + c.x() * (a.y() - b.y()));
    if (std::abs(d) < 1e-12) return std::nullopt;
    const double a2 = a.squaredNorm(), b2 = b.squaredNorm(), c2 = c.squaredNorm();
    return Point2((a2 * (b.y() - c.y()) + b2 * (c.y() - a.y()) + c2 * (a.y() - b.y())) / d,
                  (a2 * (c.x() - b.x()) + b2 * (a.x() - c.x()) + c2 * (b.x() - a.x())) / d);
}

Batch rectangle(const Point2& a, const Point2& c) {
    const Point2 corners[4] = {a, Point2(c.x(), a.y()), c, Point2(a.x(), c.y())};
    Batch out;
    int n = 1;
    sides(out, corners, n);
    return out;
}

Batch centerRectangle(const Point2& c, const Point2& k) {
    const double dx = k.x() - c.x(), dy = k.y() - c.y();
    const Point2 corners[4] = {Point2(c.x() - dx, c.y() - dy), Point2(c.x() + dx, c.y() - dy), Point2(c.x() + dx, c.y() + dy),
                               Point2(c.x() - dx, c.y() + dy)};
    Batch out;
    int n = 1;
    sides(out, corners, n);
    const std::string center = tid(n++);
    out.items.push_back(pointItem(center, c));
    out.roles["center"] = center;
    // Construction diagonals with endpoints at the corners; midpoint — only on diagonal 0 (the second is redundant).
    std::string diag[2];
    for (int dI = 0; dI < 2; ++dI) {
        const std::string a = tid(n++), b = tid(n++);
        diag[dI] = tid(n++);
        out.items.push_back(pointItem(a, corners[dI]));
        out.items.push_back(pointItem(b, corners[dI + 2]));
        out.items.push_back(Json{{"id", diag[dI]}, {"type", "line"}, {"p1", a}, {"p2", b}, {"construction", true}});
        out.items.push_back(constraintItem("coincident", {a, out.roles["corner" + std::to_string(dI)]}));
        out.items.push_back(constraintItem("coincident", {b, out.roles["corner" + std::to_string(dI + 2)]}));
    }
    out.items.push_back(constraintItem("midpoint", {center, diag[0]}));
    return out;
}

std::optional<Batch> polygon(const Point2& c, const Point2& first, int n, bool inscribed) {
    if (n < 3 || n > 40) return std::nullopt;
    const double d = (first - c).norm();
    if (!(d > 0)) return std::nullopt;
    const double pi = 3.14159265358979323846;
    const double a0 = angleOf(c, first);
    // Inscribed: first is vertex 0; circumscribed: first is the midpoint of side 0, vertices shifted by ±π/n.
    const double R = inscribed ? d : d / std::cos(pi / n);
    const double start = inscribed ? a0 : a0 - pi / n;
    Batch out;
    int k = 1;
    const std::string center = tid(k++);
    out.items.push_back(pointItem(center, c));
    out.roles["center"] = center;
    std::vector<std::string> p1(n), p2(n), side(n);
    auto vertex = [&](int i) { return Point2(c + R * Point2(std::cos(start + 2 * pi * i / n), std::sin(start + 2 * pi * i / n))); };
    for (int i = 0; i < n; ++i) {
        p1[i] = tid(k++);
        p2[i] = tid(k++);
        out.items.push_back(pointItem(p1[i], vertex(i)));
        out.items.push_back(pointItem(p2[i], vertex((i + 1) % n)));
    }
    for (int i = 0; i < n; ++i) {
        side[i] = tid(k++);
        out.items.push_back(Json{{"id", side[i]}, {"type", "line"}, {"p1", p1[i]}, {"p2", p2[i]}});
        out.roles["side" + std::to_string(i)] = side[i];
    }
    // Vertices on circle R and equal sides give a regular polygon for any n (equal chords); a circumscribed one
    // also gets an inscribed circle tangent to side 0 (its diameter is the "across flats" size). Tangency of all sides
    // with equal sides does not define a regular polygon for even n (rhombus) — app-ux §6.3.
    const std::string outer = tid(k++);
    out.items.push_back(Json{{"id", outer}, {"type", "circle"}, {"center", center}, {"r", R}, {"construction", true}});
    out.roles["circle"] = outer;
    if (!inscribed) {
        // Each curve has its own point (sketch.md §2.1): inscribed circle center is own point, coincident with center.
        const std::string innerCenter = tid(k++), inner = tid(k++);
        out.items.push_back(pointItem(innerCenter, c));
        out.items.push_back(Json{{"id", inner}, {"type", "circle"}, {"center", innerCenter}, {"r", d}, {"construction", true}});
        out.items.push_back(constraintItem("coincident", {innerCenter, center}));
        out.roles["circle"] = inner;
        out.roles["outerCircle"] = outer;
    }
    out.roles["vertex0"] = p1[0];
    for (int i = 0; i < n; ++i) out.items.push_back(constraintItem("coincident", {p2[i], p1[(i + 1) % n]}));
    for (int i = 0; i < n; ++i) out.items.push_back(constraintItem("pointOnObject", {p1[i], outer}));
    for (int i = 1; i < n; ++i) out.items.push_back(constraintItem("equal", {side[0], side[i]}));
    if (!inscribed) out.items.push_back(constraintItem("tangent", {side[0], out.roles["circle"]}));
    return out;
}

namespace {
Batch splineBatch(const spline::Curve& c, bool bezierType) {
    Batch out;
    Json poles = Json::array();
    for (size_t i = 0; i < c.poles.size(); ++i) {
        const std::string id = "t" + std::to_string(i + 1);
        out.items.push_back(pointItem(id, c.poles[i]));
        out.roles["pole" + std::to_string(i)] = id;
        poles.push_back(id);
    }
    const std::string sid = "t" + std::to_string(c.poles.size() + 1);
    Json e{{"id", sid}, {"type", bezierType ? "bezier" : "bspline"}, {"poles", poles}};
    if (!bezierType) {
        e["degree"] = c.degree;
        e["knots"] = c.knots;
        e["mults"] = c.mults;
        if (c.periodic) e["periodic"] = true;
    }
    out.items.push_back(e);
    out.roles["spline"] = sid;
    return out;
}
}  // namespace

std::optional<Batch> splineByPoles(const std::vector<Point2>& P, bool closed) {
    const int n = static_cast<int>(P.size());
    if (n < (closed ? 3 : 2)) return std::nullopt;
    for (int i = 0; i + 1 < n; ++i)
        if ((P[i + 1] - P[i]).norm() <= 0) return std::nullopt;
    spline::Curve c;
    c.poles = P;
    if (closed) {
        c.degree = std::min(3, n - 1);
        c.periodic = true;
        for (int i = 0; i <= n; ++i) c.knots.push_back(i), c.mults.push_back(1);
        return splineBatch(c, false);
    }
    if (n <= 4) return splineBatch(spline::bezier(P), true);
    c.degree = 3;
    const int inner = n - 4;  // uniform interior knots
    c.knots.push_back(0), c.mults.push_back(4);
    for (int i = 1; i <= inner; ++i) c.knots.push_back(static_cast<double>(i) / (inner + 1)), c.mults.push_back(1);
    c.knots.push_back(1), c.mults.push_back(4);
    return splineBatch(c, false);
}

std::optional<Batch> splineThrough(const std::vector<Point2>& points) {
    const spline::Curve c = spline::interpolate(points);
    if (c.poles.empty()) return std::nullopt;
    return splineBatch(c, false);
}

std::optional<Batch> ellipse(const Point2& c, const Point2& a, const Point2& b) {
    const Point2 ax = a - c;
    const double rx = ax.norm();
    if (rx <= 0) return std::nullopt;
    const Point2 u = ax / rx;
    const Point2 w = b - c;
    const double ry = std::abs(u.x() * w.y() - u.y() * w.x());
    if (ry <= 0) return std::nullopt;
    Batch out;
    out.items.push_back(pointItem("t1", c));
    out.items.push_back(Json{{"id", "t2"}, {"type", "ellipse"}, {"center", "t1"}, {"rx", rx}, {"ry", ry}, {"angle", std::atan2(u.y(), u.x())}});
    out.roles["center"] = "t1";
    out.roles["ellipse"] = "t2";
    return out;
}

std::optional<Batch> circleThreePoints(const Point2& a, const Point2& b, const Point2& m) {
    const auto c = circumcenter(a, b, m);
    if (!c) return std::nullopt;
    Batch out;
    out.items.push_back(pointItem("t1", *c));
    out.items.push_back(Json{{"id", "t2"}, {"type", "circle"}, {"center", "t1"}, {"r", (a - *c).norm()}});
    out.roles["center"] = "t1";
    out.roles["circle"] = "t2";
    return out;
}

std::optional<Batch> arcThreePoints(const Point2& a, const Point2& b, const Point2& m) {
    const auto c = circumcenter(a, b, m);
    if (!c) return std::nullopt;
    // m must lie on the counterclockwise arc start→end; otherwise start and end are swapped.
    const bool swap = ccwSweep(angleOf(*c, a), angleOf(*c, m)) > ccwSweep(angleOf(*c, a), angleOf(*c, b));
    Batch out;
    out.items.push_back(pointItem("t1", *c));
    out.items.push_back(pointItem("t2", swap ? b : a));
    out.items.push_back(pointItem("t3", swap ? a : b));
    out.items.push_back(Json{{"id", "t4"}, {"type", "arc"}, {"center", "t1"}, {"start", "t2"}, {"end", "t3"}});
    out.roles = {{"center", "t1"}, {"a", swap ? "t3" : "t2"}, {"b", swap ? "t2" : "t3"}, {"arc", "t4"}};
    return out;
}

Batch arcCenterStartEnd(const Point2& c, const Point2& start, const Point2& end, bool ccw) {
    Batch out;
    out.items.push_back(pointItem("t1", c));
    out.items.push_back(pointItem("t2", ccw ? start : end));
    out.items.push_back(pointItem("t3", ccw ? end : start));
    out.items.push_back(Json{{"id", "t4"}, {"type", "arc"}, {"center", "t1"}, {"start", "t2"}, {"end", "t3"}});
    out.roles = {{"center", "t1"}, {"start", ccw ? "t2" : "t3"}, {"end", ccw ? "t3" : "t2"}, {"arc", "t4"}};
    return out;
}

std::optional<Batch> tangentArc(const Point2& p, const Point2& tangent, const Point2& end, const std::string& fromPoint,
                                const std::string& fromCurve) {
    const Point2 t = tangent.normalized();
    const Point2 nrm(-t.y(), t.x());
    const Point2 v = end - p;
    const double den = 2 * v.dot(nrm);
    if (std::abs(den) < 1e-9) return std::nullopt;
    const double sgn = v.squaredNorm() / den;  // center = p + sgn·n
    const Point2 c = p + sgn * nrm;
    const bool ccw = sgn > 0;  // turning left — a counterclockwise arc from p to end
    Batch out;
    out.items.push_back(pointItem("t1", c));
    out.items.push_back(pointItem("t2", ccw ? p : end));
    out.items.push_back(pointItem("t3", ccw ? end : p));
    out.items.push_back(Json{{"id", "t4"}, {"type", "arc"}, {"center", "t1"}, {"start", "t2"}, {"end", "t3"}});
    const std::string atP = ccw ? "t2" : "t3", atEnd = ccw ? "t3" : "t2";
    out.items.push_back(constraintItem("coincident", {atP, fromPoint}));
    if (!fromCurve.empty()) out.items.push_back(constraintItem("tangent", {fromCurve, "t4"}));
    out.roles = {{"end", atEnd}, {"arc", "t4"}};
    return out;
}

namespace {

Point2 reflect(const Point2& p, const Point2& a, const Point2& b) {
    const Point2 u = (b - a).normalized();
    const Point2 q = a + u * (p - a).dot(u);
    return q * 2 - p;
}

std::optional<Point2> lineLine(const Point2& p1, const Point2& d1, const Point2& p2, const Point2& d2) {
    return geom2::lineLine(p1, d1, p2, d2, DEGENERATE_EPS);
}

}  // namespace

std::optional<Batch> mirror(const sketch::SketchData& d, const std::vector<std::string>& selected, const std::string& axis,
                            const Point2& a, const Point2& b) {
    if ((b - a).norm() < 1e-12) return std::nullopt;
    auto pos = [&](const std::string& id) {
        const auto* e = d.findEntity(id);
        return e ? Point2(e->x, e->y) : Point2(0, 0);
    };
    // Selected points and ends/centers of selected curves; coincidence groups (coincident within the set).
    std::vector<const sketch::Entity*> curves, points;
    std::set<std::string> pts;
    for (const auto& id : selected) {
        if (id == axis) continue;
        const auto* e = d.findEntity(id);
        if (!e) continue;
        if (e->type == sketch::EntityType::Point) points.push_back(e), pts.insert(e->id);
        if (e->type == sketch::EntityType::Line) curves.push_back(e), pts.insert(e->p1), pts.insert(e->p2);
        if (e->type == sketch::EntityType::Circle || e->type == sketch::EntityType::Ellipse) curves.push_back(e), pts.insert(e->center);
        if (e->type == sketch::EntityType::Arc) curves.push_back(e), pts.insert(e->center), pts.insert(e->start), pts.insert(e->end);
    }
    if (curves.empty() && points.empty()) return std::nullopt;
    std::map<std::string, std::string> root;
    std::function<std::string(const std::string&)> find = [&](const std::string& x) {
        auto it = root.find(x);
        if (it == root.end() || it->second == x) return x;
        return it->second = find(it->second);
    };
    for (const auto& p : pts) root[p] = p;
    for (const auto& c : d.constraints)
        if (c.type == "coincident" && c.refs.size() == 2 && pts.count(c.refs[0]) && pts.count(c.refs[1])) root[find(c.refs[0])] = find(c.refs[1]);
    Batch out;
    int k = 1;
    std::map<std::string, std::string> copyOf;  // source point → temporary ID of the copy
    std::set<std::string> groupDone;
    const double tol = 1e-9 * std::max(1.0, (b - a).norm());
    auto copyPoint = [&](const std::string& p) {
        const std::string id = "t" + std::to_string(k++);
        out.items.push_back(pointItem(id, reflect(pos(p), a, b)));
        copyOf[p] = id;
        const Point2 u = (b - a).normalized(), w = pos(p) - a;
        const bool onAxis = std::abs(u.x() * w.y() - u.y() * w.x()) < tol;
        const std::string g = find(p);
        if (onAxis) {
            out.items.push_back(constraintItem("coincident", {id, p}));  // point on the axis — its copy is in place
        } else if (!groupDone.count(g)) {
            out.items.push_back(constraintItem("symmetric", {p, id, axis}));
            groupDone.insert(g);
            copyOf["#" + g] = id;
        } else {
            out.items.push_back(constraintItem("coincident", {id, copyOf.at("#" + g)}));
        }
        return id;
    };
    for (const auto* e : points) copyPoint(e->id);
    for (const auto* e : curves) {
        const std::string id = "t" + std::to_string(k++);
        if (e->type == sketch::EntityType::Line) {
            const std::string p1 = copyPoint(e->p1), p2 = copyPoint(e->p2);
            Json j = {{"id", id}, {"type", "line"}, {"p1", p1}, {"p2", p2}};
            if (e->construction) j["construction"] = true;
            out.items.push_back(j);
        } else if (e->type == sketch::EntityType::Circle) {
            const std::string c = copyPoint(e->center);
            Json j = {{"id", id}, {"type", "circle"}, {"center", c}, {"r", e->r}};
            if (e->construction) j["construction"] = true;
            out.items.push_back(j);
            out.items.push_back(constraintItem("equal", {e->id, id}));
        } else if (e->type == sketch::EntityType::Ellipse) {
            // Ellipse (FX7-SK-012): reflected center and axis direction, the shape mirrored by symmetric(E, E', axis).
            const std::string c = copyPoint(e->center);
            const double axisAngle = std::atan2((b - a).y(), (b - a).x());
            Json j = {{"id", id}, {"type", "ellipse"}, {"center", c}, {"rx", e->rx}, {"ry", e->ry}, {"angle", 2 * axisAngle - e->angle}};
            if (e->construction) j["construction"] = true;
            out.items.push_back(j);
            out.items.push_back(constraintItem("symmetric", {e->id, id, axis}));
        } else {
            // Reflection reverses traversal: the copy's start is the reflected end, its end — the reflected start.
            const std::string c = copyPoint(e->center), s = copyPoint(e->end), en = copyPoint(e->start);
            Json j = {{"id", id}, {"type", "arc"}, {"center", c}, {"start", s}, {"end", en}};
            if (e->construction) j["construction"] = true;
            out.items.push_back(j);
        }
        out.roles["copy:" + e->id] = id;
    }
    return out;
}

bool diametralApplicable(const sketch::SketchData& d, const sketch::Constraint& c) {
    if (c.type != "distance" || c.refs.size() != 2) return false;
    const auto* p = d.findEntity(c.refs[0]);
    const auto pp = p ? d.projections.find(p->id) : d.projections.end();
    const bool projectedPoint = p && p->type == sketch::EntityType::Projected && pp != d.projections.end() &&
                                pp->second.kind == sketch::Projection::Kind::Point;
    if (c.refs[0] != "origin" && (!p || (p->type != sketch::EntityType::Point && !projectedPoint))) return false;
    const auto* l = d.findEntity(c.refs[1]);
    if (!l) return false;
    if (l->type == sketch::EntityType::Line) return true;
    if (l->type != sketch::EntityType::Projected) return false;
    const auto it = d.projections.find(l->id);
    return it != d.projections.end() && it->second.kind == sketch::Projection::Kind::Line;
}

std::set<std::string> ownedPoints(const sketch::SketchData& d) { return sketch::ownedPoints(d); }

std::optional<int> diametralAxisRef(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c,
                                    const std::vector<DiametralAxis>& revolveAxes) {
    if ((c.type != "distance" && c.type != "distanceX" && c.type != "distanceY") || c.refs.size() != 2) return std::nullopt;
    auto pos = [&](const std::string& id) -> std::optional<Point2> {
        if (id == "origin") return Point2::Zero();
        const auto* e = d.findEntity(id);
        if (!e) return std::nullopt;
        if (e->type == sketch::EntityType::Point) {
            const auto it = st.points.find(id);
            return it != st.points.end() ? Point2(it->second) : Point2(e->x, e->y);
        }
        if (e->type != sketch::EntityType::Projected) return std::nullopt;
        const auto pr = d.projections.find(id);
        if (pr == d.projections.end() || pr->second.kind != sketch::Projection::Kind::Point) return std::nullopt;
        return Point2(pr->second.px, pr->second.py);
    };
    const auto a = pos(c.refs[0]), b = pos(c.refs[1]);
    if (!a || !b) return std::nullopt;
    Point2 u = c.type == "distanceX" ? Point2(1, 0) : c.type == "distanceY" ? Point2(0, 1) : Point2(*b - *a);
    if (u.norm() < DIAMETRAL_AXIS_TOL) return std::nullopt;
    u.normalize();
    std::vector<DiametralAxis> axes = revolveAxes;
    // Construction lines — only axes, not contour ones (sides of a construction rectangle, S7F-SK-002).
    const std::set<std::string> axisIds = sketch::axisLines(d);
    for (const auto& e : d.entities) {
        if (e.type == sketch::EntityType::Line && axisIds.count(e.id)) {
            const auto p1 = st.points.find(e.p1), p2 = st.points.find(e.p2);
            if (p1 != st.points.end() && p2 != st.points.end()) axes.push_back({p1->second, Point2(p2->second - p1->second)});
        } else if (e.type == sketch::EntityType::Projected) {
            const auto pr = d.projections.find(e.id);
            if (pr != d.projections.end() && pr->second.kind == sketch::Projection::Kind::Line)
                axes.push_back({Point2(pr->second.x1, pr->second.y1), Point2(pr->second.x2 - pr->second.x1, pr->second.y2 - pr->second.y1)});
        }
    }
    if (axes.empty()) axes = {{Point2::Zero(), Point2(1, 0)}, {Point2::Zero(), Point2(0, 1)}};  // the sketch axes
    for (int i = 0; i < 2; ++i) {
        const Point2 q = i == 0 ? *a : *b;
        for (const auto& ax : axes) {
            if (ax.dir.norm() < DIAMETRAL_AXIS_TOL) continue;
            const Point2 dv = ax.dir.normalized();
            if (std::abs(dv.dot(u)) > DIAMETRAL_AXIS_TOL) continue;  // the axis must be perpendicular to the dimension
            const Point2 w = q - ax.p;
            if (std::abs(dv.x() * w.y() - dv.y() * w.x()) <= DIAMETRAL_AXIS_TOL) return i;
        }
    }
    return std::nullopt;
}

bool diametralApplicable(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c,
                         const std::vector<DiametralAxis>& revolveAxes) {
    return diametralApplicable(d, c) || diametralAxisRef(d, st, c, revolveAxes).has_value();
}

ConstraintPlan constraintsForSelection(const sketch::SketchData& d, const std::string& type, const std::vector<std::string>& selected) {
    ConstraintPlan plan;
    std::vector<std::string> ents;
    for (const auto& id : selected)
        if (id == "origin" || d.findEntity(id)) ents.push_back(id);
    if (ents.empty()) {
        plan.outcome = ConstraintOutcome::PickEntities;
        return plan;
    }
    auto kindOf = [&](const std::string& id) -> int {  // 0 point, 1 segment, 2 curve
        if (id == "origin") return 0;
        const auto* e = d.findEntity(id);
        if (!e) return -1;
        if (e->type == sketch::EntityType::Point) return 0;
        if (e->type == sketch::EntityType::Line) return 1;
        // A projection — by its geometry kind (sketch.md §2.5): a body vertex is a point, a straight edge a segment.
        if (e->type == sketch::EntityType::Projected) {
            const auto it = d.projections.find(id);
            if (it != d.projections.end() && it->second.kind == sketch::Projection::Kind::Point) return 0;
            if (it != d.projections.end() && it->second.kind == sketch::Projection::Kind::Line) return 1;
        }
        return 2;
    };
    Json items = Json::array();
    if (type == "coincident") {
        std::string t = type;
        if (ents.size() == 2 && (kindOf(ents[0]) == 0) != (kindOf(ents[1]) == 0)) t = "pointOnObject";
        for (size_t i = 1; i < ents.size(); ++i) items.push_back(constraintItem(t, {ents[0], ents[i]}));
    } else if ((type == "horizontal" || type == "vertical") &&
               std::all_of(ents.begin(), ents.end(), [&](const std::string& id) { return kindOf(id) == 1; })) {
        for (const auto& id : ents) items.push_back(constraintItem(type, {id}));
    } else if (type == "fix") {
        for (const auto& id : ents) {
            const auto* e = id == "origin" ? nullptr : d.findEntity(id);
            if (!e) continue;
            if (e->type == sketch::EntityType::Arc || e->type == sketch::EntityType::EllipseArc) {
                plan.outcome = ConstraintOutcome::FixArcUnsupported;
                return plan;
            }
            if (e->type == sketch::EntityType::Line) {
                items.push_back(constraintItem("fix", {e->p1}));
                items.push_back(constraintItem("fix", {e->p2}));
            } else {
                items.push_back(constraintItem("fix", {id}));
            }
        }
    } else if (type == "coradial" && ents.size() >= 2) {
        // Coradial — common center and radius (app-ux §6.8, S1-SOLVE-009): concentric + equal to the first one.
        for (size_t i = 1; i < ents.size(); ++i) {
            items.push_back(constraintItem("concentric", {ents[0], ents[i]}));
            items.push_back(constraintItem("equal", {ents[0], ents[i]}));
        }
    } else if (type == "intersection") {
        // Point at the intersection of two curves (app-ux §6.8, S2-SK-004): a point on both — two pointOnObject.
        std::string p;
        std::vector<std::string> curves;
        for (const auto& id : ents) (kindOf(id) == 0 ? p : (curves.push_back(id), p)) = kindOf(id) == 0 ? id : p;
        for (const auto& cv : curves) items.push_back(constraintItem("pointOnObject", {p, cv}));
    } else if (type == "pathLength") {
        // Path length (S2-SK-005): a dimension with a value — the current sum of lengths (mm), driving.
        double sum = 0;
        auto pt = [&](const std::string& id) {
            const auto* e = d.findEntity(id);
            return e ? Point2(e->x, e->y) : Point2(0, 0);
        };
        for (const auto& id : ents) {
            const auto* e = d.findEntity(id);
            if (!e) continue;
            if (e->type == sketch::EntityType::Line) sum += (pt(e->p2) - pt(e->p1)).norm();
            if (e->type == sketch::EntityType::Arc) {
                const Point2 c = pt(e->center), s = pt(e->start) - c, en = pt(e->end) - c;
                double a = std::atan2(s.x() * en.y() - s.y() * en.x(), s.dot(en));
                if (a <= 0) a += 2 * std::numbers::pi;
                sum += s.norm() * a;
            }
        }
        Json it = constraintItem("pathLength", ents);
        it["value"] = canonicalNumber(std::round(sum * 1e6) / 1e6) + " mm";  // explicit unit: sketchops lacks doc units
        it["driving"] = true;
        items.push_back(it);
    } else if ((type == "parallel" || type == "equal" || type == "concentric") && ents.size() >= 2) {
        for (size_t i = 1; i < ents.size(); ++i) items.push_back(constraintItem(type, {ents[0], ents[i]}));
    } else {
        items.push_back(constraintItem(type, ents));
    }
    // An already set constraint (same type and set of refs) is not added.
    for (const auto& it : items) {
        bool exists = false;
        for (const auto& c : d.constraints) {
            if (c.type != it["type"].get<std::string>()) continue;
            std::vector<std::string> a = c.refs, b;
            for (const auto& r : it["refs"]) b.push_back(r);
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a == b) exists = true;
        }
        if (!exists) plan.items.push_back(it);
    }
    if (plan.items.empty()) plan.outcome = ConstraintOutcome::AlreadyExists;
    return plan;
}

namespace {
// Entity geometry for dimensions (app-ux §6.9, core-api §4.6.4): a sketch entity or a projection — by its geometry kind
// (sketch.md §2.5). centerId — center point (a projected curve has none), p1Id — segment start (a projection has none).
struct DimGeo {
    enum class Kind { None, Point, Line, Circle, Arc };
    Kind kind = Kind::None;
    Point2 p = Point2::Zero(), a = Point2::Zero(), b = Point2::Zero(), c = Point2::Zero();
    double r = 0;
    std::string centerId, p1Id;
    bool projected = false;
    bool construction = false;  // construction line (centerline)
    bool curve() const { return kind == Kind::Circle || kind == Kind::Arc; }
};

DimGeo dimGeo(const sketch::SketchData& d, const PointPos& pos, const std::map<std::string, double>& radii, const std::string& id) {
    DimGeo g;
    if (id == "origin") {
        g.kind = DimGeo::Kind::Point, g.p = pos(id);
        return g;
    }
    const auto* e = d.findEntity(id);
    if (!e) return g;
    switch (e->type) {
        case sketch::EntityType::Point: g.kind = DimGeo::Kind::Point, g.p = pos(id); break;
        case sketch::EntityType::Line:
            g.kind = DimGeo::Kind::Line, g.a = pos(e->p1), g.b = pos(e->p2), g.p1Id = e->p1, g.construction = e->construction;
            break;
        case sketch::EntityType::Circle: {
            g.kind = DimGeo::Kind::Circle, g.c = pos(e->center), g.centerId = e->center;
            const auto it = radii.find(id);
            g.r = it != radii.end() ? it->second : e->r;
            break;
        }
        case sketch::EntityType::Arc: g.kind = DimGeo::Kind::Arc, g.c = pos(e->center), g.r = (pos(e->start) - g.c).norm(), g.centerId = e->center; break;
        case sketch::EntityType::Projected: {
            const auto it = d.projections.find(id);
            if (it == d.projections.end()) break;
            const auto& pg = it->second;
            g.projected = true;
            switch (pg.kind) {
                case sketch::Projection::Kind::Point: g.kind = DimGeo::Kind::Point, g.p = Point2(pg.px, pg.py); break;
                case sketch::Projection::Kind::Line: g.kind = DimGeo::Kind::Line, g.a = Point2(pg.x1, pg.y1), g.b = Point2(pg.x2, pg.y2); break;
                case sketch::Projection::Kind::Circle: g.kind = DimGeo::Kind::Circle, g.c = Point2(pg.cx, pg.cy), g.r = pg.r; break;
                case sketch::Projection::Kind::Arc:
                    g.kind = DimGeo::Kind::Arc, g.c = Point2(pg.cx, pg.cy), g.r = std::hypot(pg.sx - pg.cx, pg.sy - pg.cy);
                    break;
            }
            break;
        }
        default: break;
    }
    return g;
}

// Distance from a point to the line of segment a–b.
using geom2::distToLine;
}  // namespace

std::optional<std::string> dimensionKind(const sketch::SketchData& d, const PointPos& pos, const std::vector<std::string>& ids,
                                         const Point2& at, const std::function<Point2(const Point2&)>& view,
                                         double parallelTolDeg) {
    const std::map<std::string, double> noRadii;
    auto byPosition = [&](const Point2& p, const Point2& q, const char* dflt) -> std::string {
        const Point2 a = view(p), b = view(q);
        const bool inX = at.x() >= std::min(a.x(), b.x()) && at.x() <= std::max(a.x(), b.x());
        const bool inY = at.y() >= std::min(a.y(), b.y()) && at.y() <= std::max(a.y(), b.y());
        if (inX && !inY) return "distanceX";
        if (!inX && inY) return "distanceY";
        return dflt;
    };
    if (ids.size() == 1) {
        const DimGeo g = dimGeo(d, pos, noRadii, ids[0]);
        if (g.projected) return std::nullopt;  // a dimension of a single projection defines nothing
        if (g.kind == DimGeo::Kind::Line) {
            const bool axis = std::abs(g.a.x() - g.b.x()) < 1e-9 || std::abs(g.a.y() - g.b.y()) < 1e-9;
            return axis ? std::string("length") : byPosition(g.a, g.b, "length");
        }
        if (g.kind == DimGeo::Kind::Circle) return std::string("diameter");
        if (g.kind == DimGeo::Kind::Arc) return std::string("radius");
        if (const auto* e = d.findEntity(ids[0]); e && sketch::isEllipseType(e->type)) {
            // Ellipse (app-ux §6.9, S4-SKT-004): the cursor in affine coordinates of the semi-axes' screen images.
            const Point2 c = pos(e->center), u(std::cos(e->angle), std::sin(e->angle)), v(-u.y(), u.x());
            const Point2 C = view(c), A = view(c + e->rx * u) - C, B = view(c + e->ry * v) - C, W = at - C;
            const double det = A.x() * B.y() - A.y() * B.x();
            if (std::abs(det) < 1e-12) return std::string("semiMajor");
            const double s = (W.x() * B.y() - W.y() * B.x()) / det, t = (A.x() * W.y() - A.y() * W.x()) / det;
            return std::string(std::abs(s) >= std::abs(t) ? "semiMajor" : "semiMinor");
        }
        return std::nullopt;
    }
    if (ids.size() == 2) {
        // A pole and its spline — the pole weight (app-ux §6.9, S4A-SPL-002).
        for (int i = 0; i < 2; ++i) {
            const auto* sp = d.findEntity(ids[1 - i]);
            if (sp && sketch::isSplineType(sp->type) && std::find(sp->poles.begin(), sp->poles.end(), ids[i]) != sp->poles.end())
                return std::string("weight");
        }
        const DimGeo ga = dimGeo(d, pos, noRadii, ids[0]), gb = dimGeo(d, pos, noRadii, ids[1]);
        if (ga.kind == DimGeo::Kind::None || gb.kind == DimGeo::Kind::None) return std::nullopt;
        if (ga.kind == DimGeo::Kind::Point && gb.kind == DimGeo::Kind::Point) return byPosition(ga.p, gb.p, "distance");
        if (ga.kind == DimGeo::Kind::Line && gb.kind == DimGeo::Kind::Line) {
            const Point2 da = (ga.b - ga.a).normalized(), db = (gb.b - gb.a).normalized();
            const double ang = std::acos(std::clamp(std::abs(da.dot(db)), 0.0, 1.0)) * 180 / PI;
            return ang < parallelTolDeg ? std::string("distance") : std::string("angle");
        }
        return std::string("distance");
    }
    return std::nullopt;
}

std::optional<DimensionPlan> dimensionFor(const sketch::SketchData& d, const PointPos& pos, const std::map<std::string, double>& radii,
                                          const std::vector<std::string>& ids, const std::string& k, const std::optional<Point2>& at) {
    auto ent = [&](const std::string& id) { return id == "origin" ? nullptr : d.findEntity(id); };
    DimensionPlan out;
    // distanceX/distanceY: ref order — difference ≥ 0 (core-api §4.6.4, FX1-SKG-003).
    auto signedPair = [&](const std::string& pa, const Point2& a, const std::string& pb, const Point2& b) {
        const Point2 dd = b - a;
        const double v = k == "distanceX" ? dd.x() : dd.y();
        out.refs = v < 0 ? std::vector<std::string>{pb, pa} : std::vector<std::string>{pa, pb};
        out.value = std::abs(v);
    };
    if (k == "weight" && ids.size() == 2) {  // pole weight (S4A-SPL-002): refs [pole, spline]
        const bool spFirst = ent(ids[0]) && sketch::isSplineType(ent(ids[0])->type);
        const auto* sp = ent(ids[spFirst ? 0 : 1]);
        const std::string pole = ids[spFirst ? 1 : 0];
        if (!sp || !sketch::isSplineType(sp->type)) return std::nullopt;
        const auto it = std::find(sp->poles.begin(), sp->poles.end(), pole);
        if (it == sp->poles.end()) return std::nullopt;
        const size_t i = static_cast<size_t>(it - sp->poles.begin());
        out.refs = {pole, sp->id};
        out.value = sp->weights.size() == sp->poles.size() ? sp->weights[i] : 1.0;
        out.number = true;
        return out;
    }
    if (ids.size() == 1) {
        const auto* e = ent(ids[0]);
        if (!e) return std::nullopt;
        if (k == "length") {
            out.refs = {e->id};
            out.value = (pos(e->p2) - pos(e->p1)).norm();
        } else if (k == "distanceX" || k == "distanceY") {
            signedPair(e->p1, pos(e->p1), e->p2, pos(e->p2));
        } else if (k == "diameter") {
            out.refs = {e->id};
            const auto it = radii.find(e->id);
            if (it == radii.end()) return std::nullopt;
            out.value = 2 * it->second;
        } else if (k == "semiMajor" || k == "semiMinor" || k == "ellipseAngle") {  // ellipse (S4-SKT-004)
            if (!sketch::isEllipseType(e->type)) return std::nullopt;
            out.refs = {e->id};
            if (k == "ellipseAngle") {
                double a = std::fmod(e->angle, PI);
                if (a < 0) a += PI;
                out.value = a;
                out.angle = true;
            } else {
                out.value = k == "semiMajor" ? e->rx : e->ry;
            }
        } else {
            out.refs = {e->id};
            out.value = (pos(e->start) - pos(e->center)).norm();
        }
    } else if (ids.size() == 2) {
        const std::string a = ids[0], b = ids[1];
        const DimGeo ga = dimGeo(d, pos, radii, a), gb = dimGeo(d, pos, radii, b);
        if (ga.kind == DimGeo::Kind::None || gb.kind == DimGeo::Kind::None) return std::nullopt;
        // Point to measure an entity "as a point": a point is itself, a curve its center (a projected curve: none).
        auto asPoint = [&](const std::string& id, const DimGeo& g) -> std::optional<std::pair<std::string, Point2>> {
            if (g.kind == DimGeo::Kind::Point) return std::pair{id, g.p};
            if (g.curve() && !g.centerId.empty()) return std::pair{g.centerId, g.c};
            return std::nullopt;
        };
        if (k == "angle") {
            if (ga.kind != DimGeo::Kind::Line || gb.kind != DimGeo::Kind::Line) return std::nullopt;
            out.refs = {a, b};
            out.angle = true;
            Point2 da = (ga.b - ga.a).normalized(), db = (gb.b - gb.a).normalized();
            // Sector with label (§6.9): from lines' intersection X, label L = X + a·da + b·db — rays sign(a)·da,
            // sign(b)·db; the angle between them.
            const double det = da.x() * db.y() - da.y() * db.x();
            if (at && std::abs(det) > 1e-12) {
                const Point2 p = ga.a, q = gb.a;
                const double t = ((q.x() - p.x()) * db.y() - (q.y() - p.y()) * db.x()) / det;
                const Point2 x = p + t * da, v = *at - x;
                const double sa = (v.x() * db.y() - v.y() * db.x()) / det, sb = (da.x() * v.y() - da.y() * v.x()) / det;
                if (sa < 0) da = -da;
                if (sb < 0) db = -db;
            }
            out.value = std::acos(std::clamp(da.dot(db), -1.0, 1.0));
        } else if (ga.kind == DimGeo::Kind::Line && gb.kind == DimGeo::Kind::Line) {
            // Parallel: segment start point — the other segment (a projected segment only second: its ends are not
            // sketch points, §6.9).
            // A segment and a centerline (construction line, FX8-SK-001): the point is taken from the segment and
            // the line is the centerline, so the dimension can be made diametral about the axis.
            // A segment lying on an axis (an axis line or collinear with one, e.g. the contour edge on the revolve axis;
            // FX13-DIM-001) is the line too — whichever segment was picked first.
            const std::set<std::string> axisIds = sketch::axisLines(d);
            auto onAxis = [&](const DimGeo& g, const std::string& id) {
                if (g.construction || axisIds.count(id)) return true;
                const Point2 u = g.b - g.a;
                if (u.norm() < DIAMETRAL_AXIS_TOL) return false;
                for (const auto& e : d.entities) {
                    if (e.type != sketch::EntityType::Line || !axisIds.count(e.id)) continue;
                    const Point2 p1 = pos(e.p1), v = pos(e.p2) - p1;
                    if (v.norm() < DIAMETRAL_AXIS_TOL) continue;
                    const Point2 dv = v.normalized();
                    auto off = [&](const Point2& x) { return std::abs(dv.x() * (x - p1).y() - dv.y() * (x - p1).x()); };
                    if (off(g.a) <= DIAMETRAL_AXIS_TOL && off(g.b) <= DIAMETRAL_AXIS_TOL) return true;
                }
                return false;
            };
            const bool aAxis = onAxis(ga, a), bAxis = !gb.projected && onAxis(gb, b);
            const bool aReal = !ga.projected && !(aAxis && !bAxis && !gb.projected);
            const DimGeo& pl = aReal ? ga : gb;
            const DimGeo& other = aReal ? gb : ga;
            if (pl.projected) return std::nullopt;  // both are projections
            out.refs = {pl.p1Id, aReal ? b : a};
            out.value = distToLine(pl.a, other.a, other.b);
        } else if (k == "distance" && (ga.curve() || gb.curve()) && [&] {
                       // To circle/arc — nearest distance (S1-SOLVE-010, app-ux §6.9); from a line — to the tangent
                       // along the line's normal (FX1-SKG-002); when intersecting — as before, to the center.
                       const bool cb = gb.curve();
                       const DimGeo& gc = cb ? gb : ga;
                       const DimGeo& go = cb ? ga : gb;
                       out.refs = {cb ? a : b, cb ? b : a};
                       out.arc = "near";
                       if (ga.curve() && gb.curve()) out.value = (ga.c - gb.c).norm() - ga.r - gb.r;
                       else if (go.kind == DimGeo::Kind::Line) {
                           const double dl = distToLine(gc.c, go.a, go.b);
                           out.value = dl - gc.r;
                           if (out.value <= 0 && gc.r - dl > 0) {
                               // Line intersects the circle: to the tangent beyond the line, R − d — "far" with
                               // the branch opposite to the center's side (solver.md §4.2.15а).
                               const Point2 dv = go.b - go.a, w = gc.c - go.a;
                               const double sdc = dv.x() * w.y() - dv.y() * w.x();
                               out.value = gc.r - dl;
                               out.arc = "far";
                               out.side = sdc >= 0 ? -1 : 1;
                           }
                       } else out.value = std::abs((go.p - gc.c).norm() - gc.r);
                       return out.value > 0;
                   }()) {
        } else {
            out.arc.clear();
            const bool la = ga.kind == DimGeo::Kind::Line, lb = gb.kind == DimGeo::Kind::Line;
            if (la || lb) {
                const DimGeo& line = la ? ga : gb;
                const auto other = la ? asPoint(b, gb) : asPoint(a, ga);
                if (!other) return std::nullopt;
                out.refs = {other->first, la ? a : b};
                out.value = distToLine(other->second, line.a, line.b);
            } else {
                const auto pa = asPoint(a, ga), pb = asPoint(b, gb);
                if (!pa || !pb) return std::nullopt;
                if (k == "distanceX" || k == "distanceY") {
                    signedPair(pa->first, pa->second, pb->first, pb->second);
                } else {
                    out.value = (pb->second - pa->second).norm();
                    out.refs = {pa->first, pb->first};
                }
            }
        }
    } else {
        return std::nullopt;
    }
    if (!out.angle && k != "distanceX" && k != "distanceY" && out.value <= 0) return std::nullopt;
    return out;
}

Json dimensionValueChanges(const sketch::Constraint& c, double current, const std::string& text, double entered) {
    // app-ux §6.9 "Entering a value" (FX1-SKG-003): positive — same side, negative — the other side,
    // the absolute value is written.
    std::string t = text;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    auto absText = [&] {
        if (!t.empty() && t[0] == '-') {
            std::string r = t.substr(1);
            while (!r.empty() && std::isspace(static_cast<unsigned char>(r.front()))) r.erase(r.begin());
            return r;
        }
        return "-(" + t + ")";
    };
    Json ch = {{"id", c.id}};
    if ((c.type == "distanceX" || c.type == "distanceY") && c.refs.size() == 2) {
        const double signedNew = (current < 0 ? -1.0 : 1.0) * entered;
        if (signedNew < 0) ch["refs"] = Json::array({c.refs[1], c.refs[0]});
        ch["value"] = entered < 0 ? absText() : t;
    } else if (c.type == "distance" && c.branch && c.branch->kind == sketch::Branch::Kind::Side && entered < 0) {
        ch["value"] = absText();
        ch["branch"] = Json{{"side", -c.branch->side}};
    } else {
        ch["value"] = t;
    }
    return Json::array({ch});
}

std::vector<std::string> applicableConstraints(const sketch::SketchData& d, const std::vector<std::string>& selected) {
    // Kind of the selected: 0 point, 1 segment, 2 circle, 3 arc, ellipse, spline; unknown ones are not counted.
    int points = 0, lines = 0, circles = 0, arcs = 0, ellipses = 0, ellipseArcs = 0, splines = 0;
    for (const auto& id : selected) {
        if (id == "origin") {
            ++points;
            continue;
        }
        const auto* e = d.findEntity(id);
        if (!e) continue;
        sketch::EntityType t = e->type;
        if (t == sketch::EntityType::Projected) {
            const auto it = d.projections.find(id);
            if (it == d.projections.end()) continue;
            switch (it->second.kind) {
                case sketch::Projection::Kind::Point: t = sketch::EntityType::Point; break;
                case sketch::Projection::Kind::Line: t = sketch::EntityType::Line; break;
                case sketch::Projection::Kind::Circle: t = sketch::EntityType::Circle; break;
                case sketch::Projection::Kind::Arc: t = sketch::EntityType::Arc; break;
            }
        }
        if (t == sketch::EntityType::Point) ++points;
        else if (t == sketch::EntityType::Line) ++lines;
        else if (t == sketch::EntityType::Circle) ++circles;
        else if (t == sketch::EntityType::Arc) ++arcs;
        else if (sketch::isEllipseType(t)) ++ellipses, ellipseArcs += t == sketch::EntityType::EllipseArc;  // S4-SKT-004, -007
        else if (sketch::isSplineType(t)) ++splines;    // S4A-SPL-002
    }
    const int curves = circles + arcs, n = points + lines + curves + ellipses + splines;
    std::vector<std::string> out;
    if (n == 0) return out;
    if (splines > 0) {
        // Spline (app-ux §6.8, S4A-SPL-002): point on it; at an end joint with a segment, arc or spline — tangency and
        // equal curvature.
        if (n == 2 && points == 1) out.push_back("coincident");
        if (n == 2 && (lines == 1 || arcs == 1 || splines == 2)) out.push_back("tangent"), out.push_back("curvature");
        return out;
    }
    if ((points == 2 && n == 2) || (points == 1 && n == 2) || (points >= 3 && n == points)) out.push_back("coincident");
    const bool onlyLines = lines == n, twoPoints = points == 2 && n == 2;
    // Point + segment: horizontal/vertical with the segment midpoint (S1-SK-014).
    if (onlyLines || twoPoints || (points == 1 && lines == 1 && n == 2)) {
        out.push_back("horizontal");
        out.push_back("vertical");
    }
    if (onlyLines && lines >= 2) out.push_back("parallel");
    if (onlyLines && lines == 2) out.push_back("perpendicular");
    if (onlyLines && lines == 2) out.push_back("collinear");
    if (n == 2 && ((lines == 1 && curves == 1) || curves == 2 || (lines == 1 && ellipses == 1) || (curves == 1 && ellipses == 1) ||
                   ellipses == 2))  // with an ellipse — S4-SKT-007
        out.push_back("tangent");
    if ((onlyLines && lines >= 2) || (curves == n && curves >= 2) || (ellipses == n && ellipses == 2)) out.push_back("equal");
    if (curves + ellipses == n && n >= 2) out.push_back("concentric");
    if (curves == n && curves >= 2) out.push_back("coradial");
    if (n == 2 && points == 1 && (lines == 1 || arcs == 1)) out.push_back("midpoint");  // midpoint of a segment or arc
    if (n == 3 && points == 1 && lines + curves + ellipses == 2) out.push_back("intersection");  // S2-SK-004
    if (n >= 2 && points == 0 && circles == 0 && ellipses == 0) out.push_back("pathLength");  // S2-SK-005
    if (n == 3 && points == 2 && lines == 1) out.push_back("symmetric");
    if (arcs == 0 && ellipseArcs == 0) out.push_back("fix");  // an ellipse is fixed whole (FX7-SK-009)
    return out;
}

// ---------------------------------------------------------------- label layout (core-api §4.6.2, FX4-SK-001)

namespace {

using Seg = std::pair<Point2, Point2>;

constexpr int LAYOUT_CIRCLE_SEGMENTS = 48;      // circle polyline for intersection checks
constexpr double LAYOUT_EXTENT_TEXT = 40;       // default label height — extent / 40
constexpr double LAYOUT_CHAR_RATIO = 0.6;       // default character width — 0.6 of height
constexpr double LAYOUT_PAD = 0.5;              // label horizontal padding — 0.5 of height
constexpr double LAYOUT_ROW = 2;                // first row offset and row step — 2 label heights
constexpr double LAYOUT_OVERSHOOT = 0.2;        // extension line overshoot past the dimension line — 0.2 of height
constexpr double LAYOUT_DEFAULT_OFFSET = 0.75;  // no label — near the refs' midpoint shifted by (0.75; 0.75) heights
constexpr double W_RECT_RECT = 1000, W_DIM = 100, W_RECT_GEOM = 10, W_LINE_GEOM = 1, W_FAR = 0.01, W_OUTSIDE = 0.005;
// Near placement (FX10-IO-006): distance of an axis dimension line from its element — per label height; an extension
// line crossing the geometry.
constexpr double W_DIST = 0.05, W_EXT_GEOM = 0.2;
constexpr int LAYOUT_MAX_ROWS = 40;

using geom2::cross2;

// Intersection of segments at interior points of both (touching at ends does not count) or overlap on one line
// longer than eps.
bool segsCross(const Seg& s, const Seg& t, double eps) {
    const Point2 r = s.second - s.first, q = t.second - t.first, w = t.first - s.first;
    const double lr = r.norm(), lq = q.norm();
    if (lr <= eps || lq <= eps) return false;
    const Point2 ur = r / lr, uq = q / lq;
    if (std::abs(cross2(ur, uq)) < 1e-9) {
        if (std::abs(cross2(ur, w)) > eps) return false;
        double b0 = ur.dot(t.first - s.first), b1 = ur.dot(t.second - s.first);
        if (b0 > b1) std::swap(b0, b1);
        return std::min(lr, b1) - std::max(0.0, b0) > eps;
    }
    const double den = cross2(r, q);
    const double tt = cross2(w, q) / den, uu = cross2(w, r) / den;
    const double er = eps / lr, eq = eps / lq;
    return tt > er && tt < 1 - er && uu > eq && uu < 1 - eq;
}

// A segment passes through the rectangle interior (the clipped part is longer than eps).
bool segHitsRect(const Seg& s, const Point2& lo, const Point2& hi, double eps) {
    double t0 = 0, t1 = 1;
    const Point2 d = s.second - s.first;
    auto clip = [&](double p, double q) {
        if (std::abs(p) < 1e-15) return q >= 0;
        const double r = q / p;
        if (p < 0) {
            if (r > t1) return false;
            t0 = std::max(t0, r);
        } else {
            if (r < t0) return false;
            t1 = std::min(t1, r);
        }
        return true;
    };
    if (!clip(-d.x(), s.first.x() - lo.x()) || !clip(d.x(), hi.x() - s.first.x()) || !clip(-d.y(), s.first.y() - lo.y()) ||
        !clip(d.y(), hi.y() - s.first.y()))
        return false;
    return (t1 - t0) * d.norm() > eps;
}

bool rectsOverlap(const DimFootprint& a, const DimFootprint& b, double eps) {
    return std::min(a.hi.x(), b.hi.x()) - std::max(a.lo.x(), b.lo.x()) > eps &&
           std::min(a.hi.y(), b.hi.y()) - std::max(a.lo.y(), b.lo.y()) > eps;
}

std::optional<Point2> posOf(const sketch::SolvedState& st, const std::string& id) {
    if (id == "origin") return Point2(0, 0);
    if (auto it = st.points.find(id); it != st.points.end()) return it->second;
    return std::nullopt;
}

// Segment (endpoints) of a ref: a sketch segment or a projected segment.
std::optional<Seg> lineOf(const sketch::SketchData& d, const sketch::SolvedState& st, const std::string& id) {
    const auto* e = d.findEntity(id);
    if (!e) return std::nullopt;
    if (e->type == sketch::EntityType::Line) {
        auto a = posOf(st, e->p1), b = posOf(st, e->p2);
        if (a && b) return Seg{*a, *b};
    }
    if (e->type == sketch::EntityType::Projected)
        if (auto it = d.projections.find(id); it != d.projections.end() && it->second.kind == sketch::Projection::Kind::Line)
            return Seg{Point2(it->second.x1, it->second.y1), Point2(it->second.x2, it->second.y2)};
    return std::nullopt;
}

// Center and radius of a curve ref: a circle, an arc or a projected curve.
std::optional<std::pair<Point2, double>> curveOf(const sketch::SketchData& d, const sketch::SolvedState& st, const std::string& id) {
    const auto* e = d.findEntity(id);
    if (!e) return std::nullopt;
    if (e->type == sketch::EntityType::Circle) {
        auto c = posOf(st, e->center);
        if (!c) return std::nullopt;
        auto it = st.radii.find(id);
        return std::pair{*c, it != st.radii.end() ? it->second : e->r};
    }
    if (e->type == sketch::EntityType::Arc) {
        auto c = posOf(st, e->center), s = posOf(st, e->start);
        if (c && s) return std::pair{*c, (*s - *c).norm()};
    }
    if (e->type == sketch::EntityType::Projected)
        if (auto it = d.projections.find(id);
            it != d.projections.end() && (it->second.kind == sketch::Projection::Kind::Arc || it->second.kind == sketch::Projection::Kind::Circle))
            return std::pair{Point2(it->second.cx, it->second.cy), it->second.r};
    return std::nullopt;
}

// Anchor point of a ref: a point is itself, a segment — its midpoint, a curve — its center.
std::optional<Point2> anchorOf(const sketch::SketchData& d, const sketch::SolvedState& st, const std::string& id) {
    if (auto p = posOf(st, id)) return p;
    if (auto l = lineOf(d, st, id)) return Point2((l->first + l->second) / 2);
    if (auto c = curveOf(d, st, id)) return c->first;
    if (auto it = d.projections.find(id); it != d.projections.end() && it->second.kind == sketch::Projection::Kind::Point)
        return Point2(it->second.px, it->second.py);
    return std::nullopt;
}

}  // namespace

std::optional<Point2> dimensionAnchor(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c) {
    auto refAnchor = [&](const std::string& id) -> std::optional<Point2> {
        if (auto a = anchorOf(d, st, id)) return a;
        const auto* e = d.findEntity(id);
        if (!e) return std::nullopt;
        if (sketch::isEllipseType(e->type)) return posOf(st, e->center);
        if (e->type == sketch::EntityType::Text) return posOf(st, e->center);
        if (sketch::isSplineType(e->type) && !e->poles.empty()) {
            Point2 sum(0, 0);
            for (const auto& q : e->poles) {
                const auto p = posOf(st, q);
                if (!p) return std::nullopt;
                sum += *p;
            }
            return Point2(sum / double(e->poles.size()));
        }
        return std::nullopt;
    };
    if (c.refs.empty()) return std::nullopt;
    if (c.type == "weight") return refAnchor(c.refs[0]);
    Point2 sum(0, 0);
    for (const auto& r : c.refs) {
        const auto a = refAnchor(r);
        if (!a) return std::nullopt;
        sum += *a;
    }
    return Point2(sum / double(c.refs.size()));
}

std::optional<Point2> dimensionLabelPoint(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c) {
    if (!c.labelOffset) return std::nullopt;
    const auto a = dimensionAnchor(d, st, c);
    if (!a) return std::nullopt;
    return Point2(*a + Point2((*c.labelOffset)[0], (*c.labelOffset)[1]));
}

std::optional<std::array<double, 2>> labelOffsetFor(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c,
                                                    const Point2& label) {
    const auto a = dimensionAnchor(d, st, c);
    if (!a) return std::nullopt;
    return std::array<double, 2>{label.x() - a->x(), label.y() - a->y()};
}

namespace {

// Char count of a dimension label (app-ux §6.6): value number (an expression — 6 chars), prefix, reference brackets.
int dimTextChars(const sketch::Constraint& c) {
    std::string v = c.value.value_or("0");
    if (auto sp = v.find(' '); sp != std::string::npos) v = v.substr(0, sp);
    bool number = !v.empty();
    for (char ch : v) number = number && (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == '+' || ch == 'e');
    int n = number ? static_cast<int>(v.size()) : 6;
    if (number && v[0] == '-') --n;  // the sign is not shown
    if (c.type == "radius" || c.type == "diameter" || c.type == "arcLength" || c.type == "arcAngle" || c.type == "pathLength" || c.diametral)
        ++n;
    if (sketch::isAngleDimension(c.type)) ++n;
    if (!c.driving.value_or(true)) n += 2;
    return std::max(n, 1);
}

struct Box {
    Point2 lo{0, 0}, hi{0, 0};  // the origin is always included
    void add(const Point2& p) { lo = lo.cwiseMin(p), hi = hi.cwiseMax(p); }
};

Box sketchBox(const sketch::SketchData& d, const sketch::SolvedState& st);

}  // namespace

std::vector<std::pair<Point2, Point2>> geometrySegments(const sketch::SketchData& d, const sketch::SolvedState& st) {
    std::vector<Seg> out;
    auto arc = [&](const Point2& c, double r, double a0, double sweep) {
        const int n = std::max(2, static_cast<int>(std::ceil(LAYOUT_CIRCLE_SEGMENTS * sweep / (2 * PI))));
        for (int i = 0; i < n; ++i) {
            const double u = a0 + sweep * i / n, v = a0 + sweep * (i + 1) / n;
            out.push_back({c + r * Point2(std::cos(u), std::sin(u)), c + r * Point2(std::cos(v), std::sin(v))});
        }
    };
    auto ccwSweep = [](const Point2& c, const Point2& s, const Point2& e) {
        const double a0 = angleOf(c, s);
        return std::pair{a0, geom2::ccwSweepPositive(a0, angleOf(c, e))};
    };
    for (const auto& e : d.entities) {
        if (e.type == sketch::EntityType::Line) {
            if (auto l = lineOf(d, st, e.id)) out.push_back(*l);
        } else if (e.type == sketch::EntityType::Circle) {
            if (auto c = curveOf(d, st, e.id)) arc(c->first, c->second, 0, 2 * PI);
        } else if (e.type == sketch::EntityType::Arc) {
            auto c = posOf(st, e.center), s = posOf(st, e.start), en = posOf(st, e.end);
            if (c && s && en) {
                const auto [a0, sw] = ccwSweep(*c, *s, *en);
                arc(*c, (*s - *c).norm(), a0, sw);
            }
        } else if (e.type == sketch::EntityType::Projected) {
            auto it = d.projections.find(e.id);
            if (it == d.projections.end()) continue;
            const auto& p = it->second;
            if (p.kind == sketch::Projection::Kind::Line) {
                out.push_back({Point2(p.x1, p.y1), Point2(p.x2, p.y2)});
            } else if (p.kind == sketch::Projection::Kind::Circle) {
                arc(Point2(p.cx, p.cy), p.r, 0, 2 * PI);
            } else if (p.kind == sketch::Projection::Kind::Arc) {
                const auto [a0, sw] = ccwSweep(Point2(p.cx, p.cy), Point2(p.sx, p.sy), Point2(p.ex, p.ey));
                arc(Point2(p.cx, p.cy), p.r, a0, sw);
            }
        }
    }
    return out;
}

namespace {

Box sketchBox(const sketch::SketchData& d, const sketch::SolvedState& st) {
    Box b;
    for (const auto& s : geometrySegments(d, st)) b.add(s.first), b.add(s.second);
    for (const auto& e : d.entities)
        if (auto p = posOf(st, e.id)) b.add(*p);
    return b;
}

}  // namespace

FullyDefineLayout resolvedLayout(const sketch::SketchData& d, const sketch::SolvedState& st, const FullyDefineLayout& m) {
    FullyDefineLayout r = m;
    if (r.textHeight <= 0) {
        const Box b = sketchBox(d, st);
        r.textHeight = std::max({b.hi.x() - b.lo.x(), b.hi.y() - b.lo.y(), 1.0}) / LAYOUT_EXTENT_TEXT;
    }
    if (r.charWidth <= 0) r.charWidth = LAYOUT_CHAR_RATIO * r.textHeight;
    return r;
}

std::optional<DimFootprint> dimensionFootprint(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c,
                                               const Point2& L, const FullyDefineLayout& m) {
    if (!sketch::isDimensionType(c.type)) return std::nullopt;
    const double h = m.textHeight, w = dimTextChars(c) * m.charWidth + LAYOUT_PAD * h;
    DimFootprint fp;
    fp.lo = L - Point2(w / 2, h / 2), fp.hi = L + Point2(w / 2, h / 2);
    const double eps = 1e-9 * std::max(1.0, h);
    auto unit = [](const Point2& v, const Point2& def) {
        const double l = v.norm();
        return l > 0 ? Point2(v / l) : def;
    };
    const auto& t = c.type;
    if (t == "length" || t == "distance" || t == "distanceX" || t == "distanceY") {
        std::optional<Point2> A, B;
        if (t == "length") {
            if (auto l = c.refs.empty() ? std::nullopt : lineOf(d, st, c.refs[0])) A = l->first, B = l->second;
        } else if (c.refs.size() == 2) {
            A = anchorOf(d, st, c.refs[0]);
            const auto l = lineOf(d, st, c.refs[1]);
            if (l && A && !lineOf(d, st, c.refs[0])) {  // point — line: to the foot of the perpendicular
                const Point2 dv = l->second - l->first;
                B = dv.squaredNorm() > 0 ? Point2(l->first + (*A - l->first).dot(dv) / dv.squaredNorm() * dv) : l->first;
            } else {
                B = anchorOf(d, st, c.refs[1]);
            }
        }
        if (!A || !B) return std::nullopt;
        Point2 u = unit(*B - *A, Point2(1, 0));
        if (t == "distanceX") u = Point2(1, 0);
        if (t == "distanceY") u = Point2(0, 1);
        const Point2 n(-u.y(), u.x());
        const Point2 A2 = *A + n * n.dot(L - *A), B2 = *B + n * n.dot(L - *B);
        for (const auto& [from, to] : {std::pair{*A, A2}, std::pair{*B, B2}})
            if ((to - from).norm() > eps) fp.extension.push_back({from, to + unit(to - from, n) * (LAYOUT_OVERSHOOT * h)});
        fp.dimension.push_back({A2, B2});
        const Point2 ub = unit(B2 - A2, u);
        const double sl = (L - A2).dot(ub), total = (B2 - A2).norm();
        if (sl < 0) fp.dimension.push_back({A2, A2 + ub * sl});
        if (sl > total) fp.dimension.push_back({B2, A2 + ub * sl});
        return fp;
    }
    if (t == "radius" || t == "diameter") {
        auto cc = c.refs.empty() ? std::nullopt : curveOf(d, st, c.refs[0]);
        if (!cc) return std::nullopt;
        const Point2 C = cc->first;
        const double r = cc->second;
        const Point2 v = unit(L - C, Point2(1, 0));
        if (t == "radius") fp.dimension.push_back({C, L});
        else fp.dimension.push_back({C - v * r, (L - C).norm() > r ? L : Point2(C + v * r)});
        return fp;
    }
    return fp;  // other kinds — label only
}

std::vector<Point2> layoutDimensionLabels(const sketch::SketchData& d, const sketch::SolvedState& st,
                                          const std::vector<sketch::Constraint>& added, const FullyDefineLayout& m0) {
    const FullyDefineLayout m = resolvedLayout(d, st, m0);
    const double h = m.textHeight, eps = 1e-9 * std::max(1.0, h);
    const std::vector<Seg> geom = geometrySegments(d, st);
    const Box box = sketchBox(d, st);
    std::vector<DimFootprint> placed;
    // Existing dimensions: label at label or near the refs' midpoint with an offset.
    for (const auto& c : d.constraints) {
        if (!sketch::isDimensionType(c.type)) continue;
        Point2 L;
        if (const auto lp = dimensionLabelPoint(d, st, c)) {
            L = *lp;
        } else {
            Point2 s(0, 0);
            int n = 0;
            for (const auto& r : c.refs)
                if (auto a = anchorOf(d, st, r)) s += *a, ++n;
            L = (n ? Point2(s / n) : s) + Point2(LAYOUT_DEFAULT_OFFSET * h, LAYOUT_DEFAULT_OFFSET * h);
        }
        if (auto fp = dimensionFootprint(d, st, c, L, m)) placed.push_back(*fp);
    }
    // Bounding boxes (label rectangle and all lines) — fast rejection of pairs that cannot intersect: the result is the
    // same as without it (intersection and hits require overlapping boxes), work — only among neighbors (S4-WEAK-001:
    // weak dimension layout after every command).
    struct BB {
        Point2 lo, hi;
        bool meets(const BB& o, double e) const {
            return lo.x() <= o.hi.x() + e && o.lo.x() <= hi.x() + e && lo.y() <= o.hi.y() + e && o.lo.y() <= hi.y() + e;
        }
    };
    auto bbOfSeg = [](const Seg& s) {
        return BB{Point2(std::min(s.first.x(), s.second.x()), std::min(s.first.y(), s.second.y())),
                  Point2(std::max(s.first.x(), s.second.x()), std::max(s.first.y(), s.second.y()))};
    };
    auto bbOf = [&](const DimFootprint& fp) {
        BB b{fp.lo, fp.hi};
        for (const auto* set : {&fp.dimension, &fp.extension})
            for (const auto& s : *set) {
                const BB sb = bbOfSeg(s);
                b.lo = b.lo.cwiseMin(sb.lo), b.hi = b.hi.cwiseMax(sb.hi);
            }
        return b;
    };
    std::vector<BB> geomBB;
    for (const auto& g : geom) geomBB.push_back(bbOfSeg(g));
    std::vector<BB> placedBB;
    for (const auto& p : placed) placedBB.push_back(bbOf(p));
    // bound — a score after which counting can stop (the candidate is no longer the best).
    auto cost = [&](const DimFootprint& fp, double bound) {
        double k = 0;
        const BB fb = bbOf(fp);
        for (size_t pi = 0; pi < placed.size() && k < bound; ++pi) {
            const auto& p = placed[pi];
            if (!fb.meets(placedBB[pi], eps)) continue;
            if (rectsOverlap(fp, p, eps)) k += W_RECT_RECT;
            for (const auto* set : {&fp.dimension, &fp.extension})
                for (const auto& s : *set) k += segHitsRect(s, p.lo, p.hi, eps) ? W_DIM : 0;
            for (const auto* set : {&p.dimension, &p.extension})
                for (const auto& s : *set) k += segHitsRect(s, fp.lo, fp.hi, eps) ? W_DIM : 0;
            for (const auto& s : fp.dimension) {
                for (const auto& t : p.dimension) k += segsCross(s, t, eps) ? W_DIM : 0;
                for (const auto& t : p.extension) k += segsCross(s, t, eps) ? W_DIM : 0;
            }
            for (const auto& s : fp.extension)
                for (const auto& t : p.dimension) k += segsCross(s, t, eps) ? W_DIM : 0;
        }
        for (size_t gi = 0; gi < geom.size() && k < bound; ++gi) {
            const auto& g = geom[gi];
            if (!fb.meets(geomBB[gi], eps)) continue;
            if (segHitsRect(g, fp.lo, fp.hi, eps)) k += W_RECT_GEOM;
            for (const auto& s : fp.dimension) k += segsCross(s, g, eps) ? W_LINE_GEOM : 0;
            for (const auto& s : fp.extension) k += segsCross(s, g, eps) ? W_LINE_GEOM + W_EXT_GEOM : 0;
        }
        return k;
    };
    // Axis dimension: distanceX/distanceY and length of a horizontal/vertical segment (S4-WEAK-005: width and height)
    // are laid out alike — in rows and columns outside the extent. Answer: X (else Y) and anchors; not axial — empty.
    struct AxisDim {
        bool X;
        Point2 a, b;
    };
    auto axisDim = [&](const sketch::Constraint& c) -> std::optional<AxisDim> {
        if ((c.type == "distanceX" || c.type == "distanceY") && c.refs.size() == 2) {
            auto a = anchorOf(d, st, c.refs[0]), b = anchorOf(d, st, c.refs[1]);
            if (a && b) return AxisDim{c.type == "distanceX", *a, *b};
        } else if (c.type == "length" && c.refs.size() == 1) {
            if (auto l = lineOf(d, st, c.refs[0])) {
                const Point2 dv = l->second - l->first;
                const double tol = 1e-9 * std::max(1.0, dv.norm());
                if (std::abs(dv.y()) <= tol) return AxisDim{true, l->first, l->second};
                if (std::abs(dv.x()) <= tol) return AxisDim{false, l->first, l->second};
            }
        }
        return std::nullopt;
    };
    // Order: X dimensions, then Y — by ascending absolute coordinate difference (short ones in nearer rows), then
    // the others; ties — order in added.
    auto group = [&](const sketch::Constraint& c) {
        const auto ad = axisDim(c);
        return ad ? (ad->X ? 0 : 1) : 2;
    };
    auto span = [&](const sketch::Constraint& c) {
        const auto ad = axisDim(c);
        if (!ad) return 0.0;
        return ad->X ? std::abs(ad->b.x() - ad->a.x()) : std::abs(ad->b.y() - ad->a.y());
    };
    std::vector<size_t> order(added.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t i, size_t j) {
        const int gi = group(added[i]), gj = group(added[j]);
        if (gi != gj) return gi < gj;
        return span(added[i]) < span(added[j]) - eps;
    });
    double wmaxY = 2 * h;  // column width for vertical dimensions — by the widest label
    for (const auto& c : added)
        if (group(c) == 1) wmaxY = std::max(wmaxY, dimTextChars(c) * m.charWidth + LAYOUT_PAD * h);
    const int rows = std::min(m0.maxRows > 0 ? m0.maxRows : LAYOUT_MAX_ROWS, 2 * static_cast<int>(added.size()) + 4);
    std::vector<Point2> out(added.size(), Point2::Zero());
    for (size_t idx : order) {
        const auto& c = added[idx];
        const double w = dimTextChars(c) * m.charWidth + LAYOUT_PAD * h;
        struct Cand {
            Point2 at;
            double penalty;
        };
        std::vector<Cand> cands;
        if (const auto ad = axisDim(c)) {
            const bool X = ad->X;
            const Point2* a = &ad->a;
            const Point2* b = &ad->b;
            const double lo = X ? std::min(a->x(), b->x()) : std::min(a->y(), b->y());
            const double hi = X ? std::max(a->x(), b->x()) : std::max(a->y(), b->y());
            const double along = X ? w : h;
            const std::array<std::pair<double, double>, 3> alongPos{
                {{(lo + hi) / 2, 0.0}, {hi + along / 2 + h, W_OUTSIDE}, {lo - along / 2 - h, W_OUTSIDE}}};
            // Across the dimension: the anchors' own extent (near rows inside the sketch extent, FX10-IO-006) and the
            // rows outside the extent; the penalty grows with the distance from the anchors.
            const double aLo = X ? std::min(a->y(), b->y()) : std::min(a->x(), b->x());
            const double aHi = X ? std::max(a->y(), b->y()) : std::max(a->x(), b->x());
            const double bLo = X ? box.lo.y() : box.lo.x(), bHi = X ? box.hi.y() : box.hi.x();
            auto offset = [&](int k) { return X ? LAYOUT_ROW * h * (k + 1) : (h + wmaxY / 2) + k * (wmaxY + h); };
            auto addRow = [&](double across, double pen) {
                const double dist = across < aLo ? aLo - across : across > aHi ? across - aHi : 0.0;
                for (const auto& [p, pa] : alongPos) cands.push_back({X ? Point2(p, across) : Point2(across, p), pen + pa + W_DIST * dist / h});
            };
            for (int k = 0; k < rows; ++k) {
                if (aLo - offset(k) > bLo) addRow(aLo - offset(k), W_FAR * k);
                if (aHi + offset(k) < bHi) addRow(aHi + offset(k), W_FAR * k);
            }
            for (int k = 0; k < rows; ++k)
                for (int side : {-1, 1}) addRow(side < 0 ? bLo - offset(k) : bHi + offset(k), W_FAR * k);
        } else if ((c.type == "radius" || c.type == "diameter") && !c.refs.empty()) {
            if (auto cc = curveOf(d, st, c.refs[0]))
                for (int k = 0; k < 4; ++k)
                    for (int j = 0; j < 8; ++j) {
                        const double ang = PI / 4 + j * PI / 4, dist = cc->second + h + w / 2 + k * LAYOUT_ROW * h;
                        cands.push_back({cc->first + dist * Point2(std::cos(ang), std::sin(ang)), W_FAR * k});
                    }
        }
        if (cands.empty()) {  // other kinds: around the refs' midpoint
            Point2 s(0, 0);
            int n = 0;
            for (const auto& r : c.refs)
                if (auto p = anchorOf(d, st, r)) s += *p, ++n;
            const Point2 mid = n ? Point2(s / n) : s;
            for (int k = 0; k < 4; ++k)
                for (int j = 0; j < 8; ++j) {
                    const double ang = PI / 4 + j * PI / 4, dist = h + w / 2 + k * LAYOUT_ROW * h;
                    cands.push_back({mid + dist * Point2(std::cos(ang), std::sin(ang)), W_FAR * k});
                }
        }
        double best = std::numeric_limits<double>::infinity();
        Point2 bestAt = cands.front().at;
        std::optional<DimFootprint> bestFp;
        for (const auto& cd : cands) {
            if (cd.penalty >= best) continue;  // candidate's penalty is already not less than the best score
            const auto fp = dimensionFootprint(d, st, c, cd.at, m);
            if (!fp) continue;
            const double k = cost(*fp, best - cd.penalty) + cd.penalty;
            if (k < best) best = k, bestAt = cd.at, bestFp = fp;
        }
        out[idx] = bestAt;
        if (bestFp) placed.push_back(*bestFp), placedBB.push_back(bbOf(*bestFp));
    }
    return out;
}

Result<std::vector<sketch::GroupItem>> groupItems(const sketch::SketchData& d, const std::string& type,
                                                  const std::vector<std::string>& sources, const Json& params,
                                                  const expr::DocumentUnits& units, const expr::Env& env) {
    return generateGroup(d, type, sources, params, units, env, "", nullptr);
}

CornerFind findCorner(const sketch::SketchData& d, const std::string& point) {
    CornerFind out;
    const auto* pe = d.findEntity(point);
    if (!pe || pe->type != sketch::EntityType::Point) {
        out.problem = "not a point";
        return out;
    }
    // Coincidence cluster: transitive closure over two-ref coincident constraints.
    std::set<std::string> cluster{point};
    for (bool grown = true; grown;) {
        grown = false;
        for (const auto& k : d.constraints) {
            if (k.type != "coincident" || k.refs.size() != 2) continue;
            const bool a = cluster.count(k.refs[0]) > 0, b = cluster.count(k.refs[1]) > 0;
            if (a != b) cluster.insert(a ? k.refs[1] : k.refs[0]), grown = true;
        }
    }
    for (const auto& e : d.entities)
        if (cluster.count(e.id)) out.cluster.push_back(e.id);
    bool otherCurve = false;
    std::vector<std::pair<std::string, std::string>> sides;  // (line, its end in the cluster)
    for (const auto& e : d.entities) {
        if (e.construction) continue;
        if (e.type == sketch::EntityType::Line) {
            const bool a = cluster.count(e.p1) > 0, b = cluster.count(e.p2) > 0;
            if (a && b) continue;  // degenerate line inside the corner — not a side
            if (a || b) sides.emplace_back(e.id, a ? e.p1 : e.p2);
        } else if (e.type != sketch::EntityType::Point && e.type != sketch::EntityType::Text) {
            std::vector<std::string> ends{e.p1, e.p2, e.start, e.end};
            if (!e.poles.empty() && !e.periodic) ends.push_back(e.poles.front()), ends.push_back(e.poles.back());
            for (const auto& r : ends)
                if (!r.empty() && cluster.count(r)) otherCurve = true;
        }
    }
    out.lines = int(sides.size());
    // The clicked end's line goes first (stable choice of l1, as before FX7-SK-006).
    std::stable_partition(sides.begin(), sides.end(), [&](const auto& s) { return s.second == point; });
    if (sides.size() == 2 && !otherCurve) {
        out.l1 = sides[0].first, out.p = sides[0].second;
        out.l2 = sides[1].first, out.q = sides[1].second;
        return out;
    }
    out.problem = sides.size() > 2 ? "many lines" : otherCurve ? "not lines" : sides.empty() ? "no lines" : "one line";
    return out;
}

std::optional<EqualMatch> equalTarget(const sketch::SketchData& d, const sketch::SolvedState& st, bool radius, double value, double absTol,
                                      const std::set<std::string>& exclude) {
    auto P = [&](const std::string& id) -> std::optional<Point2> {
        if (const auto it = st.points.find(id); it != st.points.end()) return it->second;
        if (const auto* e = d.findEntity(id)) return Point2(e->x, e->y);
        return std::nullopt;
    };
    std::optional<EqualMatch> best;
    double bestDiff = absTol;
    for (const auto& e : d.entities) {
        if (exclude.count(e.id)) continue;
        std::optional<double> v;
        if (radius && e.type == sketch::EntityType::Circle) {
            const auto it = st.radii.find(e.id);
            v = it != st.radii.end() ? it->second : e.r;
        } else if (radius && e.type == sketch::EntityType::Arc) {
            if (auto c = P(e.center), a = P(e.start); c && a) v = (*a - *c).norm();
        } else if (!radius && e.type == sketch::EntityType::Line) {
            if (auto a = P(e.p1), b = P(e.p2); a && b) v = (*b - *a).norm();
        }
        if (!v || *v <= 0) continue;
        const double diff = std::abs(value - *v);
        if (diff <= bestDiff && (!best || diff < bestDiff)) best = EqualMatch{e.id, *v}, bestDiff = diff;
    }
    return best;
}

std::optional<double> neighborFilletRadius(const sketch::SketchData& d, const sketch::SolvedState& st, const std::string& point) {
    const CornerFind corner = findCorner(d, point);
    if (!corner.problem.empty()) return std::nullopt;
    auto P = [&](const std::string& id) -> Point2 {
        if (const auto it = st.points.find(id); it != st.points.end()) return it->second;
        const auto* e = d.findEntity(id);
        return e ? Point2(e->x, e->y) : Point2::Zero();
    };
    // Coincidence clusters of points (union-find over two-ref coincident constraints).
    std::map<std::string, std::string> parent;
    std::function<std::string(const std::string&)> root = [&](const std::string& x) -> std::string {
        auto it = parent.find(x);
        if (it == parent.end() || it->second == x) return x;
        return it->second = root(it->second);
    };
    for (const auto& k : d.constraints)
        if (k.type == "coincident" && k.refs.size() == 2) {
            const std::string a = root(k.refs[0]), b = root(k.refs[1]);
            if (a != b) parent[a] = b;
        }
    // Curves of the contour: non-construction lines and arcs, connected through their ends (branching allowed).
    std::vector<const sketch::Entity*> curves;
    for (const auto& e : d.entities)
        if (!e.construction && (e.type == sketch::EntityType::Line || e.type == sketch::EntityType::Arc)) curves.push_back(&e);
    auto ends = [](const sketch::Entity& e) {
        return e.type == sketch::EntityType::Line ? std::array<std::string, 2>{e.p1, e.p2} : std::array<std::string, 2>{e.start, e.end};
    };
    std::set<std::string> reached{root(corner.p)}, inComponent;
    for (bool grown = true; grown;) {
        grown = false;
        for (const auto* c : curves) {
            if (inComponent.count(c->id)) continue;
            const auto en = ends(*c);
            if (!reached.count(root(en[0])) && !reached.count(root(en[1]))) continue;
            inComponent.insert(c->id);
            reached.insert(root(en[0])), reached.insert(root(en[1]));
            grown = true;
        }
    }
    // The nearest arc by the distance from the corner to the arc's middle; ties — the first by entity order.
    const Point2 at = P(point);
    std::optional<double> best;
    double bestDist = std::numeric_limits<double>::infinity();
    for (const auto* c : curves) {
        if (c->type != sketch::EntityType::Arc || !inComponent.count(c->id)) continue;
        const Point2 o = P(c->center), a = P(c->start) - o, b = P(c->end) - o;
        const double r = a.norm();
        if (r <= 0) continue;
        double a0 = std::atan2(a.y(), a.x()), sw = geom2::ccwSweepPositive(a0, std::atan2(b.y(), b.x()));
        const Point2 mid = o + r * Point2(std::cos(a0 + sw / 2), std::sin(a0 + sw / 2));
        const double dd = (mid - at).norm();
        if (dd < bestDist) bestDist = dd, best = r;
    }
    return best;
}

std::string tangentAtJoin(const sketch::SketchData& d, const sketch::SolvedState& st, const std::string& point, const Point2& dir,
                          double tolRad) {
    if (dir.norm() <= 0) return {};
    auto P = [&](const std::string& id) {
        if (const auto it = st.points.find(id); it != st.points.end()) return it->second;
        const auto* e = d.findEntity(id);
        return e ? Point2(e->x, e->y) : Point2(0, 0);
    };
    std::set<std::string> joint{point};
    for (const auto& c : d.constraints)
        if (c.type == "coincident" && c.refs.size() == 2 && (c.refs[0] == point || c.refs[1] == point)) joint.insert(c.refs[0] == point ? c.refs[1] : c.refs[0]);
    const Point2 u = dir.normalized();
    for (const auto& e : d.entities) {
        Point2 t = Point2::Zero();
        if (e.type == sketch::EntityType::Line && (joint.count(e.p1) || joint.count(e.p2))) {
            t = P(e.p2) - P(e.p1);
        } else if (e.type == sketch::EntityType::Arc && (joint.count(e.start) || joint.count(e.end))) {
            const Point2 r = P(joint.count(e.start) ? e.start : e.end) - P(e.center);
            t = Point2(-r.y(), r.x());
        } else {
            continue;
        }
        if (t.norm() <= 0) continue;
        t.normalize();
        if (std::abs(u.x() * t.y() - u.y() * t.x()) <= std::sin(tolRad)) return e.id;
    }
    return {};
}

std::vector<std::string> contourOf(const sketch::SketchData& d, const std::string& curve) { return sketch::contourOf(d, curve); }

int offsetSign(const sketch::SketchData& d, const std::vector<std::string>& sources, const std::string& curve, int side) {
    return sketch::offsetSign(d, sources, curve, side);
}

}  // namespace construe::api::sketchops
