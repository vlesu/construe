// Decomposing the body core into extrusions or a revolution (spec/reconstruction.md §4.5).
#include "rec/rec_internal.h"

#include <algorithm>
#include <limits>

namespace construe::rec::detail {

using SK = kernel::SurfaceInfo::Kind;
using C2 = kernel::Curve2::Kind;

using Curves = std::vector<kernel::Curve2>;

// Gluing pieces of one line or circle at degree-2 nodes (a section cuts faces differently at different heights).
Curves mergePieces(Curves cs) {
    auto deg = [&](const Point2& p) {
        int n = 0;
        for (const auto& c : cs) {
            if (c.kind == C2::Line) n += ((c.p1 - p).norm() <= REC_LIN_TOL * 10) + ((c.p2 - p).norm() <= REC_LIN_TOL * 10);
            if (c.kind == C2::Arc) n += ((c.start - p).norm() <= REC_LIN_TOL * 10) + ((c.end - p).norm() <= REC_LIN_TOL * 10);
            if (c.kind == C2::Spline) n += ((c.p1 - p).norm() <= REC_LIN_TOL * 10) + ((c.p2 - p).norm() <= REC_LIN_TOL * 10);
        }
        return n;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < cs.size() && !changed; ++i)
            for (size_t j = 0; j < cs.size() && !changed; ++j) {
                if (i == j) continue;
                auto& a = cs[i];
                auto& b = cs[j];
                if (a.kind == C2::Line && b.kind == C2::Line) {
                    // Common end, collinear.
                    for (int ea = 0; ea < 2 && !changed; ++ea)
                        for (int eb = 0; eb < 2 && !changed; ++eb) {
                            const Point2 pa = ea ? a.p2 : a.p1, pb = eb ? b.p2 : b.p1;
                            if ((pa - pb).norm() > REC_LIN_TOL * 10 || deg(pa) != 2) continue;
                            const Point2 qa = ea ? a.p1 : a.p2, qb = eb ? b.p1 : b.p2;
                            const Vec2 u = (pa - qa).normalized(), v = (qb - pa).normalized();
                            if (std::abs(u.x() * v.y() - u.y() * v.x()) > 1e-9 || u.dot(v) < 0) continue;
                            a.p1 = qa, a.p2 = qb;
                            cs.erase(cs.begin() + long(j));
                            changed = true;
                        }
                } else if (a.kind == C2::Arc && b.kind == C2::Arc && (a.center - b.center).norm() <= REC_LIN_TOL * 10 &&
                           std::abs(a.radius - b.radius) <= REC_LIN_TOL * 10 && (a.end - b.start).norm() <= REC_LIN_TOL * 10 &&
                           deg(a.end) == 2) {
                    if ((b.end - a.start).norm() <= REC_LIN_TOL * 10) {
                        a.kind = C2::Circle;  // closed
                    } else {
                        a.end = b.end;
                    }
                    cs.erase(cs.begin() + long(j));
                    changed = true;
                }
            }
    }
    return cs;
}

namespace {

bool sameSet(const Curves& a, const Curves& b) {
    if (a.size() != b.size()) return false;
    for (const auto& x : a) {
        bool f = false;
        for (const auto& y : b) f = f || sameCurve(x, y, REC_LIN_TOL * 10);
        if (!f) return false;
    }
    return true;
}

// a ⊆ b; extra is the curves of b that are not in a.
bool subset(const Curves& a, const Curves& b, Curves& extra) {
    extra.clear();
    for (const auto& x : a) {
        bool f = false;
        for (const auto& y : b) f = f || sameCurve(x, y, REC_LIN_TOL * 10);
        if (!f) return false;
    }
    for (const auto& y : b) {
        bool f = false;
        for (const auto& x : a) f = f || sameCurve(x, y, REC_LIN_TOL * 10);
        if (!f) extra.push_back(y);
    }
    return !extra.empty();
}

// Converting curves from local coordinates of plane from to to (the planes are parallel; the normal may be reversed).
Curves remap(const Curves& cs, const Plane& from, const Plane& to) {
    Curves out;
    const bool flip = from.normal.dot(to.normal) < 0;
    auto m = [&](const Point2& p) { return to.toLocal(from.toWorld(p)); };
    for (auto c : cs) {
        c.p = m(c.p), c.p1 = m(c.p1), c.p2 = m(c.p2), c.center = m(c.center);
        const Point2 s = m(c.start), e = m(c.end);
        c.start = flip ? e : s;
        c.end = flip ? s : e;
        out.push_back(c);
    }
    return out;
}

// End face levels along d; false means the direction is unsuitable (§4.5 item 1).
bool levelsFor(const Topo& t, const Vec3& d, std::vector<double>& lv) {
    lv.clear();
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        if (si.kind == SK::Plane) {
            if (parallel(si.dir, d)) {
                const double z = si.point.dot(d);
                bool seen = false;
                for (double x : lv) seen = seen || std::abs(x - z) <= REC_LIN_TOL * 10;
                if (!seen) lv.push_back(z);
            } else if (!perpendicular(si.dir, d)) {
                return false;
            }
        } else if (si.kind == SK::Cylinder) {
            if (!parallel(si.dir, d)) return false;
        } else if (!t.ruledAlong(f, d)) {  // spline wall ∥ d (S4A-SPL-004)
            return false;
        }
    }
    std::sort(lv.begin(), lv.end());
    return lv.size() >= 2;
}

// Draft extrusion along d (§4.5 item 1а): like levelsFor, but planes tilted from ∥ d by an angle in
// (REC_ANGLE_TOL, REC_DRAFT_MAX] are allowed (at least one) — the drafted faces; each must reach both levels.
bool draftLevelsFor(const Topo& t, const Vec3& d, std::vector<double>& lv, std::vector<int>& tilted) {
    lv.clear();
    tilted.clear();
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        if (si.kind == SK::Plane) {
            if (parallel(si.dir, d)) {
                const double z = si.point.dot(d);
                bool seen = false;
                for (double x : lv) seen = seen || std::abs(x - z) <= REC_LIN_TOL * 10;
                if (!seen) lv.push_back(z);
            } else if (!perpendicular(si.dir, d)) {
                const double tilt = std::asin(std::min(1.0, std::abs(si.dir.normalized().dot(d.normalized()))));
                if (tilt > REC_DRAFT_MAX) return false;
                tilted.push_back(f);
            }
        } else if (si.kind == SK::Cylinder) {
            if (!parallel(si.dir, d)) return false;
        } else if (si.kind == SK::Cone && parallel(si.dir, d) && si.halfAngle > REC_ANGLE_TOL && si.halfAngle <= REC_DRAFT_MAX) {
            tilted.push_back(f);  // a drafted cylinder (S4-REC-023)
        } else {
            return false;
        }
    }
    std::sort(lv.begin(), lv.end());
    if (lv.size() != 2 || tilted.empty()) return false;
    for (int f : tilted) {
        bool lo = false, hi = false;
        for (int le : t.faceLE[size_t(f)]) {
            const double z = t.edgePoint(le).dot(d);
            lo = lo || std::abs(z - lv[0]) <= REC_LIN_TOL * 10;
            hi = hi || std::abs(z - lv[1]) <= REC_LIN_TOL * 10;
        }
        if (!lo || !hi) return false;
    }
    return true;
}

std::optional<Curves> sectionAt(const kernel::Kernel& k, const kernel::Shape& s, const Plane& p) {
    auto r = k.planeSection(s, p);
    if (!r || r->empty()) return std::nullopt;
    return mergePieces(*r);
}

Op extrudeOp(const Plane& p, Curves cs, double depth, bool cut, bool reverse) {
    Op op;
    op.kind = Op::Kind::Extrude;
    op.plane = p;
    op.curves = std::move(cs);
    op.depth = depth;
    op.cut = cut;
    op.reverse = reverse;
    return op;
}

std::vector<Vec3> candidateDirs(const Topo& t) {
    std::vector<Vec3> dirs = {Vec3::UnitY(), Vec3::UnitZ(), Vec3::UnitX()};
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        if (si.kind != SK::Plane && si.kind != SK::Cylinder) continue;
        const Vec3 d = canonicalAxis(si.dir);
        bool seen = false;
        for (const auto& x : dirs) seen = seen || parallel(x, d);
        if (!seen) dirs.push_back(d);
    }
    return dirs;
}

// Extrusion layers along d (§4.5 item 3).
std::optional<std::vector<Op>> slabs(const kernel::Kernel& k, const kernel::Shape& s, const Vec3& d, const std::vector<double>& lv) {
    struct Slab {
        double z0, z1;
        Curves c;  // in the plane planeFrom(z·d, d) (the basis does not depend on z)
    };
    std::vector<Slab> sl;
    for (size_t i = 0; i + 1 < lv.size(); ++i) {
        auto c = sectionAt(k, s, planeFrom(((lv[i] + lv[i + 1]) / 2) * d, d));
        if (!c) return std::nullopt;
        if (!sl.empty() && sameSet(sl.back().c, *c)) sl.back().z1 = lv[i + 1];
        else sl.push_back({lv[i], lv[i + 1], *c});
    }
    std::vector<Op> cuts;
    for (bool changed = true; changed && sl.size() >= 2;) {
        changed = false;
        Curves extra;
        // Pocket on top.
        if (subset(sl[sl.size() - 2].c, sl.back().c, extra)) {
            const double top = sl.back().z1, bnd = sl.back().z0;
            cuts.push_back(extrudeOp(planeFrom(top * d, d), extra, top - bnd, true, true));
            sl[sl.size() - 2].z1 = top;
            sl.pop_back();
            changed = true;
            continue;
        }
        // Pocket on the bottom.
        if (subset(sl[1].c, sl[0].c, extra)) {
            const double bot = sl[0].z0, bnd = sl[0].z1;
            const Plane up = planeFrom(bot * d, d), down = planeFrom(bot * d, -d);
            cuts.push_back(extrudeOp(down, remap(extra, up, down), bnd - bot, true, true));
            sl[1].z0 = bot;
            sl.erase(sl.begin());
            changed = true;
        }
    }
    std::vector<Op> ops;
    for (const auto& x : sl) ops.push_back(extrudeOp(planeFrom(x.z0 * d, d), x.c, x.z1 - x.z0, false, false));
    // Cuts come after the main extrusions, in reverse order of discovery (the outer pocket, found last, goes first).
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) ops.push_back(*it);
    return ops;
}

// Outline of the end faces ⊥ d at level z: projections of their edges onto P, glued (§4.5 items 1, 1а).
std::optional<Curves> capOutline(const kernel::Kernel& k, const Topo& t, const Vec3& d, double z, const Plane& P) {
    Curves cs;
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        if (si.kind != SK::Plane || !parallel(si.dir, d) || std::abs(si.point.dot(d) - z) > REC_LIN_TOL * 10) continue;
        for (int le : t.faceLE[size_t(f)]) {
            auto c = k.projectEdge(t.s, le, P);
            if (!c) return std::nullopt;
            cs.push_back(*c);
        }
    }
    if (cs.empty()) return std::nullopt;
    return mergePieces(cs);
}

// Extrusion and draft (§4.5 item 1а): the profile is the outline of the end faces at the lower level z0 (exact: the faces
// tilted from ∥ d meet z0 along the lines of the profile), the neutral plane is z0; one draft operation per pull direction
// and angle.
std::optional<std::vector<Op>> draftExtrude(const kernel::Kernel& k, const Topo& t, const Vec3& d, const std::vector<double>& lv,
                                            const std::vector<int>& tilted) {
    const Plane P0 = planeFrom(lv[0] * d, d);
    auto outline = capOutline(k, t, d, lv[0], P0);
    if (!outline) return std::nullopt;
    std::vector<Op> ops = {extrudeOp(P0, *outline, lv[1] - lv[0], false, false)};
    // Drafted faces grouped by pull direction (±d) and angle.
    for (int f : tilted) {
        // A point of the face on the neutral plane: the middle of its edge at level z0; the outward normal there (a
        // cone — a drafted cylinder — has the same tilt at every point).
        Point3 at = Point3::Zero();
        bool found = false;
        for (int le : t.faceLE[size_t(f)])
            if (!found && std::abs(t.edgePoint(le).dot(d) - lv[0]) <= REC_LIN_TOL * 10) at = t.edgePoint(le), found = true;
        if (!found) return std::nullopt;
        const Vec3 m = t.surf[size_t(f)].kind == SK::Plane ? Vec3(t.surf[size_t(f)].dir.normalized()) : Vec3(k.faceNormalAt(t.s, f, at).normalized());
        const double md = m.dot(d.normalized());
        const bool rev = md < 0;  // the face leans outward along d: pull against d
        const double ang = std::asin(std::min(1.0, std::abs(md)));
        const Vec3 m0 = (m - md * d.normalized()).normalized();  // outward normal of the face before the draft
        Op* g = nullptr;
        for (size_t i = 1; i < ops.size(); ++i)
            if (ops[i].reverse == rev && std::abs(ops[i].angle - ang) <= REC_ANGLE_TOL) g = &ops[i];
        if (!g) {
            Op op;
            op.kind = Op::Kind::Draft;
            op.plane = P0;
            op.reverse = rev;
            op.angle = ang;
            ops.push_back(op);
            g = &ops.back();
        }
        g->points.push_back(at);
        g->normals.push_back(m0);
    }
    return ops;
}

}  // namespace

// Half profile (§4.5 item 2): the section by plane p through the axis (p.y — the axis, p.origin on it) in the half-plane
// x ≥ 0 — segments and arcs crossing the axis are cut at it — closed by segments along the axis between the points on
// it (ordered by y, joined in pairs); nullopt — no section or an odd number of points on the axis.
std::optional<Curves> halfProfile(const kernel::Kernel& k, const kernel::Shape& s, const Plane& p) {
    auto sec = k.planeSection(s, p, true);
    if (!sec || sec->empty()) return std::nullopt;
    const double tol = REC_LIN_TOL * 10;
    Curves half;
    std::vector<double> axisY;
    auto onAxis = [&](const Point2& q) {
        if (std::abs(q.x()) <= tol) axisY.push_back(q.y());
    };
    for (auto c : mergePieces(*sec)) {
        if (c.kind == C2::Line) {
            const bool in1 = c.p1.x() >= -tol, in2 = c.p2.x() >= -tol;
            if (!in1 && !in2) continue;
            if (in1 != in2) {
                const Point2 q = c.p1 + (c.p2 - c.p1) * (c.p1.x() / (c.p1.x() - c.p2.x()));
                (in1 ? c.p2 : c.p1) = Point2(0, q.y());
            }
            if (std::abs(c.p1.x()) <= tol) c.p1.x() = 0;
            if (std::abs(c.p2.x()) <= tol) c.p2.x() = 0;
            if ((c.p2 - c.p1).norm() <= tol) continue;
            if (c.p1.x() == 0 && c.p2.x() == 0) continue;  // on the axis: not a face (never happens)
            onAxis(c.p1), onAxis(c.p2);
            half.push_back(c);
        } else if (c.kind == C2::Spline) {
            // A meridian of a free-form surface of revolution: the half x ≥ 0 by its ends.
            if (c.p1.x() < -tol || c.p2.x() < -tol) continue;
            onAxis(c.p1), onAxis(c.p2);
            half.push_back(c);
        } else if (c.kind == C2::Circle || c.kind == C2::Arc) {
            if (std::abs(c.center.x()) <= tol) {
                // Sphere: the part of the arc in x ≥ 0 is from −π/2 to π/2 (counterclockwise).
                const double r = c.radius;
                auto ang = [&](const Point2& q) { return std::atan2(q.y() - c.center.y(), q.x() - c.center.x()); };
                double s0 = -PI / 2, s1 = PI / 2;
                if (c.kind == C2::Arc) {
                    double as = ang(c.start), ae = ang(c.end);
                    if (ae <= as) ae += 2 * PI;
                    // Intersection of [as, ae] with [−π/2, π/2] (taking a 2π shift into account).
                    bool found = false;
                    for (double sh : {-2 * PI, 0.0, 2 * PI}) {
                        const double lo = std::max(as, -PI / 2 + sh), hi = std::min(ae, PI / 2 + sh);
                        if (hi - lo > 1e-9) s0 = lo - sh, s1 = hi - sh, found = true;
                    }
                    if (!found) continue;
                }
                c.kind = C2::Arc;
                c.center.x() = 0;
                c.start = c.center + r * Point2(std::cos(s0), std::sin(s0));
                c.end = c.center + r * Point2(std::cos(s1), std::sin(s1));
                if (std::abs(c.start.x()) <= tol) c.start.x() = 0;
                if (std::abs(c.end.x()) <= tol) c.end.x() = 0;
                onAxis(c.start), onAxis(c.end);
                half.push_back(c);
            } else if (c.center.x() > 0) {
                half.push_back(c);
            }
        }
    }
    std::sort(axisY.begin(), axisY.end());
    std::vector<double> ys;
    for (double y : axisY)
        if (ys.empty() || std::abs(y - ys.back()) > tol) ys.push_back(y);
    if (ys.size() % 2) return std::nullopt;
    for (size_t i = 0; i + 1 < ys.size(); i += 2) {
        kernel::Curve2 l;
        l.kind = C2::Line;
        l.p1 = Point2(0, ys[i]);
        l.p2 = Point2(0, ys[i + 1]);
        half.push_back(l);
    }
    if (half.empty()) return std::nullopt;
    return half;
}

std::optional<kernel::Profile> profileFromCurves(const Plane& p, const Curves& curves) {
    auto seg = [&](kernel::Loop3& loop, const kernel::Curve2& c, bool rev, int& n) {
        kernel::Segment3 sg;
        sg.id = "r" + std::to_string(++n);
        sg.reversed = rev;
        if (c.kind == C2::Line) {
            sg.kind = kernel::Segment3::Kind::Line;
            sg.p1 = p.toWorld(c.p1), sg.p2 = p.toWorld(c.p2);
        } else if (c.kind == C2::Spline) {
            sg.kind = kernel::Segment3::Kind::Spline;
            for (const auto& q : c.spline.poles) sg.poles.push_back(p.toWorld(q));
            sg.weights = c.spline.weights, sg.knots = c.spline.knots, sg.mults = c.spline.mults, sg.degree = c.spline.degree;
        } else {
            sg.kind = c.kind == C2::Arc ? kernel::Segment3::Kind::Arc : kernel::Segment3::Kind::Circle;
            sg.center = p.toWorld(c.center), sg.radius = c.radius;
            sg.start = p.toWorld(c.start), sg.end = p.toWorld(c.end);
        }
        loop.segments.push_back(sg);
    };
    auto ends = [](const kernel::Curve2& c) { return c.kind == C2::Arc ? std::make_pair(c.start, c.end) : std::make_pair(c.p1, c.p2); };
    const double tol = REC_LIN_TOL * 100;
    std::vector<bool> used(curves.size(), false);
    std::vector<kernel::Loop3> loops;
    std::vector<double> sizes;
    int n = 0;
    for (size_t i = 0; i < curves.size(); ++i) {
        const auto& c0 = curves[i];
        if (used[i]) continue;
        if (c0.kind != C2::Line && c0.kind != C2::Arc && c0.kind != C2::Circle && c0.kind != C2::Spline) return std::nullopt;
        used[i] = true;
        kernel::Loop3 loop;
        Eigen::AlignedBox2d box;
        auto grow = [&](const kernel::Curve2& c) {
            if (c.kind == C2::Line) box.extend(c.p1), box.extend(c.p2);
            else if (c.kind == C2::Spline) for (const auto& q : c.spline.poles) box.extend(q);
            else box.extend(Point2(c.center + Point2(c.radius, c.radius))), box.extend(Point2(c.center - Point2(c.radius, c.radius)));
        };
        grow(c0);
        seg(loop, c0, false, n);
        if (c0.kind != C2::Circle) {
            const Point2 first = ends(c0).first;
            Point2 cur = ends(c0).second;
            while ((cur - first).norm() > tol) {
                bool found = false;
                for (size_t j = 0; j < curves.size() && !found; ++j) {
                    if (used[j] || curves[j].kind == C2::Circle) continue;
                    if (curves[j].kind != C2::Line && curves[j].kind != C2::Arc && curves[j].kind != C2::Spline) return std::nullopt;
                    const auto [a, b] = ends(curves[j]);
                    if ((a - cur).norm() <= tol) used[j] = found = true, seg(loop, curves[j], false, n), grow(curves[j]), cur = b;
                    else if ((b - cur).norm() <= tol) used[j] = found = true, seg(loop, curves[j], true, n), grow(curves[j]), cur = a;
                }
                if (!found) return std::nullopt;
            }
        }
        loops.push_back(std::move(loop));
        sizes.push_back(box.sizes().prod());
    }
    if (loops.empty()) return std::nullopt;
    size_t outer = 0;
    for (size_t i = 1; i < loops.size(); ++i)
        if (sizes[i] > sizes[outer]) outer = i;
    kernel::Profile pr;
    pr.plane = p;
    pr.outer = loops[outer];
    for (size_t i = 0; i < loops.size(); ++i)
        if (i != outer) pr.holes.push_back(loops[i]);
    return pr;
}

std::optional<kernel::Shape> revolveSolid(const kernel::Kernel& k, const Plane& p, const Curves& curves, const Point3& origin,
                                          const Vec3& dir) {
    auto pr = profileFromCurves(p, curves);
    if (!pr || !pr->holes.empty()) return std::nullopt;
    auto r = k.revolve(*pr, kernel::Axis{origin, dir.normalized()}, kernel::RevolveExtent{});
    if (!r || k.counts(r->shape).solids != 1) return std::nullopt;
    return r->shape;
}

std::optional<kernel::Shape> applyOp(const kernel::Kernel& k, const kernel::Shape& body, const Op& op) {
    auto pr = profileFromCurves(op.plane, op.curves);
    if (!pr || !kernel::validateProfile(*pr)) return std::nullopt;  // loops side by side are no outer loop with holes
    Result<kernel::OpResult> tool = Error("KERNEL_INVALID_ARGUMENT");
    if (op.kind == Op::Kind::Revolve) {
        kernel::RevolveExtent ext;
        if (op.sector > 0) ext.kind = kernel::RevolveExtent::Kind::Symmetric, ext.value = op.sector;
        tool = k.revolve(*pr, kernel::Axis{op.axisOrigin, op.axisDir.normalized()}, ext);
    } else if (op.kind == Op::Kind::Extrude) {
        const auto mp = k.massProperties(body);
        const double reach = (mp.centroid - op.plane.origin).norm() + std::sqrt(mp.area) * 4 + 10;
        kernel::Extent ext{kernel::Extent::Kind::Blind, op.depth, {}};
        if (op.both) ext = kernel::Extent{kernel::Extent::Kind::Symmetric, reach * 4, {}};
        else if (op.through) ext.depth = reach * 2;
        tool = k.extrude(*pr, ext, op.reverse);
    } else {
        return std::nullopt;
    }
    if (!tool) return std::nullopt;
    auto r = k.boolean(op.cut ? kernel::BoolKind::Subtract : kernel::BoolKind::Union, body, tool->shape);
    if (!r) return std::nullopt;
    return r->shape;
}

namespace {

// Revolution (§4.5 item 2).
std::optional<Op> revolveOp(const kernel::Kernel& k, const Topo& t) {
    Point3 o = Point3::Zero();
    Vec3 a = Vec3::Zero();
    bool any = false;
    for (int f = 0; f < t.faces() && !any; ++f)
        if (t.surf[size_t(f)].kind != SK::Cone) any = revolutionAxis(t.surf[size_t(f)], o, a);
    for (int f = 0; f < t.faces() && !any; ++f) any = revolutionAxis(t.surf[size_t(f)], o, a);
    // Only a sphere: any axis through its center.
    if (!any && t.faces() == 1 && t.surf[0].kind == SK::Sphere) o = t.surf[0].point, a = Vec3::UnitY(), any = true;
    if (!any) return std::nullopt;
    a = canonicalAxis(a);
    std::vector<int> sides;  // planes through the axis: the ends of a sector
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        Point3 o2;
        Vec3 a2;
        if (si.kind == SK::Plane) {
            if (perpendicular(si.dir, a) && distToLine(si.point, o, a) <= axisTol()) sides.push_back(f);
            else if (!parallel(si.dir, a)) return std::nullopt;
        } else if (si.kind == SK::Sphere) {
            if (distToLine(si.point, o, a) > axisTol()) return std::nullopt;
        } else if (si.kind == SK::Cone) {
            if (!coneOnAxis(o, a, si.point, si.dir)) return std::nullopt;
        } else if (revolutionAxis(si, o2, a2)) {
            if (!coaxial(o, a, o2, a2)) return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    const Point3 oAxis = o - o.dot(a) * a;  // projection of the coordinate origin onto the axis
    // x is the built-in axis most orthogonal to a.
    Vec3 e = Vec3::UnitX();
    double best = 2;
    for (const Vec3& c : {Vec3(Vec3::UnitX()), Vec3(Vec3::UnitY()), Vec3(Vec3::UnitZ())})
        if (std::abs(c.dot(a)) < best - 1e-12) best = std::abs(c.dot(a)), e = c;
    Plane p;
    p.origin = oAxis;
    p.y = a;
    p.x = (e - e.dot(a) * a).normalized();
    double sector = 0;
    if (!sides.empty()) {
        // Every other face reaches an end of the sector: otherwise the sector is only a part of the body.
        for (int f = 0; f < t.faces(); ++f) {
            if (std::find(sides.begin(), sides.end(), f) != sides.end()) continue;
            bool reaches = false;
            for (int le : t.faceLE[size_t(f)]) reaches = reaches || std::find(sides.begin(), sides.end(), t.other(le, f)) != sides.end();
            if (!reaches) return std::nullopt;
        }
        // A sector: two half-planes through the axis; the sketch is on the bisector, the revolution is symmetric to it.
        std::vector<std::pair<Vec3, Vec3>> ends;  // radial direction of an end half-plane, outward normal of its face
        for (int f : sides) {
            const Point3 c = t.k.closestPointOnFace(t.s, f, t.k.faceCentroid(t.s, f));
            const Vec3 r = (c - oAxis) - (c - oAxis).dot(a) * a;
            if (r.norm() < REC_LIN_TOL) return std::nullopt;
            const Vec3 x = r.normalized();
            bool seen = false;
            for (const auto& q : ends) seen = seen || (q.first - x).norm() <= 1e-6;
            if (!seen) ends.push_back({x, t.k.faceNormalAt(t.s, f, c).normalized()});
        }
        if (ends.size() != 2) return std::nullopt;
        // From the end whose material lies counterclockwise about a.
        if (ends[0].second.dot(a.cross(ends[0].first)) > 0) std::swap(ends[0], ends[1]);
        double th = std::atan2(a.dot(ends[0].first.cross(ends[1].first)), ends[0].first.dot(ends[1].first));
        if (th <= 0) th += 2 * PI;
        sector = th;
        p.x = rotateAbout(ends[0].first, a, th / 2).normalized();
    }
    p.normal = p.x.cross(p.y).normalized();
    auto half = halfProfile(k, t.s, p);
    if (!half) return std::nullopt;
    Op op;
    op.kind = Op::Kind::Revolve;
    op.plane = p;
    op.curves = *half;
    op.axisOrigin = oAxis;
    op.axisDir = a;
    op.sector = sector;
    return op;
}

}  // namespace

std::optional<Op> revolveBase(const kernel::Kernel& k, const kernel::Shape& s) {
    Topo t(k, s);
    return revolveOp(k, t);
}

std::vector<Op> decomposeBase(const kernel::Kernel& k, const kernel::Shape& core, std::string& kind, std::vector<Note>& notes, bool simple) {
    kind = "none";
    Topo t(k, core);
    const auto dirs = candidateDirs(t);
    std::vector<double> lv;
    if (simple) {
        // 1. A single extrusion; 1а. an extrusion with drafted faces (per direction, before the next direction).
        for (const auto& d : dirs) {
            if (levelsFor(t, d, lv) && lv.size() == 2) {
                auto c = sectionAt(k, core, planeFrom(((lv[0] + lv[1]) / 2) * d, d));
                // A section of spline walls is not a line or circle (kernel-interface §3.20): the outline of the lower
                // end faces, as for a draft (S4A-SPL-004).
                if (!c) c = capOutline(k, t, d, lv[0], planeFrom(lv[0] * d, d));
                if (!c) continue;
                kind = "extrude";
                return {extrudeOp(planeFrom(lv[0] * d, d), *c, lv[1] - lv[0], false, false)};
            }
            std::vector<int> tilted;
            if (draftLevelsFor(t, d, lv, tilted))
                if (auto ops = draftExtrude(k, t, d, lv, tilted)) {
                    kind = "extrude";
                    return *ops;
                }
        }
        // 2. Revolution.
        if (auto r = revolveOp(k, t)) {
            kind = "revolve";
            return {*r};
        }
        return {};
    }
    // 3. Layers: the direction with the fewest levels.
    std::optional<Vec3> bestD;
    std::vector<double> bestLv;
    for (const auto& d : dirs)
        if (levelsFor(t, d, lv) && (!bestD || lv.size() < bestLv.size())) bestD = d, bestLv = lv;
    if (bestD)
        if (auto ops = slabs(k, core, *bestD, bestLv)) {
            kind = "extrude";
            return *ops;
        }
    notes.push_back({"base", "core not decomposed"});
    return {};
}

}  // namespace construe::rec::detail
