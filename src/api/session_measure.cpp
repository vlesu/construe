// Session: measuring picked entities (spec/core-api.md §5 measure, FX10-MEAS-001).
#include "api/session.h"

#include "api/errors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace construe::api {

namespace {

// A picked entity reduced to what the measurements need (core-api.md §5 measure).
struct Picked {
    int body = -1;  // body index in the state; -1 — not a body entity (a reference plane)
    naming::EntityKind kind = naming::EntityKind::Face;
    double length = 0, area = 0;
    std::optional<kernel::SurfaceInfo> surface;
    std::optional<kernel::CurveInfo> curve;
    std::optional<Point3> vertex;
    // Roles for the pair measurements: a point (vertex, circle center), a line (straight edge, cylinder axis), a plane.
    std::optional<Point3> point;
    std::optional<std::pair<Point3, Vec3>> line;  // point and unit direction
    std::optional<std::pair<Point3, Vec3>> plane;  // point and unit normal
    // A point of the entity itself (FX12-MEAS-001): face and edge centroid, vertex, datum plane origin — where the
    // distance arrow starts on a line or plane carrier (projected onto it).
    std::optional<Point3> anchor;
};

Point3 footOnLine(const Point3& p, const std::pair<Point3, Vec3>& l) { return l.first + l.second * (p - l.first).dot(l.second); }
Point3 footOnPlane(const Point3& p, const std::pair<Point3, Vec3>& pl) { return p - pl.second * (p - pl.first).dot(pl.second); }

double distPointLine(const Point3& p, const std::pair<Point3, Vec3>& l) { return (p - l.first).cross(l.second).norm(); }
double distPointPlane(const Point3& p, const std::pair<Point3, Vec3>& pl) { return std::abs((p - pl.first).dot(pl.second)); }
// Acute angle between two directions, [0, π/2].
double acuteAngle(const Vec3& a, const Vec3& b) { return std::atan2(a.cross(b).norm(), std::abs(a.dot(b))); }

void finishRoles(Picked& p) {
    if (p.vertex) p.point = p.vertex;
    if (p.curve) {
        if (p.curve->kind == kernel::CurveInfo::Kind::Line) p.line = {{p.curve->p1, (p.curve->p2 - p.curve->p1).normalized()}};
        if (p.curve->kind == kernel::CurveInfo::Kind::Circle) p.point = p.curve->center;
    }
    if (p.surface) {
        if (p.surface->kind == kernel::SurfaceInfo::Kind::Plane) p.plane = {{p.surface->point, p.surface->dir.normalized()}};
        if (p.surface->kind == kernel::SurfaceInfo::Kind::Cylinder) p.line = {{p.surface->point, p.surface->dir.normalized()}};
    }
}

}  // namespace

Result<std::vector<MeasureItem>> Session::measure(const std::vector<std::string>& refs, const StateRef& ref) const {
    if (refs.empty() || refs.size() > 2) return Error("API_INVALID_ARGUMENT").with("name", std::string("refs")).with("actual", double(refs.size()));
    auto st = stateOf(ref);
    if (!st) return st.error();
    const regen::PartState& s = **st;
    std::vector<Picked> picks;
    for (const auto& name : refs) {
        Picked p;
        if (const auto dp = datumPlane(name)) {  // a source plane (plane:front …)
            p.plane = {{dp->origin, dp->normal.normalized()}};
            p.anchor = dp->origin;
        } else if (auto rp = s.planes.find(name); rp != s.planes.end()) {  // a reference plane feature
            p.plane = {{rp->second.origin, rp->second.normal.normalized()}};
        } else {
            auto it = s.names.byName.find(name);
            if (it == s.names.byName.end()) return notFound(name);
            p.kind = it->second.kind;
            switch (p.kind) {
                case naming::EntityKind::Face: {
                    const auto& fe = s.names.faces.at(it->second.index);
                    p.body = fe.body;
                    p.area = fe.measure;
                    p.surface = k_.faceSurface(s.bodies.at(size_t(fe.body)).shape, fe.face);
                    p.anchor = k_.faceCentroid(s.bodies.at(size_t(fe.body)).shape, fe.face);
                    break;
                }
                case naming::EntityKind::Edge: {
                    const auto& ee = s.names.edges.at(it->second.index);
                    p.body = ee.body;
                    p.length = ee.measure;
                    p.curve = k_.edgeCurve(s.bodies.at(size_t(ee.body)).shape, ee.logical);
                    p.anchor = k_.edgeCentroid(s.bodies.at(size_t(ee.body)).shape, ee.logical);
                    break;
                }
                case naming::EntityKind::Vertex: {
                    const auto& ve = s.names.vertices.at(it->second.index);
                    p.body = ve.body;
                    p.vertex = k_.vertexPoint(s.bodies.at(size_t(ve.body)).shape, ve.vertex);
                    p.anchor = p.vertex;
                    break;
                }
            }
        }
        finishRoles(p);
        picks.push_back(std::move(p));
    }
    std::vector<MeasureItem> out;
    auto add = [&](const char* kind, double v, const Point3& pt = Point3::Zero()) { out.push_back({kind, v, pt}); };
    if (picks.size() == 1) {
        const Picked& p = picks[0];
        if (p.vertex) add("point", 0, *p.vertex);
        if (p.curve) {
            add("length", p.length);
            if (p.curve->kind == kernel::CurveInfo::Kind::Circle) {
                if (p.curve->closed) add("diameter", 2 * p.curve->radius);
                else add("radius", p.curve->radius);
            }
        }
        if (p.surface) {
            add("area", p.area);
            if (p.surface->kind == kernel::SurfaceInfo::Kind::Cylinder) add("diameter", 2 * p.surface->radius);
        }
        if (p.body >= 0) {  // the body of the entity: volume, mass by the part material, center of mass
            const auto mp = k_.massProperties(s.bodies.at(size_t(p.body)).shape);
            add("volume", mp.volume);
            if (const auto& el = doc().active(); el.material) add("mass", (*el.material)["density"].get<double>() * mp.volume * 1e-9);
            add("centroid", 0, mp.centroid);
        }
        return out;
    }
    const Picked &a = picks[0], &b = picks[1];
    const auto parallel = [](const Vec3& u, const Vec3& v) { return u.cross(v).norm() <= MEASURE_PARALLEL_TOL; };
    // The distance with its two points — from the first selected to the second (FX12-MEAS-001).
    auto addSpan = [&](double v, const Point3& from, const Point3& to, bool swap) {
        add("distance", v);
        out.back().span = swap ? std::array<Point3, 2>{to, from} : std::array<Point3, 2>{from, to};
    };
    // The start on a carrier: the entity's own point projected onto it.
    auto onLine = [](const Picked& p) { return footOnLine(p.anchor.value_or(p.line->first), *p.line); };
    auto onPlane = [](const Picked& p) { return footOnPlane(p.anchor.value_or(p.plane->first), *p.plane); };
    if (a.point && b.point) {
        addSpan((*a.point - *b.point).norm(), *a.point, *b.point, false);
    } else if ((a.point && b.line) || (a.line && b.point)) {
        const Point3& p = a.point ? *a.point : *b.point;
        const auto& l = a.point ? *b.line : *a.line;
        addSpan(distPointLine(p, l), p, footOnLine(p, l), !a.point);
    } else if ((a.point && b.plane) || (a.plane && b.point)) {
        const Point3& p = a.point ? *a.point : *b.point;
        const auto& pl = a.point ? *b.plane : *a.plane;
        addSpan(distPointPlane(p, pl), p, footOnPlane(p, pl), !a.point);
    } else if (a.line && b.line) {
        const Vec3 n = a.line->second.cross(b.line->second);
        if (parallel(a.line->second, b.line->second)) {
            const Point3 p = onLine(a);
            addSpan(distPointLine(a.line->first, *b.line), p, footOnLine(p, *b.line), false);
        } else {
            add("angle", acuteAngle(a.line->second, b.line->second));
            // Skew lines: the length of the common perpendicular, between its feet on the two lines.
            const double d = std::abs((b.line->first - a.line->first).dot(n.normalized()));
            if (d > MEASURE_COPLANAR_TOL) {
                const Vec3 u = a.line->second, v = b.line->second, w = a.line->first - b.line->first;
                const double uv = u.dot(v), den = 1 - uv * uv;
                const double sa = (uv * v.dot(w) - u.dot(w)) / den, sb = (v.dot(w) - uv * u.dot(w)) / den;
                addSpan(d, a.line->first + u * sa, b.line->first + v * sb, false);
            }
        }
    } else if (a.plane && b.plane) {
        if (parallel(a.plane->second, b.plane->second)) {
            const Point3 p = onPlane(a);
            addSpan(distPointPlane(a.plane->first, *b.plane), p, footOnPlane(p, *b.plane), false);
        } else {
            add("angle", acuteAngle(a.plane->second, b.plane->second));
        }
    } else if ((a.line && b.plane) || (a.plane && b.line)) {
        const Picked& lp = a.line ? a : b;
        const auto& l = *lp.line;
        const auto& pl = a.plane ? *a.plane : *b.plane;
        if (std::abs(l.second.dot(pl.second)) <= MEASURE_PARALLEL_TOL) {
            const Point3 p = onLine(lp);
            addSpan(distPointPlane(l.first, pl), p, footOnPlane(p, pl), !a.line);
        } else {
            add("angle", std::numbers::pi / 2 - acuteAngle(l.second, pl.second));
        }
    }
    if (a.body >= 0 && b.body >= 0 && a.body != b.body) {  // the shortest distance between the two bodies
        const auto& sa = s.bodies.at(size_t(a.body)).shape;
        const auto& sb = s.bodies.at(size_t(b.body)).shape;
        double best = std::numeric_limits<double>::infinity();
        for (const auto* pair : {&sa, &sb}) {
            const auto& from = *pair;
            const auto& to = pair == &sa ? sb : sa;
            auto t = k_.tessellate(from, kernel::TessParams{MEASURE_BODY_CHORD, kernel::TessParams{}.angleTol});
            if (!t) continue;
            for (const auto& f : t->faces)
                for (size_t i = 0; i + 2 < f.positions.size(); i += 3)
                    best = std::min(best, k_.distanceToShape(to, Point3(f.positions[i], f.positions[i + 1], f.positions[i + 2])));
            for (const auto& v : t->vertices)
                best = std::min(best, k_.distanceToShape(to, Point3(v.position[0], v.position[1], v.position[2])));
        }
        if (std::isfinite(best)) add("bodyDistance", best);
    }
    return out;
}

}  // namespace construe::api
