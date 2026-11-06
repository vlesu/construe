// History recognition: removal of fillets, chamfers, holes; grouping of holes; match check
// (spec/reconstruction.md §4.1–§4.4, §6).
#include "rec/rec_internal.h"

#include "regen/hole_tables.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <sstream>

namespace construe::rec {

using namespace detail;
using SK = kernel::SurfaceInfo::Kind;
using CK = kernel::CurveInfo::Kind;

namespace detail {

namespace {

// A surface of revolution written as such in a file, whose meridian is a line or a circle, is a cylinder, a cone, a sphere or a
// torus (§2.1): the rest of the recognizer sees the canonical surface.
void canonicalRevolution(const kernel::Kernel& k, const kernel::Shape& s, int f, kernel::SurfaceInfo& si) {
    const Vec3 a = si.dir.normalized();
    const Point3 c = k.closestPointOnFace(s, f, k.faceCentroid(s, f));
    const Vec3 r = (c - si.point) - (c - si.point).dot(a) * a;
    if (r.norm() < REC_LIN_TOL) return;
    auto lines = k.curvePoints(s, kernel::CurveSet{-1, f, r.normalized()}, 1e-3);
    if (!lines || lines->empty()) return;
    // Meridian in (ρ, t): distance from the axis, level along it.
    std::vector<Point2> q;
    for (const auto& l : *lines)
        for (const auto& p : l) {
            const Vec3 v = p - si.point;
            q.push_back(Point2((v - v.dot(a) * a).norm(), v.dot(a)));
        }
    if (q.size() < 3) return;
    const double tol = REC_LIN_TOL * 100;
    const Point2 p0 = q.front(), p1 = q.back();
    const Vec2 u = (p1 - p0).norm() > 0 ? Vec2((p1 - p0).normalized()) : Vec2(0, 1);
    double dev = 0;
    for (const auto& p : q) dev = std::max(dev, std::abs((p - p0).x() * u.y() - (p - p0).y() * u.x()));
    if (dev <= tol) {
        const double sinA = std::abs(u.x());
        if (sinA <= 1e-9) {
            si.kind = SK::Cylinder;
            si.radius = p0.x();
            si.convex = k.faceNormalAt(s, f, c).dot(r) > 0;
        } else {
            si.kind = SK::Cone;
            si.halfAngle = std::asin(sinA);
            const double tApex = p0.y() - p0.x() * u.y() / u.x();  // ρ = 0
            si.point = si.point + tApex * a;
            si.dir = (p0.y() + p1.y()) / 2 > tApex ? a : Vec3(-a);
            si.convex = k.faceNormalAt(s, f, c).dot(r) > 0;
        }
        return;
    }
    // A circle through the first, a middle and the last point.
    const Point2 pm = q[q.size() / 2];
    const double d = 2 * (p0.x() * (pm.y() - p1.y()) + pm.x() * (p1.y() - p0.y()) + p1.x() * (p0.y() - pm.y()));
    if (std::abs(d) < 1e-12) return;
    const double s0 = p0.squaredNorm(), sm = pm.squaredNorm(), s1 = p1.squaredNorm();
    const Point2 o((s0 * (pm.y() - p1.y()) + sm * (p1.y() - p0.y()) + s1 * (p0.y() - pm.y())) / d,
                   (s0 * (p1.x() - pm.x()) + sm * (p0.x() - p1.x()) + s1 * (pm.x() - p0.x())) / d);
    const double rad = (p0 - o).norm();
    for (const auto& p : q)
        if (std::abs((p - o).norm() - rad) > tol) return;
    const Point3 center = si.point + o.y() * a;
    // Convex: the normal points away from the circle center.
    const Point2 lc((r).norm(), (c - si.point).dot(a));
    const Vec3 toSurface = r.normalized() * (lc.x() - o.x()) + a * (lc.y() - o.y());
    si.convex = k.faceNormalAt(s, f, c).dot(toSurface) > 0;
    if (std::abs(o.x()) <= tol) {
        si.kind = SK::Sphere;
        si.point = center;
        si.radius = rad;
        si.dir = a;
    } else {
        si.kind = SK::Torus;
        si.point = center;
        si.radius = o.x();
        si.minorRadius = rad;
        si.dir = a;
    }
}

}  // namespace

Topo::Topo(const kernel::Kernel& kk, const kernel::Shape& sh) : k(kk), s(sh), ct(kernel::canonicalTopology(kk, sh)) {
    const int nf = kk.counts(sh).faces;
    surf.resize(size_t(nf));
    faceLE.resize(size_t(nf));
    seam.assign(size_t(nf), false);
    for (int f = 0; f < nf; ++f) {
        surf[size_t(f)] = kk.faceSurface(sh, f);
        if (surf[size_t(f)].kind == SK::Other && surf[size_t(f)].otherKind == "revolution") canonicalRevolution(kk, sh, f, surf[size_t(f)]);
    }
    for (size_t le = 0; le < ct.logicalEdges.size(); ++le) {
        curve.push_back(kk.edgeCurve(sh, int(le)));
        const auto [a, b] = ct.logicalEdges[le].faces;
        faceLE[size_t(a)].push_back(int(le));
        if (b != a) faceLE[size_t(b)].push_back(int(le));
    }
    for (int e : ct.seams) seam[size_t(kk.edgeFaces(sh, e).first)] = true;
}

Point3 Topo::edgePoint(int le) const {
    const auto& c = curve[size_t(le)];
    if (c.kind == CK::Line) return (c.p1 + c.p2) / 2;
    const auto& L = ct.logicalEdges[size_t(le)];
    const Point3 v = k.vertexPoint(s, k.edgeVertices(s, L.edges[L.edges.size() / 2]).first);
    if (c.kind == CK::Circle && !c.closed) {
        const Vec3 ax = c.axis.normalized();
        Vec3 d = kernel::logicalEdgeKey(k, s, L) - c.center;
        d -= d.dot(ax) * ax;
        if (d.norm() > REC_LIN_TOL) return c.center + c.radius * d.normalized();
    }
    return v;
}

bool Topo::smooth(int le) const {
    const auto [f, g] = ct.logicalEdges[size_t(le)].faces;
    if (f == g) return true;
    const Point3 p = edgePoint(le);
    return k.faceNormalAt(s, f, p).dot(k.faceNormalAt(s, g, p)) >= std::cos(REC_SMOOTH_ANGLE);
}

EdgeKey Topo::key(int le) const {
    const auto& L = ct.logicalEdges[size_t(le)];
    EdgeKey e;
    e.key = kernel::logicalEdgeKey(k, s, L);
    for (int x : L.edges) e.length += k.edgeLength(s, x);
    e.kind = curve[size_t(le)].kind;
    return e;
}

bool Topo::ruledAlong(int f, const Vec3& dIn) const {
    if (surf[size_t(f)].kind != SK::Other) return false;
    const Vec3 d = dIn.normalized();
    // Samples: the face centroid (projected onto the face), the ends and parameter midpoints of its kernel edges.
    std::vector<Point3> pts{k.closestPointOnFace(s, f, k.faceCentroid(s, f))};
    for (int le : faceLE[size_t(f)])
        for (int e : ct.logicalEdges[size_t(le)].edges) {
            pts.push_back(k.edgeMidpoint(s, e));
            const auto [v1, v2] = k.edgeVertices(s, e);
            pts.push_back(k.vertexPoint(s, v1));
            pts.push_back(k.vertexPoint(s, v2));
        }
    for (const auto& p : pts)
        if (std::abs(k.faceNormalAt(s, f, p).normalized().dot(d)) > REC_RULED_TOL) return false;
    return true;
}

Plane planeFrom(const Point3& o, const Vec3& nIn) {
    const Vec3 n = nIn.normalized();
    Vec3 x;
    if (std::abs(n.dot(Vec3::UnitX())) > 1 - 1e-9) x = n.x() > 0 ? Vec3(0, 0, -1) : Vec3(0, 0, 1);  // like plane:right
    else x = (Vec3::UnitX() - n.dot(Vec3::UnitX()) * n).normalized();
    Plane p;
    p.origin = o;
    p.normal = n;
    p.x = x;
    p.y = n.cross(x).normalized();
    return p;
}

bool sameCurve(const kernel::Curve2& a, const kernel::Curve2& b, double tol) {
    using C2 = kernel::Curve2::Kind;
    if (a.kind != b.kind) return false;
    auto eq = [&](const Point2& p, const Point2& q) { return (p - q).norm() <= tol; };
    switch (a.kind) {
        case C2::Line: return (eq(a.p1, b.p1) && eq(a.p2, b.p2)) || (eq(a.p1, b.p2) && eq(a.p2, b.p1));
        case C2::Circle: return eq(a.center, b.center) && std::abs(a.radius - b.radius) <= tol;
        case C2::Arc: return eq(a.center, b.center) && std::abs(a.radius - b.radius) <= tol && eq(a.start, b.start) && eq(a.end, b.end);
        case C2::Point: return eq(a.p, b.p);
        case C2::Spline: {
            const auto& x = a.spline;
            const auto& y = b.spline;
            if (x.degree != y.degree || x.poles.size() != y.poles.size() || x.knots.size() != y.knots.size() || x.mults.size() != y.mults.size())
                return false;
            const size_t n = x.poles.size(), m = x.knots.size();
            auto weight = [](const spline::Curve& c, size_t i) { return c.weights.empty() ? 1.0 : c.weights[i]; };
            // Forward or reversed (reversal: poles, weights and multiplicities in reverse order, knot spans mirrored).
            for (const bool rev : {false, true}) {
                bool ok = true;
                const double lx = x.knots.back() - x.knots.front(), ly = y.knots.back() - y.knots.front();
                for (size_t i = 0; i < n && ok; ++i) {
                    const size_t j = rev ? n - 1 - i : i;
                    ok = eq(x.poles[i], y.poles[j]) && std::abs(weight(x, i) - weight(y, j)) <= 1e-9;
                }
                for (size_t i = 0; i < m && ok; ++i) {
                    const size_t j = rev ? m - 1 - i : i;
                    const double ux = (x.knots[i] - x.knots.front()) / lx;
                    const double uy = rev ? (y.knots.back() - y.knots[j]) / ly : (y.knots[j] - y.knots.front()) / ly;
                    ok = x.mults[i] == y.mults[j] && std::abs(ux - uy) <= 1e-9;
                }
                if (ok) return true;
            }
            return false;
        }
    }
    return false;
}

// Face removal with a check (§4.1).
std::optional<kernel::OpResult> removeChecked(const kernel::Kernel& k, const kernel::Shape& s, const std::vector<int>& faces) {
    if (!spendRemoval()) return std::nullopt;
    // The kernel call is the one place the limits of the search cannot reach (§3): it has its own time.
    Cancel call;
    call.parent = budget().cancel;
    const auto t0 = std::chrono::steady_clock::now();
    call.deadline = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(REC_REMOVE_MAX_SECONDS * REC_TIME_SCALE));
    auto r = k.removeFaces(s, faces, &call);
    if (std::getenv("CONSTRUE_REC_TRACE")) {
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (sec > 1) std::fprintf(stderr, "slow removeFaces %.1f s: %zu faces of %d, %s\n", sec, faces.size(), k.counts(s).faces, r ? "ok" : r.error().code.c_str());
    }
    if (!r) return std::nullopt;
    if (k.counts(r->shape).solids != 1 || !(k.massProperties(r->shape).volume > 0)) return std::nullopt;
    // Removal without changing the body (the kernel returned the input) is not a removal.
    const double v0 = k.massProperties(s).volume, v1 = k.massProperties(r->shape).volume;
    if (std::abs(v1 - v0) <= REC_REMOVE_MIN_REL * v0) return std::nullopt;
    return *r;
}

double& bodyTolerance() {
    thread_local double t = 0;
    return t;
}

Budget& budget() {
    thread_local Budget b;
    return b;
}

bool spendRemoval() {
    Budget& b = budget();
    if (stopped()) return false;
    if (++b.removals > REC_MAX_REMOVALS) {
        b.exhausted = true;
        return false;
    }
    return true;
}

}  // namespace detail

Vec3 canonicalAxis(const Vec3& aIn) {
    Vec3 a = aIn.normalized();
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(a[i]) > std::abs(a[best]) + 1e-12) best = i;
    return a[best] < 0 ? Vec3(-a) : a;
}

namespace {

// Images of the input faces in the result (FromFace history).
std::map<int, std::vector<int>> imagesOf(const kernel::OpResult& r) {
    std::map<int, std::vector<int>> m;
    for (size_t i = 0; i < r.history.faceOrigins.size(); ++i)
        for (const auto& o : r.history.faceOrigins[i])
            if (o.kind == kernel::Origin::Kind::FromFace) m[o.input.face].push_back(int(i));
    return m;
}

// Sharp body edges after removal (§4.2 item 3): between images of g1, g2, closer than limit to face f of the old body
// (or to one of the removed pieces of its surface, §4.3 item 2: the edge of a chamfer split in pieces runs along all of them).
std::vector<int> sharpEdges(const kernel::Kernel& k, const kernel::Shape& before, int f, int g1, int g2, double limit, const Topo& after,
                            const std::map<int, std::vector<int>>& img, const std::vector<int>& pieces = {}) {
    std::vector<int> out;
    // Face image: from history, otherwise result faces on the same surface (removal history can be incomplete).
    auto image = [&](int g) {
        std::set<int> r;
        if (auto it = img.find(g); it != img.end()) r.insert(it->second.begin(), it->second.end());
        if (!r.empty()) return r;
        const auto sg = k.faceSurface(before, g);
        for (int f2 = 0; f2 < after.faces(); ++f2) {
            const auto& sa = after.surf[size_t(f2)];
            if (sa.kind != sg.kind || std::abs(sa.radius - sg.radius) > radiusTol() || sa.dir.normalized().dot(sg.dir.normalized()) < 1 - 1e-9)
                continue;
            if (sg.kind == SK::Plane ? std::abs((sa.point - sg.point).dot(sg.dir.normalized())) <= axisTol()
                                     : (sa.point - sg.point).norm() <= axisTol())
                r.insert(f2);
        }
        return r;
    };
    // Pieces of the neighbor's surface adjacent to f (§4.2 item 3: a neighbor split into halves) — images of all of them.
    auto pieceImage = [&](int g) {
        std::set<int> r = image(g);
        const auto sg = k.faceSurface(before, g);
        for (int e : k.faceEdges(before, f)) {
            const auto [x, y] = k.edgeFaces(before, e);
            const int h = x == f ? y : x;
            if (h == f || h == g || h < 0 || !samePiece(sg, k.faceSurface(before, h))) continue;
            const auto rh = image(h);
            r.insert(rh.begin(), rh.end());
        }
        return r;
    };
    const std::set<int> a = pieceImage(g1), b = pieceImage(g2);
    for (size_t le = 0; le < after.ct.logicalEdges.size(); ++le) {
        const auto [x, y] = after.ct.logicalEdges[le].faces;
        if (!((a.count(x) && b.count(y)) || (a.count(y) && b.count(x)))) continue;
        const Point3 p = after.edgePoint(int(le));
        bool close = k.distanceToFace(before, f, p) <= limit + REC_LIN_TOL;
        for (size_t i = 0; i < pieces.size() && !close; ++i) close = k.distanceToFace(before, pieces[i], p) <= limit + REC_LIN_TOL;
        if (close) out.push_back(int(le));
    }
    return out;
}

// Keys of the sharp edges (§4.2 item 3): connected pieces of one circle or line (a neighbor split into halves gives
// an edge in pieces) are one edge — length is the sum, key the length-weighted mean of the piece keys (the key of
// the union, kernel-interface §4.4); the piece keys are kept for a built part that has the same pieces (§5.3).
std::vector<EdgeKey> mergedKeys(const Topo& t, const std::vector<int>& les) {
    auto sameCurve3 = [&](int a, int b) {
        const auto &ca = t.curve[size_t(a)], &cb = t.curve[size_t(b)];
        if (ca.kind != cb.kind) return false;
        if (ca.kind == CK::Circle)
            return (ca.center - cb.center).norm() <= REC_LIN_TOL * 10 && std::abs(ca.radius - cb.radius) <= REC_LIN_TOL * 10 &&
                   parallel(ca.axis, cb.axis);
        if (ca.kind == CK::Line)
            return parallel(ca.p2 - ca.p1, cb.p2 - cb.p1) && distToLine(cb.p1, ca.p1, ca.p2 - ca.p1) <= REC_LIN_TOL * 10;
        return false;
    };
    auto touch = [&](int a, int b) {
        for (int v : t.ct.logicalEdges[size_t(a)].ends)
            for (int w : t.ct.logicalEdges[size_t(b)].ends)
                if (v == w) return true;
        return false;
    };
    std::vector<int> group(les.size());
    std::iota(group.begin(), group.end(), 0);
    std::function<int(int)> find = [&](int x) { return group[size_t(x)] == x ? x : group[size_t(x)] = find(group[size_t(x)]); };
    for (size_t i = 0; i < les.size(); ++i)
        for (size_t j = i + 1; j < les.size(); ++j)
            if (sameCurve3(les[i], les[j]) && touch(les[i], les[j])) group[size_t(find(int(i)))] = find(int(j));
    std::vector<EdgeKey> out;
    std::map<int, size_t> at;  // group root -> index in out (order of the first piece)
    for (size_t i = 0; i < les.size(); ++i) {
        const EdgeKey e = t.key(les[i]);
        const int g = find(int(i));
        if (auto it = at.find(g); it != at.end()) {
            EdgeKey& m = out[it->second];
            if (m.pieces.empty()) m.pieces.push_back(EdgeKey{m.key, m.length, m.kind, {}});
            m.pieces.push_back(e);
            m.key = (m.key * m.length + e.key * e.length) / (m.length + e.length);
            m.length += e.length;
        } else {
            at[g] = out.size();
            out.push_back(e);
        }
    }
    return out;
}

// ---------------------------------------------------------------- fillets (§4.2)

using Blend = BlendFace;

struct Bevel {
    int face = 0;
    int g1 = -1, g2 = -1;
    int side1 = -1, side2 = -1;  // logical side edges (planar chamfer) / circular edges (cone): to g1, to g2
    bool cone = false;
};
std::vector<Bevel> chamferCandidates(const Topo& t);

// Cylinders and tori of fillets (§4.2 item 1), before the order rule.
std::vector<Blend> rawFillets(const Topo& t) {
    std::vector<Blend> cand;
    std::set<int> isCand;
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        if (si.kind == SK::Cylinder && !t.seam[size_t(f)]) {
            const Vec3 a = si.dir.normalized();
            std::vector<int> smoothLines;
            for (int le : t.faceLE[size_t(f)]) {
                const auto& c = t.curve[size_t(le)];
                if (c.kind == CK::Line && parallel(c.p2 - c.p1, a, 1e-6) && t.smooth(le)) smoothLines.push_back(le);
            }
            if (smoothLines.size() != 2) continue;
            const int g1 = t.other(smoothLines[0], f), g2 = t.other(smoothLines[1], f);
            if (g1 == g2 || g1 == f || g2 == f) continue;
            auto radial = [&](int le) {
                const Point3 p = t.edgePoint(le);
                Vec3 v = p - si.point;
                v -= v.dot(a) * a;
                return v.normalized();
            };
            const double span = std::acos(std::clamp(radial(smoothLines[0]).dot(radial(smoothLines[1])), -1.0, 1.0));
            if (span >= PI - 1e-3) continue;  // slot with rounded ends (§4.2 item 1)
            cand.push_back({f, si.radius, g1, g2, smoothLines[0], smoothLines[1]});
            isCand.insert(f);
        } else if (si.kind == SK::Torus) {
            // Rails: smooth latitude circles (coaxial with the torus); meridian circles join the torus to the next face of
            // a chain or to an end face (FX10-REC-006).
            const Vec3 a = si.dir.normalized();
            std::vector<int> rails;
            for (int le : t.faceLE[size_t(f)]) {
                const auto& c = t.curve[size_t(le)];
                if (c.kind == CK::Circle && parallel(c.axis, a, 1e-6) && distToLine(c.center, si.point, a) <= axisTol() && t.smooth(le))
                    rails.push_back(le);
            }
            // Neighbors that are pieces of one surface (§4.4.1 item 2) count as one face: the first of them; other pieces
            // of this torus are no neighbors.
            std::vector<int> sides, sideRail;
            for (int le : rails) {
                const int g = t.other(le, f);
                if (g == f || samePiece(si, t.surf[size_t(g)])) continue;
                bool seen = false;
                for (size_t i = 0; i < sides.size() && !seen; ++i) seen = sides[i] == g || samePiece(t.surf[size_t(sides[i])], t.surf[size_t(g)]);
                if (seen) continue;
                sides.push_back(g);
                sideRail.push_back(le);
            }
            if (sides.size() != 2) continue;
            // Lowest face index first, as before (the order of the neighbors set).
            if (sides[0] > sides[1]) std::swap(sides[0], sides[1]), std::swap(sideRail[0], sideRail[1]);
            cand.push_back({f, si.minorRadius, sides[0], sides[1], sideRail[0], sideRail[1]});
            isCand.insert(f);
        }
    }
    return cand;
}

std::vector<Blend> filletCandidates(const Topo& t) {
    std::vector<Blend> cand = rawFillets(t);
    // A fillet that another candidate runs along (its support) was made before it: it is removed after it (FX10-REC-006:
    // the top contour rounded over rounded vertical edges; a chamfer of the contour over them). Mutual supports (two
    // blends tangent to each other) — neither.
    std::set<int> supports;
    for (const auto& b : cand) supports.insert(b.g1), supports.insert(b.g2);
    for (const auto& b : chamferCandidates(t)) supports.insert(b.g1), supports.insert(b.g2);
    std::erase_if(cand, [&](const Blend& b) { return supports.count(b.face) > 0; });
    std::set<int> isCand;
    for (const auto& b : cand) isCand.insert(b.face);
    // Corner blends: spheres, all edges smooth, all neighbors are fillets of the same radius.
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        if (si.kind != SK::Sphere || t.faceLE[size_t(f)].empty()) continue;
        bool ok = true;
        for (int le : t.faceLE[size_t(f)]) {
            const int g = t.other(le, f);
            bool sameR = false;
            for (const auto& b : cand) sameR = sameR || (b.face == g && std::abs(b.size - si.radius) <= REC_LIN_TOL);
            ok = ok && t.smooth(le) && sameR;
        }
        if (ok) cand.push_back({f, si.radius, -1, -1, -1, -1});
    }
    return cand;
}

// ---------------------------------------------------------------- chamfers (§4.3)


bool standardCountersink(double r, double rk) {
    for (const auto& row : regen::holeTables()["sizes"]) {
        bool clr = false;
        for (const char* fit : {"normal", "fine", "loose"})
            clr = clr || std::abs(2 * r - row["clearance"][fit].get<double>()) <= REC_HOLE_TOL;
        if (clr && std::abs(2 * rk - row["countersink"]["d"].get<double>()) <= REC_HOLE_TOL) return true;
    }
    return false;
}

std::vector<Bevel> chamferCandidates(const Topo& t) {
    std::vector<Bevel> cand;
    const double c45 = std::cos(PI / 4);
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        if (si.kind == SK::Plane) {
            const Vec3 n = si.dir.normalized();
            // Sides: straight edges to planes at 45°, pairwise orthogonal.
            std::vector<int> sides;
            for (int le : t.faceLE[size_t(f)]) {
                if (t.curve[size_t(le)].kind != CK::Line) continue;
                const int g = t.other(le, f);
                if (t.surf[size_t(g)].kind != SK::Plane) continue;
                if (std::abs(std::abs(n.dot(t.surf[size_t(g)].dir.normalized())) - c45) <= 1e-6) sides.push_back(le);
            }
            for (size_t i = 0; i < sides.size(); ++i)
                for (size_t j = i + 1; j < sides.size(); ++j) {
                    const auto &c1 = t.curve[size_t(sides[i])], &c2 = t.curve[size_t(sides[j])];
                    const int g1 = t.other(sides[i], f), g2 = t.other(sides[j], f);
                    if (g1 == g2 || !perpendicular(t.surf[size_t(g1)].dir, t.surf[size_t(g2)].dir, 1e-6)) continue;
                    if (!parallel(c1.p2 - c1.p1, c2.p2 - c2.p1, 1e-6)) continue;
                    const double width = distToLine(c2.p1, c1.p1, c1.p2 - c1.p1);
                    if (width > std::min((c1.p2 - c1.p1).norm(), (c2.p2 - c2.p1).norm()) + REC_LIN_TOL) continue;
                    // A chamfer is smaller than the faces it lies between (else it is a body face between two chamfers).
                    const double ap = t.k.faceArea(t.s, f);
                    if (ap >= t.k.faceArea(t.s, g1) || ap >= t.k.faceArea(t.s, g2)) continue;
                    cand.push_back({f, g1, g2, sides[i], sides[j], false});
                }
        } else if (si.kind == SK::Cone && std::abs(si.halfAngle - PI / 4) <= 1e-6) {
            int cyl = -1, pl = -1, eCyl = -1, ePl = -1;
            const Vec3 a = si.dir.normalized();
            for (int le : t.faceLE[size_t(f)]) {
                if (t.curve[size_t(le)].kind != CK::Circle) continue;
                const int g = t.other(le, f);
                const auto& sg = t.surf[size_t(g)];
                if (sg.kind == SK::Cylinder && coaxial(sg.point, sg.dir, si.point, a)) cyl = g, eCyl = le;
                else if (sg.kind == SK::Plane && parallel(sg.dir, a)) pl = g, ePl = le;
            }
            if (cyl < 0 || pl < 0) continue;
            // A hole edge matching the countersink standard is not a chamfer (§4.4: recognized as a hole).
            if (!t.surf[size_t(cyl)].convex && standardCountersink(t.surf[size_t(cyl)].radius, t.curve[size_t(ePl)].radius)) continue;
            cand.push_back({f, cyl, pl, eCyl, ePl, true});
        }
    }
    return cand;
}

// ---------------------------------------------------------------- removal steps

// Fillets: groups by radius, removed one at a time from the smallest radius (§4.2 items 2, 4).
kernel::Shape removeFillets(const kernel::Kernel& k, kernel::Shape s, std::vector<Step>& steps, std::vector<Note>& notes) {
    for (int guard = 0; guard < 64 && !stopped(); ++guard) {
        Topo t(k, s);
        auto cand = filletCandidates(t);
        if (cand.empty()) return s;
        std::map<double, std::vector<Blend>> groups;
        for (const auto& b : cand) {
            bool put = false;
            for (auto& [r, v] : groups)
                if (std::abs(r - b.size) <= REC_LIN_TOL) v.push_back(b), put = true;
            if (!put) groups[b.size].push_back(b);
        }
        bool removed = false;
        for (const auto& [r, g] : groups) {
            // Own removal by filling first (§4.2а, FX10-REC-006); the kernel's face removal — when it gives nothing.
            // Whole, then by chains of the faces (§4.2а item 6) — before the kernel: a kernel removal cancelled at its
            // deadline may leave the body's shared data unusable for the kernel's fillet afterwards (a fault in OCCT).
            if (auto own = sharpenStep(t, g, false, r, false)) {
                steps.push_back({{own->second}, s});
                s = own->first;
                removed = true;
                break;
            }
            if (auto own = sharpenStep(t, g, false, r, true)) {
                steps.push_back({{own->second}, s});
                s = own->first;
                removed = true;
                break;
            }
            std::vector<int> faces;
            for (const auto& b : g) faces.push_back(b.face);
            auto res = removeChecked(k, s, faces);
            // A fillet takes or gives no more than its area × radius: more means the kernel closed something else.
            double reach = 0;
            for (int f : faces) reach += k.faceArea(s, f) * r;
            if (res && std::abs(k.massProperties(res->shape).volume - k.massProperties(s).volume) > reach) res.reset();
            if (!res) {
                std::ostringstream os;
                os << "R" << r << ": " << faces.size() << " faces";
                notes.push_back({"fillet", os.str()});
                continue;
            }
            Topo after(k, res->shape);
            const auto img = imagesOf(*res);
            Op op;
            op.kind = Op::Kind::Fillet;
            op.size2 = r;
            std::set<int> used;
            std::vector<int> sharp;
            for (const auto& b : g) {
                if (b.g1 < 0) continue;
                for (int le : sharpEdges(k, s, b.face, b.g1, b.g2, r, after, img))
                    if (used.insert(le).second) sharp.push_back(le);
            }
            op.edges = mergedKeys(after, sharp);
            if (op.edges.empty()) {
                notes.push_back({"fillet", "no edges"});
                continue;
            }
            steps.push_back({{op}, s});
            s = res->shape;
            removed = true;
            break;
        }
        if (!removed) {
            // No group was removed: they stay in the body.
            return s;
        }
    }
    return s;
}

kernel::Shape removeChamfers(const kernel::Kernel& k, kernel::Shape s, std::vector<Step>& steps, std::vector<Note>& notes) {
    // Tries that failed are not repeated (§4.3 item 3): all at once — once; a chain or a piece group — by the centroids
    // of its faces (faces away from a removed chamfer keep their centroids), so the attempts grow linearly.
    bool allFailed = false;
    std::vector<std::vector<Point3>> failedTries;
    auto keyOf = [&](const Topo& t, const std::vector<Bevel>& group) {
        std::vector<Point3> key;
        for (const auto& b : group) key.push_back(k.faceCentroid(t.s, b.face));
        std::sort(key.begin(), key.end(), [](const Point3& a, const Point3& b) {
            for (int i = 0; i < 3; ++i)
                if (std::abs(a[i] - b[i]) > REC_LIN_TOL * 100) return a[i] < b[i];
            return false;
        });
        return key;
    };
    auto sameKey = [](const std::vector<Point3>& a, const std::vector<Point3>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if ((a[i] - b[i]).norm() > REC_LIN_TOL * 100) return false;
        return true;
    };
    const int guardMax = std::max(64, k.counts(s).faces);
    for (int guard = 0; guard < guardMax && !stopped(); ++guard) {
        Topo t(k, s);
        auto cand = chamferCandidates(t);
        // A chamfer a fillet runs along was made before the fillet: removed after it (FX10-REC-006).
        {
            std::set<int> supports;
            for (const auto& b : rawFillets(t)) supports.insert(b.g1), supports.insert(b.g2);
            std::erase_if(cand, [&](const Bevel& b) { return supports.count(b.face) > 0; });
        }
        if (cand.empty()) return s;
        // All candidates at once; on failure, by chains — candidates connected by common edges and pieces of one
        // surface (chamfers meeting at a corner are removed together); then by pieces of one surface (a chamfer face
        // split into halves is removed whole, §4.3 item 3).
        std::vector<std::vector<Bevel>> tries;
        auto push = [&](const std::vector<Bevel>& g) {
            const auto key = keyOf(t, g);
            if (std::any_of(failedTries.begin(), failedTries.end(), [&](const auto& f) { return sameKey(f, key); })) return;
            for (const auto& x : tries)
                if (x.size() == g.size() && std::equal(x.begin(), x.end(), g.begin(), [](const Bevel& p, const Bevel& q) { return p.face == q.face; }))
                    return;
            tries.push_back(g);
        };
        if (!allFailed) push(cand);
        std::vector<int> root(cand.size());
        std::iota(root.begin(), root.end(), 0);
        std::function<int(int)> find = [&](int x) { return root[size_t(x)] == x ? x : root[size_t(x)] = find(root[size_t(x)]); };
        for (size_t i = 0; i < cand.size(); ++i)
            for (size_t j = i + 1; j < cand.size(); ++j) {
                bool link = samePiece(t.surf[size_t(cand[i].face)], t.surf[size_t(cand[j].face)]);
                for (int le : t.faceLE[size_t(cand[i].face)]) link = link || t.other(le, cand[i].face) == cand[j].face;
                if (link) root[size_t(find(int(j)))] = find(int(i));
            }
        std::vector<std::vector<Bevel>> chains;  // in order of the first candidate
        std::map<int, size_t> chainOf;
        for (size_t i = 0; i < cand.size(); ++i) {
            const auto [it, fresh] = chainOf.emplace(find(int(i)), chains.size());
            if (fresh) chains.emplace_back();
            chains[it->second].push_back(cand[i]);
        }
        for (const auto& chain : chains)
            if (chain.size() > 1) push(chain);
        std::vector<bool> taken(cand.size(), false);
        for (size_t i = 0; i < cand.size(); ++i) {
            if (taken[i]) continue;
            std::vector<Bevel> piece{cand[i]};
            for (size_t j = i + 1; j < cand.size(); ++j)
                if (!taken[j] && samePiece(t.surf[size_t(cand[i].face)], t.surf[size_t(cand[j].face)])) piece.push_back(cand[j]), taken[j] = true;
            push(piece);
        }
        bool removed = false;
        for (const auto& group : tries) {
            auto failTry = [&] {
                if (group.size() == cand.size()) allFailed = true;
                failedTries.push_back(keyOf(t, group));
            };
            std::vector<int> faces;
            for (const auto& b : group) faces.push_back(b.face);
            auto res = removeChecked(k, s, faces);
            if (!res) {
                if (group.size() > 1 && group.size() == cand.size()) notes.push_back({"chamfer", std::to_string(group.size()) + " faces: not removed together"});
                failTry();
                continue;
            }
            Topo after(k, res->shape);
            const auto img = imagesOf(*res);
            std::map<double, Op> byD;
            std::map<double, std::vector<int>> sharpByD;  // sharp edges per chamfer size (pieces merged below)
            bool ok = true;
            for (const auto& b : group) {
                const auto& c1 = t.curve[size_t(b.side1)];
                const auto& c2 = t.curve[size_t(b.side2)];
                const double limit = b.cone ? std::abs(c1.radius - c2.radius) * 2 + 1 : distToLine(c2.p1, c1.p1, c1.p2 - c1.p1);
                // The removed pieces of the same surface connected to b.face through common edges (a cone split by the
                // seam; separate arcs of one cone between flats are other chamfers).
                std::vector<int> pieces{b.face};
                for (size_t n = 0; n < pieces.size(); ++n)
                    for (int le : t.faceLE[size_t(pieces[n])]) {
                        const int o = t.other(le, pieces[n]);
                        if (std::find(pieces.begin(), pieces.end(), o) != pieces.end() || !samePiece(t.surf[size_t(b.face)], t.surf[size_t(o)])) continue;
                        if (std::any_of(group.begin(), group.end(), [&](const Bevel& x) { return x.face == o; })) pieces.push_back(o);
                    }
                pieces.erase(pieces.begin());
                const auto sharp = sharpEdges(k, s, b.face, b.g1, b.g2, limit, after, img, pieces);
                // One edge, possibly in connected pieces of one curve (a neighbor split into halves).
                if (sharp.empty() || mergedKeys(after, sharp).size() != 1) {
                    if (group.size() > 1) notes.push_back({"chamfer", "sharp edges: " + std::to_string(sharp.size())});
                    ok = false;
                    break;
                }
                const auto& e = after.curve[size_t(sharp[0])];
                double d1 = 0, d2 = 0;
                if (!b.cone) {
                    if (e.kind != CK::Line) {
                        ok = false;
                        break;
                    }
                    d1 = distToLine(c1.p1, e.p1, e.p2 - e.p1);
                    d2 = distToLine(c2.p1, e.p1, e.p2 - e.p1);
                } else {
                    if (e.kind != CK::Circle) {
                        ok = false;
                        break;
                    }
                    // side1 is the edge on the cylinder (shift along the axis), side2 on the plane (radius difference).
                    d1 = std::abs((c1.center - e.center).dot(e.axis.normalized()));
                    d2 = std::abs(c2.radius - e.radius);
                }
                if (std::abs(d1 - d2) > axisTol()) {
                    ok = false;
                    break;
                }
                Op* op = nullptr;
                for (auto& [d, o] : byD)
                    if (std::abs(d - d1) <= axisTol()) op = &o;
                if (!op) {
                    op = &byD[d1];
                    op->kind = Op::Kind::Chamfer;
                    op->size2 = d1;
                }
                auto& les = sharpByD[op->size2];
                for (int le : sharp)
                    if (std::find(les.begin(), les.end(), le) == les.end()) les.push_back(le);
            }
            if (!ok) {
                failTry();
                continue;
            }
            for (auto& [d, o] : byD) o.edges = mergedKeys(after, sharpByD[d]);
            Step st;
            st.before = s;
            for (auto& [d, o] : byD) st.ops.push_back(o);
            steps.push_back(st);
            s = res->shape;
            removed = true;
            break;
        }
        // The kernel removed none of the tries: own removal by filling (§4.2а, FX10-REC-006), the tries in the same order —
        // the chamfers of a try with one symmetric size.
        for (size_t n = 0; n < tries.size() && !removed; ++n) {
            const auto& group = tries[n];
            {
                std::vector<BlendFace> bf;
                std::optional<double> d0;
                bool same = true;
                for (const auto& b : group) {
                    BlendFace x{b.face, 0, b.g1, b.g2, b.side1, b.side2};
                    const auto d = chamferSize(t, x);
                    if (!d || (d0 && std::abs(*d - *d0) > axisTol())) {
                        same = false;
                        break;
                    }
                    if (!d0) d0 = d;
                    x.size = *d;
                    bf.push_back(x);
                }
                if (same && d0) {
                    if (auto own = sharpenStep(t, bf, true, *d0)) {
                        steps.push_back({{own->second}, s});
                        s = own->first;
                        removed = true;
                        break;
                    }
                }
            }
        }
        if (!removed) {
            for (size_t i = 0; i < cand.size(); ++i) notes.push_back({"chamfer", "face not removed"});
            return s;
        }
    }
    return s;
}

// ---------------------------------------------------------------- holes (§4.4)

constexpr int REC_HOLE_PASSES = 3;  // passes of one-by-one hole removal (§4.4.3)

struct HoleShape {
    std::vector<int> faces;  // all faces of the hole
    Point3 c = Point3::Zero();
    Vec3 dHat = Vec3::UnitY();  // drilling direction
    Point3 entry = Point3::Zero();  // entry point on the axis
    bool entryGood = false;      // entry is a plane ⊥ axis
    bool farGood = false;        // into a cavity: the far end of the wall is a plane ⊥ axis too (its cap when filled, §4.4.3)
    double r = 0;
    bool through = false;
    std::string bottom;  // through | flat | drill | cavity (through the wall into a cavity, §4.4.1 item 5)
    double depth = 0;    // from the entry plane to the end of the wall
    std::string form = "simple";  // simple | counterbore | countersink
    double rc = 0, t = 0;  // counterbore
    double rk = 0;         // countersink: radius at the entry
    bool pointFree = true; // into a cavity: the drill point beyond the wall end cuts no material (§4.4.2)
};

// The drill point of a hole into a cavity (§4.4.2): the cone beyond the far end of the wall (base radius r, half-angle
// 59°) does not overlap the body — the volume of the cone minus the body is the volume of the cone.
bool drillPointFree(const kernel::Kernel& k, const kernel::Shape& s, const HoleShape& h) {
    const Vec3 dh = h.dHat.normalized();
    const Vec3 u = dh.unitOrthogonal();
    const Point3 base = h.entry + h.depth * dh;
    kernel::Profile pr;
    pr.plane.origin = base;
    pr.plane.x = u;
    pr.plane.y = dh;
    pr.plane.normal = u.cross(dh).normalized();
    const std::vector<Point3> tri{base, base + h.r * u, base + h.r / std::tan(59.0 * PI / 180) * dh};
    for (size_t i = 0; i < tri.size(); ++i) {
        kernel::Segment3 sg;
        sg.id = "p" + std::to_string(i);
        sg.kind = kernel::Segment3::Kind::Line;
        sg.p1 = tri[i];
        sg.p2 = tri[(i + 1) % tri.size()];
        pr.outer.segments.push_back(sg);
    }
    auto cone = k.revolve(pr, kernel::Axis{base, dh}, kernel::RevolveExtent{});
    if (!cone) return false;
    const double v = k.massProperties(cone->shape).volume;
    auto rest = k.boolean(kernel::BoolKind::Subtract, cone->shape, s);
    if (!rest) return false;
    return std::abs(k.massProperties(rest->shape).volume - v) <= REC_MATCH_REL * std::max(v, k.massProperties(s).volume);
}

std::vector<HoleShape> findHoles(const Topo& t, std::vector<Note>& notes) {
    std::vector<HoleShape> out;
    std::set<int> taken;
    auto isWall = [&](int f, const Point3& o, const Vec3& a, double r) {
        const auto& si = t.surf[size_t(f)];
        return si.kind == SK::Cylinder && !si.convex && std::abs(si.radius - r) <= REC_LIN_TOL && coaxial(o, a, si.point, si.dir);
    };
    // Walls from the smallest radius: a narrow wall takes the counterbore (§4.4.1 item 2).
    std::vector<int> order;
    for (int f = 0; f < t.faces(); ++f) order.push_back(f);
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return t.surf[size_t(x)].radius < t.surf[size_t(y)].radius; });
    for (int f0 : order) {
        const auto& s0 = t.surf[size_t(f0)];
        if (taken.count(f0) || s0.kind != SK::Cylinder || s0.convex) continue;
        const Point3 o = s0.point;
        const Vec3 a = canonicalAxis(s0.dir);
        const double r = s0.radius;
        // Wall: parts of one cylinder connected by straight edges; closed means no straight edges to other faces.
        std::vector<int> wall{f0};
        std::set<int> inWall{f0};
        bool closed = true;
        for (size_t i = 0; i < wall.size(); ++i)
            for (int le : t.faceLE[size_t(wall[i])]) {
                if (t.curve[size_t(le)].kind != CK::Line) continue;
                const int g = t.other(le, wall[i]);
                if (isWall(g, o, a, r)) {
                    if (inWall.insert(g).second) wall.push_back(g);
                } else {
                    closed = false;
                }
            }
        if (!closed) {
            for (int w : wall) taken.insert(w);
            continue;
        }
        // Pieces of the same wall separated by another hole (cross holes) or by body walls (a transverse
        // hole in a ring) are one wall (§4.4.1 item 1).
        // Extent of a piece along the axis and whether a flat disc closes its end on the side of the level t (a blind hole).
        auto extent = [&](int f) {
            double lo = std::numeric_limits<double>::max(), hi = -lo;
            for (int le : t.faceLE[size_t(f)]) {
                if (t.curve[size_t(le)].kind == CK::Line) continue;
                for (const Point3& q : {t.edgePoint(le), t.curve[size_t(le)].center}) {
                    if (t.curve[size_t(le)].kind == CK::Other && (q - t.edgePoint(le)).norm() > 0) continue;
                    const double x = (q - o).dot(a);
                    lo = std::min(lo, x), hi = std::max(hi, x);
                }
            }
            return std::make_pair(lo, hi);
        };
        auto discAt = [&](int f, double level) {
            for (int le : t.faceLE[size_t(f)]) {
                const auto& c = t.curve[size_t(le)];
                if (c.kind != CK::Circle || std::abs((c.center - o).dot(a) - level) > REC_LIN_TOL * 10) continue;
                const int g = t.other(le, f);
                if (t.surf[size_t(g)].kind != SK::Plane || !parallel(t.surf[size_t(g)].dir, a)) continue;
                bool disc = true;
                for (int e : t.faceLE[size_t(g)])
                    disc = disc && t.curve[size_t(e)].kind == CK::Circle && std::abs(t.curve[size_t(e)].radius - r) <= REC_LIN_TOL * 10;
                if (disc) return true;
            }
            return false;
        };
        // A flat bottom between two pieces: two blind holes against each other, not one wall.
        auto bottomBetween = [&](int f, int g) {
            const auto [flo, fhi] = extent(f);
            const auto [glo, ghi] = extent(g);
            if (fhi <= glo + REC_LIN_TOL * 10) return discAt(f, fhi) || discAt(g, glo);
            if (ghi <= flo + REC_LIN_TOL * 10) return discAt(g, ghi) || discAt(f, flo);
            return false;
        };
        for (int g : order) {
            if (taken.count(g) || inWall.count(g) || !isWall(g, o, a, r)) continue;
            bool gClosed = true;
            for (int le : t.faceLE[size_t(g)])
                if (t.curve[size_t(le)].kind == CK::Line && !isWall(t.other(le, g), o, a, r)) gClosed = false;
            for (int w : wall) gClosed = gClosed && !bottomBetween(w, g);
            if (gClosed) wall.push_back(g), inWall.insert(g);
        }
        for (int w : wall) taken.insert(w);
        // A bore coaxial with the body's outer cylinder (pipe, flange, pulley) is base, not a hole (§4.4.1 item 1).
        bool bore = false;
        for (int g = 0; g < t.faces(); ++g) {
            const auto& sg = t.surf[size_t(g)];
            bore = bore || (sg.kind == SK::Cylinder && sg.convex && coaxial(o, a, sg.point, sg.dir));
        }
        if (bore) continue;
        // Wall ends by the coordinate along a: circular edges by the center, others by the ends and an interior point.
        auto tOf = [&](const Point3& p) { return (p - o).dot(a); };
        auto tRange = [&](int le) {
            const auto& c = t.curve[size_t(le)];
            if (c.kind == CK::Circle) return std::make_pair(tOf(c.center), tOf(c.center));
            double lo = tOf(t.edgePoint(le)), hi = lo;
            for (int v : t.ct.logicalEdges[size_t(le)].ends) {
                const double x = tOf(t.k.vertexPoint(t.s, v));
                lo = std::min(lo, x), hi = std::max(hi, x);
            }
            return std::make_pair(lo, hi);
        };
        double t0 = std::numeric_limits<double>::max(), t1 = -t0;
        for (int w : wall)
            for (int le : t.faceLE[size_t(w)])
                if (t.curve[size_t(le)].kind != CK::Line) {
                    const auto [lo, hi] = tRange(le);
                    t0 = std::min(t0, lo), t1 = std::max(t1, hi);
                }
        if (!(t1 > t0 + REC_LIN_TOL)) continue;
        struct End {
            std::string kind;  // entry | disc | drill | cbore | csink | fail
            std::vector<int> faces;  // hole faces beyond the end
            int entryFace = -1;
            double tEntry = 0;
            double rc = 0, tc = 0, rk = 0;
        };
        // Neighbors of the wall at end tEnd.
        auto neighborsAt = [&](const std::vector<int>& ws, double tEnd) {
            std::set<int> nb;
            for (int w : ws)
                for (int le : t.faceLE[size_t(w)]) {
                    if (t.curve[size_t(le)].kind == CK::Line) continue;
                    const auto [lo, hi] = tRange(le);
                    if (std::abs(lo - tEnd) > REC_LIN_TOL * 10 && std::abs(hi - tEnd) > REC_LIN_TOL * 10) continue;
                    const int g = t.other(le, w);
                    if (!std::count(ws.begin(), ws.end(), g)) nb.insert(g);
                }
            return nb;
        };
        // The end is formed by circles (entry on a plane ⊥ axis is possible).
        auto circularEnd = [&](double tEnd) {
            for (int w : wall)
                for (int le : t.faceLE[size_t(w)]) {
                    if (t.curve[size_t(le)].kind == CK::Line) continue;
                    const auto [lo, hi] = tRange(le);
                    if ((std::abs(lo - tEnd) <= REC_LIN_TOL * 10 || std::abs(hi - tEnd) <= REC_LIN_TOL * 10) &&
                        t.curve[size_t(le)].kind != CK::Circle)
                        return false;
                }
            return true;
        };
        // Pieces of one surface (§4.4.1 item 2, samePiece); a single face of any kind is its own piece.
        auto samePieces = [&](const std::set<int>& P) {
            if (P.empty()) return false;
            const auto& s0 = t.surf[size_t(*P.begin())];
            return P.size() == 1 || std::all_of(P.begin(), P.end(), [&](int g) { return samePiece(s0, t.surf[size_t(g)]); });
        };
        // Edges of the pieces to faces not among the pieces.
        auto pieceEdges = [&](const std::set<int>& P) {
            std::vector<std::pair<int, int>> out;  // (edge, piece)
            for (int g : P)
                for (int le : t.faceLE[size_t(g)])
                    if (!P.count(t.other(le, g))) out.push_back({le, g});
            return out;
        };
        auto entryFrom = [&](const std::set<int>& nb, double tEnd, End& e) {
            e.kind = "entry";
            e.tEntry = tEnd;
            if (samePieces(nb) && circularEnd(tEnd)) {
                const auto& sg = t.surf[size_t(*nb.begin())];
                if (sg.kind == SK::Plane && parallel(sg.dir, a)) e.entryFace = *nb.begin();
            }
        };
        auto walk = [&](double tEnd, double sgn) {  // sgn: +1 is end t1 (outward along +a), −1 is t0
            End e;
            const auto nb = neighborsAt(wall, tEnd);
            if (samePieces(nb)) {
                const std::set<int> P = nb;
                const int g = *P.begin();
                const auto& sg = t.surf[size_t(g)];
                const auto pe = pieceEdges(P);
                if (sg.kind == SK::Plane && parallel(sg.dir, a)) {
                    // Disk (all edges are circles of radius r) or annulus (circles r and Rc).
                    std::vector<double> radii;
                    bool circles = true;
                    for (const auto& [le, pc] : pe) {
                        const auto& c = t.curve[size_t(le)];
                        circles = circles && c.kind == CK::Circle && distToLine(c.center, o, a) <= REC_LIN_TOL * 10;
                        radii.push_back(c.radius);
                    }
                    const bool outward = sg.dir.normalized().dot(a) * sgn > 0;  // normal outward, away from the wall: entry
                    double rmax = 0;
                    for (double x : radii) rmax = std::max(rmax, x);
                    if (circles && !outward && rmax <= r + REC_LIN_TOL) {
                        e.kind = "disc";
                        e.faces.assign(P.begin(), P.end());
                        return e;
                    }
                    if (circles && outward && rmax > r + REC_LIN_TOL) {
                        // An annulus is a counterbore bottom if a concave coaxial cylinder Rc follows; else the entry (hub).
                        std::set<int> nb2;
                        for (const auto& [le, pc] : pe) {
                            const auto& c = t.curve[size_t(le)];
                            if (std::abs(c.radius - rmax) <= REC_LIN_TOL) nb2.insert(t.other(le, pc));
                        }
                        bool cbWall = samePieces(nb2);
                        for (int x : nb2) cbWall = cbWall && isWall(x, o, a, rmax);
                        if (cbWall) {
                            double ce0 = std::numeric_limits<double>::max(), ce1 = -ce0;
                            for (const auto& [le, pc] : pieceEdges(nb2))
                                if (t.curve[size_t(le)].kind == CK::Circle) {
                                    const double tc = tOf(t.curve[size_t(le)].center);
                                    ce0 = std::min(ce0, tc), ce1 = std::max(ce1, tc);
                                }
                            const double tFar = sgn > 0 ? ce1 : ce0;
                            std::set<int> nb3 = neighborsAt(std::vector<int>(nb2.begin(), nb2.end()), tFar);
                            for (int x : P) nb3.erase(x);
                            entryFrom(nb3, tFar, e);
                            e.kind = "cbore";
                            e.faces.assign(P.begin(), P.end());
                            e.faces.insert(e.faces.end(), nb2.begin(), nb2.end());
                            e.rc = rmax;
                            e.tc = std::abs(tFar - tEnd);
                            return e;
                        }
                    }
                }
                if (sg.kind == SK::Cone && !sg.convex && distToLine(sg.point, o, a) <= REC_LIN_TOL * 10) {
                    const Vec3 open = sg.dir.normalized();  // from the apex to the opening
                    const bool widensOut = open.dot(a) * sgn > 0;
                    if (!widensOut) {
                        // Drill point bottom: the cone closes at the apex (all edges to other faces are the circle r). Any
                        // other angle is no drill point, and not an opening either: left to the hypotheses (§4.10).
                        bool closes = !pe.empty();
                        for (const auto& [le, pc] : pe) closes = closes && t.curve[size_t(le)].kind == CK::Circle && std::abs(t.curve[size_t(le)].radius - r) <= REC_LIN_TOL;
                        if (closes) {
                            e.kind = std::abs(sg.halfAngle - 59.0 * PI / 180) <= 1e-6 ? "drill" : "fail";
                            e.faces.assign(P.begin(), P.end());
                            return e;
                        }
                    }
                    if (widensOut && std::abs(sg.halfAngle - PI / 4) <= 1e-6) {
                        double rk = 0, tk = tEnd;
                        for (const auto& [le, pc] : pe) {
                            const auto& c = t.curve[size_t(le)];
                            if (c.kind == CK::Circle && c.radius > rk) rk = c.radius, tk = tOf(c.center);
                        }
                        std::set<int> nb2;
                        for (const auto& [le, pc] : pe) {
                            const auto& c = t.curve[size_t(le)];
                            if (c.kind == CK::Circle && std::abs(c.radius - rk) <= REC_LIN_TOL) nb2.insert(t.other(le, pc));
                        }
                        entryFrom(nb2, tk, e);
                        e.kind = "csink";
                        e.faces.assign(P.begin(), P.end());
                        e.rk = rk;
                        return e;
                    }
                }
            }
            entryFrom(nb, tEnd, e);
            return e;
        };
        End lo = walk(t0, -1), hi = walk(t1, +1);
        if (lo.kind == "fail" || hi.kind == "fail") {
            notes.push_back({"hole", "unsupported hole form"});
            continue;
        }
        auto isEntryish = [](const End& e) { return e.kind == "entry" || e.kind == "cbore" || e.kind == "csink"; };
        HoleShape h;
        h.c = o;
        h.r = r;
        for (int w : wall) h.faces.push_back(w);
        const End* en = nullptr;
        const End* bt = nullptr;
        double sgnEntry = 1;
        if (isEntryish(lo) && isEntryish(hi)) {
            // Through hole: entry is the counterbore/countersink, otherwise from the +a⁺ side (§4.4.1 item 4).
            if (lo.kind != "entry" && hi.kind != "entry") {
                notes.push_back({"hole", "two counterbores"});
                continue;
            }
            // FX14-REC-001: two plain ends, the +a⁺ one not usable (not a plane ⊥ axis) and the other one usable — the
            // entry is the opposite end.
            if (lo.kind != "entry" || (hi.kind == "entry" && hi.entryFace < 0 && lo.entryFace >= 0)) en = &lo, bt = &hi, sgnEntry = -1;
            else en = &hi, bt = &lo, sgnEntry = 1;
            h.through = true;
            h.bottom = "through";
        } else if (isEntryish(hi) && (lo.kind == "disc" || lo.kind == "drill")) {
            en = &hi, bt = &lo, sgnEntry = 1;
            h.bottom = lo.kind == "disc" ? "flat" : "drill";
        } else if (isEntryish(lo) && (hi.kind == "disc" || hi.kind == "drill")) {
            en = &lo, bt = &hi, sgnEntry = -1;
            h.bottom = hi.kind == "disc" ? "flat" : "drill";
        } else {
            notes.push_back({"hole", "no entry"});
            continue;
        }
        // A through hole into a cavity (a radial hole through the wall of a sleeve into its bore, §4.4.1 item 5): beyond
        // the wall on one side the axis meets the body again — a through cut would cut that material too; the hole is
        // drilled from the other side up to the far end of the wall.
        if (h.through) {
            const double tMin = REC_LIN_TOL * 100;
            // The axis and points of the hole circle (FX10-REC-003: a cavity narrower than the hole or off its axis — the
            // axis ray passes, the cut does not).
            const Vec3 u0 = a.unitOrthogonal(), v0 = a.cross(u0);
            auto blocked = [&](double tEnd, const Vec3& dir) {
                if (t.k.rayHitsShape(t.s, o + tEnd * a, dir, tMin)) return true;
                for (int j = 0; j < REC_HOLE_RAYS; ++j) {
                    const double th = 2 * PI * j / REC_HOLE_RAYS;
                    if (t.k.rayHitsShape(t.s, o + tEnd * a + 0.95 * r * (std::cos(th) * u0 + std::sin(th) * v0), dir, tMin)) return true;
                }
                return false;
            };
            const bool blockedLo = blocked(t0, -a), blockedHi = blocked(t1, a);
            if (std::getenv("CONSTRUE_REC_TRACE"))
                std::fprintf(stderr, "hole r %.4g at (%.4g %.4g %.4g): lo %s blocked %d, hi %s blocked %d\n", r, o.x(), o.y(), o.z(), lo.kind.c_str(), int(blockedLo), hi.kind.c_str(), int(blockedHi));
            // Into the cavity at the end farLo (true — t0): entry at the other end, the far end of the wall exactly (its exit
            // into the cavity is a saddle curve: the ends and one interior point of an edge do not give its extent) — the
            // extreme point of its edges along the axis.
            auto intoCavity = [&](bool farLo) {
                en = farLo ? &hi : &lo, bt = farLo ? &lo : &hi, sgnEntry = farLo ? 1 : -1;
                h.through = false;
                h.bottom = "cavity";
                const Vec3 out = farLo ? Vec3(-a) : a;
                double far = farLo ? t0 : t1;
                for (int w : wall)
                    for (int le : t.faceLE[size_t(w)])
                        if (t.curve[size_t(le)].kind != CK::Line)
                            if (auto q = t.k.extremePoint(t.s, kernel::CurveSet{le, -1, Vec3::Zero()}, out))
                                far = farLo ? std::min(far, tOf(*q)) : std::max(far, tOf(*q));
                (farLo ? t0 : t1) = far;
            };
            if (blockedLo != blockedHi) {
                if ((blockedLo ? lo : hi).kind == "entry") intoCavity(blockedLo);
            } else if (blockedLo && lo.kind == "entry" && hi.kind == "entry" && (lo.entryFace >= 0 || hi.entryFace >= 0)) {
                // FX14-REC-001: both ends blocked — a through cut would cut the material beyond both of them. The hole is
                // drilled from a usable end (the +a⁺ one first, otherwise the opposite one) up to the far end of the wall.
                intoCavity(hi.entryFace >= 0);
            }
        }
        for (int g : en->faces) h.faces.push_back(g);
        for (int g : bt->faces) h.faces.push_back(g);
        h.dHat = -sgnEntry * a;
        h.entry = o + en->tEntry * a;
        h.entryGood = en->entryFace >= 0;
        h.farGood = h.bottom == "cavity" && bt->kind == "entry" && bt->entryFace >= 0;
        const double tWallEnd = sgnEntry > 0 ? t0 : t1;
        h.depth = std::abs(en->tEntry - tWallEnd);
        if (en->kind == "cbore") h.form = "counterbore", h.rc = en->rc, h.t = en->tc;
        if (en->kind == "csink") h.form = "countersink", h.rk = en->rk;
        if (h.bottom == "cavity") h.pointFree = drillPointFree(t.k, t.s, h);
        for (int g : h.faces) taken.insert(g);
        out.push_back(h);
    }
    return out;
}

// Table row lookup (§4.4.2): fills op as Hole; false means not standard.
bool matchStandard(const HoleShape& h, Op& op) {
    const auto& sizes = regen::holeTables()["sizes"];
    auto near = [](double a, double b) { return std::abs(a - b) <= REC_HOLE_TOL; };
    for (const auto& row : sizes) {
        for (const char* fit : {"normal", "fine", "loose"}) {
            if (!near(2 * h.r, row["clearance"][fit].get<double>())) continue;
            if (h.form == "simple" && h.bottom != "flat") {
                op.holeKind = "simple";
            } else if (h.form == "counterbore" && h.bottom != "flat" && near(2 * h.rc, row["counterbore"]["d"].get<double>()) &&
                       near(h.t, row["counterbore"]["t"].get<double>())) {
                op.holeKind = "counterbore";
            } else if (h.form == "countersink" && h.bottom != "flat" && near(2 * h.rk, row["countersink"]["d"].get<double>())) {
                op.holeKind = "countersink";
            } else {
                continue;
            }
            op.size = row["size"].get<std::string>();
            op.fit = fit;
            return true;
        }
        if (h.form == "simple" && h.bottom != "flat") {
            const auto& pitches = row["pitches"];
            for (size_t i = 0; i < pitches.size(); ++i)
                if (near(2 * h.r, pitches[i]["drill"].get<double>())) {
                    op.holeKind = "tapped";
                    op.size = row["size"].get<std::string>();
                    op.pitch = i == 0 ? 0 : pitches[i]["pitch"].get<double>();
                    return true;
                }
        }
    }
    return false;
}

// Hole operation (§4.4.2); nullopt means it is not removed.
std::optional<Op> holeOp(const HoleShape& h) {
    Op op;
    op.points = {h.entry};
    op.through = h.through;
    op.depth = h.depth;
    op.plane = planeFrom(h.entry, -h.dHat);
    // The Hole feature is only for complex standard holes (§4.4.2, FX10-REC-005): a drill point, a counterbore, a countersink.
    const bool complex = h.form != "simple" || h.bottom == "drill";
    if (complex && h.entryGood && matchStandard(h, op) && (h.bottom != "cavity" || h.pointFree)) {
        op.kind = Op::Kind::Hole;
        return op;
    }
    op.holeKind.clear(), op.size.clear(), op.fit.clear(), op.pitch = 0;
    if (h.form != "simple") return std::nullopt;
    op.kind = Op::Kind::CircleCut;
    op.radius = h.r;
    if (h.bottom == "cavity") return op;  // blind from the entry plane up to the far end of the wall (§4.4.2)
    if (h.through && !h.entryGood) {
        const Vec3 ap = canonicalAxis(h.dHat);
        const Point3 c0 = h.c - h.c.dot(ap) * ap;  // projection of the coordinate origin onto the axis
        op.both = true;
        op.points = {c0};
        op.plane = planeFrom(c0, ap);
        return op;
    }
    if (!h.entryGood) return std::nullopt;
    if (h.through) return op;
    if (h.bottom == "flat") return op;
    return std::nullopt;
}

// A single hole group signature (§4.4.4): equal parameters, common entry plane.
bool sameSignature(const Op& a, const Op& b) {
    const double tol = REC_LIN_TOL * 100;
    if (a.kind != b.kind || a.holeKind != b.holeKind || a.size != b.size || a.fit != b.fit || a.through != b.through || a.both != b.both)
        return false;
    if (std::abs(a.pitch - b.pitch) > tol || std::abs(a.radius - b.radius) > tol || std::abs(a.depth - b.depth) > tol) return false;
    if (a.plane.normal.dot(b.plane.normal) < 1 - 1e-9) return false;
    return std::abs((b.plane.origin - a.plane.origin).dot(a.plane.normal)) <= tol;
}

bool lexLess(const Point3& a, const Point3& b) {
    for (int i = 0; i < 3; ++i)
        if (std::abs(a[i] - b[i]) > REC_LIN_TOL * 10) return a[i] < b[i];
    return false;
}

// Point group → operations (§4.4.4): a hole from the first point and a pattern/mirror, or a single feature.
void groupOps(Op base, std::vector<Point3> pts, std::vector<Op>& out) {
    std::sort(pts.begin(), pts.end(), lexLess);
    const size_t N = pts.size();
    const Vec3 n = base.plane.normal;
    auto single = [&] {
        base.points = pts;
        out.push_back(base);
    };
    auto withSeed = [&](Op p) {
        Op seed = base;
        seed.points = {pts[0]};
        out.push_back(seed);
        p.target = int(out.size()) - 1;
        p.points = pts;
        out.push_back(p);
    };
    if (N >= 3) {
        // Circular: equal distances to the mean, equal angular step.
        Point3 o = Point3::Zero();
        for (const auto& p : pts) o += p;
        o /= double(N);
        const double R = (pts[0] - o).norm();
        bool ok = R > REC_LIN_TOL * 100;
        for (const auto& p : pts) ok = ok && std::abs((p - o).norm() - R) <= REC_LIN_TOL * 100;
        if (ok) {
            const Vec3 u = (pts[0] - o).normalized(), v = n.cross(u);
            std::vector<double> ang;
            for (const auto& p : pts) {
                double t = std::atan2((p - o).dot(v), (p - o).dot(u));
                if (t < -1e-9) t += 2 * PI;
                ang.push_back(t);
            }
            std::sort(ang.begin(), ang.end());
            for (size_t i = 0; i < N; ++i) ok = ok && std::abs(ang[i] - 2 * PI * double(i) / double(N)) <= 1e-6;
        }
        if (ok) {
            Op p;
            p.kind = Op::Kind::Pattern;
            p.circular = true;
            p.axisOrigin = o;
            p.axisDir = -n;  // by the right-hand rule around d̂ (drilling direction)
            p.count = int(N);
            // The start point is the first one; rotation by +2π/N around axisDir must give the adjacent one.
            withSeed(p);
            // Rotation direction: if rotating the first point about −n does not reach a pattern point, reverse the axis.
            const Op& pat = out.back();
            const Vec3 ax = pat.axisDir.normalized();
            const Vec3 r0 = pts[0] - o;
            const double th = 2 * PI / double(N);
            const Vec3 r1 = r0 * std::cos(th) + ax.cross(r0) * std::sin(th) + ax * ax.dot(r0) * (1 - std::cos(th));
            bool hit = false;
            for (const auto& q : pts) hit = hit || (o + r1 - q).norm() <= REC_LIN_TOL * 100;
            if (!hit) out.back().axisDir = -out.back().axisDir;
            return;
        }
    }
    // Mirror (N = 2): symmetry about a built-in plane.
    if (N == 2) {
        const Point3 m = (pts[0] + pts[1]) / 2;
        const Vec3 d = pts[1] - pts[0];
        for (const auto& [name, e] : sketch::datumPlaneNormals()) {  // d is parallel to at most one normal
            if (std::abs(m.dot(e)) > REC_LIN_TOL * 10) continue;
            if (!parallel(d, e, 1e-9)) continue;
            Op p;
            p.kind = Op::Kind::Mirror;
            p.mirrorPlane = name;
            withSeed(p);
            return;
        }
    }
    // Linear: an n1 × n2 grid along the coordinate axes.
    if (N >= 2) {
        std::vector<std::pair<Vec3, std::vector<double>>> axes;
        for (int ax = 0; ax < 3; ++ax) {
            Vec3 e = Vec3::Zero();
            e[ax] = 1;
            if (!perpendicular(e, n, 1e-9)) continue;
            std::vector<double> vals;
            for (const auto& p : pts) {
                const double x = p[ax];
                bool seen = false;
                for (double v : vals) seen = seen || std::abs(v - x) <= REC_LIN_TOL * 10;
                if (!seen) vals.push_back(x);
            }
            std::sort(vals.begin(), vals.end());
            axes.push_back({e, vals});
        }
        std::vector<Op::Direction> dirs;
        size_t total = 1;
        bool ok = !axes.empty();
        for (const auto& [e, vals] : axes) {
            if (vals.size() < 2) continue;
            const double step = vals[1] - vals[0];
            for (size_t i = 1; i < vals.size(); ++i) ok = ok && std::abs(vals[i] - vals[0] - step * double(i)) <= REC_LIN_TOL * 10;
            dirs.push_back({e, int(vals.size()), step});
            total *= vals.size();
        }
        ok = ok && !dirs.empty() && dirs.size() <= 2 && total == N;
        if (ok) {
            // All grid nodes are present.
            for (const auto& p : pts) (void)p;
            Op p;
            p.kind = Op::Kind::Pattern;
            p.linear = dirs;
            withSeed(p);
            return;
        }
    }
    single();
}

// Filling a hole with an inverse tool (§4.4.3, S4-REC-011): the solid of revolution of the hole profile (wall,
// counterbore, countersink, drill point) is united with the body. Accepted only if the tool fits exactly: volume grew by
// exactly the tool volume (no material hit) and area by exactly the entry "caps" minus the hole faces (nothing sticks out).
std::optional<kernel::Shape> fillHole(const kernel::Kernel& k, const kernel::Shape& s, const HoleShape& h) {
    if (!h.entryGood || !spendRemoval()) return std::nullopt;
    const double D = h.depth;
    const double re = h.form == "counterbore" ? h.rc : h.form == "countersink" ? h.rk : h.r;  // entry radius
    std::vector<Point2> poly{{0, 0}, {re, 0}};  // (ρ, depth from the entry)
    if (h.form == "counterbore") poly.push_back({h.rc, h.t}), poly.push_back({h.r, h.t});
    if (h.form == "countersink") poly.push_back({h.r, h.rk - h.r});
    poly.push_back({h.r, D});
    poly.push_back({0, h.bottom == "drill" ? D + h.r / std::tan(59.0 * PI / 180) : D});
    const Vec3 dh = h.dHat.normalized();
    const Vec3 u = dh.unitOrthogonal();
    // Tool; grow is a radial allowance (it enters the wall material: the kernel boolean does not always merge
    // coincident cylindrical surfaces).
    auto makeTool = [&](double grow) {
        kernel::Profile pr;
        pr.plane.origin = h.entry;
        pr.plane.x = u;
        pr.plane.y = dh;
        pr.plane.normal = u.cross(dh).normalized();
        for (size_t i = 0; i < poly.size(); ++i) {
            Point2 a = poly[i], b = poly[(i + 1) % poly.size()];
            if (a.x() > 0) a.x() += grow;
            if (b.x() > 0) b.x() += grow;
            if ((b - a).norm() <= REC_LIN_TOL) continue;
            kernel::Segment3 sg;
            sg.id = "f" + std::to_string(i);
            sg.kind = kernel::Segment3::Kind::Line;
            sg.p1 = h.entry + a.x() * u + a.y() * dh;
            sg.p2 = h.entry + b.x() * u + b.y() * dh;
            pr.outer.segments.push_back(sg);
        }
        return k.revolve(pr, kernel::Axis{h.entry, dh}, kernel::RevolveExtent{});
    };
    auto exact = makeTool(0);
    if (!exact) return std::nullopt;
    auto tool = makeTool(REC_FILL_GROW);
    if (!tool) return std::nullopt;
    auto u2 = k.boolean(kernel::BoolKind::Union, s, tool->shape);
    if (!u2 || k.counts(u2->shape).solids != 1) return std::nullopt;
    const auto m0 = k.massProperties(s), mt = k.massProperties(exact->shape), m1 = k.massProperties(u2->shape);
    if (std::abs(m1.volume - (m0.volume + mt.volume)) > REC_MATCH_REL * m0.volume) return std::nullopt;
    double holeArea = 0;
    for (int f : h.faces) holeArea += k.faceArea(s, f);
    const double caps = PI * re * re + (h.through || h.farGood ? PI * h.r * h.r : 0);
    if (std::abs(m1.area - (m0.area - holeArea + caps)) > REC_MATCH_REL * m0.area) return std::nullopt;
    return u2->shape;
}

// The wall of hole h is still in the body (§4.4.3, FX14-REC-001): a concave cylindrical face of the same axis and radius
// whose centroid lies on the axis within the wall. Face removal of OCCT silently keeps the features it cannot remove
// (its warnings are not observed, kernel-interface.md §9.6д) — a removal of several holes may change the body and still
// leave some of them.
bool wallRemains(const Topo& t, const HoleShape& h) {
    const Vec3 d = h.dHat.normalized();
    const double tol = REC_LIN_TOL * 100;
    for (int f = 0; f < t.faces(); ++f) {
        const auto& si = t.surf[size_t(f)];
        if (si.kind != SK::Cylinder || si.convex || std::abs(si.radius - h.r) > REC_LIN_TOL * 10 || !coaxial(h.c, d, si.point, si.dir)) continue;
        const double x = (t.k.faceCentroid(t.s, f) - h.entry).dot(d);
        if (x >= -tol && x <= h.depth + tol) return true;
    }
    return false;
}

kernel::Shape removeHoles(const kernel::Kernel& k, kernel::Shape s, std::vector<Op>& holeOps, std::vector<Note>& notes) {
    Topo t(k, s);
    const auto holes = findHoles(t, notes);
    std::vector<std::pair<HoleShape, Op>> ok;
    for (const auto& h : holes) {
        auto op = holeOp(h);
        if (!op) {
            notes.push_back({"hole", "non-standard hole r=" + std::to_string(h.r)});
            continue;
        }
        ok.push_back({h, *op});
    }
    if (ok.empty()) return s;
    std::vector<int> all;
    for (const auto& [h, op] : ok) all.insert(all.end(), h.faces.begin(), h.faces.end());
    std::vector<std::pair<HoleShape, Op>> removed;
    // A hole counts as removed only if its wall is gone (FX14-REC-001); the rest is removed one at a time below.
    if (auto r = removeChecked(k, s, all)) {
        const Topo tr(k, r->shape);
        for (const auto& x : ok)
            if (!wallRemains(tr, x.first)) removed.push_back(x);
        if (!removed.empty()) s = r->shape;
    }
    if (removed.size() < ok.size()) {
        // One at a time: face indices change, so recognition is redone after each removal (§4.4.3). A hole that failed
        // is not tried again in this pass (by its entry point and radius); a pass that removed something after a failure
        // is followed by another pass (at most REC_HOLE_PASSES) — the attempts grow linearly with the number of holes.
        auto oneByOne = [&](const std::function<std::optional<kernel::Shape>(const HoleShape&)>& take) {
            for (int pass = 0; pass < REC_HOLE_PASSES && removed.size() < ok.size(); ++pass) {
                std::vector<std::pair<Point3, double>> failed;
                bool anyAfterFail = false;
                for (size_t guard = 0; guard < ok.size() + 1 && removed.size() < ok.size() && !stopped(); ++guard) {
                    Topo tt(k, s);
                    std::vector<Note> dummy;
                    bool any = false;
                    for (const auto& h : findHoles(tt, dummy)) {
                        auto op = holeOp(h);
                        if (!op) continue;
                        if (std::any_of(failed.begin(), failed.end(), [&](const auto& x) {
                                return (x.first - h.entry).norm() <= REC_LIN_TOL * 100 && std::abs(x.second - h.r) <= REC_LIN_TOL * 100;
                            }))
                            continue;
                        if (auto r = take(h)) {
                            s = *r;
                            removed.push_back({h, *op});
                            any = true;
                            anyAfterFail = anyAfterFail || !failed.empty();
                            break;
                        }
                        failed.push_back({h.entry, h.r});
                    }
                    if (!any) break;
                }
                if (!anyAfterFail) break;
            }
        };
        oneByOne([&](const HoleShape& h) -> std::optional<kernel::Shape> {
            if (auto r = removeChecked(k, s, h.faces); r && !wallRemains(Topo(k, r->shape), h)) return r->shape;
            return std::nullopt;
        });
        // Holes not removed by face removal: filling with an inverse tool with a check (§4.4.3).
        oneByOne([&](const HoleShape& h) { return fillHole(k, s, h); });
        if (removed.size() < ok.size()) notes.push_back({"hole", std::to_string(ok.size() - removed.size()) + " holes not removed"});
    }
    // Groups by signature in order of the first point.
    std::vector<std::pair<int, std::pair<Op, std::vector<Point3>>>> groups;
    for (const auto& [h, op] : removed) {
        bool put = false;
        for (auto& [g, v] : groups)
            if (!put && sameSignature(v.first, op)) {
                bool dup = false;
                for (const auto& q : v.second) dup = dup || (q - op.points[0]).norm() <= REC_LIN_TOL * 100;
                if (!dup) v.second.push_back(op.points[0]);
                put = true;
            }
        if (!put) groups.push_back({0, {op, {op.points[0]}}});
    }
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
        auto pa = a.second.second, pb = b.second.second;
        std::sort(pa.begin(), pa.end(), lexLess);
        std::sort(pb.begin(), pb.end(), lexLess);
        return lexLess(pa[0], pb[0]);
    });
    // Radial circular patterns (§4.4.4 item 5): single holes with equal parameters on different entry planes, each the
    // rotation of the first about one axis by a multiple of 2π/N.
    std::vector<int> seedOf(groups.size(), -1);  // group index of the cluster seed; -1 — not in a cluster
    std::vector<std::optional<Op>> radial(groups.size());
    for (size_t i = 0; i < groups.size(); ++i) {
        if (seedOf[i] >= 0 || groups[i].second.second.size() != 1) continue;
        const Op& a = groups[i].second.first;
        std::vector<size_t> members{i};
        for (size_t j = i + 1; j < groups.size(); ++j) {
            if (seedOf[j] >= 0 || groups[j].second.second.size() != 1) continue;
            Op b = groups[j].second.first;
            b.plane = a.plane;  // all but the entry plane
            if (sameSignature(a, b)) members.push_back(j);
        }
        if (members.size() < 2) continue;
        const Point3 p0 = groups[i].second.second[0];
        const Vec3 n0 = a.plane.normal.normalized();
        Vec3 u = Vec3::Zero();
        for (size_t m = 1; m < members.size() && u.norm() < 0.5; ++m) {
            const Vec3 c = n0.cross(groups[members[m]].second.first.plane.normal.normalized());
            if (c.norm() > 1e-6) u = c.normalized();
        }
        if (u.norm() < 0.5) continue;
        u = canonicalAxis(u);
        // Center: p1 − o = R(θ)(p0 − o) in the plane ⊥ u, from the first other member.
        auto angleOf = [&](const Vec3& n) { return std::atan2(u.dot(n0.cross(n)), n0.dot(n)); };
        const Op& b0 = groups[members[1]].second.first;
        const double th1 = angleOf(b0.plane.normal.normalized());
        const Point3 p1 = groups[members[1]].second.second[0];
        if (std::abs(std::sin(th1 / 2)) < 1e-6) continue;
        // (I − R)o = p1 − R·p0 in the plane ⊥ u: solved on the basis (e, u × e).
        const Vec3 e = n0.cross(u).norm() > 1e-6 ? Vec3((n0 - n0.dot(u) * u).normalized()) : Vec3(u.unitOrthogonal());
        const Vec3 f = u.cross(e);
        const Vec3 rhs = p1 - (p0.dot(u) * u + rotateAbout(p0 - p0.dot(u) * u, u, th1));
        Eigen::Matrix2d M;
        M << 1 - std::cos(th1), std::sin(th1), -std::sin(th1), 1 - std::cos(th1);
        const Eigen::Vector2d oc = M.inverse() * Eigen::Vector2d(rhs.dot(e), rhs.dot(f));
        const Point3 o = oc.x() * e + oc.y() * f + p0.dot(u) * u;
        std::vector<double> angles;
        bool ok = true;
        for (size_t m : members) {
            const Op& b = groups[m].second.first;
            const double th = angleOf(b.plane.normal.normalized());
            const Point3 pm = groups[m].second.second[0];
            ok = ok && std::abs(b.plane.normal.normalized().dot(u)) <= 1e-6 &&
                 (o + rotateAbout(p0 - o, u, th) - pm).norm() <= REC_LIN_TOL * 100;
            angles.push_back(th < -1e-9 ? th + 2 * PI : th);
        }
        if (!ok) continue;
        std::sort(angles.begin(), angles.end());
        const double N = double(members.size());
        for (size_t q = 0; q < angles.size(); ++q) ok = ok && std::abs(angles[q] - 2 * PI * double(q) / N) <= 1e-6;
        if (!ok) continue;
        for (size_t m : members) seedOf[m] = int(i);
        Op pat;
        pat.kind = Op::Kind::Pattern;
        pat.circular = true;
        pat.axisOrigin = o;
        pat.axisDir = u;
        pat.count = int(members.size());
        for (size_t m : members) pat.points.push_back(groups[m].second.second[0]);
        radial[i] = pat;
    }
    for (size_t i = 0; i < groups.size(); ++i) {
        auto& g = groups[i].second;
        if (seedOf[i] < 0) {
            groupOps(g.first, g.second, holeOps);
        } else if (seedOf[i] == int(i)) {
            Op seed = g.first;
            seed.points = g.second;
            holeOps.push_back(seed);
            Op pat = *radial[i];
            pat.target = int(holeOps.size()) - 1;
            holeOps.push_back(pat);
        }
    }
    return s;
}

}  // namespace

namespace detail {

std::vector<EdgeKey> edgeKeys(const Topo& t, const std::vector<int>& les) { return mergedKeys(t, les); }
std::vector<BlendFace> blendFaces(const Topo& t) { return filletCandidates(t); }

Peel peelBody(const kernel::Kernel& k, const kernel::Shape& body, std::vector<Note>& notes) {
    Peel p;
    // Lettering (§4.7) first: letter outlines would produce false fillets and cuts; each lettering is its own block.
    kernel::Shape s0 = body;
    for (int guard = 0; guard < 8 && !stopped(); ++guard) {
        auto tr = removeText(k, s0, notes);
        if (!tr) break;
        p.texts.push_back({tr->op, s0});
        s0 = tr->restored;
    }
    // Fillets and chamfers in turn while either removes a step: a chamfer made over a fillet frees the fillet, and the
    // other way round (FX10-REC-006).
    kernel::Shape s = s0;
    for (int round = 0; round < 8 && !stopped(); ++round) {
        s = removeFillets(k, s, p.steps, notes);
        const size_t afterFillets = p.steps.size();
        s = removeChamfers(k, s, p.steps, notes);
        if (p.steps.size() == afterFillets) break;  // no chamfer removed: the fillets have nothing new
    }
    p.beforeHoles = s;
    s = removeHoles(k, s, p.holes, notes);
    // A loft added onto the body (§4.9): removed after the holes, built after the base.
    if (auto lr = removeLoft(k, s, notes)) {
        p.loft = std::make_pair(lr->op, s);
        s = lr->restored;
    }
    p.core = s;
    return p;
}

CoreSplit splitCore(const kernel::Kernel& k, const kernel::Shape& core, std::vector<Note>& notes) {
    CoreSplit r;
    r.core = core;
    // Base (§4.5): a single extrusion or revolution; otherwise a revolution with cuts (§4.6); otherwise layers.
    r.base = decomposeBase(k, core, r.kind, notes, true);
    if (r.base.empty()) {
        // Layers with a feature pattern (§4.8) beat a revolution with cuts: ribs on a disk are a pattern, not a cut
        // around them (§4.6 item 3).
        std::string slabKind;
        std::vector<Note> slabNotes;
        std::vector<Op> slabs = splitPatterns(decomposeBase(k, core, slabKind, slabNotes, false));
        const bool slabPattern = std::any_of(slabs.begin(), slabs.end(), [](const Op& o) { return o.kind == Op::Kind::Pattern; });
        std::optional<RevolveCuts> rc;
        if (!slabPattern) {
            // Cuts by face removal (§4.6); if face removal does not free the body — by a section (§4.6а); notes of the
            // way that did not succeed are kept only if both fail.
            std::vector<Note> cutNotes, sectionNotes;
            rc = removeRevolveCuts(k, core, cutNotes);
            if (!rc) rc = revolveCutsBySection(k, core, sectionNotes);
            if (!rc) notes.insert(notes.end(), cutNotes.begin(), cutNotes.end());
            if (!rc) notes.insert(notes.end(), sectionNotes.begin(), sectionNotes.end());
        }
        if (rc) {
            r.core = rc->restored;
            r.base = {rc->revolve};
            r.kind = "revolve";
            r.cutBlocks = groupRevolveCuts(*rc);
        } else {
            r.base = slabs;
            r.kind = slabKind;
            r.layered = slabs.size() > 1;
            notes.insert(notes.end(), slabNotes.begin(), slabNotes.end());
        }
    }
    r.base = splitPatterns(r.base);  // identical loops in a pattern: feature and pattern (§4.8; idempotent)
    return r;
}

}  // namespace detail

Plan recognize(const kernel::Kernel& k, const kernel::Shape& body, const Cancel* cancel) {
    Plan plan;
    plan.core = body;
    if (body.null() || k.counts(body).solids != 1) {
        plan.notes.push_back({"multiSolid", std::to_string(body.null() ? 0 : k.counts(body).solids) + " solids"});
        return plan;
    }
    // Work limits (§3, S4-REC-023): removal attempts are limited by the budget (what is not removed stays in the body);
    // there is no face-count limit — a huge body is stopped by the user with «Прервать» ("Abort") (user's instruction 2026-10-20).
    struct BudgetScope {
        Budget saved;
        explicit BudgetScope(const Cancel* c) : saved(budget()) { budget() = Budget{0, false, c}; }
        ~BudgetScope() { budget() = saved; }
    } budgetScope(cancel);
    // The body's own tolerance for axis comparisons (§2.1): healShape with tolerance 0 only measures it.
    struct TolScope {
        double saved;
        explicit TolScope(double t) : saved(bodyTolerance()) { bodyTolerance() = t; }
        ~TolScope() { bodyTolerance() = saved; }
    } tolScope([&] {
        auto h = k.healShape(body, 0);
        return h ? h->toleranceBefore : 0.0;
    }());
    const auto t0 = std::chrono::steady_clock::now();
    auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    Peel peel = peelBody(k, body, plan.notes);
    if (std::getenv("CONSTRUE_REC_TRACE")) std::fprintf(stderr, "peel done %.1f s\n", since());
    CoreSplit split = splitCore(k, peel.core, plan.notes);
    if (std::getenv("CONSTRUE_REC_TRACE")) std::fprintf(stderr, "split done %.1f s\n", since());
    // Several fillets or chamfers along the extrusion belong to its profile (§4.1): the body without the holes is one extrusion.
    if (peel.steps.size() >= 2 && peel.texts.empty() && !peel.loft) {
        std::vector<Note> holeNotes;
        Peel direct;
        direct.beforeHoles = body;
        direct.core = removeHoles(k, body, direct.holes, holeNotes);
        std::string kind;
        std::vector<Note> baseNotes;
        auto base = decomposeBase(k, direct.core, kind, baseNotes, true);
        if (!base.empty() && planCost(direct, base) + 1 < planCost(peel, split.base)) {
            peel = std::move(direct);
            split = CoreSplit{};
            split.core = peel.core;
            split.base = splitPatterns(base);
            split.kind = kind;
            plan.notes = holeNotes;
        }
    }
    // The hypothesis tree (§4.10): when the obvious removals do not end in a simple base. levels: from the source body
    // inwards, what is built after the deepest core.
    std::vector<SearchLevel> levels;
    if (needsSearch(split)) {
        std::vector<Note> searchNotes;
        if (auto found = searchHistory(k, peel, split, searchNotes)) {
            levels = std::move(found->levels);
            split = std::move(found->core);
            plan.notes.clear();
            plan.notes.insert(plan.notes.end(), found->notes.begin(), found->notes.end());
        } else {
            levels.push_back({std::move(peel), {}, {}});
        }
        plan.notes.insert(plan.notes.end(), searchNotes.begin(), searchNotes.end());
    } else {
        levels.push_back({std::move(peel), {}, {}});
    }
    if (budget().exhausted)
        plan.notes.push_back({"limit", std::to_string(REC_MAX_REMOVALS) + " removal attempts spent: the rest stays in the body"});
    plan.baseKind = split.kind;
    plan.core = split.core;
    plan.baseOps = split.base.size();
    // Operation blocks (§5.5): the body after the block and the index of the next block.
    auto push = [&](const std::vector<Op>& block, const kernel::Shape& after) {
        const size_t first = plan.ops.size(), end = first + block.size();
        for (auto op : block) {
            if (op.target >= 0) op.target += int(first);
            plan.ops.push_back(op);
            plan.after.push_back(after);
            plan.resume.push_back(end);
        }
    };
    push(split.base, split.core);
    // Cuts on a body of revolution: in reverse removal order, a cut or a pattern/mirror of cuts per block (§4.6).
    for (const auto& [block, before] : split.cutBlocks) push(block, before);
    // From the deepest level to the source body: what the level's hypothesis took away, then the level's finishing.
    for (auto lv = levels.rbegin(); lv != levels.rend(); ++lv) {
        if (!lv->ops.empty()) push(lv->ops, lv->peel.core);
        const Peel& pl = lv->peel;
        if (pl.loft) push({pl.loft->first}, pl.loft->second);
        push(pl.holes, pl.beforeHoles);  // target indices of hole patterns are within the block
        for (auto it = pl.steps.rbegin(); it != pl.steps.rend(); ++it) push(it->ops, it->before);
        for (auto it = pl.texts.rbegin(); it != pl.texts.rend(); ++it) push({it->first}, it->second);
    }
    mergeLevelOps(plan);
    tracePlan(k, plan);
    return plan;
}

bool bodiesMatch(const kernel::Kernel& k, const kernel::Shape& a, const kernel::Shape& b, double roundTol) {
    if (a.null() || b.null() || k.counts(b).solids != k.counts(a).solids) return false;
    const auto ma = k.massProperties(a), mb = k.massProperties(b);
    // Rounding of dimensions (§5.4) moves faces by no more than dist: volume by area·dist, area by
    // 2·(sum of edge lengths)·dist.
    const double dist = REC_MATCH_DIST + (roundTol > 0 ? REC_ROUND_SPREAD * roundTol : 0);
    if (std::getenv("CONSTRUE_REC_TRACE")) std::fprintf(stderr, "match: V %.6f %.6f A %.6f %.6f round %.3g\n", ma.volume, mb.volume, ma.area, mb.area, roundTol);
    double edges = 0;
    for (int e = 0, ne = k.counts(a).edges; e < ne; ++e) edges += k.edgeLength(a, e);
    const double skin = roundTol > 0 ? dist : REC_MATCH_SKIN;
    const double dv = ma.area * skin, ds = 2 * edges * skin;
    if (std::abs(ma.volume - mb.volume) > REC_MATCH_REL * ma.volume + dv) return false;
    if (std::abs(ma.area - mb.area) > REC_MATCH_REL * ma.area + ds) return false;
    auto points = [&](const kernel::Shape& s) {
        std::vector<Point3> pts;
        const auto ct = kernel::canonicalTopology(k, s);
        for (int v : ct.essentialVertices) pts.push_back(k.vertexPoint(s, v));
        const int nf = k.counts(s).faces;
        for (int f = 0; f < nf; ++f) pts.push_back(k.closestPointOnFace(s, f, k.faceCentroid(s, f)));
        return pts;
    };
    const bool trace = std::getenv("CONSTRUE_REC_TRACE") != nullptr;
    for (const auto& p : points(a))
        if (const double d = k.distanceToShape(b, p); d > dist) {
            if (trace) std::fprintf(stderr, "match: point (%.4f %.4f %.4f) of the first body %.4g away\n", p.x(), p.y(), p.z(), d);
            return false;
        }
    for (const auto& p : points(b))
        if (const double d = k.distanceToShape(a, p); d > dist) {
            if (trace) std::fprintf(stderr, "match: point (%.4f %.4f %.4f) of the second body %.4g away\n", p.x(), p.y(), p.z(), d);
            return false;
        }
    return true;
}

double roundValue(double v, double tol) {
    if (!(tol > 0) || !std::isfinite(v) || v == 0) return v;
    // Steps in decreasing order: 1·10ⁿ and 5·10ⁿ⁻¹ from the first power of ten ≥ |v|; a step ≤ 2·tol shifts ≤ tol —
    // the search is finite.
    double p = std::pow(10.0, std::ceil(std::log10(std::abs(v))));
    for (int guard = 0; guard < 64; ++guard, p /= 10) {
        for (const double step : {p, p / 2}) {
            const double r = std::round(v / step) * step;
            if (r != 0 && std::abs(r - v) <= tol) {
                // Output without binary arithmetic noise: via decimal text with the step's precision.
                const double digits = std::max(0.0, std::ceil(-std::log10(step)) + 1);
                const double scale = std::pow(10.0, digits);
                return std::round(r * scale) / scale;
            }
        }
    }
    return v;
}

std::vector<Plan> finishingReordered(const Plan& plan, size_t failed) {
    std::vector<Plan> out;
    const size_t n = plan.ops.size();
    if (failed >= n || failed < plan.baseOps || plan.after.size() != n || plan.resume.size() != n) return out;
    auto finishing = [&](size_t i) { return plan.ops[i].kind == Op::Kind::Fillet || plan.ops[i].kind == Op::Kind::Chamfer; };
    if (!finishing(failed)) return out;
    // The run of fillets and chamfers with the failed one.
    size_t r0 = failed, r1 = failed + 1;
    while (r0 > plan.baseOps && finishing(r0 - 1)) --r0;
    while (r1 < n && finishing(r1)) ++r1;
    if (r1 - r0 < 2) return out;
    std::vector<std::vector<size_t>> orders;
    std::vector<size_t> id(n);
    for (size_t i = 0; i < n; ++i) id[i] = i;
    // 1: the run reversed; 2: the failed operation first, the rest of the run in order.
    std::vector<size_t> rev = id;
    std::reverse(rev.begin() + long(r0), rev.begin() + long(r1));
    orders.push_back(rev);
    std::vector<size_t> first = id;
    first.erase(first.begin() + long(failed));
    first.insert(first.begin() + long(r0), failed);
    if (first != rev && first != id) orders.push_back(first);
    for (const auto& order : orders) {
        if (int(out.size()) >= REC_REORDER_VARIANTS) break;
        std::vector<int> pos(n, -1);
        for (size_t k = 0; k < n; ++k) pos[order[k]] = int(k);
        Plan p = plan;
        bool ok = true;
        for (size_t k = 0; k < n && ok; ++k) {
            Op op = plan.ops[order[k]];
            for (int* ref : {&op.target, &op.sketchOf}) {
                if (*ref < 0) continue;
                if (*ref >= int(n) || pos[size_t(*ref)] >= int(k)) ok = false;
                else *ref = pos[size_t(*ref)];
            }
            p.ops[k] = std::move(op);
            p.after[k] = plan.after[order[k]];
            p.resume[k] = k + 1;  // one attempt: every operation its own block
        }
        if (ok) out.push_back(std::move(p));
    }
    return out;
}

}  // namespace construe::rec
