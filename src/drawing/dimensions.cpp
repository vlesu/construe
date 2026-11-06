// Drawing dimensions (spec/drawing.md §6а, GOST 2.307, ISO 129-1): reference geometry, layout, dimension construction,
// view geometry picking. Dimensions from the model (§6а.10) — model_dimensions.cpp.
#include "drawing/dimension_parts.h"

#include "drawing/render.h"
#include "drawing/standard.h"
#include "drawing/view_pmi.h"
#include "base/number_format.h"
#include "regen/naming.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace construe::drawing {

namespace dimdetail {

Error badRefs(const char* detail) { return Error("DRAWING_DIM_BAD_REFS").with("detail", std::string(detail)); }

bool sameRefs(const doc::SheetDimension& a, const doc::SheetDimension& b) {
    if (a.kind != b.kind || a.refs.size() != b.refs.size()) return false;
    auto key = [](const doc::SheetDimension& x) {
        std::vector<std::string> k;
        for (const auto& r : x.refs) k.push_back(r.kind + ":" + r.name + ":" + std::to_string(r.side) + ":" + r.arc);
        std::sort(k.begin(), k.end());
        return k;
    };
    return key(a) == key(b);
}

}  // namespace dimdetail

namespace {

using namespace dimdetail;

using geom2::angleOf;
Point2 dirOf(double a) { return Point2(std::cos(a), std::sin(a)); }

// Readable text angle (§6а.4): reduction to (−π/2, π/2].
double readable(double t) {
    while (t > PI / 2) t -= PI;
    while (t <= -PI / 2) t += PI;
    return t;
}

bool arcContains(double a0, double a1, double x) {
    return geom2::ccwSweep(a0, x) <= sweepOf(a0, a1);
}

}  // namespace

namespace dimdetail {

// Curve edge (§6а.2, FX7-DRW-010): the point set of the logical edge; geometry — by kernel queries on use.
RefGeom curveEdge(RefGeom g, const naming::EdgeEntry& e) {
    g.kind = RefGeom::Kind::Curve;
    g.set.edge = e.logical;
    g.body = e.body;
    g.depth = g.depthMin = -1e300;  // by its polyline vertices (§6а.7)
    return g;
}

Result<RefGeom> edgeGeom(const Ctx& c, int index) {
    const auto& e = c.st.names.edges.at(size_t(index));
    const kernel::Shape& shape = c.st.bodies.at(size_t(e.body)).shape;
    const kernel::CurveInfo ci = c.k.edgeCurve(shape, e.logical);
    RefGeom g;
    g.edgeEntry = index;
    if (ci.kind == kernel::CurveInfo::Kind::Line) {
        g.kind = RefGeom::Kind::Segment;
        g.a = c.vp.toView(ci.p1);
        g.b = c.vp.toView(ci.p2);
        if ((g.b - g.a).norm() <= DIM_EPS) return badRefs("edgeOnView");
        g.p1 = ci.p1;
        g.p2 = ci.p2;
        g.dir3 = (ci.p2 - ci.p1).normalized();
        g.depth = std::max(ci.p1.dot(c.vp.n), ci.p2.dot(c.vp.n));
        g.depthMin = std::min(ci.p1.dot(c.vp.n), ci.p2.dot(c.vp.n));
        return g;
    }
    // Closed circle seen edge-on (FX1-DRW-002): the axis lies in the view plane — a diameter segment
    // c ± r·w, w = n × axis (§6а.2).
    if (ci.kind == kernel::CurveInfo::Kind::Circle && ci.closed && std::abs(ci.axis.normalized().dot(c.vp.n)) <= DIM_PARALLEL_TOL) {
        const Vec3 w = c.vp.n.cross(ci.axis.normalized()).normalized();
        const Point3 p1 = ci.center - ci.radius * w, p2 = ci.center + ci.radius * w;
        g.kind = RefGeom::Kind::Segment;
        g.a = c.vp.toView(p1);
        g.b = c.vp.toView(p2);
        if ((g.b - g.a).norm() <= DIM_EPS) return badRefs("edgeOnView");
        g.p1 = p1;
        g.p2 = p2;
        g.dir3 = w;
        g.depth = g.depthMin = ci.center.dot(c.vp.n);
        g.edgeOn = true;
        g.edgeOnRadius = ci.radius;
        return g;
    }
    if (ci.kind == kernel::CurveInfo::Kind::Circle && 1 - std::abs(ci.axis.dot(c.vp.n)) <= DIM_PARALLEL_TOL) {
        g.center = c.vp.toView(ci.center);
        g.radius = ci.radius;
        g.depth = g.depthMin = ci.center.dot(c.vp.n);
        if (ci.closed) {
            g.kind = RefGeom::Kind::Circle;
            return g;
        }
        const auto ct = kernel::canonicalTopology(c.k, shape);
        const auto& ends = ct.logicalEdges.at(size_t(e.logical)).ends;
        if (ends.size() != 2) return curveEdge(g, e);
        const double a0 = angleOf(c.vp.toView(c.k.vertexPoint(shape, ends[0])) - g.center);
        const double a1 = angleOf(c.vp.toView(c.k.vertexPoint(shape, ends[1])) - g.center);
        const double am = angleOf(c.vp.toView(e.key) - g.center);
        g.kind = RefGeom::Kind::Arc;
        if (arcContains(a0, a1, am)) g.start = a0, g.end = a1;
        else g.start = a1, g.end = a0;
        return g;
    }
    // Any other edge — a curve (FX7-DRW-010): inclined circle, edge-on arc, ellipse, spline, composite.
    return curveEdge(g, e);
}

// Silhouette or cut line of a face (§6а.2, FX3-DRW-002).
Result<RefGeom> faceLineGeom(const Ctx& c, int fi, bool cut, int side) {
    const auto& fe = c.st.names.faces.at(size_t(fi));
    const kernel::Shape& shape = c.st.bodies.at(size_t(fe.body)).shape;
    const kernel::SurfaceInfo si = c.k.faceSurface(shape, fe.face);
    const char* err = cut ? "cut" : "silhouette";
    if (cut && !c.vp.cutOrigin) return badRefs("cut");
    const Vec3 n = c.vp.n;
    const Point3 O = cut ? *c.vp.cutOrigin : Point3::Zero();
    const double cutDepth = O.dot(n);
    auto onFace = [&](const Point3& X) { return c.k.distanceToFace(shape, fe.face, X) <= DIM_FACE_TOL; };
    // Extent of the face along the axis (A, a) — from the vertices of its edges, as for center lines (§6.15 item 1).
    auto extent = [&](const Point3& A, const Vec3& a) -> std::optional<std::pair<double, double>> {
        double t0 = 1e300, t1 = -1e300;
        for (int e : c.k.faceEdges(shape, fe.face)) {
            const auto [v0, v1] = c.k.edgeVertices(shape, e);
            for (int v : {v0, v1}) {
                if (v < 0) continue;
                const double t = (c.k.vertexPoint(shape, v) - A).dot(a);
                t0 = std::min(t0, t);
                t1 = std::max(t1, t);
            }
        }
        if (t1 - t0 < DIM_EPS) return std::nullopt;
        return std::pair{t0, t1};
    };
    RefGeom g;
    g.faceEntry = fi;
    g.side = side;
    auto segment = [&](const Point3& P1, const Point3& P2, double depth) -> Result<RefGeom> {
        g.kind = RefGeom::Kind::Segment;
        g.a = c.vp.toView(P1);
        g.b = c.vp.toView(P2);
        if ((g.b - g.a).norm() <= DIM_EPS) return badRefs("edgeOnView");
        g.p1 = P1;
        g.p2 = P2;
        g.dir3 = (P2 - P1).normalized();
        g.depth = g.depthMin = depth;
        return g;
    };
    // Outline circle (center C in the plane ⊥ n, radius r): all four points C ± r·r̂, C ± r·û lie on the face.
    auto outline = [&](const Point3& C, double r, double depth) -> Result<RefGeom> {
        if (side != 0) return badRefs(err);
        for (const Vec3& v : {c.vp.r, Vec3(-c.vp.r), c.vp.u, Vec3(-c.vp.u)})
            if (!onFace(C + r * v)) return badRefs(err);
        g.kind = RefGeom::Kind::Circle;
        g.center = c.vp.toView(C);
        g.radius = r;
        g.depth = g.depthMin = depth;
        return g;
    };
    using SK = kernel::SurfaceInfo::Kind;
    const Vec3 a = si.dir.normalized();
    const bool axisSide = std::abs(a.dot(n)) <= DIM_PARALLEL_TOL, axisAlong = 1 - std::abs(a.dot(n)) <= DIM_PARALLEL_TOL;
    if (si.kind == SK::Cylinder && axisSide) {
        if (side != 1 && side != -1) return badRefs(err);
        const Vec3 w = n.cross(a).normalized();
        double h = si.radius;
        Point3 base = si.point;
        if (cut) {  // cutting plane ∥ axis at distance g: generatrices at √(ρ² − g²) from the axis projection
            const double gg = (si.point - O).dot(n);
            if (std::abs(gg) >= si.radius - DIM_EPS) return badRefs(err);
            h = std::sqrt(si.radius * si.radius - gg * gg);
            base = si.point - gg * n;
        }
        const auto ext = extent(si.point, a);
        if (!ext) return badRefs(err);
        const Point3 P1 = base + ext->first * a + side * h * w, P2 = base + ext->second * a + side * h * w;
        if (!onFace((P1 + P2) / 2)) return badRefs(err);
        g.cylRadius = si.radius;
        g.halfWidth = h;
        return segment(P1, P2, cut ? cutDepth : si.point.dot(n));
    }
    if (si.kind == SK::Cylinder && axisAlong) {
        if (!cut) {
            const auto ext = extent(si.point, a);
            if (!ext) return badRefs(err);
            return outline(si.point + (ext->first + ext->second) / 2 * a, si.radius, si.point.dot(n));
        }
        return outline(si.point + (O - si.point).dot(a) * a, si.radius, cutDepth);
    }
    if (si.kind == SK::Cone && axisSide && !cut) {
        if (side != 1 && side != -1) return badRefs(err);
        const Vec3 w = n.cross(a).normalized();
        const auto ext = extent(si.point, a);  // from the cone apex
        if (!ext) return badRefs(err);
        const double sa = ext->first + ext->second >= 0 ? 1.0 : -1.0;  // the cone opens along sa·a
        const double t0 = std::max(0.0, sa > 0 ? ext->first : -ext->second), t1 = sa > 0 ? ext->second : -ext->first;
        if (t1 - t0 < DIM_EPS) return badRefs(err);
        const double tn = std::tan(si.halfAngle);
        const Point3 P1 = si.point + t0 * sa * a + side * t0 * tn * w, P2 = si.point + t1 * sa * a + side * t1 * tn * w;
        if (!onFace((P1 + P2) / 2)) return badRefs(err);
        return segment(P1, P2, si.point.dot(n));
    }
    // Surface of revolution of an arbitrary profile and a torus seen from the side (FX7-DRW-010): the meridian in the
    // view plane on the side `side` — a curve.
    if (!cut && axisSide && (si.kind == SK::Torus || (si.kind == SK::Other && si.otherKind == "revolution"))) {
        if (side != 1 && side != -1) return badRefs(err);
        g.kind = RefGeom::Kind::Curve;
        g.set.face = fe.face;
        g.set.half = double(side) * n.cross(a).normalized();
        g.body = fe.body;
        g.axis3 = a;
        g.depth = g.depthMin = si.point.dot(n);
        return g;
    }
    if (si.kind == SK::Sphere) {
        if (!cut) return outline(si.point, si.radius, si.point.dot(n));
        const double gg = (si.point - O).dot(n);
        if (std::abs(gg) >= si.radius - DIM_EPS) return badRefs(err);
        return outline(si.point - gg * n, std::sqrt(si.radius * si.radius - gg * gg), cutDepth);
    }
    if (si.kind == SK::Plane && cut) {
        if (side != 0) return badRefs(err);
        const Vec3 N = a;
        if (1 - std::abs(N.dot(n)) <= DIM_PARALLEL_TOL) return badRefs(err);  // face ∥ cutting plane
        const Vec3 D = N.cross(n).normalized(), q = n.cross(D);
        const Point3 X0 = O - N.dot(O - si.point) / N.dot(q) * q;  // point on the plane intersection line
        std::vector<double> ts;
        for (const auto& e : c.st.names.edges) {
            if ((e.f1 != fi && e.f2 != fi) || e.body != fe.body) continue;
            const kernel::CurveInfo ci = c.k.edgeCurve(shape, e.logical);
            std::vector<Point3> cand;
            if (ci.kind == kernel::CurveInfo::Kind::Line) {
                const double g1 = (ci.p1 - O).dot(n), g2 = (ci.p2 - O).dot(n);
                if (g1 * g2 <= 0 && g1 != g2) cand.push_back(ci.p1 + g1 / (g1 - g2) * (ci.p2 - ci.p1));
            } else if (ci.kind == kernel::CurveInfo::Kind::Circle && 1 - std::abs(ci.axis.normalized().dot(N)) <= DIM_PARALLEL_TOL) {
                const double ac = (ci.center - X0).dot(D);
                const Vec3 perp = (ci.center - X0) - ac * D;
                const double d2 = ci.radius * ci.radius - perp.squaredNorm();
                if (d2 >= 0) {
                    cand.push_back(X0 + (ac - std::sqrt(d2)) * D);
                    cand.push_back(X0 + (ac + std::sqrt(d2)) * D);
                }
            }
            for (const auto& p : cand) {
                double dist = 1e300;
                for (int ke : e.kernelEdges) dist = std::min(dist, c.k.distanceToEdge(shape, ke, p));
                if (dist <= DIM_FACE_TOL) ts.push_back((p - X0).dot(D));
            }
        }
        if (ts.size() < 2) return badRefs(err);
        const auto [lo, hi] = std::minmax_element(ts.begin(), ts.end());
        if (*hi - *lo <= DIM_EPS) return badRefs(err);
        return segment(X0 + *lo * D, X0 + *hi * D, cutDepth);
    }
    return badRefs("curveType");
}

// Axis of a cylinder or cone face together with the faces of the same surface of its body (§6а.2 «Axis and center»,
// FX9-DRW-001; grouping and angular span — as for center lines, §6.15 items 1, 2).
std::optional<AxisSurface> axisSurface(const Ctx& c, int fi) {
    const auto& fe = c.st.names.faces.at(size_t(fi));
    using SK = kernel::SurfaceInfo::Kind;
    auto info = [&](const naming::FaceEntry& f) -> std::optional<AxisSurface> {
        const kernel::Shape& shape = c.st.bodies.at(size_t(f.body)).shape;
        const kernel::SurfaceInfo si = c.k.faceSurface(shape, f.face);
        if (si.kind != SK::Cylinder && si.kind != SK::Cone) return std::nullopt;
        AxisSurface g;
        g.cone = si.kind == SK::Cone;
        g.A = si.point;
        g.a = si.dir.normalized();
        g.radius = si.radius;
        g.halfAngle = si.halfAngle;
        double t0 = 1e300, t1 = -1e300;
        for (int e : c.k.faceEdges(shape, f.face)) {
            const auto [v0, v1] = c.k.edgeVertices(shape, e);
            for (int v : {v0, v1}) {
                if (v < 0) continue;
                const double t = (c.k.vertexPoint(shape, v) - g.A).dot(g.a);
                t0 = std::min(t0, t);
                t1 = std::max(t1, t);
            }
        }
        if (!(t1 - t0 >= DIM_EPS)) return std::nullopt;
        g.t0 = t0;
        g.t1 = t1;
        const double area = c.k.faceArea(shape, f.face);
        if (!g.cone) {
            g.phi = g.radius > 0 ? area / (g.radius * (t1 - t0)) : 0;
        } else {
            const double ta = std::tan(g.halfAngle), r0 = std::abs(t0) * ta, r1 = std::abs(t1) * ta;
            const double lat = (t1 - t0) / std::cos(g.halfAngle) * (r0 + r1) / 2;
            g.phi = lat > 0 ? area / lat : 0;
        }
        return g;
    };
    auto g = info(fe);
    if (!g) return std::nullopt;
    g->first = fi;
    for (size_t i = 0; i < c.st.names.faces.size(); ++i) {
        const auto& o = c.st.names.faces[i];
        if (int(i) == fi || o.body != fe.body) continue;
        const auto h = info(o);
        if (!h || h->cone != g->cone || h->a.cross(g->a).norm() > 1e-9) continue;
        const Vec3 w = h->A - g->A;
        if ((w - w.dot(g->a) * g->a).norm() > 1e-6) continue;  // axes on one line
        if (!g->cone ? std::abs(h->radius - g->radius) > 1e-6 : (w.norm() > 1e-6 || std::abs(h->halfAngle - g->halfAngle) > 1e-9 || h->a.dot(g->a) <= 0))
            continue;
        const double shift = w.dot(g->a);
        const bool along = h->a.dot(g->a) > 0;
        g->t0 = std::min(g->t0, along ? shift + h->t0 : shift - h->t1);
        g->t1 = std::max(g->t1, along ? shift + h->t1 : shift - h->t0);
        g->phi += h->phi;
        g->first = std::min(g->first, int(i));
    }
    return g;
}

// Axis (a segment, the axis seen from the side) or center (a point, the axis along the view direction) of a face (§6а.2).
Result<RefGeom> axisGeom(const Ctx& c, int fi, bool center) {
    const char* err = center ? "center" : "axis";
    const auto s = axisSurface(c, fi);
    if (!s) return badRefs("curveType");
    const double an = std::abs(s->a.dot(c.vp.n));
    RefGeom g;
    g.faceEntry = fi;
    if (center) {
        if (1 - an > AXIS_PARALLEL_TOL) return badRefs(err);
        g.kind = RefGeom::Kind::Point;
        g.p1 = g.p2 = s->A;
        g.a = c.vp.toView(s->A);
        g.depth = g.depthMin = s->A.dot(c.vp.n);
        return g;
    }
    if (an > AXIS_PARALLEL_TOL) return badRefs(err);
    g.kind = RefGeom::Kind::Segment;
    g.p1 = s->A + s->t0 * s->a;
    g.p2 = s->A + s->t1 * s->a;
    g.a = c.vp.toView(g.p1);
    g.b = c.vp.toView(g.p2);
    g.dir3 = s->a;
    g.depth = g.depthMin = s->A.dot(c.vp.n);
    return g;
}

Result<RefGeom> refGeom(const Ctx& c, const doc::DimensionRef& r) {
    if (r.kind == "construction") return constructionRefGeom(c, r.name);  // a construction of the view (§6.29, FX14-DRW-006)
    const bool vertex = r.kind == "vertex", axis = r.kind == "axis" || r.kind == "center";
    const bool face = r.kind == "silhouette" || r.kind == "cut" || axis;
    const auto kind = vertex ? naming::EntityKind::Vertex : face ? naming::EntityKind::Face : naming::EntityKind::Edge;
    const auto rr = naming::resolve(r.name, kind, c.st.names);
    switch (naming::refFate(rr, false)) {
        case naming::RefFate::Resolved: break;
        case naming::RefFate::Ambiguous: return Error("DRAWING_DIM_REF_AMBIGUOUS").with("name", r.name);
        case naming::RefFate::Lost: return Error("DRAWING_DIM_REF_LOST").with("name", r.name);
    }
    const int idx = rr.entities.front().index;
    Result<RefGeom> g = Error("DRAWING_DIM_REF_LOST");
    if (axis) g = r.side != 0 ? Result<RefGeom>(badRefs(r.kind.c_str())) : axisGeom(c, idx, r.kind == "center");
    else if (face) g = faceLineGeom(c, idx, r.kind == "cut", r.side);
    else if (!vertex) g = edgeGeom(c, idx);
    if (!vertex) {
        if (g) g->arc = r.arc;
        return g;
    }
    const auto& v = c.st.names.vertices.at(size_t(idx));
    const Point3 P = c.k.vertexPoint(c.st.bodies.at(size_t(v.body)).shape, v.vertex);
    RefGeom pg;
    pg.kind = RefGeom::Kind::Point;
    pg.a = c.vp.toView(P);
    pg.p1 = pg.p2 = P;
    pg.depth = pg.depthMin = P.dot(c.vp.n);
    pg.arc = r.arc;
    return pg;
}

Result<Point3> curveExtreme(const Ctx& c, const RefGeom& g, const Vec3& D) {
    auto p = c.k.extremePoint(c.st.bodies.at(size_t(g.body)).shape, g.set, D);
    if (!p) return badRefs(g.set.face >= 0 ? "silhouette" : "curveType");
    return *p;
}

Result<std::vector<std::vector<Point3>>> curvePolylines(const Ctx& c, const RefGeom& g) {
    auto p = c.k.curvePoints(c.st.bodies.at(size_t(g.body)).shape, g.set, DIM_CURVE_DEFLECTION);
    if (!p) return badRefs(g.set.face >= 0 ? "silhouette" : "curveType");
    return p;
}

const SketchFits* Ctx::sketchFits() const {
    if (!annotated || !md || !fits) return nullptr;
    const bool any = std::any_of(annotated->annotations.begin(), annotated->annotations.end(), [](const doc::Json& a) {
        return a.value("type", std::string()) == "fit" && a.contains("dimension");
    });
    if (!any) return nullptr;
    auto it = fits->find(annotated->id);
    if (it == fits->end()) it = fits->emplace(annotated->id, std::make_shared<const SketchFits>(dimdetail::sketchFits(*md, *annotated, st, k))).first;
    return it->second.get();
}

}  // namespace dimdetail

namespace {

// ---------------------------------------------------------------- primitives (§6а.5)

struct Out {
    std::string role;
    std::vector<Primitive> prims;
    double arrowAngleDeg = DIM_ARROW_ANGLE_DEG;
    void line(const Point2& a, const Point2& b) {
        Primitive p;
        p.kind = PrimitiveKind::Line;
        p.points = {a, b};
        p.role = role;
        prims.push_back(std::move(p));
    }
    void arc(const Point2& c, double r, double a0, double a1) {
        Primitive p;
        p.kind = PrimitiveKind::Arc;
        p.center = c;
        p.radius = r;
        p.start = a0;
        p.end = a1;
        p.role = role;
        prims.push_back(std::move(p));
    }
    // Arrow: tip at tip, tail in direction b (unit), §6а.4.
    void arrow(const Point2& tip, const Point2& b) {
        const double h = DIM_ARROW_LEN * std::tan(arrowAngleDeg * PI / 180 / 2);
        Primitive p;
        p.kind = PrimitiveKind::Polyline;
        p.points = {tip, tip + DIM_ARROW_LEN * b + h * geom2::perp(b), tip + DIM_ARROW_LEN * b - h * geom2::perp(b)};
        p.closed = true;
        p.filled = true;
        p.role = role;
        prims.push_back(std::move(p));
    }
    void text(const std::string& t, const Point2& at, double angle, const char* align = "center bottom") {
        Primitive p;
        p.kind = PrimitiveKind::Text;
        p.position = at;
        p.height = DIM_TEXT_HEIGHT;
        p.angle = angle;
        p.text = t;
        p.align = align;
        p.role = role;
        prims.push_back(std::move(p));
    }
    // Number on the leader shelf (§6а.4): leader from from in direction dir of length len (with an arrow at from — arrow),
    // horizontal shelf toward dir.x, number above the shelf.
    void shelf(const Point2& from, const Point2& dir, double len, const std::string& t, double tw, bool withArrow) {
        if (withArrow) arrow(from, dir);
        const Point2 E = from + len * dir;
        line(from, E);
        const double sg = dir.x() >= 0 ? 1.0 : -1.0;
        const double W = tw + 2 * DIM_SHELF_PAD;
        line(E, E + Point2(sg * W, 0));
        const double x0 = sg > 0 ? E.x() : E.x() - W;
        text(t, Point2(x0 + DIM_SHELF_PAD, E.y() + DIM_TEXT_GAP), 0, "left bottom");
    }
};

Point2 textNormal(double readableAngle) { return Point2(-std::sin(readableAngle), std::cos(readableAngle)); }

double widthOf(const std::string& t) {
    const auto tw = textWidth(t, DIM_TEXT_HEIGHT);
    return tw ? *tw : 0;
}

// Dimension text (§6а.3); modelSuffix — tolerance from the model (the suffix field overrides it).
std::string composeText(const doc::SheetDimension& a, const std::string& value, const std::string& modelSuffix = {},
                        bool diameterLike = false) {
    const std::string prefix = a.prefix                              ? *a.prefix
                               : a.kind == "diameter" || diameterLike ? "\xC3\x98"
                               : a.kind == "radius"                   ? "R"
                               : a.kind == "arcLength"                ? "\xE2\x8C\x92"  // «⌒» (FX2-DRW-001)
                                                                      : "";
    return prefix + (a.text ? *a.text : value) + (a.suffix ? *a.suffix : modelSuffix);
}

// Tolerance from the model (§6а.3, FX3-DRW-001): fit designation on a cylindrical face (faces, radius — for a diameter dimension),
// then the tolerance of the sketch dimension matched to this dimension (kind and reference set). value — sketch dimension nominal.
struct ModelTol {
    std::string text;
    bool found = false;
};
ModelTol modelTolerance(const Ctx& c, const doc::SheetDimension& a, const std::vector<int>& faces, double radius, double value) {
    if (a.fit) return {detail::toleranceText(*a.fit, value, a.tolerance), true};  // set in the drawing (FX9-DRW-009)
    if (!c.annotated) return {};
    // Feature dimension placed from the model (FX12-PMI-007): the feature's tolerance of this parameter.
    if (a.source && a.source->is_object() && a.source->contains("feature") && a.source->contains("param"))
        if (const auto f = c.annotated->find((*a.source)["feature"].get<std::string>()); f && !f->suppressed)
            if (const doc::Json* t = doc::featureTolerance(*f, (*a.source)["param"].get<std::string>()))
                return {detail::toleranceText(*t, value, a.tolerance), true};
    if (!faces.empty())
        if (auto mf = detail::faceFit(*c.annotated, c.k, c.st, faces, radius)) return {detail::toleranceText(*mf->fit, mf->nominal, a.tolerance), true};
    if (const SketchFits* sf = c.sketchFits())
        for (const auto& f : *sf)
            for (const auto& form : f.forms)
                if (sameRefs(form, a)) return {detail::toleranceText(*f.fit, value, a.tolerance), true};
    return {};
}

std::vector<int> edgeFaces(const Ctx& c, int edgeEntry) {
    if (edgeEntry < 0) return {};
    const auto& e = c.st.names.edges.at(size_t(edgeEntry));
    return {e.f1, e.f2};
}

// Dimension in the face plane on an isometric view (§6а.4): direction of extension lines on the sheet — projection of the direction
// in the plane of the adjacent planar face of the edge, perpendicular to the edge and pointing outward from the face; of two faces —
// the one facing the viewer more. Projection shorter than FACE_PLANE_MIN or nearly along the edge — nullopt (dimension in sheet plane).
constexpr double FACE_PLANE_MIN = 0.2;
std::optional<Point2> facePlaneDir(const Ctx& c, const RefGeom& g) {
    const auto& e = c.st.names.edges.at(size_t(g.edgeEntry));
    const Point2 dd = (g.b - g.a).normalized();
    double best = -1;
    Point2 out(0, 0);
    for (int fi : {e.f1, e.f2}) {
        const auto& fe = c.st.names.faces.at(size_t(fi));
        const kernel::Shape& shape = c.st.bodies.at(size_t(fe.body)).shape;
        const kernel::SurfaceInfo si = c.k.faceSurface(shape, fe.face);
        if (si.kind != kernel::SurfaceInfo::Kind::Plane) continue;
        const Vec3 N = si.dir.normalized();
        Vec3 E = N.cross(g.dir3);
        if (E.norm() < 1e-9) continue;
        E.normalize();
        if ((((g.p1 + g.p2) / 2) - c.k.faceCentroid(shape, fe.face)).dot(E) < 0) E = -E;
        const Point2 e2(E.dot(c.vp.r), E.dot(c.vp.u));
        if (e2.norm() < FACE_PLANE_MIN || std::abs(cross2(dd, e2.normalized())) < FACE_PLANE_MIN) continue;
        const double score = std::abs(N.dot(c.vp.n));
        if (score > best) {
            best = score;
            out = e2.normalized();
        }
    }
    if (best < 0) return std::nullopt;
    return out;
}

void fillLayout(DimensionPreview& res, const Out& out, const Point2& d, const Point2& m, double hmax, double hmin) {
    auto& L = res.layout;
    L.valid = true;
    L.d = d;
    L.m = m;
    L.hmax = hmax;
    L.hmin = hmin;
    L.lo = 1e300;
    L.hi = -1e300;
    auto take = [&](const Point2& p) {
        L.lo = std::min(L.lo, p.dot(d));
        L.hi = std::max(L.hi, p.dot(d));
    };
    for (const auto& p : out.prims) {
        for (const auto& q : p.points) take(q);
        if (p.kind == PrimitiveKind::Text) {
            const double w = widthOf(p.text);
            const Point2 x = dirOf(p.angle);
            const Point2 c0 = std::string(p.align).rfind("left", 0) == 0 ? p.position : p.position - w / 2 * x;
            take(c0);
            take(c0 + w * x);
        }
    }
}

// Linear dimension (§6а.4).
Status linear(const Ctx& c, doc::SheetDimension& a, const std::vector<RefGeom>& g, const std::optional<Point2>& cursor, Out& out,
              DimensionPreview& res) {
    const bool one = g.size() == 1;
    if (one && g[0].kind != RefGeom::Kind::Segment) return badRefs("refs");
    bool anyCurve = false;
    for (const auto& r : g) {
        if (!r.arc.empty() && r.kind != RefGeom::Kind::Circle && r.kind != RefGeom::Kind::Arc && r.kind != RefGeom::Kind::Curve) return badRefs("refs");
        anyCurve = anyCurve || r.kind == RefGeom::Kind::Curve;
    }
    const RefGeom* line = nullptr;
    for (const auto& r : g)
        if (r.kind == RefGeom::Kind::Segment) {
            line = &r;
            break;
        }
    if (cursor && a.orientation.empty()) {  // §6а.7: type by objects and cursor position
        const Point2 dl = line ? Point2(line->b - line->a) : Point2(0, 0);
        if (line && !one) {
            a.orientation = std::abs(dl.y()) <= DIM_EPS ? "vertical" : std::abs(dl.x()) <= DIM_EPS ? "horizontal" : "aligned";
        } else if (line && std::abs(dl.y()) <= DIM_EPS) {
            a.orientation = "horizontal";
        } else if (line && std::abs(dl.x()) <= DIM_EPS) {
            a.orientation = "vertical";
        } else {
            Eigen::AlignedBox2d box;
            for (const auto& r : g) {
                if (r.kind == RefGeom::Kind::Curve) {  // vertices of its polylines (FX7-DRW-010)
                    auto poly = curvePolylines(c, r);
                    if (!poly) return poly.error();
                    for (const auto& pl : *poly)
                        for (const auto& P : pl) box.extend(c.T(c.vp.toView(P)));
                } else if (r.pointLike()) {
                    box.extend(c.T(r.point()));
                } else {
                    box.extend(c.T(r.a)), box.extend(c.T(r.b));
                }
            }
            const bool inX = cursor->x() >= box.min().x() && cursor->x() <= box.max().x();
            const bool inY = cursor->y() >= box.min().y() && cursor->y() <= box.max().y();
            a.orientation = inX && !inY ? "horizontal" : inY && !inX ? "vertical" : "aligned";
            // A curve without a segment does not define an aligned direction (§6а.4 item 1): by the cursor.
            if (anyCurve && !line && a.orientation == "aligned") a.orientation = inY ? "vertical" : "horizontal";
        }
    }
    // Dimension in the face plane (isometric, §6а.4): extension lines — along the direction in the face plane.
    std::optional<Point2> skew;
    if (c.facePlane && one && a.orientation == "aligned" && !g[0].edgeOn && g[0].edgeEntry >= 0) skew = facePlaneDir(c, g[0]);
    // Curve points (FX7-DRW-010): extremes X⁺, X⁻ along the measurement direction and the middle M of their extent.
    struct CurvePts {
        Point2 plus{0, 0}, minus{0, 0}, mid{0, 0};
    };
    std::vector<CurvePts> cp(g.size());
    auto curvePts = [&](const Point2& dv) -> Status {
        const Vec3 D = dv.x() * c.vp.r + dv.y() * c.vp.u;
        for (size_t i = 0; i < g.size(); ++i) {
            if (g[i].kind != RefGeom::Kind::Curve) continue;
            auto xp = curveExtreme(c, g[i], D), xm = curveExtreme(c, g[i], -D);
            if (!xp) return xp.error();
            if (!xm) return xm.error();
            cp[i].plus = c.vp.toView(*xp);
            cp[i].minus = c.vp.toView(*xm);
            cp[i].mid = (cp[i].plus + cp[i].minus) / 2;
        }
        return okStatus();
    };
    // Measurement direction.
    Point2 d(1, 0);
    if (a.orientation == "vertical") {
        d = Point2(0, 1);
    } else if ((a.orientation == "aligned" || a.orientation.empty()) && anyCurve) {
        // A curve and a segment — perpendicular to the segment, toward the curve; otherwise not defined (§6а.4 item 1).
        if (one || !line || g[0].kind == g[1].kind) return badRefs("refs");
        const size_t ci = g[0].kind == RefGeom::Kind::Curve ? 0 : 1;
        d = geom2::perp((line->b - line->a).normalized());
        if (auto st = curvePts(d); !st) return st.error();
        const double s = (cp[ci].mid - line->a).dot(d);
        if (std::abs(s) <= DIM_EPS) return badRefs("zero");
        if (s < 0) d = -d;
    } else if (a.orientation == "aligned" || a.orientation.empty()) {
        if (one) {
            d = (g[0].b - g[0].a).normalized();
        } else if (g[0].pointLike() && g[1].pointLike()) {
            const Point2 v = g[1].point() - g[0].point();
            if (v.norm() <= DIM_EPS) return badRefs("zero");
            d = v.normalized();
        } else if (g[0].pointLike() != g[1].pointLike()) {
            const RefGeom& L = g[0].pointLike() ? g[1] : g[0];
            const Point2 Q = g[0].pointLike() ? g[0].point() : g[1].point();
            d = geom2::perp((L.b - L.a).normalized());
            const double s = (Q - L.a).dot(d);
            if (std::abs(s) <= DIM_EPS) return badRefs("zero");
            if (s < 0) d = -d;
        } else {
            const Point2 t1 = (g[0].b - g[0].a).normalized(), t2 = (g[1].b - g[1].a).normalized();
            if (std::abs(cross2(t1, t2)) > DIM_PARALLEL_TOL) return badRefs("notParallel");
            d = geom2::perp(t1);
            const double s = (g[1].a - g[0].a).dot(d);
            if (std::abs(s) <= DIM_EPS) return badRefs("zero");
            if (s < 0) d = -d;
        }
    }
    if (anyCurve)
        if (auto st = curvePts(d); !st) return st.error();
    // Points of point references (view): circle center; for arc — its point nearest (near) or farthest (far) from the other
    // reference along the measurement direction (§6а.4, FX3-DRW-004); for a curve — its extreme point nearest (near) or
    // farthest (far, no field) from the other reference (FX7-DRW-010).
    auto refPoint = [&](size_t i) -> Point2 {  // point of the other reference for near / far
        const RefGeom& o = g[i];
        return o.kind == RefGeom::Kind::Curve ? cp[i].mid : o.pointLike() ? o.point() : Point2((o.a + o.b) / 2);
    };
    Point2 pt[2] = {Point2(0, 0), Point2(0, 0)};
    for (size_t i = 0; i < g.size(); ++i) {
        if (g[i].kind == RefGeom::Kind::Curve) {
            double sg = (refPoint(1 - i) - cp[i].mid).dot(d) >= 0 ? 1.0 : -1.0;
            if (g[i].arc != "near") sg = -sg;
            pt[i] = sg > 0 ? cp[i].plus : cp[i].minus;
            continue;
        }
        if (!g[i].pointLike()) continue;
        pt[i] = g[i].point();
        if (g[i].arc.empty() || one) continue;
        double sg = (refPoint(1 - i) - g[i].center).dot(d) >= 0 ? 1.0 : -1.0;
        if (g[i].arc == "far") sg = -sg;
        pt[i] = g[i].center + sg * g[i].radius * d;
    }
    const Point2 m = skew ? *skew : geom2::perp(d);
    const double den = cross2(d, m);
    auto beta = [&](const Point2& p) { return cross2(d, p) / den; };  // coordinate along m in the basis (d, m)
    std::vector<Point2> cand;
    for (size_t i = 0; i < g.size(); ++i) {
        if (g[i].kind != RefGeom::Kind::Segment) cand.push_back(c.T(pt[i]));
        else cand.push_back(c.T(g[i].a)), cand.push_back(c.T(g[i].b));
    }
    double hmax = -1e300, hmin = 1e300;
    for (const auto& q : cand) hmax = std::max(hmax, beta(q)), hmin = std::min(hmin, beta(q));
    if (cursor) {
        const double pm = beta(*cursor);
        a.offset = pm >= (hmax + hmin) / 2 ? std::max(pm - hmax, DIM_MIN_OFFSET) : std::min(pm - hmin, -DIM_MIN_OFFSET);
    }
    const double h = a.offset >= 0 ? hmax + a.offset : hmin + a.offset;
    // Anchor points (view).
    Point2 q[2];
    if (one) {
        q[0] = g[0].a;
        q[1] = g[0].b;
    } else {
        for (int i = 0; i < 2; ++i) {
            if (g[i].kind != RefGeom::Kind::Segment) {
                q[i] = pt[i];
            } else {
                const double da = std::abs(beta(c.T(g[i].a)) - h), db = std::abs(beta(c.T(g[i].b)) - h);
                q[i] = da <= db ? g[i].a : g[i].b;
            }
        }
    }
    res.value = skew ? (g[0].p2 - g[0].p1).norm() : std::abs((q[1] - q[0]).dot(d));
    if (res.value <= DIM_EPS) return badRefs("zero");
    // Diameter: one edge-on circle measured along itself, or silhouettes (cuts through the axis) of one cylinder on both
    // sides measured across — the "Ø" sign and the face tolerance (§6а.3).
    bool diaLike = false;
    std::vector<int> tolFaces;
    double tolRadius = 0;
    if (one && g[0].edgeOn && std::abs(std::abs((g[0].b - g[0].a).normalized().dot(d)) - 1) <= DIM_PARALLEL_TOL) {
        diaLike = true;
        tolFaces = edgeFaces(c, g[0].edgeEntry);
        tolRadius = g[0].edgeOnRadius;
    } else if (!one && g[0].faceEntry >= 0 && g[0].faceEntry == g[1].faceEntry && g[0].side == -g[1].side && g[0].side != 0 &&
               g[0].cylRadius > 0 && std::abs(g[0].halfWidth - g[0].cylRadius) <= DIM_EPS &&
               std::abs((g[0].b - g[0].a).normalized().dot(d)) <= DIM_PARALLEL_TOL) {
        diaLike = true;
        tolFaces = {g[0].faceEntry};
        tolRadius = g[0].cylRadius;
    } else if (!one && g[0].kind == RefGeom::Kind::Curve && g[1].kind == RefGeom::Kind::Curve && g[0].faceEntry >= 0 &&
               g[0].faceEntry == g[1].faceEntry && g[0].side == -g[1].side && g[0].side != 0 && g[0].arc != "near" &&
               g[1].arc != "near") {
        // Silhouette curves of one surface of revolution on both sides, measured across the axis — the greatest diameter
        // (FX7-DRW-010).
        const Point2 ax(g[0].axis3.dot(c.vp.r), g[0].axis3.dot(c.vp.u));
        if (ax.norm() > 0 && std::abs(ax.normalized().dot(d)) <= DIM_PARALLEL_TOL) {
            diaLike = true;
            tolFaces = {g[0].faceEntry};
            tolRadius = res.value / 2;
        }
    }
    const ModelTol tol = modelTolerance(c, a, tolFaces, tolRadius, res.value);
    res.modelTolerance = tol.found;
    res.text = composeText(a, formatDimensionLength(res.value), tol.text, diaLike);
    // Extension lines (for ISO 129 — with a gap from the outline), dimension line, arrows, text.
    Point2 av[2], e[2];
    for (int i = 0; i < 2; ++i) {
        av[i] = c.T(q[i]);
        e[i] = av[i] + (h - beta(av[i])) * m;
    }
    for (int i = 0; i < (a.brk ? 1 : 2); ++i) {
        const double gap = h - beta(av[i]);
        if (std::abs(gap) <= DIM_EPS) continue;
        const double sg = gap > 0 ? 1.0 : -1.0;
        const double start = c.iso ? DIM_ISO_EXT_GAP : 0.0;
        if (std::abs(gap) <= start) continue;
        out.line(av[i] + start * sg * m, e[i] + DIM_EXT_OVERSHOOT * sg * m);
    }
    const double L = (e[1] - e[0]).norm();
    const Point2 w = L > 0 ? Point2((e[1] - e[0]) / L) : d;
    const double ta = readable(angleOf(w));
    const Point2 tn = textNormal(ta), wr = dirOf(ta);
    const double tw = widthOf(res.text);
    if (a.brk) {  // break (GOST 2.307): from the first extension line past the middle (symmetry axis), one arrow
        const Point2 end = (e[0] + e[1]) / 2 + DIM_BREAK_OVERRUN * w;
        out.line(e[0], end);
        out.arrow(e[0], w);
        out.text(res.text, (e[0] + end) / 2 + DIM_TEXT_GAP * tn, ta);
        fillLayout(res, out, d, m, hmax, hmin);
        return okStatus();
    }
    const bool inside = L >= 2 * DIM_ARROW_LEN + DIM_ARROW_GAP;
    const double room = inside ? L - 2 * DIM_ARROW_LEN : L;
    const bool fits = tw + 2 * DIM_TEXT_PAD <= room;
    if (inside) {
        out.line(e[0], e[1]);
        out.arrow(e[0], w);
        out.arrow(e[1], -w);
    } else {
        const double ext = DIM_ARROW_LEN + DIM_EXT_OVERSHOOT;
        out.line(e[0] - ext * w, e[1] + ext * w);
        out.arrow(e[0], -w);
        out.arrow(e[1], w);
    }
    if (fits) {
        out.text(res.text, (e[0] + e[1]) / 2 + DIM_TEXT_GAP * tn, ta);
    } else if (inside) {
        // The number does not fit between the arrows — on the extension of the dimension line beyond the extension line in reading direction.
        const bool forward = wr.dot(w) >= 0;
        const Point2 eEnd = forward ? e[1] : e[0], dirOut = forward ? w : Point2(-w);
        out.line(eEnd, eEnd + (2 * DIM_TEXT_SHIFT_GAP + tw) * dirOut);
        out.text(res.text, eEnd + (DIM_TEXT_SHIFT_GAP + tw / 2) * dirOut + DIM_TEXT_GAP * tn, ta);
    } else {
        // Neither arrows nor number fit — number on the leader shelf from the middle of the dimension line.
        out.shelf((e[0] + e[1]) / 2, Point2((wr + tn).normalized()), DIM_SHELF_LEADER_MM, res.text, tw, false);
    }
    fillLayout(res, out, d, m, hmax, hmin);
    return okStatus();
}

// Diameter and radius (§6а.4).
Status circular(const Ctx& c, doc::SheetDimension& a, const RefGeom& g, const std::optional<Point2>& cursor, Out& out, DimensionPreview& res) {
    if (g.kind != RefGeom::Kind::Circle && g.kind != RefGeom::Kind::Arc) return badRefs("refs");
    if (!g.arc.empty()) return badRefs("refs");
    const Point2 C = c.T(g.center);
    const double R = c.vp.scale * g.radius;
    if (cursor && (*cursor - C).norm() > 0) {
        a.angle = angleOf(*cursor - C);
        a.offset = (*cursor - C).norm();
    }
    const bool dia = a.kind == "diameter";
    res.value = dia ? 2 * g.radius : g.radius;
    const ModelTol tol = modelTolerance(c, a, dia ? edgeFaces(c, g.edgeEntry) : std::vector<int>{}, g.radius, res.value);
    res.modelTolerance = tol.found;
    res.text = composeText(a, formatDimensionLength(res.value), tol.text);
    const double wt = widthOf(res.text);
    const Point2 u = dirOf(a.angle);
    const double s = a.offset;
    const bool brk = dia && a.brk;  // break: arrow on the circle, the line runs past the center (GOST 2.307)
    out.line(C + std::min(brk ? -DIM_BREAK_OVERRUN : dia ? -R : 0.0, s - wt / 2) * u, C + std::max(R, s + wt / 2) * u);
    out.arrow(C + R * u, -u);
    if (dia && !brk) out.arrow(C - R * u, u);
    const double ta = readable(a.angle);
    out.text(res.text, C + s * u + DIM_TEXT_GAP * textNormal(ta), ta);
    return okStatus();
}

// Dimension arc with radial extension lines from an arc of radius R (sheet) — central angle and arc length (§6а.4,
// FX2-DRW-001): center Os, endpoints a0, a0 + φ, dimension arc radius rho.
void arcDimension(Out& out, const Point2& Os, double R, double a0, double phi, double rho, const std::string& text) {
    const double a1 = a0 + phi;
    for (double ai : {a0, a1}) {
        if (rho > R + DIM_EPS) out.line(Os + R * dirOf(ai), Os + (rho + DIM_EXT_OVERSHOOT) * dirOf(ai));
        else if (rho < R - DIM_EPS) out.line(Os + R * dirOf(ai), Os + (rho - DIM_EXT_OVERSHOOT) * dirOf(ai));
    }
    out.arc(Os, rho, a0, a1);
    const double inward = rho * phi >= 2 * DIM_ARROW_LEN + DIM_ARROW_GAP ? 1.0 : -1.0;
    out.arrow(Os + rho * dirOf(a0), inward * Point2(-std::sin(a0), std::cos(a0)));
    out.arrow(Os + rho * dirOf(a1), inward * Point2(std::sin(a1), -std::cos(a1)));
    const double am = a0 + phi / 2;
    const double ta = readable(am - PI / 2);
    out.text(text, Os + rho * dirOf(am) + DIM_TEXT_GAP * textNormal(ta), ta);
}

// Arc length (§6а.4, FX2-DRW-001).
Status arcLength(const Ctx& c, doc::SheetDimension& a, const std::vector<RefGeom>& g, const std::optional<Point2>& cursor, Out& out,
                 DimensionPreview& res) {
    if (g.size() != 1 || g[0].kind != RefGeom::Kind::Arc || !g[0].arc.empty()) return badRefs("refs");
    const Point2 Os = c.T(g[0].center);
    const double R = c.vp.scale * g[0].radius;
    if (cursor) a.offset = (*cursor - Os).norm() - R;
    const double phi = sweepOf(g[0].start, g[0].end);
    res.value = g[0].radius * phi;
    const ModelTol tol = modelTolerance(c, a, {}, 0, res.value);
    res.modelTolerance = tol.found;
    res.text = composeText(a, formatDimensionLength(res.value), tol.text);
    arcDimension(out, Os, R, g[0].start, phi, std::max(R + a.offset, DIM_MIN_OFFSET), res.text);
    return okStatus();
}

// Angular dimension (§6а.4).
Status angular(const Ctx& c, doc::SheetDimension& a, const std::vector<RefGeom>& g, const std::optional<Point2>& cursor, Out& out,
               DimensionPreview& res) {
    for (const auto& r : g)
        if (!r.arc.empty()) return badRefs("refs");
    if (g.size() == 1) {  // central angle of an arc (FX2-DRW-001)
        if (g[0].kind != RefGeom::Kind::Arc) return badRefs("refs");
        const Point2 Os = c.T(g[0].center);
        if (cursor && (*cursor - Os).norm() > 0) {
            a.angle = angleOf(*cursor - Os);
            a.offset = std::max((*cursor - Os).norm(), DIM_MIN_OFFSET);
        }
        const double phi = sweepOf(g[0].start, g[0].end);
        res.value = phi * 180 / PI;
        const ModelTol tol = modelTolerance(c, a, {}, 0, res.value);
        res.modelTolerance = tol.found;
        res.text = composeText(a, formatDimensionAngle(res.value), tol.text);
        arcDimension(out, Os, c.vp.scale * g[0].radius, g[0].start, phi, a.offset, res.text);
        return okStatus();
    }
    if (g.size() != 2 || g[0].kind != RefGeom::Kind::Segment || g[1].kind != RefGeom::Kind::Segment) return badRefs("refs");
    const Point2 t1 = (g[0].b - g[0].a).normalized(), t2 = (g[1].b - g[1].a).normalized();
    const double cr = cross2(t1, t2);
    if (std::abs(cr) <= DIM_PARALLEL_TOL) return badRefs("parallel");
    const Point2 O = g[0].a + cross2(g[1].a - g[0].a, t2) / cr * t1;
    const Point2 Os = c.T(O);
    if (cursor && (*cursor - Os).norm() > 0) {
        a.angle = angleOf(*cursor - Os);
        a.offset = std::max((*cursor - Os).norm(), DIM_MIN_OFFSET);
    }
    const Point2 u = dirOf(a.angle);
    const double l1 = cross2(u, t2) / cr, l2 = cross2(t1, u) / cr;
    const double s1g = l1 >= 0 ? 1 : -1, s2g = l2 >= 0 ? 1 : -1;
    const Point2 s1 = s1g * t1, s2 = s2g * t2;
    const double dot3 = std::clamp((s1g * g[0].dir3).dot(s2g * g[1].dir3), -1.0, 1.0);
    res.value = std::acos(dot3) * 180 / PI;
    const ModelTol tol = modelTolerance(c, a, {}, 0, res.value);
    res.modelTolerance = tol.found;
    res.text = composeText(a, formatDimensionAngle(res.value), tol.text);
    double a0 = angleOf(s1), a1 = angleOf(s2);
    if (cross2(s1, s2) <= 0) std::swap(a0, a1);
    const double phi = sweepOf(a0, a1);
    a1 = a0 + phi;
    const double rho = a.offset;
    // Extension lines: ray 1, then 2.
    for (int i = 0; i < 2; ++i) {
        const Point2 si = i == 0 ? s1 : s2;
        const double ta = (c.T(g[i].a) - Os).dot(si), tb = (c.T(g[i].b) - Os).dot(si);
        const double tmin = std::min(ta, tb), tmax = std::max(ta, tb);
        if (rho > tmax + DIM_EPS) out.line(Os + tmax * si, Os + (rho + DIM_EXT_OVERSHOOT) * si);
        else if (rho < tmin - DIM_EPS) out.line(Os + tmin * si, Os + (rho - DIM_EXT_OVERSHOOT) * si);
    }
    out.arc(Os, rho, a0, a1);
    const double inward = rho * phi >= 2 * DIM_ARROW_LEN + DIM_ARROW_GAP ? 1.0 : -1.0;
    out.arrow(Os + rho * dirOf(a0), inward * Point2(-std::sin(a0), std::cos(a0)));
    out.arrow(Os + rho * dirOf(a1), inward * Point2(std::sin(a1), -std::cos(a1)));
    const double am = a0 + phi / 2;
    const double ta = readable(am - PI / 2);
    out.text(res.text, Os + rho * dirOf(am) + DIM_TEXT_GAP * textNormal(ta), ta);
    return okStatus();
}

// Path length (§6а.4, FX3-DRW-004): sum of edge lengths, number on the leader shelf from the first edge.
Status pathLength(const Ctx& c, doc::SheetDimension& a, const std::optional<Point2>& cursor, Out& out, DimensionPreview& res) {
    double total = 0;
    int first = -1;
    for (const auto& r : a.refs) {
        if (r.kind != "edge" || !r.arc.empty()) return badRefs("refs");
        const auto rr = naming::resolve(r.name, naming::EntityKind::Edge, c.st.names);
        if (const auto fate = naming::refFate(rr, false); fate != naming::RefFate::Resolved)
            return Error(fate == naming::RefFate::Lost ? "DRAWING_DIM_REF_LOST" : "DRAWING_DIM_REF_AMBIGUOUS").with("name", r.name);
        const int idx = rr.entities.front().index;
        if (first < 0) first = idx;
        total += c.st.names.edges.at(size_t(idx)).measure;
    }
    if (first < 0) return badRefs("refs");
    auto g = edgeGeom(c, first);
    if (!g) return g.error();
    Point2 S;
    switch (g->kind) {
        case RefGeom::Kind::Segment: S = c.T((g->a + g->b) / 2); break;
        case RefGeom::Kind::Arc: S = c.T(g->center) + c.vp.scale * g->radius * dirOf(g->start + sweepOf(g->start, g->end) / 2); break;
        case RefGeom::Kind::Circle: S = c.T(g->center) + c.vp.scale * g->radius * dirOf(MODEL_DIM_CIRCLE_ANGLE); break;
        case RefGeom::Kind::Curve: {  // the middle vertex of its first polyline (FX7-DRW-010)
            auto poly = curvePolylines(c, *g);
            if (!poly) return poly.error();
            const auto& pl = poly->front();
            S = c.T(c.vp.toView(pl[pl.size() / 2]));
            break;
        }
        case RefGeom::Kind::Point: return badRefs("refs");
    }
    if (cursor && (*cursor - S).norm() > 0) {
        a.angle = angleOf(*cursor - S);
        a.offset = (*cursor - S).norm();
    }
    res.value = total;
    const ModelTol tol = modelTolerance(c, a, {}, 0, res.value);
    res.modelTolerance = tol.found;
    res.text = composeText(a, formatDimensionLength(res.value), tol.text);
    out.shelf(S, dirOf(a.angle), std::max(a.offset, DIM_MIN_OFFSET), res.text, widthOf(res.text), true);
    return okStatus();
}

// Clipping by the detail view circle (§6а.5): segments — by the circle, arrows with the tip outside the circle are not drawn.
void clipToCircle(std::vector<Primitive>& prims, const Point2& K, double R) {
    std::vector<Primitive> kept;
    for (auto& p : prims) {
        if (p.kind == PrimitiveKind::Line && p.points.size() == 2) {
            const Point2 A = p.points[0], v = p.points[1] - p.points[0];
            const double qa = v.squaredNorm(), qb = 2 * v.dot(A - K), qc = (A - K).squaredNorm() - R * R;
            if (qa <= 0) {
                if (qc <= 0) kept.push_back(std::move(p));
                continue;
            }
            const double disc = qb * qb - 4 * qa * qc;
            if (disc < 0) continue;
            const double t0 = std::max(0.0, (-qb - std::sqrt(disc)) / (2 * qa)), t1 = std::min(1.0, (-qb + std::sqrt(disc)) / (2 * qa));
            if (t1 - t0 <= 1e-12) continue;
            p.points = {A + t0 * v, A + t1 * v};
            kept.push_back(std::move(p));
        } else if (p.kind == PrimitiveKind::Polyline && p.filled && !p.points.empty()) {
            if ((p.points[0] - K).norm() <= R + DIM_EPS) kept.push_back(std::move(p));
        } else {
            kept.push_back(std::move(p));
        }
    }
    prims = std::move(kept);
}

}  // namespace

namespace dimdetail {

Ctx makeCtx(const doc::Document& d, const doc::Element* drawing, const doc::Sheet& sheet, const std::string& viewId, const Setup& su,
            const kernel::Kernel& k, FitCache* fits) {
    Ctx c{su.vp, stateOrEmpty(su.state), k, detail::annotatedModel(d, *su.vp.base), &d, fits};
    c.sheet = &sheet;  // constructions of the sheet (§6.29, FX14-DRW-006)
    c.viewId = viewId;
    c.iso = drawing && drawingStandard(*drawing) == font::DrawingStandard::Iso;
    c.facePlane = su.vp.base->orientation == "iso";
    if (const int vi = sheet.viewIndex(viewId); vi >= 0) {
        const auto& v = sheet.views[size_t(vi)];
        if (v.kind == "detail") c.clip = std::pair{su.vp.toSheet(Point2(v.center[0], v.center[1])), v.radius * su.vp.scale};
    }
    return c;
}

}  // namespace dimdetail

namespace dimdetail {

// built — sheet views already built: the placement is taken from placements (§3) so that the dimension matches the view.
Result<Setup> setup(const doc::Sheet& sheet, const std::string& viewId, const ModelProvider& models, bool& viewError, const SheetGraphics* built) {
    viewError = false;
    if (sheet.viewIndex(viewId) < 0) return Error("DRAWING_DIM_VIEW_NOT_FOUND").with("view", viewId);
    auto vp = viewMapping(sheet, viewId, models);
    if (!vp) {
        viewError = true;
        return vp.error();
    }
    if (built)
        for (const auto& pl : built->placements)
            if (pl.view == viewId) vp->offset = pl.offset, vp->scale = pl.scale;
    auto st = viewModel(models, *vp->base);
    if (!st) {
        viewError = true;
        return st.error();
    }
    return Setup{*vp, *st};
}

const regen::PartState& stateOrEmpty(const regen::StatePtr& s) {
    static const regen::PartState empty;
    return s ? *s : empty;
}

Result<DimensionPreview> build(const doc::Document& d, const doc::Element* drawing, const doc::Sheet& sheet, const doc::SheetDimension& a0,
                               const std::optional<Point2>& cursor, const ModelProvider& models, bool& viewError, const SheetGraphics* built,
                               FitCache* fits) {
    auto su = setup(sheet, a0.view, models, viewError, built);
    if (!su) return su.error();
    FitCache local;
    const Ctx c = makeCtx(d, drawing, sheet, a0.view, *su, models.kernel(), fits ? fits : &local);
    DimensionPreview res;
    res.dimension = a0;
    doc::SheetDimension& a = res.dimension;
    Out out{"dim:" + a.id, {}};
    if (c.iso) out.arrowAngleDeg = DIM_ISO_ARROW_ANGLE_DEG;
    Status s = okStatus();
    if (a.kind == "pathLength") {
        s = pathLength(c, a, cursor, out, res);
    } else {
        std::vector<RefGeom> g;
        for (const auto& r : a.refs) {
            auto rg = refGeom(c, r);
            if (!rg) return rg.error();
            g.push_back(*rg);
        }
        if (a.kind == "linear") s = g.empty() || g.size() > 2 ? Status(badRefs("refs")) : linear(c, a, g, cursor, out, res);
        else if (a.kind == "diameter" || a.kind == "radius") s = g.size() != 1 ? Status(badRefs("refs")) : circular(c, a, g[0], cursor, out, res);
        else if (a.kind == "angle") s = angular(c, a, g, cursor, out, res);
        else if (a.kind == "arcLength") s = arcLength(c, a, g, cursor, out, res);
        else s = badRefs("refs");
    }
    if (!s) return s.error();
    if (c.clip) clipToCircle(out.prims, c.clip->first, c.clip->second);
    if (a.fit)
        for (auto& p : out.prims) p.local = true;  // a tolerance set in the drawing (FX9-DRW-009)
    res.primitives = std::move(out.prims);
    return res;
}

}  // namespace dimdetail

std::string formatDimensionLength(double mm) {
    return decimalComma(fixedTrimmed(mm, DIM_DECIMALS));
}

std::string formatDimensionAngle(double deg) {
    const long long m = std::llround(std::nearbyint(deg * 60));  // nearbyint — to even at exactly half
    const long long D = m / 60, M = m % 60;
    std::string s = std::to_string(D) + "\xC2\xB0";
    if (M != 0) s += std::to_string(M) + "'";
    return s;
}

void appendDimensions(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models, SheetGraphics& out) {
    FitCache fits;
    for (const auto& a : sheet.dimensions) {
        // View already built (§4 item 3): not computed yet — the dimension is pending too; with an error — dimension without its own error.
        if (std::find(out.pending.begin(), out.pending.end(), a.view) != out.pending.end()) {
            out.pending.push_back(a.id);
            continue;
        }
        if (std::any_of(out.errors.begin(), out.errors.end(), [&](const ViewError& e) { return e.view == a.view; })) continue;
        bool viewError = false;
        auto r = build(d, &drawing, sheet, a, std::nullopt, models, viewError, &out, &fits);
        if (!r) {
            if (r.error().code == VIEW_PENDING) out.pending.push_back(a.id);
            else if (!viewError) out.errors.push_back({a.id, r.error()});
            continue;
        }
        for (auto& p : r->primitives) out.primitives.push_back(std::move(p));
    }
}

Result<DimensionPreview> previewDimension(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const doc::SheetDimension& a,
                                          const std::optional<Point2>& cursor, const ModelProvider& models) {
    bool viewError = false;
    return build(d, &drawing, sheet, a, cursor, models, viewError);
}

namespace {

// Face whose line an edge is on the view (§6а.7, FX9-DRW-009): of the edge's faces the plane seen edge-on, else the first.
std::string edgeLineFace(const Ctx& c, const naming::EdgeEntry& e) {
    const auto& faces = c.st.names.faces;
    std::string first;
    for (const int fi : {e.f1, e.f2}) {
        if (fi < 0 || size_t(fi) >= faces.size()) continue;
        const auto& fe = faces[size_t(fi)];
        if (first.empty()) first = fe.full;
        const kernel::SurfaceInfo si = c.k.faceSurface(c.st.bodies.at(size_t(fe.body)).shape, fe.face);
        if (si.kind == kernel::SurfaceInfo::Kind::Plane && std::abs(si.dir.normalized().dot(c.vp.n)) <= DIM_PARALLEL_TOL) return fe.full;
    }
    return first;
}

}  // namespace

Result<std::vector<ViewEntity>> viewEntities(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const std::string& viewId,
                                             const ModelProvider& models) {
    bool viewError = false;
    auto su = setup(sheet, viewId, models, viewError);
    if (!su) return su.error();
    const Ctx c = makeCtx(d, &drawing, sheet, viewId, *su, models.kernel(), nullptr);
    std::vector<ViewEntity> out;
    const auto& names = c.st.names;
    // Section (§6.11): entities entirely in the removed part of the model are not offered.
    const double cutDepth = c.vp.cutOrigin ? c.vp.cutOrigin->dot(c.vp.n) : 1e300;
    // Detail view: entities entirely outside circle K are not offered (§6а.7).
    auto outside = [&](const ViewEntity& e) {
        if (!c.clip) return false;
        const Point2 K = c.clip->first;
        const double R = c.clip->second;
        switch (e.kind) {
            case ViewEntity::Kind::Point: return (e.a - K).norm() > R + DIM_EPS;
            case ViewEntity::Kind::Line: {
                const Point2 ab = e.b - e.a;
                const double t = ab.squaredNorm() > 0 ? std::clamp((K - e.a).dot(ab) / ab.squaredNorm(), 0.0, 1.0) : 0.0;
                return (e.a + t * ab - K).norm() > R + DIM_EPS;
            }
            case ViewEntity::Kind::Circle:
            case ViewEntity::Kind::Arc: {
                const double dc = (e.center - K).norm();
                return dc > R + e.radius + DIM_EPS || dc + R < e.radius - DIM_EPS;
            }
            case ViewEntity::Kind::Curve:
                for (const auto& pl : e.polylines)
                    for (const auto& q : pl)
                        if ((q - K).norm() <= R + DIM_EPS) return false;
                return true;
        }
        return false;
    };
    auto push = [&](ViewEntity e, const RefGeom& g) {
        e.depth = g.depth;
        switch (g.kind) {
            case RefGeom::Kind::Curve: {  // FX7-DRW-010: polylines on the sheet; depth — the greatest of their vertices
                auto poly = curvePolylines(c, g);
                if (!poly) return;
                e.kind = ViewEntity::Kind::Curve;
                double dmax = -1e300, dmin = 1e300;
                for (const auto& pl : *poly) {
                    auto& out2 = e.polylines.emplace_back();
                    for (const auto& P : pl) {
                        out2.push_back(c.T(c.vp.toView(P)));
                        dmax = std::max(dmax, P.dot(c.vp.n));
                        dmin = std::min(dmin, P.dot(c.vp.n));
                    }
                }
                if (dmin > cutDepth + DIM_EPS) return;
                e.depth = g.set.face >= 0 ? g.depth : dmax;
                break;
            }
            case RefGeom::Kind::Segment:
                e.kind = ViewEntity::Kind::Line;
                e.a = c.T(g.a);
                e.b = c.T(g.b);
                break;
            case RefGeom::Kind::Circle:
            case RefGeom::Kind::Arc:
                e.kind = g.kind == RefGeom::Kind::Circle ? ViewEntity::Kind::Circle : ViewEntity::Kind::Arc;
                e.center = c.T(g.center);
                e.radius = c.vp.scale * g.radius;
                e.start = g.start;
                e.end = g.end;
                break;
            case RefGeom::Kind::Point: break;
        }
        if (!outside(e)) out.push_back(std::move(e));
    };
    for (size_t i = 0; i < names.edges.size(); ++i) {
        auto g = edgeGeom(c, int(i));
        if (!g || g->depthMin > cutDepth + DIM_EPS) continue;
        ViewEntity e;
        e.ref = "edge";
        e.name = names.edges[i].full;
        e.face = edgeLineFace(c, names.edges[i]);
        push(std::move(e), *g);
    }
    // Cut lines and face silhouettes (FX3-DRW-002), in face table order; for a face, cut lines come before silhouettes
    // (on a section through the axis they coincide — the cut is chosen).
    for (size_t i = 0; i < names.faces.size(); ++i) {
        for (const bool cut : {true, false}) {
            if (cut && !c.vp.cutOrigin) continue;
            for (const int side : {1, -1, 0}) {
                auto g = faceLineGeom(c, int(i), cut, side);
                if (!g || g->depthMin > cutDepth + DIM_EPS) continue;
                ViewEntity e;
                e.ref = cut ? "cut" : "silhouette";
                e.name = e.face = names.faces[i].full;
                e.side = side;
                push(std::move(e), *g);
            }
        }
    }
    // Axes and centers of cylinder and cone surfaces (FX9-DRW-001): one per surface — by the name of its first face, without
    // fillets (angular span below CENTERLINE_MIN_TURN, as center lines §6.15 item 2).
    for (size_t i = 0; i < names.faces.size(); ++i) {
        const auto s = axisSurface(c, int(i));
        if (!s || s->first != int(i) || s->phi < CENTERLINE_MIN_TURN) continue;
        for (const bool center : {false, true}) {
            auto g = axisGeom(c, int(i), center);
            if (!g || g->depthMin > cutDepth + DIM_EPS) continue;
            ViewEntity e;
            e.ref = center ? "center" : "axis";
            e.name = e.face = names.faces[i].full;
            if (center) {
                e.kind = ViewEntity::Kind::Point;
                e.a = c.T(g->a);
                e.depth = g->depth;
                if (!outside(e)) out.push_back(std::move(e));
            } else {
                push(std::move(e), *g);
            }
        }
    }
    for (const auto& v : names.vertices) {
        const Point3 P = c.k.vertexPoint(c.st.bodies.at(size_t(v.body)).shape, v.vertex);
        ViewEntity e;
        e.ref = "vertex";
        e.name = v.full;
        e.kind = ViewEntity::Kind::Point;
        e.a = c.T(c.vp.toView(P));
        e.depth = P.dot(c.vp.n);
        if (e.depth > cutDepth + DIM_EPS || outside(e)) continue;
        out.push_back(std::move(e));
    }
    // Constructions of the view (§6.29, FX14-DRW-006): for dimensions on them; degenerate ones are not offered.
    for (const auto& cs : sheet.constructions) {
        if (cs.view != viewId) continue;
        auto sh = constructionShape(c, cs);
        if (!sh) continue;
        const RefGeom& g = sh->geom;
        ViewEntity e;
        e.ref = "construction";
        e.name = cs.id;
        e.kind = g.kind == RefGeom::Kind::Segment ? ViewEntity::Kind::Line : g.kind == RefGeom::Kind::Arc ? ViewEntity::Kind::Arc : ViewEntity::Kind::Circle;
        e.a = c.T(g.a), e.b = c.T(g.b);
        e.center = c.T(g.center);
        e.radius = g.radius * c.vp.scale;
        e.start = g.start, e.end = g.end;
        e.depth = 0;
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace construe::drawing
