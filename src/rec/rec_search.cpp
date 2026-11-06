// Hypothesis tree over the construction history (spec/reconstruction.md §4.10; FX10-REC-002). When the obvious removals
// end without a simple base, the recognizer guesses «here may be a cut / an extrusion / a revolution», applies the guess
// backwards and goes on from the simpler body; the simplest bodies are deepened first, the others wait in the queue.
#include "rec/rec_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>

namespace construe::rec::detail {

namespace {

using SK = kernel::SurfaceInfo::Kind;
using C2 = kernel::Curve2::Kind;

// Search trace for the dataset cycle (FX10-REC-003): CONSTRUE_REC_TRACE=1, to stderr.
bool tracing() {
    static const bool on = std::getenv("CONSTRUE_REC_TRACE") != nullptr;
    return on;
}
std::string describe(const std::vector<Op>& ops) {
    std::string out;
    for (const auto& o : ops) {
        char buf[96];
        if (o.kind == Op::Kind::Fillet || o.kind == Op::Kind::Chamfer) {
            std::snprintf(buf, sizeof buf, "%s(%zu edges, %.4g) ", o.kind == Op::Kind::Fillet ? "fillet" : "chamfer", o.edges.size(), o.size2);
            out += buf;
            continue;
        }
        std::snprintf(buf, sizeof buf, "%s%s(%zu curves, depth %.4g) ", o.kind == Op::Kind::Revolve ? "revolve" : "extrude", o.cut ? "-cut" : "-add", o.curves.size(), o.depth);
        out += buf;
    }
    return out;
}

void traceBody(const kernel::Kernel& k, const kernel::Shape& s) {
    Topo t(k, s);
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        static const char* names[] = {"plane", "cyl", "cone", "sphere", "torus", "other"};
        std::fprintf(stderr, "    f%d %s p(%.4g %.4g %.4g) d(%.4g %.4g %.4g) r %.4g area %.5g convex %d edges", f, names[int(si.kind)], si.point.x(), si.point.y(), si.point.z(),
                     si.dir.x(), si.dir.y(), si.dir.z(), si.radius, k.faceArea(s, f), int(si.convex));
        for (int le : t.faceLE[size_t(f)]) std::fprintf(stderr, " %d%s", t.other(le, f), concaveEdge(t, le) ? "v" : "");
        std::fprintf(stderr, "\n");
    }
}

}  // namespace

void tracePlan(const kernel::Kernel& k, const Plan& plan) {
    if (!tracing()) return;
    std::fprintf(stderr, "plan: base %s, baseOps %zu\n", plan.baseKind.c_str(), plan.baseOps);
    for (size_t i = 0; i < plan.ops.size(); ++i) {
        const Op& o = plan.ops[i];
        std::fprintf(stderr, "  op %zu kind %d cut %d rev %d through %d both %d depth %.5g curves %zu points %zu target %d plane o(%.5g %.5g %.5g) n(%.4g %.4g %.4g) after V %.6g resume %zu\n", i, int(o.kind),
                     int(o.cut), int(o.reverse), int(o.through), int(o.both), o.depth, o.curves.size(), o.points.size(), o.target, o.plane.origin.x(), o.plane.origin.y(), o.plane.origin.z(),
                     o.plane.normal.x(), o.plane.normal.y(), o.plane.normal.z(), k.massProperties(plan.after[i]).volume, plan.resume[i]);
        if (std::getenv("CONSTRUE_REC_TRACE_CURVES"))
            for (const auto& c : o.curves)
                std::fprintf(stderr, "    curve %d p1(%.6g %.6g) p2(%.6g %.6g) c(%.6g %.6g) r %.6g s(%.6g %.6g) e(%.6g %.6g)\n", int(c.kind), c.p1.x(), c.p1.y(), c.p2.x(), c.p2.y(),
                             c.center.x(), c.center.y(), c.radius, c.start.x(), c.start.y(), c.end.x(), c.end.y());
    }
}

namespace {

// Plan cost (§4.10 item 5): operations weigh 1, sketch curves REC_SEARCH_CURVE_COST each.
double opCost(const Op& op) {
    return 1 + REC_SEARCH_CURVE_COST * double(op.curves.size() + op.curves2.size());
}
double opsCost(const std::vector<Op>& ops) {
    double c = 0;
    for (const auto& o : ops) c += opCost(o);
    return c;
}
double peelCost(const Peel& p) {
    double c = opsCost(p.holes);
    for (const auto& st : p.steps) c += opsCost(st.ops);
    for (const auto& t : p.texts) c += opCost(t.first);
    if (p.loft) c += opCost(p.loft->first);
    return c;
}

// Simplicity of a body (§4.10 item 3): faces plus logical edges.
int simplicity(const kernel::Kernel& k, const kernel::Shape& s) {
    return k.counts(s).faces + int(kernel::canonicalTopology(k, s).logicalEdges.size());
}

// Work spent (§4.10 item 7): kernel attempts weighed by the faces of the body they run on.
struct Work {
    long spent = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool timeUp() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > REC_SEARCH_MAX_SECONDS * REC_TIME_SCALE; }
    bool over() const { return spent > REC_SEARCH_MAX_WORK || stopped() || timeUp(); }
};

bool extrudeFace(const Topo& t, int f, const Vec3& d) {
    const auto& si = t.surf[size_t(f)];
    if (si.kind == SK::Plane) return parallel(si.dir, d) || perpendicular(si.dir, d);
    if (si.kind == SK::Cylinder) return parallel(si.dir, d);
    return si.kind == SK::Other && t.ruledAlong(f, d);
}

bool revolveFace(const Topo& t, int f, const Point3& o, const Vec3& a) {
    const auto& si = t.surf[size_t(f)];
    Point3 o2;
    Vec3 a2;
    if (si.kind == SK::Plane) return parallel(si.dir, a);
    if (si.kind == SK::Sphere) return distToLine(si.point, o, a) <= axisTol();
    if (si.kind == SK::Cone) return coneOnAxis(o, a, si.point, si.dir);
    return revolutionAxis(si, o2, a2) && coaxial(o, a, o2, a2);
}

// Directions (§4.10 item 2 а): normals of planes and axes of cylinders, in face order, by decreasing total area.
std::vector<Vec3> searchDirs(const Topo& t) {
    std::vector<std::pair<Vec3, double>> dirs;
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        if (si.kind != SK::Plane && si.kind != SK::Cylinder) continue;
        const Vec3 d = canonicalAxis(si.dir);
        const double area = t.k.faceArea(t.s, f);
        bool seen = false;
        for (auto& x : dirs)
            if (parallel(x.first, d)) x.second += area, seen = true;
        if (!seen) dirs.push_back({d, area});
    }
    std::stable_sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) { return a.second > b.second * (1 + 1e-9); });
    std::vector<Vec3> out;
    for (const auto& x : dirs) out.push_back(x.first);
    if (out.size() > size_t(REC_SEARCH_MAX_DIRS)) out.resize(size_t(REC_SEARCH_MAX_DIRS));
    return out;
}

struct AxisLine {
    Point3 o;
    Vec3 a;
};
std::vector<AxisLine> searchAxes(const Topo& t) {
    std::vector<AxisLine> out;
    for (int f = 0; f < t.faces(); ++f) {
        Point3 o;
        Vec3 a;
        if (!revolutionAxis(t.surf[size_t(f)], o, a)) continue;
        bool seen = false;
        for (const auto& x : out) seen = seen || coaxial(x.o, x.a, o, a);
        if (!seen) out.push_back({o, canonicalAxis(a)});
    }
    if (out.size() > size_t(REC_SEARCH_MAX_DIRS)) out.resize(size_t(REC_SEARCH_MAX_DIRS));
    return out;
}

// Radius of a sphere around the center of mass that holds the body.
double bodyReach(const kernel::Kernel& k, const Topo& t, const Point3& c) {
    double r = 0;
    for (const auto& e : t.ct.logicalEdges)
        for (int v : e.ends) r = std::max(r, (k.vertexPoint(t.s, v) - c).norm());
    for (int f = 0; f < t.faces(); ++f) r = std::max(r, (k.faceCentroid(t.s, f) - c).norm());
    for (const auto& si : t.surf) r = std::max(r, si.radius + (si.kind == SK::Plane ? 0 : (si.point - c).norm()));
    return r * 2 + 10;
}

struct Child {
    std::vector<Op> ops;      // built on restored, they give the node's core
    kernel::Shape restored;
};

double volumeOf(const kernel::Kernel& k, const kernel::Shape& s) { return k.massProperties(s).volume; }

// A prism with curved ends (§4.10 item 4 б): the walls of the solid T are ∥ d, its ends are the surface of the body. A tool
// from the planar cap (or through both ways, if there is no cap) gives the same result: a keyway in a shaft, a boss on a
// curved face. S — the body with the removed volume: restored for a cut, the body itself for an addition.
std::optional<Op> prismWithEnds(const kernel::Kernel& k, const kernel::Shape& T, const kernel::Shape& restored, bool cut, Work& w) {
    Topo t(k, T);
    const auto dirs = searchDirs(t);
    const double reach = bodyReach(k, Topo(k, restored), k.massProperties(T).centroid);
    const double vT = volumeOf(k, T);
    for (const auto& d : dirs) {
        if (tracing()) std::fprintf(stderr, "  prism d (%.3g %.3g %.3g)\n", d.x(), d.y(), d.z()), std::fflush(stderr);
        std::vector<double> capLevels;
        std::vector<int> capFaces;
        int walls = 0, ends = 0;
        for (int f = 0; f < t.faces(); ++f) {
            const auto& si = t.surf[size_t(f)];
            if (si.kind == SK::Plane && parallel(si.dir, d)) {
                capFaces.push_back(f);
                capLevels.push_back(si.point.dot(d));
            } else if (extrudeFace(t, f, d)) {
                ++walls;
            } else {
                ++ends;
            }
        }
        if (walls == 0 || ends == 0) continue;
        std::sort(capLevels.begin(), capLevels.end());
        capLevels.erase(std::unique(capLevels.begin(), capLevels.end(), [](double a, double b) { return std::abs(a - b) <= REC_LIN_TOL * 10; }), capLevels.end());
        if (capLevels.size() > 1) continue;
        // Extent along d and the widest section.
        double lo = std::numeric_limits<double>::max(), hi = -lo;
        std::vector<double> levels;
        for (const auto& e : t.ct.logicalEdges)
            for (int v : e.ends) levels.push_back(k.vertexPoint(T, v).dot(d));
        for (int f = 0; f < t.faces(); ++f) levels.push_back(k.closestPointOnFace(T, f, k.faceCentroid(T, f)).dot(d));
        // The extreme points of the edges: an intersection curve bulges between its vertices.
        for (int le = 0; le < int(t.ct.logicalEdges.size()); ++le)
            for (const double sgn : {1.0, -1.0})
                if (auto q = k.extremePoint(T, kernel::CurveSet{le, -1, Vec3::Zero()}, sgn * d)) levels.push_back(q->dot(d));
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end(), [](double a, double b) { return std::abs(a - b) <= REC_LIN_TOL * 10; }), levels.end());
        if (levels.size() < 2) continue;
        lo = levels.front(), hi = levels.back();
        std::vector<kernel::Curve2> curves;
        double best = -1;
        for (size_t i = 0; i + 1 < levels.size(); ++i) {
            auto sec = k.planeSection(T, planeFrom(((levels[i] + levels[i + 1]) / 2) * d, d));
            ++w.spent;
            if (!sec || sec->empty()) continue;
            auto pr = profileFromCurves(planeFrom(((levels[i] + levels[i + 1]) / 2) * d, d), mergePieces(*sec));
            if (!pr) continue;
            auto info = kernel::validateProfile(*pr);
            if (info && info->area > best * (1 + 1e-9)) best = info->area, curves = mergePieces(*sec);
        }
        if (curves.empty()) continue;
        Op op;
        op.kind = Op::Kind::Extrude;
        op.cut = cut;
        kernel::Extent ext{kernel::Extent::Kind::Blind, reach * 4, {}};
        if (capLevels.empty()) {
            if (!cut) continue;  // an addition needs a cap to start from
            op.plane = planeFrom(((lo + hi) / 2) * d, d);
            op.through = op.both = true;
            ext.kind = kernel::Extent::Kind::Symmetric;
        } else {
            // From the cap towards the open ends.
            const double zc = capLevels[0];
            const Vec3 u = zc <= (lo + hi) / 2 ? d : Vec3(-d);
            op.plane = planeFrom(zc * d, u);
            if (u.dot(d) < 0) {
                // The same curves seen from the other side: section once more in that plane.
                auto sec = k.planeSection(T, planeFrom(((lo + hi) / 2) * d, u));
                if (!sec || sec->empty()) continue;
                curves = mergePieces(*sec);
            }
            if (cut) {
                op.through = true;
            } else {
                op.depth = (zc <= (lo + hi) / 2 ? hi - zc : zc - lo);
                ext.depth = op.depth;
            }
        }
        op.curves = curves;
        auto pr = profileFromCurves(op.plane, op.curves);
        if (!pr || !kernel::validateProfile(*pr)) continue;
        w.spent += t.faces();
        if (tracing()) std::fprintf(stderr, "  prism tool\n"), std::fflush(stderr);
        auto tool = k.extrude(*pr, ext, false);
        if (!tool) continue;
        // The tool takes exactly the volume of T from the restored body (cut), or adds exactly it.
        auto res = cut ? k.boolean(kernel::BoolKind::Intersect, restored, tool->shape) : k.boolean(kernel::BoolKind::Subtract, tool->shape, restored);
        if (!res || res->shape.null()) continue;
        if (std::abs(volumeOf(k, res->shape) - vT) > REC_MATCH_DIST * k.massProperties(T).area) continue;
        return op;
    }
    return std::nullopt;
}

// A piece of a revolved cut (§4.10 item 4 в): the corners of a hexagon under a conical chamfer are pieces of one ring. The profile
// is the section of the piece by the half-plane through the axis and its center of mass.
std::optional<Op> ringOfPiece(const kernel::Kernel& k, const kernel::Shape& T, const std::vector<AxisLine>& axes) {
    if (tracing()) std::fprintf(stderr, "  ring\n"), std::fflush(stderr);
    Topo t(k, T);
    const Point3 c = k.massProperties(T).centroid;
    for (const auto& ax : axes) {
        bool coaxialFace = false;
        for (int f = 0; f < t.faces(); ++f) coaxialFace = coaxialFace || (t.surf[size_t(f)].kind != SK::Plane && revolveFace(t, f, ax.o, ax.a));
        if (!coaxialFace) continue;
        const Point3 origin = ax.o - ax.o.dot(ax.a) * ax.a;
        const Vec3 r = (c - origin) - (c - origin).dot(ax.a) * ax.a;
        if (r.norm() < REC_LIN_TOL * 100) continue;
        Plane p;
        p.origin = origin;
        p.y = ax.a;
        p.x = r.normalized();
        p.normal = p.x.cross(p.y).normalized();
        auto sec = k.planeSection(T, p, true);
        if (!sec || sec->empty()) continue;
        std::vector<kernel::Curve2> half;
        for (const auto& q : mergePieces(*sec)) {
            const Point2 at = q.kind == C2::Line ? Point2((q.p1 + q.p2) / 2) : q.kind == C2::Spline ? q.p1 : q.center;
            if (q.kind == C2::Arc ? q.start.x() > 0 : at.x() > 0) half.push_back(q);
        }
        if (half.empty() || !profileFromCurves(p, half)) continue;
        Op op;
        op.kind = Op::Kind::Revolve;
        op.plane = p;
        op.curves = half;
        op.axisOrigin = origin;
        op.axisDir = ax.a;
        op.cut = true;
        return op;
    }
    return std::nullopt;
}

bool sameProfile(const Op& a, const Op& b) {
    if (a.kind != Op::Kind::Revolve || b.kind != Op::Kind::Revolve || a.curves.size() != b.curves.size()) return false;
    if (!parallel(a.axisDir, b.axisDir) || distToLine(b.axisOrigin, a.axisOrigin, a.axisDir) > REC_LIN_TOL * 100) return false;
    for (const auto& c : a.curves) {
        bool found = false;
        for (const auto& e : b.curves) found = found || sameCurve(c, e, REC_LIN_TOL * 100);
        if (!found) return false;
    }
    return true;
}

// Extrusions of one plane, direction and end among the operations of a hypothesis are one operation with all the contours.
std::vector<Op> sameLevelTogether(std::vector<Op> ops) {
    std::vector<Op> out;
    for (auto& o : ops) {
        bool joined = false;
        for (auto& a : out) {
            if (a.kind != Op::Kind::Extrude || o.kind != Op::Kind::Extrude || a.cut != o.cut || a.through != o.through || a.both != o.both || a.reverse != o.reverse) continue;
            if (std::abs(a.depth - o.depth) > REC_LIN_TOL * 100 || (a.plane.origin - o.plane.origin).norm() > REC_LIN_TOL * 100) continue;
            if ((a.plane.normal - o.plane.normal).norm() > REC_ANGLE_TOL * 100) continue;
            a.curves.insert(a.curves.end(), o.curves.begin(), o.curves.end());
            joined = true;
            break;
        }
        if (!joined) out.push_back(std::move(o));
    }
    return out;
}

// The operations of a removed volume (§4.10 item 4): every solid of T is one extrusion or one revolution; restored — the body
// with the volume T (a cut) or the body without it (an addition).
std::optional<std::vector<Op>> deltaOps(const kernel::Kernel& k, const kernel::Shape& T, const kernel::Shape& restored, bool cut, Work& w) {
    std::vector<kernel::Shape> parts;
    if (k.counts(T).solids == 1) parts.push_back(T);
    else
        for (auto& [sol, faces] : k.components(T)) parts.push_back(sol);
    if (parts.empty() || int(parts.size()) > REC_SEARCH_MAX_PIECES) return std::nullopt;
    std::vector<Op> ops;
    for (const auto& p : parts) {
        std::string kind;
        std::vector<Note> notes;
        if (tracing()) std::fprintf(stderr, "  piece: %d faces\n", k.counts(p).faces), std::fflush(stderr);
        auto base = decomposeBase(k, p, kind, notes, true);
        if (tracing()) std::fprintf(stderr, "  piece base %zu\n", base.size()), std::fflush(stderr);
        if (base.size() == 1) {
            base[0].cut = cut;
            ops.push_back(base[0]);
        } else if (auto prism = prismWithEnds(k, p, restored, cut, w)) {
            ops.push_back(*prism);
        } else if (auto ring = cut ? ringOfPiece(k, p, searchAxes(Topo(k, p))) : std::nullopt) {
            // The pieces of one ring are one operation.
            if (std::none_of(ops.begin(), ops.end(), [&](const Op& o) { return sameProfile(o, *ring); })) ops.push_back(*ring);
        } else {
            return std::nullopt;
        }
    }
    return ops;
}

// The operations built on restored give the body s (§4.10 item 4): by the kernel, tool after tool.
bool opsGive(const kernel::Kernel& k, const kernel::Shape& restored, const std::vector<Op>& ops, const kernel::Shape& s, Work& w) {
    kernel::Shape cur = restored;
    for (const auto& op : ops) {
        w.spent += k.counts(cur).faces;
        auto r = applyOp(k, cur, op);
        if (!r) return false;
        cur = *r;
    }
    const auto ma = k.massProperties(s), mb = k.massProperties(cur);
    return std::abs(ma.volume - mb.volume) <= REC_MATCH_REL * ma.volume + ma.area * REC_MATCH_SKIN * 10 &&
           std::abs(ma.area - mb.area) <= REC_MATCH_REL * 10 * ma.area;
}

// A hypothesis from a body with the same outline: restored is the body before the operation.
std::optional<Child> childFrom(const kernel::Kernel& k, const kernel::Shape& s, const kernel::Shape& restored, Work& w) {
    const int nf = k.counts(s).faces;
    w.spent += nf;
    const double v0 = volumeOf(k, s), v1 = volumeOf(k, restored);
    if (!(v1 > 0) || std::abs(v1 - v0) <= REC_REMOVE_MIN_REL * v0) return std::nullopt;
    const bool cut = v1 > v0;
    auto T = cut ? k.boolean(kernel::BoolKind::Subtract, restored, s) : k.boolean(kernel::BoolKind::Subtract, s, restored);
    if (!T || T->shape.null() || k.counts(T->shape).solids < 1) return std::nullopt;
    auto ops = deltaOps(k, T->shape, restored, cut, w);
    if (!ops || !opsGive(k, restored, *ops, s, w)) return std::nullopt;  // before merging: separate contours are separate tools
    return Child{splitPatterns(sameLevelTogether(std::move(*ops))), restored};
}

// Groups of faces connected by the edges for which join is true, among the allowed faces.
std::vector<std::vector<int>> components(const Topo& t, const std::vector<char>& allowed, const std::function<bool(int)>& join) {
    const int n = t.faces();
    std::vector<int> parent(static_cast<size_t>(n));
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) { return parent[size_t(x)] == x ? x : parent[size_t(x)] = find(parent[size_t(x)]); };
    for (size_t le = 0; le < t.ct.logicalEdges.size(); ++le) {
        const auto [f, g] = t.ct.logicalEdges[le].faces;
        if (f == g || !allowed[size_t(f)] || !allowed[size_t(g)] || !join(int(le))) continue;
        parent[size_t(find(f))] = find(g);
    }
    std::map<int, std::vector<int>> by;
    for (int f = 0; f < n; ++f)
        if (allowed[size_t(f)]) by[find(f)].push_back(f);
    std::vector<std::vector<int>> out;
    for (auto& [r, v] : by) out.push_back(std::move(v));
    return out;
}

// Rectangle profile around the point c in plane p, half-size h.
kernel::Profile squareProfile(const Plane& p, double h) {
    kernel::Profile pr;
    pr.plane = p;
    const Point2 q[4] = {Point2(-h, -h), Point2(h, -h), Point2(h, h), Point2(-h, h)};
    for (int i = 0; i < 4; ++i) {
        kernel::Segment3 sg;
        sg.id = "s" + std::to_string(i + 1);
        sg.kind = kernel::Segment3::Kind::Line;
        sg.p1 = p.toWorld(q[i]);
        sg.p2 = p.toWorld(q[(i + 1) % 4]);
        pr.outer.segments.push_back(sg);
    }
    return pr;
}

// Levels of the planes ⊥ d.
std::vector<double> planeLevels(const Topo& t, const Vec3& d) {
    std::vector<double> lv;
    for (const auto& si : t.surf)
        if (si.kind == SK::Plane && parallel(si.dir, d)) lv.push_back(si.point.dot(d));
    std::sort(lv.begin(), lv.end());
    lv.erase(std::unique(lv.begin(), lv.end(), [](double a, double b) { return std::abs(a - b) <= REC_LIN_TOL * 10; }), lv.end());
    return lv;
}

// Finishing applied forward (§4.10 item 2 г): the edges of the envelope E missing from the body are rounded or chamfered, the
// kernel fillets them on E; the sizes come from the faces of S next to the edges. nullopt — the result is not S.
std::optional<std::vector<Op>> finishForward(const kernel::Kernel& k, const kernel::Shape& s, const kernel::Shape& E, Work& w, kernel::Shape& finished) {
    Topo ts(k, s);
    std::vector<Op> ops;
    kernel::Shape cur = E;
    for (int pass = 0; pass < 2; ++pass) {
        Topo te(k, cur);
        w.spent += te.faces();
        std::map<long, std::vector<int>> fillets, chamfers;  // size in 1e-6 mm → edges
        for (size_t le = 0; le < te.ct.logicalEdges.size(); ++le) {
            if (te.smooth(int(le))) continue;  // a seam between pieces of one surface left by the booleans, no edge at all
            const Point3 p = te.edgePoint(int(le));
            const double d = k.distanceToShape(s, p);
            if (d <= REC_MATCH_DIST) continue;
            // The nearest face of S names the finishing.
            int best = -1;
            double bestD = std::numeric_limits<double>::max();
            for (int f = 0; f < ts.faces(); ++f) {
                const double x = k.distanceToFace(s, f, p);
                if (x < bestD - 1e-9) bestD = x, best = f;
            }
            if (best < 0) return std::nullopt;
            const auto& si = ts.surf[size_t(best)];
            if (si.kind == SK::Plane) chamfers[std::lround(d * std::sqrt(2.0) * 1e6)].push_back(int(le));
            else if (si.kind == SK::Cylinder || si.kind == SK::Sphere) fillets[std::lround(si.radius * 1e6)].push_back(int(le));
            else if (si.kind == SK::Torus) fillets[std::lround(si.minorRadius * 1e6)].push_back(int(le));
            else return std::nullopt;
        }
        if (fillets.empty() && chamfers.empty()) break;
        const bool isFillet = !fillets.empty();
        const auto& groups = isFillet ? fillets : chamfers;
        std::vector<kernel::EdgeRef> refs;
        for (auto it = groups.rbegin(); it != groups.rend(); ++it) {
            Op op;
            op.kind = isFillet ? Op::Kind::Fillet : Op::Kind::Chamfer;
            op.size2 = double(it->first) * 1e-6;
            op.edges = edgeKeys(te, it->second);
            ops.push_back(op);
            for (int le : it->second) {
                kernel::EdgeRef r;
                r.edge = le;
                r.label = "e" + std::to_string(le);
                r.size = op.size2;
                refs.push_back(r);
            }
        }
        w.spent += 4 * te.faces();
        auto r = isFillet ? k.fillet(cur, refs, refs[0].size) : k.chamfer(cur, refs, refs[0].size);
        if (tracing()) std::fprintf(stderr, "  forward %s %zu edges size %.4g: %s\n", isFillet ? "fillet" : "chamfer", refs.size(), refs[0].size, r ? "ok" : r.error().code.c_str());
        if (!r) return std::nullopt;
        cur = r->shape;
    }
    if (ops.empty()) return std::nullopt;
    finished = cur;
    return ops;
}

// Hypothesis в (§4.10 item 2): the body is the envelope E (an extrusion or a revolution) plus added and minus cut solids.
void envelopeChild(const kernel::Kernel& k, const kernel::Shape& s, const kernel::Shape& E, Work& w, std::vector<Child>& out) {
    w.spent += 2 * k.counts(s).faces;
    const double vs = volumeOf(k, s), ve = volumeOf(k, E);
    if (!(ve > 0)) return;
    auto U = k.boolean(kernel::BoolKind::Subtract, s, E);
    auto V = k.boolean(kernel::BoolKind::Subtract, E, s);
    if (!U || !V) return;
    const double tiny = REC_REMOVE_MIN_REL * 1000 * vs;
    const bool hasU = !U->shape.null() && k.counts(U->shape).solids >= 1 && volumeOf(k, U->shape) > tiny;
    const bool hasV = !V->shape.null() && k.counts(V->shape).solids >= 1 && volumeOf(k, V->shape) > tiny;
    if (tracing()) std::fprintf(stderr, "  envelope V(E) %.5g V(S) %.5g U %d V %d\n", ve, vs, int(hasU), int(hasV));
    if (!hasU && !hasV) {
        out.push_back({{}, E});  // the body is this revolution, in surfaces no rule reads
        return;
    }
    std::vector<Op> ops;
    if (hasU) {
        auto a = deltaOps(k, U->shape, E, false, w);
        if (!a) return;
        ops = std::move(*a);
    }
    if (hasV) {
        auto c = deltaOps(k, V->shape, E, true, w);
        if (c) ops.insert(ops.end(), c->begin(), c->end());
        else ops.clear();
    }
    if (!ops.empty()) {
        if (opsGive(k, E, ops, s, w)) {
            out.push_back({splitPatterns(sameLevelTogether(std::move(ops))), E});
            return;
        }
    }
    if (hasU) return;
    kernel::Shape finished;
    auto f = finishForward(k, s, E, w, finished);
    if (tracing()) std::fprintf(stderr, "  finish forward %d\n", int(f.has_value()));
    if (!f) return;
    const auto ma = k.massProperties(s), mb = k.massProperties(finished);
    const double tol = ma.area * REC_MATCH_DIST;
    if (std::abs(ma.volume - mb.volume) > tol || std::abs(ma.area - mb.area) > tol * 100) {
        // Pockets beyond the finishing: what is left is taken from the finished body.
        auto D = k.boolean(kernel::BoolKind::Subtract, finished, s);
        if (!D || D->shape.null() || k.counts(D->shape).solids < 1) return;
        auto rest = deltaOps(k, D->shape, finished, true, w);
        if (!rest || !opsGive(k, finished, *rest, s, w)) return;
        f->insert(f->end(), rest->begin(), rest->end());
    }
    out.push_back({std::move(*f), E});
}

// Envelopes of the body (§4.10 item 2 в): for an axis — the revolution of the half profile free of foreign faces; for a
// direction — the extrusion of a section over the whole extent.
void proposeEnvelopes(const kernel::Kernel& k, const kernel::Shape& s, const Topo& t, const std::vector<Vec3>& dirs, const std::vector<AxisLine>& axes, Work& w,
                      std::vector<Child>& out) {
    const double tol = REC_LIN_TOL * 10 + axisTol();
    const double total = k.massProperties(s).area;
    for (const auto& ax : axes) {
        if (w.over()) return;
        std::vector<int> compat;
        double area = 0;
        for (int f = 0; f < t.faces(); ++f)
            if (revolveFace(t, f, ax.o, ax.a)) compat.push_back(f), area += k.faceArea(s, f);
        if (area < total * REC_SEARCH_ENVELOPE_AREA) continue;  // the axis of a rounded corner is no axis of the body
        const Point3 origin = ax.o - ax.o.dot(ax.a) * ax.a;
        Vec3 e = Vec3::UnitX();
        double best = 2;
        for (const Vec3& c : {Vec3(Vec3::UnitX()), Vec3(Vec3::UnitY()), Vec3(Vec3::UnitZ())})
            if (std::abs(c.dot(ax.a)) < best - 1e-12) best = std::abs(c.dot(ax.a)), e = c;
        const Vec3 x0 = (e - e.dot(ax.a) * ax.a).normalized(), y0 = ax.a.cross(x0).normalized();
        bool tried = false;
        for (int j = 0; j <= REC_SEARCH_ENVELOPE_ANGLES && !tried && !w.over(); ++j) {
            const bool last = j == REC_SEARCH_ENVELOPE_ANGLES;  // none is free of foreign faces: the first one all the same
            const double th = 2 * PI * ((last ? 0.0 : double(j)) + 0.37) / REC_SEARCH_ENVELOPE_ANGLES;  // off the usual symmetry planes
            Plane p;
            p.origin = origin;
            p.y = ax.a;
            p.x = (std::cos(th) * x0 + std::sin(th) * y0).normalized();
            p.normal = p.x.cross(p.y).normalized();
            w.spent += t.faces();
            auto half = halfProfile(k, s, p);
            if (tracing() && (last || j == 0)) {
                std::fprintf(stderr, "  envelope axis (%.3g %.3g %.3g) half %d curves %zu\n", ax.a.x(), ax.a.y(), ax.a.z(), int(half.has_value()), half ? half->size() : 0);
                if (auto sec = k.planeSection(s, p, true); sec) {
                    for (const auto& c : *sec) std::fprintf(stderr, "    sec kind %d (%.5g %.5g)-(%.5g %.5g) arc (%.5g %.5g)-(%.5g %.5g) r %.4g\n", int(c.kind), c.p1.x(), c.p1.y(), c.p2.x(), c.p2.y(), c.start.x(), c.start.y(), c.end.x(), c.end.y(), c.radius);
                } else {
                    std::fprintf(stderr, "    sec failed: %s\n", sec.error().code.c_str());
                }
            }
            if (!half) continue;
            bool clean = true;
            for (const auto& c : *half) {
                if (c.kind == C2::Line && c.p1.x() == 0 && c.p2.x() == 0) continue;
                const Point3 at = p.toWorld(c.kind == C2::Line ? Point2((c.p1 + c.p2) / 2) : c.kind == C2::Spline ? c.p1 : Point2(c.kind == C2::Arc ? c.start : c.center));
                clean = clean && std::any_of(compat.begin(), compat.end(), [&](int f) { return k.distanceToFace(s, f, at) <= tol * 100; });
            }
            if (!clean && !last) continue;
            tried = true;
            auto R = revolveSolid(k, p, *half, origin, ax.a);
            if (tracing()) {
                std::fprintf(stderr, "  envelope revolve %d\n", int(R.has_value()));
                for (const auto& c : *half) std::fprintf(stderr, "    curve kind %d (%.5g %.5g)-(%.5g %.5g) arc (%.5g %.5g)-(%.5g %.5g)\n", int(c.kind), c.p1.x(), c.p1.y(), c.p2.x(), c.p2.y(), c.start.x(), c.start.y(), c.end.x(), c.end.y());
            }
            if (!R) continue;
            envelopeChild(k, s, *R, w, out);
        }
    }
    const Point3 center = k.massProperties(s).centroid;
    const double reach = bodyReach(k, t, center);
    for (const auto& d : dirs) {
        auto lv = planeLevels(t, d);
        if (lv.size() < 2) continue;
        // Sections of the slabs between the levels; equal areas are one section.
        std::vector<double> tried;
        int made = 0;
        for (size_t i = 0; i + 1 < lv.size() && made < REC_SEARCH_ENVELOPES && !w.over(); ++i) {
            const Plane mid = planeFrom(((lv[i] + lv[i + 1]) / 2) * d, d);
            auto sec = k.planeSection(s, mid);
            w.spent += t.faces();
            if (!sec || sec->empty()) continue;
            auto pr = profileFromCurves(planeFrom(lv.front() * d, d), mergePieces(*sec));
            if (!pr) continue;
            auto info = kernel::validateProfile(*pr);
            if (!info) continue;
            if (std::any_of(tried.begin(), tried.end(), [&](double a) { return std::abs(a - info->area) <= 1e-6 * reach * reach; })) continue;
            tried.push_back(info->area);
            auto E = k.extrude(*pr, kernel::Extent{kernel::Extent::Kind::Blind, lv.back() - lv.front(), {}}, false);
            if (!E) continue;
            ++made;
            envelopeChild(k, s, E->shape, w, out);
        }
    }
}

// Pockets and bosses by a planar face (§4.10 item 2 д): the outline of the face swept along its walls gives the tool directly, no
// face removal: the bottom of a pocket (concave walls) is filled up, the top of a boss (convex walls) is taken off.
void proposeByFace(const kernel::Kernel& k, const kernel::Shape& s, const Topo& t, const std::vector<char>& concave, Work& w, std::vector<Child>& out) {
    std::vector<std::pair<double, int>> planes;
    for (int f = 0; f < t.faces(); ++f)
        if (t.surf[size_t(f)].kind == SK::Plane) planes.push_back({k.faceArea(s, f), f});
    std::stable_sort(planes.begin(), planes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    int tried = 0;
    for (const auto& [area, f] : planes) {
        if (tried >= REC_SEARCH_MAX_FACE_TOOLS || w.over()) return;
        const Vec3 n = k.faceNormalAt(s, f, k.closestPointOnFace(s, f, k.faceCentroid(s, f))).normalized();
        const double z0 = t.surf[size_t(f)].point.dot(n);
        // Walls: the neighbours ∥ n, all of one kind of edge; the sweep goes up to where the nearest of them ends.
        int conc = 0, conv = 0;
        double reach = std::numeric_limits<double>::max();
        for (int le : t.faceLE[size_t(f)]) {
            const int g = t.other(le, f);
            if (!extrudeFace(t, g, n) || (t.surf[size_t(g)].kind == SK::Plane && parallel(t.surf[size_t(g)].dir, n))) continue;
            (concave[size_t(le)] ? conc : conv)++;
            double lo = std::numeric_limits<double>::max(), hi = -lo;
            for (int e : t.faceLE[size_t(g)])
                for (int v : t.ct.logicalEdges[size_t(e)].ends) {
                    const double z = k.vertexPoint(s, v).dot(n);
                    lo = std::min(lo, z), hi = std::max(hi, z);
                }
            for (int e : t.faceLE[size_t(g)]) {
                const double z = t.edgePoint(e).dot(n);
                lo = std::min(lo, z), hi = std::max(hi, z);
            }
            const double h = concave[size_t(le)] ? hi - z0 : z0 - lo;
            if (h > REC_LIN_TOL * 10) reach = std::min(reach, h);
        }
        const bool pocket = conc > 0 && conv == 0, boss = conv > 0 && conc == 0;
        if ((!pocket && !boss) || reach == std::numeric_limits<double>::max()) continue;
        ++tried;
        const Plane base = planeFrom(z0 * n, pocket ? n : Vec3(-n));
        std::vector<kernel::Curve2> curves;
        bool ok = true;
        for (int le : t.faceLE[size_t(f)]) {
            auto c = k.projectEdge(s, le, base);
            if (!c) ok = false;
            else curves.push_back(*c);
        }
        if (!ok) continue;
        curves = mergePieces(curves);
        Op op;
        op.kind = Op::Kind::Extrude;
        op.plane = base;
        op.curves = curves;
        op.depth = reach;
        op.cut = false;  // the tool: added to fill a pocket, taken away from a boss
        if (boss) op.cut = true;
        w.spent += 2 * t.faces();
        auto r = applyOp(k, s, op);
        if (!r || k.counts(*r).solids != 1) continue;
        // The operation of the history is the opposite one, built on the changed body.
        Op hist = op;
        hist.cut = pocket;
        if (!opsGive(k, *r, {hist}, s, w)) continue;
        out.push_back({{hist}, *r});
    }
}

// Hypotheses of one body (§4.10 item 2), a kernel attempt each; the list is ordered and bounded.
std::vector<Child> propose(const kernel::Kernel& k, const kernel::Shape& s, Work& w, bool cheap) {
    std::vector<Child> out;
    Topo t(k, s);
    const int n = t.faces();
    std::vector<char> concave(t.ct.logicalEdges.size(), 0);
    for (size_t le = 0; le < concave.size(); ++le) concave[le] = concaveEdge(t, int(le));
    const auto dirs = searchDirs(t);
    const auto axes = searchAxes(t);
    // The envelopes first: a few booleans, and an answer at once where they fit; the rest waits for its turn in the queue.
    if (cheap) {
        proposeEnvelopes(k, s, t, dirs, axes, w, out);
        return out;
    }
    // а. Groups of faces whose removal closes a pocket or takes away a boss.
    std::vector<std::vector<int>> groups;
    auto add = [&](std::vector<int> g) {
        if (g.empty() || int(g.size()) >= n) return;
        if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(std::move(g));
    };
    auto isConcave = [&](int le) { return concave[size_t(le)] != 0; };
    auto notConcave = [&](int le) { return concave[size_t(le)] == 0; };
    proposeByFace(k, s, t, concave, w, out);
    std::vector<std::vector<int>> pockets, bosses, rings;
    for (const auto& d : dirs) {
        std::vector<char> allowed(static_cast<size_t>(n), 0);
        for (int f = 0; f < n; ++f) allowed[size_t(f)] = extrudeFace(t, f, d);
        for (auto& g : components(t, allowed, isConcave))
            if (g.size() >= 2) pockets.push_back(std::move(g));
        for (auto& g : components(t, allowed, notConcave)) bosses.push_back(std::move(g));
    }
    for (const auto& ax : axes) {
        std::vector<char> allowed(static_cast<size_t>(n), 0);
        for (int f = 0; f < n; ++f) allowed[size_t(f)] = revolveFace(t, f, ax.o, ax.a);
        for (auto& g : components(t, allowed, isConcave)) rings.push_back(std::move(g));
        for (auto& g : components(t, allowed, notConcave)) rings.push_back(std::move(g));
    }
    auto smallFirst = [](const std::vector<int>& a, const std::vector<int>& b) { return a.size() != b.size() ? a.size() < b.size() : a < b; };
    std::stable_sort(pockets.begin(), pockets.end(), smallFirst);
    std::stable_sort(bosses.begin(), bosses.end(), smallFirst);
    std::stable_sort(rings.begin(), rings.end(), smallFirst);
    for (auto& g : pockets) add(std::move(g));
    for (auto& g : rings) add(std::move(g));
    for (auto& g : bosses) add(std::move(g));
    // Single faces: the smallest first — a wall the neighbours close over.
    std::vector<std::pair<double, int>> byArea;
    for (int f = 0; f < n; ++f) byArea.push_back({k.faceArea(s, f), f});
    std::stable_sort(byArea.begin(), byArea.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [area, f] : byArea) add({f});
    int tried = 0;
    for (const auto& g : groups) {
        if (tried >= REC_SEARCH_MAX_GROUPS || w.over()) break;
        ++tried;
        w.spent += n;
        auto r = removeChecked(k, s, g);
        if (tracing()) {
            std::string list;
            for (int f : g) list += " " + std::to_string(f);
            std::fprintf(stderr, "  group%s: %s [%.1f s]\n", list.c_str(), r ? "removed" : "not removed", double(std::clock()) / CLOCKS_PER_SEC);
        }
        if (!r) continue;
        if (auto c = childFrom(k, s, r->shape, w)) out.push_back(std::move(*c));
        else if (tracing()) std::fprintf(stderr, "    not an operation\n");
    }
    // б. Splitting by the plane of a face: the part beyond it is one extrusion or revolution (a boss whose removal the
    // kernel does not close).
    const Point3 center = k.massProperties(s).centroid;
    const double reach = bodyReach(k, t, center);
    for (const auto& d : dirs) {
        const auto lv = planeLevels(t, d);
        for (size_t i = 0; i < lv.size(); ++i) {
            if (w.over()) return out;
            for (const double sgn : {1.0, -1.0}) {
                const Point3 at = center + (lv[i] - center.dot(d)) * d;
                auto tool = k.extrude(squareProfile(planeFrom(at, sgn * d), reach), kernel::Extent{kernel::Extent::Kind::Blind, reach * 2, {}}, false);
                if (!tool) continue;
                w.spent += 2 * n;
                auto top = k.boolean(kernel::BoolKind::Intersect, s, tool->shape);
                if (!top || top->shape.null() || k.counts(top->shape).solids < 1 || !(volumeOf(k, top->shape) > 0)) continue;
                auto rest = k.boolean(kernel::BoolKind::Subtract, s, tool->shape);
                if (!rest || rest->shape.null() || k.counts(rest->shape).solids != 1 || !(volumeOf(k, rest->shape) > 0)) continue;
                auto ops = deltaOps(k, top->shape, rest->shape, false, w);
                if (!ops || !opsGive(k, rest->shape, *ops, s, w)) continue;
                out.push_back({std::move(*ops), rest->shape});
            }
        }
    }
    return out;
}

// Search node: a body after the obvious removals.
struct Node {
    Peel peel;
    std::vector<Note> notes;
    std::vector<Op> ops;                // the hypothesis that led from the parent's core to this body
    kernel::Shape body;                 // the body before the peel
    std::shared_ptr<const Node> parent;
    double cost = 0;                    // plan cost from the source body down to this node (peel included)
    int simple = 0;                     // simplicity of the core
};
using NodeP = std::shared_ptr<const Node>;

std::string signature(const kernel::Kernel& k, const kernel::Shape& s) {
    const auto m = k.massProperties(s);
    const auto c = k.counts(s);
    char buf[128];
    (void)c;
    std::snprintf(buf, sizeof buf, "%.5g/%.5g", m.volume, m.area);
    return buf;
}

SearchResult resultOf(const NodeP& leaf, CoreSplit split, const std::vector<Note>& coreNotes) {
    SearchResult r;
    std::vector<NodeP> path;
    for (NodeP n = leaf; n; n = n->parent) path.push_back(n);
    std::reverse(path.begin(), path.end());
    for (size_t i = 0; i < path.size(); ++i) {
        SearchLevel lv;
        lv.peel = path[i]->peel;
        if (i + 1 < path.size()) {
            lv.ops = path[i + 1]->ops;
            lv.restored = path[i + 1]->body;
        }
        r.levels.push_back(std::move(lv));
        r.notes.insert(r.notes.end(), path[i]->notes.begin(), path[i]->notes.end());
    }
    r.notes.insert(r.notes.end(), coreNotes.begin(), coreNotes.end());
    r.core = std::move(split);
    return r;
}

}  // namespace

double planCost(const Peel& peel, const std::vector<Op>& base) { return peelCost(peel) + opsCost(base); }

bool needsSearch(const CoreSplit& split) {
    if (split.base.empty()) return true;
    if (!split.layered) return false;
    // Layers are a fine plan as long as they are few or arranged in a pattern.
    int extrusions = 0;
    for (const auto& o : split.base) {
        if (o.kind == Op::Kind::Pattern || o.kind == Op::Kind::Mirror) return false;
        extrusions += o.kind == Op::Kind::Extrude;
    }
    return extrusions >= 3;
}

std::optional<SearchResult> searchHistory(const kernel::Kernel& k, const Peel& root, const CoreSplit& split, std::vector<Note>& notes) {
    if (k.counts(root.core).faces > REC_SEARCH_MAX_FACES) return std::nullopt;
    // A free-form face that is no wall of any direction (a loft, a sweep): no hypothesis explains it, do not spend the time.
    {
        Topo t(k, root.core);
        const auto dirs = searchDirs(t);
        for (int f = 0; f < t.faces(); ++f) {
            const auto& si = t.surf[size_t(f)];
            if (si.kind != SK::Other || si.otherKind == "revolution") continue;
            if (std::none_of(dirs.begin(), dirs.end(), [&](const Vec3& d) { return t.ruledAlong(f, d); })) return std::nullopt;
        }
    }
    Work w;
    auto rootNode = std::make_shared<Node>();
    rootNode->peel = root;
    rootNode->body = root.core;
    rootNode->cost = peelCost(root);
    rootNode->simple = simplicity(k, root.core);
    // The incumbent: the layers of the obvious way, if any (§4.10 item 5); a complete plan always beats a partial one.
    double bestCost = split.base.empty() ? std::numeric_limits<double>::max() : rootNode->cost + opsCost(split.base);
    std::optional<SearchResult> best;
    NodeP bestPartial = rootNode;
    struct Item {
        int simple;
        double cost;
        size_t seq;
        NodeP node;
        bool cheap;  // the envelopes of the node; otherwise its slower hypotheses
        bool operator<(const Item& o) const {  // the priority queue gives the largest
            if (cheap != o.cheap) return o.cheap;
            if (simple != o.simple) return simple > o.simple;
            if (cost != o.cost) return cost > o.cost;
            return seq > o.seq;
        }
    };
    std::priority_queue<Item> queue;
    size_t seq = 0;
    queue.push({rootNode->simple, rootNode->cost, seq++, rootNode, true});
    std::set<std::string> seen{signature(k, root.core)};
    int expanded = 0;
    long solvedAt = -1;  // work when the first complete plan was found: the rest of the search is bounded by it (item 7)
    while (!queue.empty() && !w.over() && expanded < REC_SEARCH_MAX_NODES) {
        if (solvedAt >= 0 && w.spent > solvedAt * REC_SEARCH_AFTER_PLAN + 2000) break;
        const NodeP cur = queue.top().node;
        const bool cheap = queue.top().cheap;
        queue.pop();
        if (cur->cost >= bestCost) continue;
        if (cheap) queue.push({cur->simple, cur->cost, seq++, cur, false});
        else ++expanded;
        if (tracing()) std::fprintf(stderr, "expand #%d simple %d cost %.2f work %ld best %.2f\n", expanded, cur->simple, cur->cost, w.spent, bestCost);
        if (tracing()) traceBody(k, cur->peel.core);
        budget().removals = 0;  // the removal budget of §3 is per attempt here, the search has its own limit
        for (auto& c : propose(k, cur->peel.core, w, cheap)) {
            // What was found is looked at even when the limit has just passed: it cost the most.
            const double hyp = opsCost(c.ops);
            if (cur->cost + hyp >= bestCost) continue;
            const std::string sig = signature(k, c.restored);
            if (!seen.insert(sig).second) continue;
            auto node = std::make_shared<Node>();
            budget().removals = 0;
            w.spent += k.counts(c.restored).faces * 4;
            // The booleans of the hypotheses leave faces cut along the tools: one surface is one face again.
            if (auto u = k.unifyFaces(c.restored)) c.restored = *u;
            node->peel = peelBody(k, c.restored, node->notes);
            node->ops = std::move(c.ops);
            node->body = c.restored;
            node->parent = cur;
            node->cost = cur->cost + hyp + peelCost(node->peel);
            node->simple = simplicity(k, node->peel.core);
            if (tracing()) std::fprintf(stderr, "  child simple %d cost %.2f %s\n", node->simple, node->cost, describe(node->ops).c_str());
            if (node->simple > cur->simple + REC_SEARCH_MAX_RISE) continue;  // not a way towards a simpler body
            // A simple base ends the branch; layers end it only if they are the cheapest way so far.
            std::string kind;
            std::vector<Note> baseNotes;
            auto base = decomposeBase(k, node->peel.core, kind, baseNotes, true);
            bool layered = false;
            if (base.empty()) {
                base = decomposeBase(k, node->peel.core, kind, baseNotes, false);
                layered = base.size() > 1;
            }
            if (!base.empty()) {
                base = splitPatterns(base);
                const double total = node->cost + opsCost(base);
                if (total < bestCost) {
                    bestCost = total;
                    if (solvedAt < 0) solvedAt = w.spent;
                    CoreSplit cs;
                    cs.core = node->peel.core;
                    cs.base = std::move(base);
                    cs.kind = kind;
                    cs.layered = layered;
                    best = resultOf(node, std::move(cs), {});
                }
                if (!layered) continue;
            }
            if (node->simple < bestPartial->simple) bestPartial = node;
            queue.push({node->simple, node->cost, seq++, node, true});
        }
    }
    budget().removals = 0;
    budget().exhausted = false;
    if (w.over() && !cancelled(budget().cancel)) notes.push_back({"limit", "search budget spent"});
    if (best) return best;
    if (split.base.empty() && bestPartial != rootNode) {
        CoreSplit cs;
        cs.core = bestPartial->peel.core;
        return resultOf(bestPartial, std::move(cs), {{"base", "core not decomposed"}});
    }
    return std::nullopt;
}

}  // namespace construe::rec::detail
