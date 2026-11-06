// View decoration: center lines and center marks, thread representation (spec/drawing.md §6.15, §6.16; S3-DRW-007).
#include "drawing/view_parts.h"

#include "base/constants.h"

#include <algorithm>
#include <cmath>

namespace construe::drawing::detail {

namespace {

constexpr double ON_PLANE = 1e-6;  // model mm: axis or vertex in the cutting plane (§6.15 item 4)

Point3 place(const kernel::Motion& m, const Point3& p) {
    Eigen::Matrix3d R;
    R << m.r[0], m.r[1], m.r[2], m.r[3], m.r[4], m.r[5], m.r[6], m.r[7], m.r[8];
    return R * p + m.t;
}
Vec3 turn(const kernel::Motion& m, const Vec3& v) {
    Eigen::Matrix3d R;
    R << m.r[0], m.r[1], m.r[2], m.r[3], m.r[4], m.r[5], m.r[6], m.r[7], m.r[8];
    return R * v;
}
Point2 project(const Frame& f, const Point3& p) { return {p.dot(f.r), p.dot(f.u)}; }

struct AxisLine {
    Point2 a, b;  // endpoints on the sheet
};
struct CenterMark {
    Point2 c;
    double R;
};

}  // namespace

void appendCenterLines(const kernel::Kernel& k, const std::vector<kernel::PlacedShape>& bodies, const Frame& f, const ToSheet& toSheet,
                       double scale, const std::optional<Cut>& cut, const std::string& role, std::vector<Primitive>& out) {
    // Body surface of revolution (items 1–2): faces of one surface (same axis, same radius or cone) are combined.
    struct Surface {
        size_t body = 0;
        kernel::SurfaceInfo::Kind kind{};
        Point3 A = Point3::Zero();
        Vec3 a = Vec3::UnitZ();
        double radius = 0, halfAngle = 0;  // cylinder — radius; cone — half-angle (A — apex)
        double t0 = 1e300, t1 = -1e300, phi = 0, rho = 0;
        bool touchesPlane = false;
    };
    std::vector<Surface> surfaces;
    for (size_t bi = 0; bi < bodies.size(); ++bi) {
        const auto& b = bodies[bi];
        if (b.shape.null()) continue;
        const int faces = k.counts(b.shape).faces;
        for (int fi = 0; fi < faces; ++fi) {
            const kernel::SurfaceInfo s = k.faceSurface(b.shape, fi);
            if (s.kind != kernel::SurfaceInfo::Kind::Cylinder && s.kind != kernel::SurfaceInfo::Kind::Cone) continue;
            Surface g;
            g.body = bi;
            g.kind = s.kind;
            g.A = place(b.motion, s.point);
            g.a = turn(b.motion, s.dir).normalized();
            g.radius = s.radius;
            g.halfAngle = s.halfAngle;
            // Extent along the axis from the vertices of the face edges (item 1).
            for (int e : k.faceEdges(b.shape, fi)) {
                const auto [v1, v2] = k.edgeVertices(b.shape, e);
                for (int v : {v1, v2}) {
                    if (v < 0) continue;
                    const Point3 V = place(b.motion, k.vertexPoint(b.shape, v));
                    const double t = (V - g.A).dot(g.a);
                    g.t0 = std::min(g.t0, t);
                    g.t1 = std::max(g.t1, t);
                    if (cut && std::abs((V - cut->origin).dot(cut->normal)) <= ON_PLANE) g.touchesPlane = true;
                }
            }
            if (!(g.t1 - g.t0 >= 1e-6)) continue;
            // Angular span of the face (item 2).
            const double area = k.faceArea(b.shape, fi);
            if (s.kind == kernel::SurfaceInfo::Kind::Cylinder) {
                g.rho = s.radius;
                g.phi = g.rho > 0 ? area / (g.rho * (g.t1 - g.t0)) : 0;
            } else {
                const double ta = std::tan(s.halfAngle), ca = std::cos(s.halfAngle);
                const double r0 = std::abs(g.t0) * ta, r1 = std::abs(g.t1) * ta;
                g.rho = std::max(r0, r1);
                const double lat = (g.t1 - g.t0) / ca * (r0 + r1) / 2;
                g.phi = lat > 0 ? area / lat : 0;
            }
            // Same surface of the same body — add the angular span, merge the extent.
            auto same = std::find_if(surfaces.begin(), surfaces.end(), [&](const Surface& o) {
                if (o.body != g.body || o.kind != g.kind || o.a.cross(g.a).norm() > 1e-9) return false;
                const Vec3 w = g.A - o.A;
                if ((w - w.dot(o.a) * o.a).norm() > 1e-6) return false;  // axes on one line
                if (g.kind == kernel::SurfaceInfo::Kind::Cylinder) return std::abs(o.radius - g.radius) <= 1e-6;
                return w.norm() <= 1e-6 && std::abs(o.halfAngle - g.halfAngle) <= 1e-9 && o.a.dot(g.a) > 0;
            });
            if (same == surfaces.end()) {
                surfaces.push_back(g);
                continue;
            }
            const double shift = (g.A - same->A).dot(same->a);  // extent of g in the frame of same (axes on one line)
            const bool along = same->a.dot(g.a) > 0;
            const double s0 = along ? shift + g.t0 : shift - g.t1, s1 = along ? shift + g.t1 : shift - g.t0;
            same->t0 = std::min(same->t0, s0);
            same->t1 = std::max(same->t1, s1);
            same->phi += g.phi;
            same->rho = std::max(same->rho, g.rho);
            same->touchesPlane = same->touchesPlane || g.touchesPlane;
        }
    }
    std::vector<AxisLine> axes;
    std::vector<CenterMark> marks;
    for (const auto& g : surfaces) {
        if (g.phi < CENTERLINE_MIN_TURN) continue;  // fillets (item 2)
        const double an = std::abs(g.a.dot(f.n));
        if (an <= AXIS_PARALLEL_TOL) {  // axis seen from the side (item 3)
            if (cut && cut->sectionOnly && std::abs((g.A - cut->origin).dot(cut->normal)) > ON_PLANE) continue;
            axes.push_back({toSheet(project(f, g.A + g.t0 * g.a)), toSheet(project(f, g.A + g.t1 * g.a))});
        } else if (1 - an <= AXIS_PARALLEL_TOL) {  // circle seen end-on
            if (cut && cut->sectionOnly && !g.touchesPlane) continue;
            marks.push_back({toSheet(project(f, g.A)), scale * g.rho});
        }
    }
    // Merging collinear axes and centers (§6.15).
    std::vector<AxisLine> mergedAxes;
    for (const auto& ax : axes) {
        const Point2 d = (ax.b - ax.a).normalized();
        bool merged = false;
        for (auto& m : mergedAxes) {
            const Point2 md = (m.b - m.a).normalized();
            const double cross = md.x() * d.y() - md.y() * d.x();
            const Point2 w = ax.a - m.a;
            const double dist = std::abs(md.x() * w.y() - md.y() * w.x());
            if (std::abs(cross) > 1e-9 || dist > DRAWING_MERGE_TOL) continue;
            double lo = 0, hi = (m.b - m.a).dot(md);
            for (const Point2& q : {ax.a, ax.b}) {
                const double t = (q - m.a).dot(md);
                lo = std::min(lo, t);
                hi = std::max(hi, t);
            }
            const Point2 base = m.a;
            m.a = base + lo * md;
            m.b = base + hi * md;
            merged = true;
            break;
        }
        if (!merged) mergedAxes.push_back(ax);
    }
    std::vector<CenterMark> mergedMarks;
    for (const auto& c : marks) {
        auto it = std::find_if(mergedMarks.begin(), mergedMarks.end(), [&](const CenterMark& m) { return (m.c - c.c).norm() <= DRAWING_MERGE_TOL; });
        if (it == mergedMarks.end()) mergedMarks.push_back(c);
        else it->R = std::max(it->R, c.R);
    }
    for (const auto& ax : mergedAxes) {
        const Point2 d = (ax.b - ax.a).normalized();
        out.push_back(makeLine(ax.a - CENTERLINE_OVERSHOOT_MM * d, ax.b + CENTERLINE_OVERSHOOT_MM * d, LineType::DashDot, Weight::Thin, role));
    }
    for (const auto& m : mergedMarks) {
        const LineType lt = 2 * m.R < CENTERLINE_SOLID_DIAMETER_MM ? LineType::Solid : LineType::DashDot;
        const double L = m.R + CENTERLINE_OVERSHOOT_MM;
        for (const Point2& e : {Point2(1, 0), Point2(-1, 0), Point2(0, 1), Point2(0, -1)}) out.push_back(makeLine(m.c, m.c + L * e, lt, Weight::Thin, role));
    }
}

void appendThreads(const std::vector<regen::ThreadEntry>& threads, const Frame& f, const ToSheet& toSheet, double scale,
                   const std::optional<Cut>& cut, bool hiddenLines, const std::vector<kernel::ViewCurve>* visible, const std::string& role,
                   std::vector<Primitive>& out) {
    for (const auto& th : threads) {
        if (!(th.length > 0) || !(th.d > 0)) continue;
        const Vec3 a = th.dir.normalized();
        const Point3& A = th.origin;
        const double d1 = th.d - THREAD_MINOR_FACTOR * th.pitch;
        const double thin = th.internal ? th.d / 2 : d1 / 2;
        double t0 = 0, t1 = th.length;
        bool cutAlong = false;  // the section plane runs along the axis through the hole
        if (cut) {
            const double g0 = (A - cut->origin).dot(cut->normal), ga = a.dot(cut->normal);
            if (std::abs(ga) <= AXIS_PARALLEL_TOL) {
                if (g0 > th.d / 2) continue;
                cutAlong = std::abs(g0) < th.radius;
            } else {
                const double tz = -g0 / ga;  // g(tz) = 0
                if (ga > 0) t1 = std::min(t1, tz);
                else t0 = std::max(t0, tz);
                if (!(t1 > t0)) continue;
            }
        }
        const double an = std::abs(a.dot(f.n));
        if (an <= AXIS_PARALLEL_TOL) {  // from the side
            const bool vis = !th.internal || cutAlong;
            if (!vis && !hiddenLines) continue;
            const LineType lt = vis ? LineType::Solid : LineType::Dashed;
            const Point2 p0 = project(f, A + t0 * a), p1 = project(f, A + t1 * a);
            const Point2 ad = (p1 - p0).normalized();
            const Point2 perp(-ad.y(), ad.x());
            for (double sgn : {1.0, -1.0})
                out.push_back(makeLine(toSheet(p0 + sgn * thin * perp), toSheet(p1 + sgn * thin * perp), lt, Weight::Thin, role));
            if (t1 == th.length) {
                const Point2 e = project(f, A + th.length * a);
                out.push_back(makeLine(toSheet(e + th.d / 2 * perp), toSheet(e - th.d / 2 * perp), lt, vis ? Weight::Thick : Weight::Thin, role));
            }
        } else if (1 - an <= AXIS_PARALLEL_TOL) {  // end-on
            const Point2 c = project(f, A);
            bool vis = false;
            if (cut && cut->sectionOnly) {
                vis = true;  // the plane intersects the thread (otherwise cut off above or does not intersect)
                const double g0 = (A - cut->origin).dot(cut->normal), gL = (A + th.length * a - cut->origin).dot(cut->normal);
                if ((g0 > 0) == (gL > 0) && std::abs(g0) > ON_PLANE && std::abs(gL) > ON_PLANE) continue;
            } else if (visible) {
                for (const auto& vc : *visible)
                    if ((vc.kind == kernel::ViewCurve::Kind::Circle || vc.kind == kernel::ViewCurve::Kind::Arc) &&
                        (vc.center - c).norm() <= THREAD_MATCH_TOL && std::abs(vc.radius - th.radius) <= THREAD_MATCH_TOL)
                        vis = true;
            }
            if (!vis && !hiddenLines) continue;
            Primitive p;
            p.kind = PrimitiveKind::Arc;
            p.center = toSheet(c);
            p.radius = scale * thin;
            p.start = THREAD_ARC_START;
            p.end = THREAD_ARC_START + 1.5 * PI;
            p.lineType = vis ? LineType::Solid : LineType::Dashed;
            p.weight = Weight::Thin;
            p.role = role;
            out.push_back(std::move(p));
        }
    }
}

}  // namespace construe::drawing::detail
