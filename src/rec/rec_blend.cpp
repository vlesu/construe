// Own removal of fillet and chamfer steps by filling (spec/reconstruction.md §4.2а, FX10-REC-006): the corner each blend
// face cut off (or filled) is rebuilt as a solid of the corner region — extruded along a cylinder blend or a planar
// chamfer, revolved about a torus blend or a conical chamfer — and united with the body (convex blend) or cut from it
// (concave blend). The sharp body is accepted only when the kernel's own fillet or chamfer of its new edges gives the
// body back (§6), so a wrong fill never reaches the plan.
#include "rec/rec_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace construe::rec::detail {

namespace {

using SK = kernel::SurfaceInfo::Kind;
using CK = kernel::CurveInfo::Kind;

bool tracing() { return std::getenv("CONSTRUE_REC_TRACE") != nullptr; }

// Trace of a support surface in the section plane of a blend (§4.2а item 1): a line through p along d, or a circle.
struct Trace {
    bool circle = false;
    Point2 p = Point2::Zero(), d = Point2::UnitX();  // line
    Point2 c = Point2::Zero();                        // circle
    double R = 0;
};

// The point where the blend circle (center q) touches the trace: the foot of the perpendicular from q.
Point2 foot(const Trace& tr, const Point2& q) {
    if (!tr.circle) return tr.p + tr.d * (q - tr.p).dot(tr.d);
    const Point2 u = q - tr.c;
    return u.norm() < 1e-12 ? tr.c + Point2(tr.R, 0) : Point2(tr.c + tr.R * u.normalized());
}

double cross2(const Point2& a, const Point2& b) { return a.x() * b.y() - a.y() * b.x(); }

// Intersections of two traces; the one nearest to `near` (the corner of the tangents at the touch points). nullopt — none.
std::optional<Point2> corner(const Trace& a, const Trace& b, const Point2& near) {
    std::vector<Point2> pts;
    auto lineCircle = [&](const Trace& l, const Trace& c) {
        const Point2 f = l.p + l.d * (c.c - l.p).dot(l.d);
        const double h2 = c.R * c.R - (f - c.c).squaredNorm();
        if (h2 < -1e-9 * c.R * c.R) return;
        const double h = std::sqrt(std::max(0.0, h2));
        pts.push_back(f + h * l.d), pts.push_back(f - h * l.d);
    };
    if (!a.circle && !b.circle) {
        const double den = cross2(a.d, b.d);
        if (std::abs(den) < 1e-3) return std::nullopt;
        pts.push_back(a.p + a.d * (cross2(b.p - a.p, b.d) / den));
    } else if (!a.circle) {
        lineCircle(a, b);
    } else if (!b.circle) {
        lineCircle(b, a);
    } else {
        const Point2 w = b.c - a.c;
        const double dd = w.norm();
        if (dd < 1e-9) return std::nullopt;
        const double x = (dd * dd + a.R * a.R - b.R * b.R) / (2 * dd);
        const double h2 = a.R * a.R - x * x;
        if (h2 < -1e-9 * a.R * a.R) return std::nullopt;
        const double h = std::sqrt(std::max(0.0, h2));
        const Point2 u = w / dd, v(-u.y(), u.x());
        pts.push_back(a.c + u * x + v * h), pts.push_back(a.c + u * x - v * h);
    }
    if (pts.empty()) return std::nullopt;
    return *std::min_element(pts.begin(), pts.end(), [&](const Point2& p, const Point2& q) { return (p - near).norm() < (q - near).norm(); });
}

// Closed profile T1 → C along trace 1, C → T2 along trace 2, T2 → T1 straight (§4.2а item 1), in the plane pl whose
// local coordinates are the section's 2D coordinates.
kernel::Profile cornerProfile(const Plane& pl, const Point2& T1, const Point2& C, const Point2& T2, const Trace& t1, const Trace& t2) {
    kernel::Profile pr;
    pr.plane = pl;
    int n = 0;
    auto piece = [&](const Point2& a, const Point2& b, const Trace& tr) {
        kernel::Segment3 s;
        s.id = "b" + std::to_string(++n);
        if (!tr.circle) {
            s.kind = kernel::Segment3::Kind::Line;
            s.p1 = pl.toWorld(a), s.p2 = pl.toWorld(b);
        } else {
            // The short arc from a to b: counterclockwise about the plane normal from a, or reversed.
            s.kind = kernel::Segment3::Kind::Arc;
            s.center = pl.toWorld(tr.c), s.radius = tr.R;
            const bool ccw = cross2(a - tr.c, b - tr.c) > 0;
            s.start = pl.toWorld(ccw ? a : b), s.end = pl.toWorld(ccw ? b : a);
            s.reversed = !ccw;
        }
        pr.outer.segments.push_back(s);
    };
    piece(T1, C, t1);
    piece(C, T2, t2);
    piece(T2, T1, Trace{});
    return pr;
}

// Vertices and edge midpoints of a face.
std::vector<Point3> facePoints(const Topo& t, int f) {
    std::vector<Point3> out;
    for (int e : t.k.faceEdges(t.s, f)) {
        const auto [v1, v2] = t.k.edgeVertices(t.s, e);
        out.push_back(t.k.vertexPoint(t.s, v1));
        out.push_back(t.k.vertexPoint(t.s, v2));
        out.push_back(t.k.edgeMidpoint(t.s, e));
    }
    return out;
}

// Convex blend: the outward normal at the face point m points away from the center of curvature q.
bool convexAt(const Topo& t, int f, const Point3& m, const Point3& q) { return t.k.faceNormalAt(t.s, f, m).dot(m - q) > 0; }

struct Fill {
    kernel::Shape shape;
    bool add = true;  // united with the body (the blend took material away), otherwise cut
    double gap = -1;  // volume of the corner material the blend took or gave (§4.2а item 3); < 0 — unknown
};

// The corner material in the section (§4.2а item 3): area and centroid of the region between the traces and the arc of
// the fillet (center q, radius r) — the polygon q T1 C T2 minus the sector; a chamfer (r = 0) — the triangle T1 C T2.
// Only for straight traces; false otherwise.
bool gapSection(const Point2& q, double r, const Point2& T1, const Point2& C, const Point2& T2, const Trace& t1, const Trace& t2,
                double& area, Point2& centroid) {
    if (t1.circle || t2.circle) return false;
    auto poly = [](const std::vector<Point2>& p, double& a, Point2& c) {
        a = 0, c = Point2::Zero();
        for (size_t i = 0; i < p.size(); ++i) {
            const Point2 &u = p[i], &v = p[(i + 1) % p.size()];
            const double w = cross2(u, v);
            a += w, c += (u + v) * w;
        }
        a /= 2;
        if (std::abs(a) > 1e-300) c /= 6 * a;
        a = std::abs(a);
    };
    double ap;
    Point2 cp;
    if (r <= 0) {
        poly({T1, C, T2}, area, centroid);
        return area > 0;
    }
    poly({q, T1, C, T2}, ap, cp);
    const Point2 u1 = (T1 - q).normalized(), u2 = (T2 - q).normalized();
    const double phi = std::acos(std::clamp(u1.dot(u2), -1.0, 1.0));
    const double as = r * r * phi / 2;
    const Point2 cs = q + (u1 + u2).normalized() * (phi > 1e-9 ? 4 * r * std::sin(phi / 2) / (3 * phi) : r);
    area = ap - as;
    if (!(area > 0)) return false;
    centroid = (cp * ap - cs * as) / area;
    return true;
}

// The corner of a fillet in its section (center q, radius r, rail points R1, R2 in section coordinates) with the traces of
// the supports: touch points T1, T2 and the corner C. false — no corner.
bool filletCorner(const Point2& q, double r, const Point2& R1, const Point2& R2, Trace& t1, Trace& t2, Point2& T1, Point2& T2, Point2& C) {
    (void)r;
    T1 = foot(t1, q), T2 = foot(t2, q);
    // Tangents at the touch points (⊥ the radius): their corner chooses among the intersections of circle traces.
    auto tangent = [&](const Point2& T, const Point2& R) {
        Trace l;
        Point2 u = T - q;
        if (u.norm() < 1e-12) u = R - q;
        u.normalize();
        l.p = T, l.d = Point2(-u.y(), u.x());
        return l;
    };
    const Trace l1 = tangent(T1, R1), l2 = tangent(T2, R2);
    const double den = cross2(l1.d, l2.d);
    if (std::abs(den) < 1e-3) return false;
    const Point2 c0 = l1.p + l1.d * (cross2(l2.p - l1.p, l2.d) / den);
    const auto c = corner(t1, t2, c0);
    if (!c) return false;
    C = *c;
    return true;
}

// Cylinder fillet or planar chamfer along a line: the corner region extruded over the face's extent along the axis.
// The faces of the step next to its ends that are corner spheres (three fillets of one radius meeting) extend it to the
// corner: the three prisms of a box corner then fill the whole corner cube outside the sphere.
std::optional<Fill> prismFill(const Topo& t, const BlendFace& b, const std::set<int>& spheres, bool flip) {
    const auto& si = t.surf[size_t(b.face)];
    Vec3 a;
    Point3 o;
    Point2 T1, T2, C;
    Trace t1, t2;
    bool add = true;
    Vec3 e1, e2;
    auto frame = [&] {
        e1 = std::abs(a.x()) < 0.9 ? Vec3(Vec3::UnitX() - a.x() * a) : Vec3(Vec3::UnitY() - a.y() * a);
        e1.normalize();
        e2 = a.cross(e1);
    };
    auto to2 = [&](const Point3& p) { return Point2((p - o).dot(e1), (p - o).dot(e2)); };
    if (si.kind == SK::Cylinder) {
        a = si.dir.normalized();
        o = si.point;
        // A support cylinder along the axis lends its direction: the extruded arc then lies on a cylinder of the same
        // axis direction and the pieces merge (OCCT 8 does not merge opposite axes).
        for (int g : {b.g1, b.g2})
            if (t.surf[size_t(g)].kind == SK::Cylinder && t.surf[size_t(g)].dir.dot(a) < 0) {
                a = -a;
                break;
            }
        if (flip) a = -a;
        frame();
        const Point2 q(0, 0);
        const Point2 R1 = to2(t.edgePoint(b.rail1)), R2 = to2(t.edgePoint(b.rail2));
        // Traces of the supports in the section through o ⊥ a: a plane along the axis — its line, a cylinder along the
        // axis — its circle, anything else — the tangent at the rail.
        auto trace = [&](int g, const Point2& R) {
            const auto& sg = t.surf[size_t(g)];
            Trace tr;
            if (sg.kind == SK::Plane && perpendicular(sg.dir, a, 1e-6)) {
                Vec3 n = sg.dir - sg.dir.dot(a) * a;
                n.normalize();
                const Point2 n2(n.dot(e1), n.dot(e2));
                tr.p = to2(sg.point), tr.d = Point2(-n2.y(), n2.x());
            } else if (sg.kind == SK::Cylinder && parallel(sg.dir, a, 1e-6)) {
                tr.circle = true, tr.c = to2(sg.point), tr.R = sg.radius;
            } else {
                Point2 u = (R - q).normalized();
                tr.p = R, tr.d = Point2(-u.y(), u.x());
            }
            return tr;
        };
        t1 = trace(b.g1, R1), t2 = trace(b.g2, R2);
        if (!filletCorner(q, si.radius, R1, R2, t1, t2, T1, T2, C)) return std::nullopt;
        const Point2 mid = (T1 + T2) / 2;
        const Point2 m2 = q + si.radius * (mid - q).normalized();
        const Point3 m = o + e1 * m2.x() + e2 * m2.y() + a * (t.k.faceCentroid(t.s, b.face) - o).dot(a);
        add = convexAt(t, b.face, t.k.closestPointOnFace(t.s, b.face, m), o + a * (m - o).dot(a));
    } else if (si.kind == SK::Plane) {
        // Planar chamfer: the sides are parallel lines; the corner is where the planes of g1 and g2 meet in the section.
        const auto &c1 = t.curve[size_t(b.rail1)], &c2 = t.curve[size_t(b.rail2)];
        if (c1.kind != CK::Line || c2.kind != CK::Line) return std::nullopt;
        const auto &s1 = t.surf[size_t(b.g1)], &s2 = t.surf[size_t(b.g2)];
        if (s1.kind != SK::Plane || s2.kind != SK::Plane) return std::nullopt;
        a = (c1.p2 - c1.p1).normalized();
        o = c1.p1;
        frame();
        auto planeTrace = [&](const kernel::SurfaceInfo& sg) {
            Vec3 n = sg.dir - sg.dir.dot(a) * a;
            n.normalize();
            const Point2 n2(n.dot(e1), n.dot(e2));
            Trace tr;
            tr.p = to2(sg.point), tr.d = Point2(-n2.y(), n2.x());
            return tr;
        };
        t1 = planeTrace(s1), t2 = planeTrace(s2);
        // The sides on the supports: the side points projected onto the traces.
        T1 = foot(t1, to2(c1.p1)), T2 = foot(t2, to2(c2.p1));
        const double den = cross2(t1.d, t2.d);
        if (std::abs(den) < 1e-3) return std::nullopt;
        C = t1.p + t1.d * (cross2(t2.p - t1.p, t2.d) / den);
        // A chamfer of a convex edge took material away: the corner is outside the body.
        const Point2 mid2 = (T1 + T2) / 2;
        const Point3 mid = o + e1 * mid2.x() + e2 * mid2.y() + a * (t.k.faceCentroid(t.s, b.face) - o).dot(a);
        const Vec3 n = t.k.faceNormalAt(t.s, b.face, t.k.closestPointOnFace(t.s, b.face, mid));
        add = (C - mid2).dot(Point2(n.dot(e1), n.dot(e2))) > 0;
    } else {
        return std::nullopt;
    }
    double lo = std::numeric_limits<double>::max(), hi = -lo;
    for (const auto& p : facePoints(t, b.face)) {
        const double x = (p - o).dot(a);
        lo = std::min(lo, x), hi = std::max(hi, x);
    }
    if (!(hi - lo > REC_LIN_TOL * 100)) return std::nullopt;
    bool extended = false;
    if (si.kind == SK::Cylinder) {
        for (int le : t.faceLE[size_t(b.face)]) {
            const int g = t.other(le, b.face);
            if (!spheres.count(g)) continue;
            const double x = (t.surf[size_t(g)].point - o).dot(a);
            if (std::abs(x - lo) <= REC_LIN_TOL * 1000) lo = x - si.radius, extended = true;
            else if (std::abs(x - hi) <= REC_LIN_TOL * 1000) hi = x + si.radius, extended = true;
        }
    }
    double gap = -1, ga = 0;
    Point2 gc;
    if (!extended && gapSection(Point2(0, 0), si.kind == SK::Cylinder ? si.radius : 0, T1, C, T2, t1, t2, ga, gc)) gap = ga * (hi - lo);
    Plane pl;
    pl.origin = o + a * lo, pl.x = e1, pl.y = e2, pl.normal = a;
    const auto pr = cornerProfile(pl, T1, C, T2, t1, t2);
    kernel::Extent ext;
    ext.depth = hi - lo;
    auto r = t.k.extrude(pr, ext, false);
    if (!r) {
        if (tracing()) std::fprintf(stderr, "  fill prism face %d: %s\n", b.face, r.error().code.c_str());
        return std::nullopt;
    }
    return Fill{r->shape, add, gap};
}

// Torus fillet or conical chamfer about an axis: the corner region in the meridian half-plane revolved over the face's
// angular range (between its meridian edges; none — a full turn).
std::optional<Fill> ringFill(const Topo& t, const BlendFace& b, bool flip) {
    const auto& si = t.surf[size_t(b.face)];
    if (si.kind != SK::Torus && si.kind != SK::Cone) return std::nullopt;
    Point3 o = si.point;
    Vec3 a = si.dir.normalized();
    if (si.kind == SK::Cone) {
        // the cone's point is its apex: the axis of the coaxial cylinder of the chamfer serves
        const auto& s1 = t.surf[size_t(b.g1)];
        if (s1.kind != SK::Cylinder) return std::nullopt;
        o = s1.point;
    }
    // A support that is a coaxial cylinder or cone lends its own axis: the revolved fill then lies on exactly its surface
    // and the faces merge (the torus center of a file is off that axis by the file's noise).
    for (int g : {b.g1, b.g2}) {
        const auto& sg = t.surf[size_t(g)];
        if ((sg.kind == SK::Cylinder && coaxial(sg.point, sg.dir, o, a)) || (sg.kind == SK::Cone && si.kind == SK::Torus && coneOnAxis(o, a, sg.point, sg.dir))) {
            // its direction too: OCCT 8 does not merge pieces of one cylinder whose axes point opposite ways
            const Vec3 ga = sg.dir.normalized();
            o = sg.point + ga * (o - sg.point).dot(ga);
            a = ga;
            break;
        }
    }
    // The kernel's surface information keeps no axis orientation; pieces of one cylinder with opposite axes are not merged
    // by OCCT 8 — the other orientation is a second attempt (§4.2а item 3).
    if (flip) a = -a;
    // Meridian direction: towards the face centroid; a full ring has it on the axis — towards a point of its rail.
    Vec3 e = t.k.faceCentroid(t.s, b.face) - o;
    e -= e.dot(a) * a;
    if (e.norm() < REC_LIN_TOL * 100) {
        e = t.edgePoint(b.rail1) - o;
        e -= e.dot(a) * a;
    }
    if (e.norm() < REC_LIN_TOL * 100) return std::nullopt;
    e.normalize();
    const Vec3 f = a.cross(e);
    const auto &c1 = t.curve[size_t(b.rail1)], &c2 = t.curve[size_t(b.rail2)];
    if (c1.kind != CK::Circle || c2.kind != CK::Circle) return std::nullopt;
    auto mer = [&](const kernel::CurveInfo& c) { return Point2(c.radius, (c.center - o).dot(a)); };
    Point2 T1 = mer(c1), T2 = mer(c2), C;
    Trace t1, t2;
    bool add = true;
    Point2 Qg(0, 0);
    double rg = 0;
    if (si.kind == SK::Torus) {
        // The tube center in the meridian (the torus center may sit off the lent axis by the file's noise).
        const Point2 Q(distToLine(si.point + e * si.radius, o, a), (si.point - o).dot(a));
        const double r = si.minorRadius;
        // Traces of the supports in the meridian: a plane ⊥ the axis, a coaxial cylinder or cone — lines, a coaxial
        // sphere or torus — circles, anything else — the tangent at the rail.
        auto trace = [&](int g, const Point2& R) {
            const auto& sg = t.surf[size_t(g)];
            Trace tr;
            if (sg.kind == SK::Plane && parallel(sg.dir, a, 1e-6)) {
                tr.p = Point2(0, (sg.point - o).dot(a)), tr.d = Point2(1, 0);
            } else if (sg.kind == SK::Cylinder && coaxial(sg.point, sg.dir, o, a)) {
                tr.p = Point2(sg.radius, 0), tr.d = Point2(0, 1);
            } else if (sg.kind == SK::Cone && coneOnAxis(o, a, sg.point, sg.dir)) {
                tr.p = Point2(0, (sg.point - o).dot(a));
                tr.d = Point2(std::sin(sg.halfAngle), std::cos(sg.halfAngle) * (sg.dir.dot(a) > 0 ? 1 : -1)).normalized();
            } else if (sg.kind == SK::Sphere && distToLine(sg.point, o, a) <= axisTol()) {
                tr.circle = true, tr.c = Point2(0, (sg.point - o).dot(a)), tr.R = sg.radius;
            } else if (sg.kind == SK::Torus && coaxial(sg.point, sg.dir, o, a)) {
                tr.circle = true, tr.c = Point2(sg.radius, (sg.point - o).dot(a)), tr.R = sg.minorRadius;
            } else {
                const Point2 u = (R - Q).normalized();
                tr.p = R, tr.d = Point2(-u.y(), u.x());
            }
            return tr;
        };
        t1 = trace(b.g1, T1), t2 = trace(b.g2, T2);
        const Point2 R1 = T1, R2 = T2;
        if (!filletCorner(Q, r, R1, R2, t1, t2, T1, T2, C)) return std::nullopt;
        Qg = Q, rg = r;
        const Point2 m2 = Q + r * ((T1 + T2) / 2 - Q).normalized();
        const Point3 m = o + e * m2.x() + a * m2.y();
        add = convexAt(t, b.face, t.k.closestPointOnFace(t.s, b.face, m), o + e * Q.x() + a * Q.y());
    } else {
        // Conical chamfer between a coaxial cylinder (g1) and a plane ⊥ the axis (g2): the corner has the cylinder's
        // radius and the plane's level.
        const auto &s1 = t.surf[size_t(b.g1)], &s2 = t.surf[size_t(b.g2)];
        if (s2.kind != SK::Plane) return std::nullopt;
        C = Point2(s1.radius, (s2.point - o).dot(a));
        T1.x() = C.x(), T2.y() = C.y();
        t1.p = Point2(s1.radius, 0), t1.d = Point2(0, 1);
        t2.p = Point2(0, C.y()), t2.d = Point2(1, 0);
        const Point2 mid = (T1 + T2) / 2;
        const Point3 m = o + e * mid.x() + a * mid.y();
        const Vec3 n = t.k.faceNormalAt(t.s, b.face, t.k.closestPointOnFace(t.s, b.face, m));
        const Point2 n2(n.dot(e), n.dot(a));
        add = (C - mid).dot(n2) > 0;
    }
    if (T1.x() < -REC_LIN_TOL || T2.x() < -REC_LIN_TOL || C.x() < -REC_LIN_TOL) return std::nullopt;
    // Angular range: the meridian edges (circles whose axis is ⊥ the axis, lines) around the centroid direction.
    double lo = -PI, hi = PI;
    bool full = true;
    for (int le : t.faceLE[size_t(b.face)]) {
        if (le == b.rail1 || le == b.rail2) continue;
        const auto& c = t.curve[size_t(le)];
        Point3 p;
        if (c.kind == CK::Circle && perpendicular(c.axis, a, 1e-6)) p = c.center;
        else if (c.kind == CK::Line) p = (c.p1 + c.p2) / 2;
        else if (c.kind == CK::Circle && parallel(c.axis, a, 1e-6)) continue;  // another latitude (pieces of a rail)
        else p = t.edgePoint(le);
        Vec3 v = p - o;
        v -= v.dot(a) * a;
        if (v.norm() < REC_LIN_TOL * 100) continue;
        const double phi = std::atan2(v.dot(f), v.dot(e));
        full = false;
        if (phi < 0) lo = std::max(lo, phi);
        else hi = std::min(hi, phi);
    }
    // Meridian edges on one side of the centroid direction only (a face wider than a half-turn): no fill.
    if (!full && (lo <= -PI || hi >= PI)) return std::nullopt;
    double gap = -1, ga = 0;
    Point2 gc;
    if (gapSection(Qg, rg, T1, C, T2, t1, t2, ga, gc)) gap = ga * gc.x() * (full ? 2 * PI : hi - lo);
    const Vec3 es = full ? e : rotateAbout(e, a, lo);
    Plane pl;
    pl.origin = o;
    pl.x = es;
    pl.y = a;
    pl.normal = es.cross(a);
    const auto pr = cornerProfile(pl, T1, C, T2, t1, t2);
    kernel::RevolveExtent ext;
    if (!full) ext.kind = kernel::RevolveExtent::Kind::Angle, ext.value = hi - lo;
    auto r = t.k.revolve(pr, kernel::Axis{o, a}, ext);
    if (!r) {
        if (tracing()) std::fprintf(stderr, "  fill ring face %d: %s\n", b.face, r.error().code.c_str());
        return std::nullopt;
    }
    return Fill{r->shape, add, gap};
}

// Faces of `after` on the surface of face g of `before` (the same plane, the same coaxial cylinder or cone, the same sphere
// or torus): the boolean and the merging of faces do not keep the face's history.
std::set<int> sameSurface(const Topo& before, int g, const Topo& after) {
    std::set<int> out;
    const auto& sg = before.surf[size_t(g)];
    for (int f = 0; f < after.faces(); ++f) {
        const auto& sa = after.surf[size_t(f)];
        if (sa.kind != sg.kind) continue;
        bool same = false;
        switch (sg.kind) {
            case SK::Plane:
                same = parallel(sa.dir, sg.dir) && sa.dir.dot(sg.dir) > 0 && std::abs((sa.point - sg.point).dot(sg.dir.normalized())) <= axisTol();
                break;
            case SK::Cylinder:
                same = coaxial(sg.point, sg.dir, sa.point, sa.dir) && std::abs(sa.radius - sg.radius) <= radiusTol();
                break;
            case SK::Cone:
                same = (sa.point - sg.point).norm() <= axisTol() && parallel(sa.dir, sg.dir) && std::abs(sa.halfAngle - sg.halfAngle) <= 1e-6;
                break;
            case SK::Sphere:
                same = (sa.point - sg.point).norm() <= axisTol() && std::abs(sa.radius - sg.radius) <= radiusTol();
                break;
            case SK::Torus:
                same = (sa.point - sg.point).norm() <= axisTol() && parallel(sa.dir, sg.dir) && std::abs(sa.radius - sg.radius) <= radiusTol() &&
                       std::abs(sa.minorRadius - sg.minorRadius) <= radiusTol();
                break;
            default:
                break;
        }
        if (same) out.insert(f);
    }
    return out;
}

}  // namespace

namespace {

// Edges between adjacent faces on one surface (pieces that did not merge).
int splitPieces(const Topo& t) {
    int n = 0;
    for (size_t le = 0; le < t.ct.logicalEdges.size(); ++le) {
        const auto [x, y] = t.ct.logicalEdges[le].faces;
        if (x != y && samePiece(t.surf[size_t(x)], t.surf[size_t(y)])) ++n;
    }
    return n;
}

std::optional<kernel::Shape> fillOnce(const Topo& t, const std::vector<BlendFace>& faces, bool flip, double* expected);

}  // namespace

std::optional<kernel::Shape> fillBlends(const Topo& t, const std::vector<BlendFace>& faces, double* expected) {
    auto s = fillOnce(t, faces, false, expected);
    if (!s) return s;
    if (splitPieces(Topo(t.k, *s)) <= splitPieces(t)) return s;
    double e2 = 0;
    auto f = fillOnce(t, faces, true, &e2);
    if (f && t.k.counts(*f).faces < t.k.counts(*s).faces) {
        if (expected) *expected = e2;
        return f;
    }
    return s;
}

namespace {

std::optional<kernel::Shape> fillOnce(const Topo& t, const std::vector<BlendFace>& faces, bool flip, double* expected) {
    double sum = 0;
    bool known = true;
    std::set<int> spheres;
    for (const auto& b : faces)
        if (t.surf[size_t(b.face)].kind == SK::Sphere) spheres.insert(b.face);
    std::optional<kernel::Shape> add, cut;
    auto merge = [&](std::optional<kernel::Shape>& acc, const kernel::Shape& s) {
        if (!acc) {
            acc = s;
            return true;
        }
        auto r = t.k.boolean(kernel::BoolKind::Union, *acc, s);
        if (!r) return false;
        acc = r->shape;
        return true;
    };
    for (const auto& b : faces) {
        const auto kind = t.surf[size_t(b.face)].kind;
        if (kind == SK::Sphere) continue;  // filled by the extended prisms of its neighbors
        std::optional<Fill> fl;
        if (kind == SK::Cylinder || kind == SK::Plane) fl = prismFill(t, b, spheres, flip);
        else if (kind == SK::Torus || kind == SK::Cone) fl = ringFill(t, b, flip);
        if (!fl) return std::nullopt;
        if (fl->gap < 0) known = false;
        else sum += fl->add ? fl->gap : -fl->gap;
        if (!merge(fl->add ? add : cut, fl->shape)) return std::nullopt;
    }
    if (expected) *expected = known ? sum : std::numeric_limits<double>::quiet_NaN();
    kernel::Shape s = t.s;
    if (add) {
        auto r = t.k.boolean(kernel::BoolKind::Union, s, *add);
        if (!r) return std::nullopt;
        s = r->shape;
    }
    if (cut) {
        auto r = t.k.boolean(kernel::BoolKind::Subtract, s, *cut);
        if (!r) return std::nullopt;
        s = r->shape;
    }
    if (s.null() || t.k.counts(s).solids != 1) return std::nullopt;
    if (auto u = t.k.unifyFaces(s)) s = *u;
    return s;
}

}  // namespace

std::optional<double> chamferSize(const Topo& t, const BlendFace& b) {
    const auto& si = t.surf[size_t(b.face)];
    if (b.g1 < 0 || b.g2 < 0 || b.rail1 < 0 || b.rail2 < 0) return std::nullopt;
    const auto &c1 = t.curve[size_t(b.rail1)], &c2 = t.curve[size_t(b.rail2)];
    const auto &s1 = t.surf[size_t(b.g1)], &s2 = t.surf[size_t(b.g2)];
    double d1 = 0, d2 = 0;
    if (si.kind == SK::Plane) {
        if (c1.kind != CK::Line || c2.kind != CK::Line || s1.kind != SK::Plane || s2.kind != SK::Plane) return std::nullopt;
        // The sharp edge is the line where the planes of g1 and g2 meet: its distances to the two sides.
        const Vec3 n1 = s1.dir.normalized(), n2 = s2.dir.normalized();
        const Vec3 dir = n1.cross(n2);
        if (dir.norm() < 1e-6) return std::nullopt;
        // A point of both planes nearest to the first side.
        const double h1 = n1.dot(s1.point), h2 = n2.dot(s2.point);
        const Point3 p0 = (h1 * n2.cross(dir) + h2 * dir.cross(n1)) / dir.squaredNorm();
        d1 = distToLine(c1.p1, p0, dir);
        d2 = distToLine(c2.p1, p0, dir);
    } else if (si.kind == SK::Cone) {
        if (c1.kind != CK::Circle || c2.kind != CK::Circle || s1.kind != SK::Cylinder || s2.kind != SK::Plane) return std::nullopt;
        // side1 on the cylinder: the shift along the axis to the plane; side2 on the plane: the radius difference.
        const Vec3 a = s1.dir.normalized();
        d1 = std::abs((c1.center - s2.point).dot(a));
        d2 = std::abs(c2.radius - s1.radius);
    } else {
        return std::nullopt;
    }
    if (!(d1 > REC_LIN_TOL) || std::abs(d1 - d2) > axisTol()) return std::nullopt;
    return d1;
}

std::vector<int> blendEdges(const Topo& before, const std::vector<BlendFace>& faces, double limit, const Topo& after) {
    std::vector<int> out;
    std::set<int> used;
    for (const auto& b : faces) {
        if (b.g1 < 0 || b.g2 < 0) continue;
        const std::set<int> A = sameSurface(before, b.g1, after), B = sameSurface(before, b.g2, after);
        for (size_t le = 0; le < after.ct.logicalEdges.size(); ++le) {
            const auto [x, y] = after.ct.logicalEdges[le].faces;
            if (!((A.count(x) && B.count(y)) || (A.count(y) && B.count(x)))) continue;
            if (used.count(int(le))) continue;
            const Point3 p = after.edgePoint(int(le));
            if (before.k.distanceToFace(before.s, b.face, p) <= limit + REC_LIN_TOL * 10) {
                used.insert(int(le));
                out.push_back(int(le));
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

std::optional<std::pair<kernel::Shape, Op>> sharpenOnce(const Topo& t, const std::vector<BlendFace>& faces, bool chamfer, double size) {
    const kernel::Kernel& k = t.k;
    if (faces.empty()) return std::nullopt;
    // A face or a support written in the file as a free-form surface of revolution (read as canonical, §2.1): the fill's
    // exact surfaces do not merge with it and the kernel's fillet over the mix is unreliable (it may even fault).
    for (const auto& b : faces)
        for (int g : {b.face, b.g1, b.g2})
            if (g >= 0 && t.surf[size_t(g)].otherKind == "revolution") {
                if (tracing()) std::fprintf(stderr, "  sharpen: a surface of revolution of the file\n");
                return std::nullopt;
            }
    if (!spendRemoval()) return std::nullopt;
    double expected = 0;
    auto sharp = fillBlends(t, faces, &expected);
    if (!sharp) {
        if (tracing()) std::fprintf(stderr, "  sharpen %s %.4g, %zu faces: no fill\n", chamfer ? "chamfer" : "fillet", size, faces.size());
        return std::nullopt;
    }
    if (const char* dump = std::getenv("CONSTRUE_REC_DUMP")) {
        // debugging aid: the sharp body of each attempt as BREP
        static int n = 0;
        if (auto txt = k.shapeToBrep(*sharp)) {
            if (FILE* fp = std::fopen((std::string(dump) + "/sharp" + std::to_string(++n) + ".brep").c_str(), "w")) {
                std::fputs(txt->c_str(), fp);
                std::fclose(fp);
            }
        }
    }
    // The volume changed by the corner material (§4.2а item 3): a fill that took more or less is wrong, and the kernel's
    // fillet of a wrong body is not even safe to call. Corners of mitred chains overlap: a tenth of the material.
    if (!std::isnan(expected)) {
        const double dv = k.massProperties(*sharp).volume - k.massProperties(t.s).volume;
        if (std::abs(dv - expected) > 0.1 * std::abs(expected) + REC_MATCH_REL * k.massProperties(t.s).volume) {
            if (tracing()) std::fprintf(stderr, "  sharpen: volume change %.6g, corners %.6g\n", dv, expected);
            return std::nullopt;
        }
    }
    const Topo after(k, *sharp);
    // A blend step leaves fewer faces: its faces are gone.
    if (after.faces() >= t.faces()) {
        if (tracing()) std::fprintf(stderr, "  sharpen: %d faces after, %d before\n", after.faces(), t.faces());
        return std::nullopt;
    }
    // Unmerged pieces of one surface: the kernel's fillet across their seam is unreliable (it may even fault).
    if (splitPieces(after) > splitPieces(t)) {
        if (tracing()) std::fprintf(stderr, "  sharpen: pieces of one surface left\n");
        return std::nullopt;
    }
    // The distance from a new edge to the old face: at most the radius (fillet), the chamfer width otherwise.
    const auto les = blendEdges(t, faces, chamfer ? size * 2 : size, after);
    if (les.empty()) return std::nullopt;
    // The kernel's fillet that runs into a free-form face at an end of its edges is unreliable (it may even fault): such
    // a step is left to the face removal.
    for (int le : les)
        for (int v : after.ct.logicalEdges[size_t(le)].ends)
            for (int g : k.vertexFaces(*sharp, v))
                if (after.surf[size_t(g)].kind == SK::Other) {
                    if (tracing()) std::fprintf(stderr, "  sharpen: free-form face at an edge end\n");
                    return std::nullopt;
                }
    std::vector<kernel::EdgeRef> refs;
    for (int le : les) {
        kernel::EdgeRef r;
        r.edge = le;
        r.label = "e" + std::to_string(le);
        refs.push_back(r);
    }
    if (tracing()) {
        std::fprintf(stderr, "  sharpen: forward %s %.4g on %zu edges, faces:", chamfer ? "chamfer" : "fillet", size, les.size());
        for (const auto& b : faces) std::fprintf(stderr, " %d", b.face);
        std::fprintf(stderr, "\n");
    }
    auto fwd = chamfer ? k.chamfer(*sharp, refs, size) : k.fillet(*sharp, refs, size);
    const bool ok = fwd && bodiesMatch(k, t.s, fwd->shape);
    if (tracing())
        std::fprintf(stderr, "  sharpen %s %.4g: %zu faces, %zu edges, forward %s\n", chamfer ? "chamfer" : "fillet", size, faces.size(), les.size(),
                     !fwd ? fwd.error().code.c_str() : ok ? "match" : "differs");
    if (!ok) return std::nullopt;
    Op op;
    op.kind = chamfer ? Op::Kind::Chamfer : Op::Kind::Fillet;
    op.size2 = size;
    op.edges = edgeKeys(after, les);
    return std::make_pair(*sharp, op);
}

}  // namespace

std::optional<std::pair<kernel::Shape, Op>> sharpenStep(const Topo& t, const std::vector<BlendFace>& faces, bool chamfer, double size, bool byChains) {
    if (!byChains) return sharpenOnce(t, faces, chamfer, size);
    // Not all at once: by chains — faces connected by common edges, in the order of their first face (two fillets of one
    // radius made by different operations, one along the other's chain).
    std::vector<int> root(faces.size());
    for (size_t i = 0; i < faces.size(); ++i) root[i] = int(i);
    std::function<int(int)> find = [&](int x) { return root[size_t(x)] == x ? x : root[size_t(x)] = find(root[size_t(x)]); };
    for (size_t i = 0; i < faces.size(); ++i)
        for (int le : t.faceLE[size_t(faces[i].face)]) {
            const int o = t.other(le, faces[i].face);
            for (size_t j = 0; j < faces.size(); ++j)
                if (j != i && faces[j].face == o) root[size_t(find(int(j)))] = find(int(i));
        }
    std::map<int, std::vector<BlendFace>> chains;
    std::vector<int> order;
    for (size_t i = 0; i < faces.size(); ++i) {
        const int r = find(int(i));
        if (!chains.count(r)) order.push_back(r);
        chains[r].push_back(faces[i]);
    }
    if (order.size() < 2) return std::nullopt;
    for (int r : order) {
        if (stopped()) break;
        // A chain of corner spheres alone is no step.
        const auto& c = chains[r];
        if (std::all_of(c.begin(), c.end(), [](const BlendFace& b) { return b.g1 < 0; })) continue;
        if (auto x = sharpenOnce(t, c, chamfer, size)) return x;
    }
    return std::nullopt;
}

}  // namespace construe::rec::detail
