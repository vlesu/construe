// Snaps and hit testing in a sketch (app/sketch_picker.h).
#include "app/sketch_picker.h"
#include "app/text_box.h"

#include <limits>

#include "app/snap.h"
#include "app/theme.h"
#include "base/geom2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numbers>

namespace construe::app {

namespace {

bool isProjectedPoint(const SketchGeometry& g, const sketch::Entity& e) {
    if (e.type != sketch::EntityType::Projected) return false;
    const auto& pr = g.base().projections;
    const auto it = pr.find(e.id);
    return it != pr.end() && it->second.kind == sketch::Projection::Kind::Point;
}
// "Onto curve" with alignment along one axis (§6.4, S1-SK-015): the point goes to the intersection, nearest to the
// cursor, of curve out.target with the alignment line, if it is not farther than SNAP_CURVE_PX from the cursor.
template <class Collect>
void alignOnCurve(const SketchGeometry& g, Snap& out, const Point2& cursor, const QPointF& s, const Collect& collect) {
    Snap cand;
    collect(cursor, cand);
    if (!cand.alignX && !cand.alignY) return;
    const QPointF cs = g.toScreen(cursor);
    auto offset = [&](const Point2& target, int axis) {
        const QPointF ts = g.toScreen(target);
        return axis == 0 ? std::abs(ts.x() - cs.x()) : std::abs(ts.y() - cs.y());
    };
    // The axis with the smaller screen deviation of the cursor from the target goes first.
    std::vector<int> axes;
    if (cand.alignX) axes.push_back(0);
    if (cand.alignY) axes.push_back(1);
    if (axes.size() == 2 && offset(*cand.alignY, 1) < offset(*cand.alignX, 0)) std::swap(axes[0], axes[1]);
    for (int axis : axes) {
        const Point2 target = axis == 0 ? *cand.alignX : *cand.alignY;
        const auto q = curveAxisPoint(g, out.target, axis, target[axis], cursor);
        if (!q) continue;
        const QPointF qs = g.toScreen(*q);
        if (std::hypot(qs.x() - s.x(), qs.y() - s.y()) > SNAP_CURVE_PX) continue;
        out.at = *q;
        if (axis == 0) out.alignX = cand.alignX, out.alignXId = cand.alignXId, out.alignXSource = cand.alignXSource;
        else out.alignY = cand.alignY, out.alignYId = cand.alignYId, out.alignYSource = cand.alignYSource;
        return;
    }
}

}  // namespace

std::optional<Point2> curveAxisPoint(const SketchGeometry& g, const std::string& curve, int axis, double value, const Point2& near) {
    const auto* e = g.data()->findEntity(curve);
    if (!e) return std::nullopt;
    // Curve in local coordinates: segment (a, b) or circle/arc (c, r).
    std::optional<std::pair<Point2, Point2>> seg, arcEnds;
    std::optional<std::pair<Point2, double>> circ;
    if (e->type == sketch::EntityType::Line) seg = {{g.pointPos(e->p1), g.pointPos(e->p2)}};
    else if (e->type == sketch::EntityType::Circle) circ = {{g.pointPos(e->center), g.radius(*e)}};
    else if (e->type == sketch::EntityType::Arc) {
        const Point2 c = g.pointPos(e->center);
        circ = {{c, (g.pointPos(e->start) - c).norm()}};
        arcEnds = {{g.pointPos(e->start), g.pointPos(e->end)}};
    } else if (e->type == sketch::EntityType::Projected) {
        const auto it = g.base().projections.find(e->id);
        if (it == g.base().projections.end()) return std::nullopt;
        const auto& p = it->second;
        if (p.kind == sketch::Projection::Kind::Line) seg = {{Point2(p.x1, p.y1), Point2(p.x2, p.y2)}};
        else if (p.kind == sketch::Projection::Kind::Circle) circ = {{Point2(p.cx, p.cy), p.r}};
        else if (p.kind == sketch::Projection::Kind::Arc) {
            circ = {{Point2(p.cx, p.cy), (Point2(p.sx, p.sy) - Point2(p.cx, p.cy)).norm()}};
            arcEnds = {{Point2(p.sx, p.sy), Point2(p.ex, p.ey)}};
        } else return std::nullopt;
    } else return std::nullopt;
    const int o = 1 - axis;
    std::vector<Point2> pts;
    if (seg) {
        const Point2 a = seg->first, b = seg->second, dv = b - a;
        if (std::abs(dv[axis]) < 1e-12) return std::nullopt;
        const double t = (value - a[axis]) / dv[axis];
        if (t < 0 || t > 1) return std::nullopt;
        pts.push_back(a + t * dv);
    } else {
        const Point2 c = circ->first;
        const double r = circ->second, dd = value - c[axis];
        if (std::abs(dd) > r) return std::nullopt;
        const double h = std::sqrt(r * r - dd * dd);
        for (double sg : {-1.0, 1.0}) {
            Point2 q;
            q[axis] = value;
            q[o] = c[o] + sg * h;
            if (arcEnds) {
                const auto n = snap::nearestOnArc(c, arcEnds->first, arcEnds->second, q);
                if (!n || (*n - q).norm() > 1e-9 * std::max(1.0, r)) continue;
            }
            pts.push_back(q);
        }
    }
    std::optional<Point2> best;
    for (const auto& q : pts)
        if (!best || (q - near).norm() < (*best - near).norm()) best = q;
    return best;
}

bool alignMidAllowed(const Point2& a, const Point2& b) {
    const Point2 v = b - a;
    const double l = v.norm();
    if (l <= 0) return false;
    const double s = std::sin(ALIGN_MID_AXIS_TOL_DEG * std::numbers::pi / 180);
    return std::abs(v.x()) <= s * l || std::abs(v.y()) <= s * l;
}

namespace {

// The nearest snap candidate with target levels (app-ux §6.4 «Body target priority», FX8-SK-010): rank −1 — sketch
// geometry, 0 — body geometry in the sketch plane (equal distance — the later one wins, as before), above — implicit
// targets in list order. A candidate of a later rank wins only if closer by more than SNAP_TIE_PX, of an earlier rank —
// if not farther than SNAP_TIE_PX.
struct Nearest {
    double limit;
    double best = 0;
    int rank = 0;
    bool any = false;
    explicit Nearest(double l) : limit(l), best(l) {}
    bool better(double dd, int r) const {
        if (dd > limit) return false;
        if (!any || r == rank) return dd <= best;
        return r < rank ? dd <= best + SNAP_TIE_PX : dd < best - SNAP_TIE_PX;
    }
    void take(double dd, int r) { any = true, best = dd, rank = r; }
};

// Rank of element i of body b in the list "shown bodies, then implicit target bodies" (see Nearest).
int bodyRank(size_t b, size_t nShown, size_t i) { return b < nShown ? 0 : 1 + int(b - nShown) * 1000000 + int(i); }

}  // namespace

Snap snapAt(const SketchGeometry& g, const std::vector<DrawBody>& bodies, const QPointF& s, bool suppress,
            const std::set<std::string>& exclude, const std::vector<DrawBody>* extra) {
    // The shown bodies and the implicit targets (projections of edges and vertices off the plane — FX8-SK-010;
    // silhouettes, extremes — FX8-SK-002) as one list; the implicit targets lie in the plane by construction.
    std::vector<std::reference_wrapper<const DrawBody>> all(bodies.begin(), bodies.end());
    if (extra) all.insert(all.end(), extra->begin(), extra->end());
    const size_t nShown = bodies.size();
    const double planeTol = SNAP_PLANE_TOL;
    Snap out;
    const auto* d = g.data();
    if (!g.valid()) return out;
    if (const auto lp = g.toLocal(s)) out.at = *lp;
    if (suppress) return out;
    auto dist = [&](const Point2& q) {
        const QPointF ps = g.toScreen(q);
        return std::hypot(ps.x() - s.x(), ps.y() - s.y());
    };
    // 1. To a point (including centers and the origin).
    double best = SNAP_POINT_PX;
    auto tryPoint = [&](const std::string& id) {
        if (exclude.count(id)) return;
        const double dd = dist(g.pointPos(id));
        if (dd <= best) {
            best = dd;
            out.kind = Snap::Kind::Point;
            out.target = id;
            out.at = g.pointPos(id);
        }
    };
    tryPoint("origin");
    for (const auto& e : d->entities)
        if (e.type == sketch::EntityType::Point) tryPoint(e.id);
    // Projection points only if closer than the sketch's own points: at the same location the sketch point wins
    // (closing a chain into its start, not into the projection beneath it).
    best = out.kind == Snap::Kind::None ? SNAP_POINT_PX : best - 1e-9;
    for (const auto& e : d->entities)
        if (isProjectedPoint(g, e)) tryPoint(e.id);
    if (out.kind != Snap::Kind::None) return out;
    // 1б. Body vertices in the sketch plane (after the sketch geometry, §6.4).
    const Plane& pl = g.base().frame;
    auto local = [&](const float* p) { return pl.toLocal(Point3(p[0], p[1], p[2])); };
    {
        Nearest nv(SNAP_POINT_PX);
        for (size_t bi = 0; bi < all.size(); ++bi) {
            const DrawBody& b = all[bi];
            for (size_t i = 0; i < b.mesh.vertices.size(); ++i) {
                const auto& v = b.mesh.vertices[i];
                if (!snap::pointInPlane(pl, Point3(v.position[0], v.position[1], v.position[2]), planeTol)) continue;
                const Point2 q = local(v.position.data());
                const double dd = dist(q);
                const int r = bodyRank(bi, nShown, i);
                if (nv.better(dd, r)) {
                    nv.take(dd, r);
                    out.kind = Snap::Kind::Point;
                    out.source = v.name;
                    out.at = q;
                }
            }
        }
    }
    if (out.kind != Snap::Kind::None) return out;
    // 1г. Intersection of two curves — sketch curves, projections and straight body edges in the plane (§6.4): the
    // point is placed on both curves. Curves no farther than SNAP_POINT_PX from the cursor are considered.
    {
        struct Cand {
            snap::Curve c;
            std::string id, source;
        };
        std::vector<Cand> near;
        const Point2 lp0 = out.at;
        auto addIfNear = [&](const snap::Curve& c, const std::string& id, const std::string& source) {
            std::optional<Point2> q;
            if (c.kind == snap::Curve::Kind::Segment) q = snap::nearestOnSegment(c.a, c.b, lp0);
            else if (c.kind == snap::Curve::Kind::Circle) q = snap::nearestOnCircle(c.c, c.r, lp0);
            else q = snap::nearestOnArc(c.c, c.start, c.end, lp0);
            if (q && dist(*q) <= SNAP_POINT_PX) near.push_back({c, id, source});
        };
        for (const auto& e : d->entities) {
            if (exclude.count(e.id)) continue;
            snap::Curve c;
            if (e.type == sketch::EntityType::Line) {
                if (exclude.count(e.p1) || exclude.count(e.p2)) continue;
                c.kind = snap::Curve::Kind::Segment, c.a = g.pointPos(e.p1), c.b = g.pointPos(e.p2);
            } else if (e.type == sketch::EntityType::Circle) {
                c.kind = snap::Curve::Kind::Circle, c.c = g.pointPos(e.center), c.r = g.radius(e);
            } else if (e.type == sketch::EntityType::Arc) {
                c.kind = snap::Curve::Kind::Arc, c.c = g.pointPos(e.center), c.start = g.pointPos(e.start), c.end = g.pointPos(e.end);
                c.r = (c.start - c.c).norm();
            } else if (e.type == sketch::EntityType::Projected) {
                const auto it = g.base().projections.find(e.id);
                if (it == g.base().projections.end()) continue;
                const auto& pg = it->second;
                if (pg.kind == sketch::Projection::Kind::Line) c.kind = snap::Curve::Kind::Segment, c.a = Point2(pg.x1, pg.y1), c.b = Point2(pg.x2, pg.y2);
                else if (pg.kind == sketch::Projection::Kind::Circle) c.kind = snap::Curve::Kind::Circle, c.c = Point2(pg.cx, pg.cy), c.r = pg.r;
                else if (pg.kind == sketch::Projection::Kind::Arc) {
                    c.kind = snap::Curve::Kind::Arc, c.c = Point2(pg.cx, pg.cy), c.start = Point2(pg.sx, pg.sy), c.end = Point2(pg.ex, pg.ey);
                    c.r = (c.start - c.c).norm();
                } else continue;
            } else continue;
            addIfNear(c, e.id, {});
        }
        for (const DrawBody& b : all)
            for (const auto& ed : b.mesh.edges) {
                if (ed.polylines.size() != 1 || ed.polylines[0].size() != 6) continue;
                const float* p = ed.polylines[0].data();
                if (!snap::pointInPlane(pl, Point3(p[0], p[1], p[2]), planeTol) ||
                    !snap::pointInPlane(pl, Point3(p[3], p[4], p[5]), planeTol))
                    continue;
                // An edge already projected into the sketch is represented by its projection.
                bool projected = false;
                for (const auto& e : d->entities) projected = projected || (e.type == sketch::EntityType::Projected && e.source == ed.name);
                if (projected) continue;
                snap::Curve c;
                c.a = local(p), c.b = local(p + 3);
                addIfNear(c, {}, ed.name);
            }
        best = SNAP_POINT_PX;
        for (size_t i = 0; i < near.size(); ++i)
            for (size_t j = i + 1; j < near.size(); ++j)
                for (const auto& q : snap::intersect(near[i].c, near[j].c)) {
                    const double dd = dist(q);
                    if (dd > best) continue;
                    best = dd;
                    out.kind = Snap::Kind::Intersection;
                    out.target = near[i].id, out.source = near[i].source;
                    out.target2 = near[j].id, out.source2 = near[j].source;
                    out.at = q;
                }
        if (out.kind != Snap::Kind::None) return out;
    }
    // 1в. To the center of a projection circle/arc (target) or a circular body edge in the plane (source), §6.4.
    Nearest nc(SNAP_POINT_PX);
    for (const auto& e : d->entities) {
        if (e.type != sketch::EntityType::Projected || exclude.count(e.id)) continue;
        const auto it = g.base().projections.find(e.id);
        if (it == g.base().projections.end()) continue;
        const auto& pg = it->second;
        if (pg.kind != sketch::Projection::Kind::Circle && pg.kind != sketch::Projection::Kind::Arc) continue;
        const Point2 c(pg.cx, pg.cy);
        if (const double dd = dist(c); nc.better(dd, -1)) nc.take(dd, -1), out.kind = Snap::Kind::Center, out.target = e.id, out.source.clear(), out.at = c;
    }
    for (size_t bi = 0; bi < all.size(); ++bi)
        for (size_t ei = 0; ei < all[bi].get().mesh.edges.size(); ++ei) {
            const auto& ed = all[bi].get().mesh.edges[ei];
            std::vector<Point3> pts;
            for (const auto& poly : ed.polylines)
                for (size_t i = 0; i + 2 < poly.size(); i += 3) pts.emplace_back(poly[i], poly[i + 1], poly[i + 2]);
            if (pts.size() < 3 || !snap::polylineInPlane(pl, pts, planeTol)) continue;
            // Circular edge: center from three polyline points, all points at the same radius.
            const Point2 p0 = pl.toLocal(pts.front()), p1 = pl.toLocal(pts[pts.size() / 2]), p2 = pl.toLocal(pts[pts.size() / 3]);
            const double dd2 = 2 * (p0.x() * (p1.y() - p2.y()) + p1.x() * (p2.y() - p0.y()) + p2.x() * (p0.y() - p1.y()));
            if (std::abs(dd2) < 1e-12) continue;
            const double s0 = p0.squaredNorm(), s1 = p1.squaredNorm(), s2 = p2.squaredNorm();
            const Point2 c((s0 * (p1.y() - p2.y()) + s1 * (p2.y() - p0.y()) + s2 * (p0.y() - p1.y())) / dd2,
                           (s0 * (p2.x() - p1.x()) + s1 * (p0.x() - p2.x()) + s2 * (p1.x() - p0.x())) / dd2);
            const double r = (p0 - c).norm();
            bool round = r > 0;
            for (const auto& q : pts) round = round && std::abs((pl.toLocal(q) - c).norm() - r) <= 1e-3 * std::max(1.0, r);
            if (!round) continue;
            const int rk = bodyRank(bi, nShown, ei);
            if (const double dd = dist(c); nc.better(dd, rk)) nc.take(dd, rk), out.kind = Snap::Kind::Center, out.target.clear(), out.source = ed.name, out.at = c;
        }
    if (out.kind != Snap::Kind::None) return out;
    // 2. To a segment midpoint.
    best = SNAP_POINT_PX;
    for (const auto& e : d->entities) {
        if (e.type != sketch::EntityType::Line || exclude.count(e.id)) continue;
        const Point2 m = (g.pointPos(e.p1) + g.pointPos(e.p2)) / 2;
        const double dd = dist(m);
        if (dd <= best) {
            best = dd;
            out.kind = Snap::Kind::Midpoint;
            out.target = e.id;
            out.at = m;
        }
    }
    if (out.kind != Snap::Kind::None) return out;
    // 2б. Midpoint of a straight body edge in the sketch plane (S1-SK-014): midpoint to a construction projection.
    Nearest nm(SNAP_POINT_PX);
    for (size_t bi = 0; bi < all.size(); ++bi)
        for (size_t ei = 0; ei < all[bi].get().mesh.edges.size(); ++ei) {
            const auto& ed = all[bi].get().mesh.edges[ei];
            if (ed.polylines.size() != 1 || ed.polylines[0].size() != 6) continue;
            const float* p = ed.polylines[0].data();
            if (!snap::pointInPlane(pl, Point3(p[0], p[1], p[2]), planeTol) ||
                !snap::pointInPlane(pl, Point3(p[3], p[4], p[5]), planeTol))
                continue;
            const Point2 m = (local(p) + local(p + 3)) / 2;
            const double dd = dist(m);
            const int rk = bodyRank(bi, nShown, ei);
            if (nm.better(dd, rk)) {
                nm.take(dd, rk);
                out.kind = Snap::Kind::Midpoint;
                out.source = ed.name;
                out.at = m;
            }
        }
    if (out.kind != Snap::Kind::None) return out;
    // Alignment targets 7 (S1-SK-014): midpoints of sketch segments and projection segments, midpoints of straight
    // edges and body vertices in the sketch plane, then the origin and sketch points — on equal distance the later one
    // wins (a point outranks a midpoint). The criterion is the screen deviation from at by x / y.
    auto collectAlign = [&](const Point2& at, Snap& o) {
        double bx = SNAP_ALIGN_PX, by = SNAP_ALIGN_PX;
        const QPointF sp = g.toScreen(at);
        auto tryAlign = [&](const std::string& id, const std::string& source, const Point2& q) {
            const QPointF qs = g.toScreen(q);
            if (std::abs(qs.x() - sp.x()) <= bx) bx = std::abs(qs.x() - sp.x()), o.alignX = q, o.alignXId = id, o.alignXSource = source;
            if (std::abs(qs.y() - sp.y()) <= by) by = std::abs(qs.y() - sp.y()), o.alignY = q, o.alignYId = id, o.alignYSource = source;
        };
        // A segment midpoint is an alignment target only for a segment parallel to a sketch axis (FX8-SK-003): the middle
        // of a slanted segment is not visible, and aligning with it silently defines a degree of freedom.
        auto tryAlignMid = [&](const std::string& id, const std::string& source, const Point2& a, const Point2& b) {
            if (alignMidAllowed(a, b)) tryAlign(id, source, (a + b) / 2);
        };
        for (const auto& e : d->entities) {
            if (exclude.count(e.id)) continue;
            if (e.type == sketch::EntityType::Line) {
                if (exclude.count(e.p1) || exclude.count(e.p2)) continue;
                tryAlignMid(e.id, {}, g.pointPos(e.p1), g.pointPos(e.p2));
            } else if (e.type == sketch::EntityType::Projected) {
                const auto it = g.base().projections.find(e.id);
                if (it != g.base().projections.end() && it->second.kind == sketch::Projection::Kind::Line)
                    tryAlignMid(e.id, {}, Point2(it->second.x1, it->second.y1), Point2(it->second.x2, it->second.y2));
            }
        }
        // On an equal deviation the later target wins: implicit targets go first, the later (deeper, less important) of
        // them first, the shown bodies (geometry in the plane) after them (§6.4 «Body target priority»).
        std::vector<std::pair<const DrawBody*, bool>> order;  // body, elements in reverse order
        for (size_t bi = all.size(); bi-- > nShown;) order.push_back({&all[bi].get(), true});
        for (size_t bi = 0; bi < nShown; ++bi) order.push_back({&all[bi].get(), false});
        auto inOrder = [](const auto& v, bool rev, const auto& fn) {
            if (rev)
                for (size_t i = v.size(); i-- > 0;) fn(v[i]);
            else
                for (const auto& x : v) fn(x);
        };
        for (const auto& [b, rev] : order)
            inOrder(b->mesh.edges, rev, [&](const io::EdgePolylines& ed) {
                // A straight edge is a single polyline of two points.
                if (ed.polylines.size() != 1 || ed.polylines[0].size() != 6) return;
                const float* p = ed.polylines[0].data();
                if (!snap::pointInPlane(pl, Point3(p[0], p[1], p[2]), planeTol) ||
                    !snap::pointInPlane(pl, Point3(p[3], p[4], p[5]), planeTol))
                    return;
                tryAlignMid({}, ed.name, local(p), local(p + 3));
            });
        for (const auto& [b, rev] : order)
            inOrder(b->mesh.vertices, rev, [&](const auto& v) {
                if (snap::pointInPlane(pl, Point3(v.position[0], v.position[1], v.position[2]), planeTol))
                    tryAlign({}, v.name, local(v.position.data()));
            });
        tryAlign("origin", {}, Point2::Zero());
        for (const auto& e : d->entities)
            if (e.type == sketch::EntityType::Point && !exclude.count(e.id)) tryAlign(e.id, {}, g.pointPos(e.id));
    };
    // 3. Onto a curve — the nearest point of the curve.
    best = SNAP_CURVE_PX;
    const Point2 lp = out.at;
    for (const auto& e : d->entities) {
        if (e.type == sketch::EntityType::Point || exclude.count(e.id)) continue;
        std::optional<Point2> q;
        if (e.type == sketch::EntityType::Projected) {
            const auto it = g.base().projections.find(e.id);
            if (it == g.base().projections.end()) continue;
            const sketch::Projection& pg = it->second;
            switch (pg.kind) {
                case sketch::Projection::Kind::Point: continue;
                case sketch::Projection::Kind::Line: q = snap::nearestOnSegment(Point2(pg.x1, pg.y1), Point2(pg.x2, pg.y2), lp); break;
                case sketch::Projection::Kind::Arc: q = snap::nearestOnArc(Point2(pg.cx, pg.cy), Point2(pg.sx, pg.sy), Point2(pg.ex, pg.ey), lp); break;
                case sketch::Projection::Kind::Circle: q = snap::nearestOnCircle(Point2(pg.cx, pg.cy), pg.r, lp); break;
            }
        } else if (e.type == sketch::EntityType::Line) {
            q = snap::nearestOnSegment(g.pointPos(e.p1), g.pointPos(e.p2), lp);
        } else if (e.type == sketch::EntityType::Arc) {
            q = snap::nearestOnArc(g.pointPos(e.center), g.pointPos(e.start), g.pointPos(e.end), lp);
        } else if (sketch::isSplineType(e.type)) {  // S4A-SPL-001: by its polyline
            const auto pl = g.splineEntityPolyline(e);
            for (size_t i = 1; i < pl.size(); ++i)
                if (const Point2 c = snap::nearestOnSegment(pl[i - 1], pl[i], lp); !q || (c - lp).norm() < (*q - lp).norm()) q = c;
        } else if (sketch::isEllipseType(e.type)) {  // S4-SKT-004, -007 (an arc — by its polyline)
            const auto el = g.ellipseOf(e.id);
            if (!el) continue;
            if (e.type == sketch::EntityType::Ellipse) {
                q = snap::nearestOnEllipse(el->first, el->second.rx, el->second.ry, el->second.angle, lp);
            } else {
                const auto pl = g.ellipseEntityPolyline(e, 256);
                for (size_t i = 1; i < pl.size(); ++i)
                    if (const Point2 c = snap::nearestOnSegment(pl[i - 1], pl[i], lp); !q || (c - lp).norm() < (*q - lp).norm()) q = c;
            }
        } else {
            q = snap::nearestOnCircle(g.pointPos(e.center), g.radius(e), lp);
        }
        if (!q) continue;
        const double dd = dist(*q);
        if (dd <= best) {
            best = dd;
            out.kind = Snap::Kind::Curve;
            out.target = e.id;
            out.at = *q;
        }
    }
    if (out.kind != Snap::Kind::None) {
        alignOnCurve(g, out, lp, s, collectAlign);
        return out;
    }
    // 3б. Body edges in the sketch plane (all polyline points in the plane): nearest polyline point;
    // the exact position on the curve is given by a pointOnObject constraint to the projection.
    Nearest nb(SNAP_CURVE_PX);
    for (size_t bi = 0; bi < all.size(); ++bi)
        for (size_t ei = 0; ei < all[bi].get().mesh.edges.size(); ++ei) {
            const auto& ed = all[bi].get().mesh.edges[ei];
            std::vector<Point3> pts;
            for (const auto& poly : ed.polylines)
                for (size_t i = 0; i + 2 < poly.size(); i += 3) pts.emplace_back(poly[i], poly[i + 1], poly[i + 2]);
            if (!snap::polylineInPlane(pl, pts, planeTol)) continue;
            const int rk = bodyRank(bi, nShown, ei);
            for (const auto& poly : ed.polylines) {
                std::vector<Point2> loc;
                for (size_t i = 0; i + 2 < poly.size(); i += 3) loc.push_back(local(&poly[i]));
                const auto q = snap::nearestOnPolyline(loc, lp);
                if (!q) continue;
                const double dd = dist(*q);
                if (nb.better(dd, rk)) {
                    nb.take(dd, rk);
                    out.kind = Snap::Kind::Curve;
                    out.source = ed.name;
                    out.at = *q;
                }
            }
        }
    if (out.kind != Snap::Kind::None) return out;
    // 7. Alignment with points and midpoints by x / y (priority, §6.4 rule М9).
    collectAlign(out.at, out);
    if (out.alignX) out.at.x() = out.alignX->x();
    if (out.alignY) out.at.y() = out.alignY->y();
    // 3в. Segment extension: if the point (after alignment) is near the segment's line outside it — onto the line; when
    // aligned along one axis — to the intersection of the alignment line with the line; when aligned along both — no.
    if (out.alignX && out.alignY) return out;
    best = SNAP_CURVE_PX;
    const QPointF cur = g.toScreen(out.at);
    for (const auto& e : d->entities) {
        if (e.type != sketch::EntityType::Line || exclude.count(e.id)) continue;
        const Point2 a = g.pointPos(e.p1), b = g.pointPos(e.p2), dv = b - a;
        if (dv.squaredNorm() <= 0) continue;
        Point2 q;
        if (out.alignX || out.alignY) {
            if (out.alignX) {  // x is fixed: the point of the line with this x
                if (std::abs(dv.x()) < 1e-12) continue;
                const double t = (out.at.x() - a.x()) / dv.x();
                q = a + t * dv;
            } else {
                if (std::abs(dv.y()) < 1e-12) continue;
                const double t = (out.at.y() - a.y()) / dv.y();
                q = a + t * dv;
            }
        } else {
            q = a + (out.at - a).dot(dv) / dv.squaredNorm() * dv;
        }
        const double t = (q - a).dot(dv) / dv.squaredNorm();
        if (t >= 0 && t <= 1) continue;  // on the segment itself — that is the "onto curve" snap
        const QPointF qs = g.toScreen(q);
        const double dd = std::hypot(qs.x() - cur.x(), qs.y() - cur.y());
        if (dd <= best) {
            best = dd;
            out.extLine = e.id;
            out.at = q;
        }
    }
    return out;
}

Hit hitAt(const SketchGeometry& g, const SketchAnnotations& a, const QPointF& s) {
    Hit h;
    if (!g.valid()) return h;
    const auto* d = g.data();
    for (const auto& c : d->constraints) {
        if (!sketch::isDimensionType(c.type)) continue;
        if (auto r = a.labelRect(c.id); r && r->contains(s)) return {Hit::Kind::Label, c.id};
    }
    // Points before constraint icons: an icon lying over a point does not hide it (§3.5).
    // Sketch points before projection points: at the same spot the own point wins (chain end above a body vertex),
    // a projection only if strictly closer (§3.5, S1-SK-019).
    double best = PICK_POINT_PX;
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& e : d->entities) {
            if (pass == 0 ? e.type != sketch::EntityType::Point : !isProjectedPoint(g, e)) continue;
            const QPointF p = g.toScreen(g.pointPos(e.id));
            const double dd = std::hypot(p.x() - s.x(), p.y() - s.y());
            if (dd <= best && (h.kind != Hit::Kind::Point || dd < best)) {
                best = dd;
                h = {Hit::Kind::Point, e.id};
            }
        }
    {
        const QPointF p = g.toScreen(Point2::Zero());
        const double dd = std::hypot(p.x() - s.x(), p.y() - s.y());
        if (h.kind == Hit::Kind::None && dd <= PICK_POINT_PX) h = {Hit::Kind::Point, "origin"}, best = dd;
    }
    // Cursor on a curve and no point right next to it — the curve (short segment midpoint — the segment itself, §3.5).
    if (h.kind == Hit::Kind::Point && best > PICK_POINT_PX / 2.0) {
        for (const auto& e : d->entities) {
            if (e.type == sketch::EntityType::Point) continue;
            if (e.type == sketch::EntityType::Projected && g.projectedScreen(e.id).size() < 2) continue;
            if (g.distToEntity(e, s) <= PICK_CURVE_PX) {
                h = {};
                break;
            }
        }
    }
    if (h.kind != Hit::Kind::None) return h;
    for (const auto& gl : a.glyphs())
        if (gl.rect.contains(s)) return {Hit::Kind::Label, gl.constraint};
    best = PICK_CURVE_PX;
    for (const auto& e : d->entities) {
        if (e.type == sketch::EntityType::Point) continue;
        if (e.type == sketch::EntityType::Projected && g.projectedScreen(e.id).size() < 2) continue;  // points: above
        const double dd = g.distToEntity(e, s);
        if (dd <= best) {
            best = dd;
            h = {Hit::Kind::Curve, e.id};
        }
    }
    if (h.kind != Hit::Kind::None) return h;
    // A text label is hit by any point inside the text extent too (app-ux §6.7, FX1-SKI-005).
    for (const auto& e : d->entities) {
        if (e.type != sketch::EntityType::Text) continue;
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const auto& pl : textPolylines(e, g.pointPos(e.center)))
            for (const auto& q : pl) {
                const QPointF qs = g.toScreen(q);
                x0 = std::min(x0, qs.x()), y0 = std::min(y0, qs.y()), x1 = std::max(x1, qs.x()), y1 = std::max(y1, qs.y());
            }
        if (x0 <= x1 && s.x() >= x0 - PICK_CURVE_PX && s.x() <= x1 + PICK_CURVE_PX && s.y() >= y0 - PICK_CURVE_PX && s.y() <= y1 + PICK_CURVE_PX)
            return {Hit::Kind::Curve, e.id};
    }
    // The dimension line of a dimension (not an extension line) grabs the dimension (app-ux §6.6, FX1-SKI-001).
    best = PICK_CURVE_PX;
    for (const auto& c : d->constraints) {
        const double dd = a.dimensionLineDistance(c, s);
        if (dd <= best) best = dd, h = {Hit::Kind::Label, c.id};
    }
    return h;
}

std::optional<QPointF> screenPosOfEntity(const SketchGeometry& g, const SketchAnnotations& a, const std::string& id) {
    if (!g.valid()) return std::nullopt;
    if (id == "origin") return g.toScreen(Point2::Zero());
    const auto* e = g.data()->findEntity(id);
    if (!e) return std::nullopt;
    if (e->type == sketch::EntityType::Point || isProjectedPoint(g, *e)) return g.toScreen(g.pointPos(id));
    // Curve: the first candidate point a click on which selects exactly it (not a label/icon, not a point;
    // app-ux §12, "admissible pixel"); candidates go from the middle towards the ends.
    std::vector<Point2> cands;
    const double fr[] = {0.5, 0.35, 0.65, 0.25, 0.75, 0.15, 0.85, 0.42, 0.58};
    if (e->type == sketch::EntityType::Line) {
        const Point2 p = g.pointPos(e->p1), q = g.pointPos(e->p2);
        for (double f : fr) cands.push_back(p + f * (q - p));
    } else if (e->type == sketch::EntityType::Circle || e->type == sketch::EntityType::Arc) {
        const Point2 c = g.pointPos(e->center);
        const double r = g.radius(*e);
        double a0 = PI / 4 - PI, sw = 2 * PI;  // for a circle, the middle is at 45°
        if (e->type == sketch::EntityType::Arc) {
            const Point2 s0 = g.pointPos(e->start) - c, s1 = g.pointPos(e->end) - c;
            a0 = std::atan2(s0.y(), s0.x());
            double a1 = std::atan2(s1.y(), s1.x());
            if (a1 <= a0) a1 += 2 * PI;
            sw = a1 - a0;
        }
        for (double f : fr) cands.push_back(c + r * Point2(std::cos(a0 + f * sw), std::sin(a0 + f * sw)));
    } else if (e->type == sketch::EntityType::Ellipse) {  // S4-SKT-004: ellipse points from 45° towards the ends
        const auto el = g.ellipseOf(id);
        if (!el) return std::nullopt;
        for (double f : fr) cands.push_back(ellipsePoint(el->first, el->second, PI / 4 + 2 * PI * (f - 0.5)));
    } else if (sketch::isSplineType(e->type)) {  // S4A-SPL-001: polyline points from the middle towards the ends
        const auto pl = g.splineEntityPolyline(*e);
        if (pl.size() < 2) return std::nullopt;
        for (double f : fr) cands.push_back(pl[static_cast<size_t>(std::lround(f * double(pl.size() - 1)))]);
    } else if (e->type == sketch::EntityType::EllipseArc) {  // S4-SKT-007: from the middle towards the ends
        const auto pl = g.ellipseEntityPolyline(*e, 200);
        if (pl.empty()) return std::nullopt;
        for (double f : fr) cands.push_back(pl[static_cast<size_t>(std::lround(f * 200))]);
    } else if (e->type == sketch::EntityType::Text) {
        // Text: points of its outlines (§6.11в).
        for (const auto& pl : textPolylines(*e, g.pointPos(e->center)))
            for (size_t i = 0; i + 1 < pl.size(); i += 3) cands.push_back((pl[i] + pl[i + 1]) / 2);
    } else if (e->type == sketch::EntityType::Projected) {
        // Projection curve: points of its screen polyline (from the middle towards the ends).
        const auto poly = g.projectedScreen(id);
        if (poly.size() < 2) return std::nullopt;
        for (double f : fr) {
            const double t = f * double(poly.size() - 1);
            const size_t i = std::min(size_t(t), poly.size() - 2);
            const QPointF sp = poly[i] + (poly[i + 1] - poly[i]) * (t - double(i));
            if (auto lq = g.toLocal(sp)) cands.push_back(*lq);
        }
        if (cands.empty()) return std::nullopt;
    } else {
        return std::nullopt;
    }
    for (const auto& q : cands) {
        const QPointF sp = g.toScreen(q);
        const QPointF rp(std::round(sp.x()), std::round(sp.y()));
        const Hit h = hitAt(g, a, rp);
        if (h.kind == Hit::Kind::Curve && h.id == id) return rp;
    }
    return g.toScreen(cands.front());
}

std::vector<DrawBody> bodyTargetsBodies(const std::vector<regen::BodyTarget>& targets, const Plane& frame) {
    std::vector<DrawBody> out(2);
    out[0].key = "sketch:edgeTargets";
    out[1].key = "sketch:bodyTargets";
    for (auto& b : out) b.pickable = false;
    auto world = [&](double u, double v) {
        const Point3 p = frame.origin + u * frame.x + v * frame.y;
        return std::array<float, 3>{float(p.x()), float(p.y()), float(p.z())};
    };
    for (const auto& t : targets) {
        DrawBody& b = out[t.revolution ? 1 : 0];
        const auto& g = t.geometry;
        io::EdgePolylines e;
        e.name = t.source;
        std::vector<float> poly;
        auto add = [&](double u, double v) {
            const auto w = world(u, v);
            poly.insert(poly.end(), w.begin(), w.end());
        };
        switch (g.kind) {
            case sketch::Projection::Kind::Point: b.mesh.vertices.push_back({t.source, world(g.px, g.py)}); continue;
            case sketch::Projection::Kind::Line: add(g.x1, g.y1), add(g.x2, g.y2); break;
            case sketch::Projection::Kind::Circle:
            case sketch::Projection::Kind::Arc: {
                double a0 = 0, sweep = 2 * std::numbers::pi;
                if (g.kind == sketch::Projection::Kind::Arc) {
                    a0 = std::atan2(g.sy - g.cy, g.sx - g.cx);
                    sweep = geom2::ccwSweepPositive(a0, std::atan2(g.ey - g.cy, g.ex - g.cx));
                }
                const int n = std::max(TARGET_ARC_MIN_SEGMENTS, int(std::ceil(sweep / (2 * std::numbers::pi) * TARGET_CIRCLE_SEGMENTS)));
                for (int i = 0; i <= n; ++i) {
                    const double a = a0 + sweep * i / n;
                    add(g.cx + g.r * std::cos(a), g.cy + g.r * std::sin(a));
                }
                break;
            }
        }
        e.polylines = {std::move(poly)};
        b.mesh.edges.push_back(std::move(e));
    }
    return out;
}

ToolTarget toolTargetAt(bool dimension, const std::vector<std::string>& picked, const ToolTargetSources& src) {
    ToolTarget out;
    Hit& h = out.hit;
    h = src.sketch();
    // A sketch text is its anchor point (constraints and dimensions reference only it, sketch.md §2.1).
    if (h.kind == Hit::Kind::Curve)
        if (const auto* te = src.entity(h.id); te && te->type == sketch::EntityType::Text) h = {Hit::Kind::Point, te->center};
    // No sketch entity under the cursor — a body edge or vertex (§6.9, М9; §6.8): label "srce:"/"srcv:" + name; the
    // projection is created when the dimension / constraint is placed.
    if (h.kind == Hit::Kind::None && (!dimension || picked.size() < 2)) {
        const PickTarget t = src.view();
        if (t.kind == PickTarget::Kind::Edge) h = {Hit::Kind::Curve, "srce:" + t.name}, out.view = t;
        if (t.kind == PickTarget::Kind::Vertex) h = {Hit::Kind::Point, "srcv:" + t.name}, out.view = t;
        // Silhouette line or extreme point of a body of revolution (FX8-SK-002) — like a body edge / vertex.
        if (h.kind == Hit::Kind::None)
            if (const auto bt = src.body()) h = {bt->second ? Hit::Kind::Point : Hit::Kind::Curve, (bt->second ? "srcv:" : "srce:") + bt->first};
        // A sketch axis is an implicit target (S1-SK-021): for a dimension — as the first object or the second after a
        // point (otherwise a click near the axis places the dimension).
        const auto* first = picked.size() == 1 && picked[0] != "origin" ? src.entity(picked[0]) : nullptr;
        const bool needSecond = picked.size() == 1 && (picked[0] == "origin" || picked[0].rfind("srcv:", 0) == 0 ||
                                                       (first && first->type == sketch::EntityType::Point));
        // A reference axis feature (FX9-SK-001) goes before the sketch axes, by the same rules.
        if (h.kind == Hit::Kind::None && (!dimension || picked.empty() || needSecond))
            if (const std::string ra = src.refAxis(); !ra.empty()) h = {Hit::Kind::Curve, "srce:refaxis:" + ra};
        if (h.kind == Hit::Kind::None && (!dimension || picked.empty() || needSecond))
            if (const std::string a = src.axis(); !a.empty()) h = {Hit::Kind::Curve, "srce:" + a};
    }
    return out;
}

std::string toolTargetId(const ToolTarget& t) {
    return t.hit.kind == Hit::Kind::Point || t.hit.kind == Hit::Kind::Curve ? t.hit.id : std::string();
}

}  // namespace construe::app
