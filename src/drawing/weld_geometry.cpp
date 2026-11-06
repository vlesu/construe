// Weld seam geometry (drawing/weld_geometry.h, spec/weld.md §6а, §6б).
#include "drawing/weld_geometry.h"

#include "regen/naming.h"
#include "base/geom2.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace construe::drawing {

double WeldPath::length() const {
    double l = 0;
    for (const auto& p : pieces) l += p.length;
    return l;
}

namespace {

constexpr int ARC_SEGMENTS = 64;  // polyline of a full circle (as WELD_CIRCLE_SEGMENTS of the drawing)

// Resolved entries of a reference: EXACT, REMAPPED — the entity, SPLIT — all parts (weld.md §6).
std::vector<int> resolvedEntries(const naming::NameTable& names, const std::string& ref, naming::EntityKind kind) {
    const auto rr = naming::resolve(ref, kind, names);
    if (rr.error || rr.entities.empty()) return {};
    if (rr.status != naming::ResolveStatus::Exact && rr.status != naming::ResolveStatus::Remapped && rr.status != naming::ResolveStatus::Split)
        return {};
    std::vector<int> out;
    for (const auto& e : rr.entities) out.push_back(e.index);
    return out;
}

// Part (assembly part path) of a body of the virtual state: the longest part path that prefixes its id.
std::string partOf(const regen::PartState& s, const std::string& bodyId) {
    std::string best;
    for (const auto& p : s.assemblyParts)
        if (bodyId.size() > p.path.size() && bodyId.compare(0, p.path.size(), p.path) == 0 && bodyId[p.path.size()] == '/' &&
            p.path.size() > best.size())
            best = p.path;
    return best.empty() ? bodyId : best;
}

// Orthogonal pair (e1, e2) to unit n (assembly.md §7.4): e = the basis vector least parallel to n.
std::pair<Vec3, Vec3> orthoPair(const Vec3& n) {
    int i = 0;
    if (std::abs(n.y()) < std::abs(n[i])) i = 1;
    if (std::abs(n.z()) < std::abs(n[i])) i = 2;
    const Vec3 u = n.cross(Vec3::Unit(i)).normalized();
    return {u, n.cross(u)};
}

// An edge as a curve over u ∈ [0, 1] (§6а "Кривая ребра").
struct Curve {
    WeldPiece::Kind kind = WeldPiece::Kind::Line;
    bool closed = false;
    Point3 p1 = Point3::Zero(), p2 = Point3::Zero();  // Line
    Point3 center = Point3::Zero();                   // Arc
    Vec3 axis = Vec3::UnitZ(), e1 = Vec3::UnitX(), e2 = Vec3::UnitY();
    double radius = 0, a0 = 0, sweep = 0;
    std::vector<Point3> pts;  // Polyline
    std::vector<double> cum;  // cumulative lengths of pts
    double len = 0;

    Point3 at(double u) const {
        switch (kind) {
            case WeldPiece::Kind::Line: return p1 + u * (p2 - p1);
            case WeldPiece::Kind::Arc: {
                const double a = a0 + u * sweep;
                return center + radius * (std::cos(a) * e1 + std::sin(a) * e2);
            }
            case WeldPiece::Kind::Polyline: {
                const double d = std::clamp(u, 0.0, 1.0) * len;
                const auto it = std::upper_bound(cum.begin(), cum.end(), d);
                const size_t i = it == cum.begin() ? 0 : std::min(size_t(it - cum.begin()) - 1, pts.size() - 2);
                const double l = cum[i + 1] - cum[i];
                return l > 0 ? Point3(pts[i] + (pts[i + 1] - pts[i]) * ((d - cum[i]) / l)) : pts[i];
            }
        }
        return p1;
    }

    // The piece of the curve over [lo, hi] (§6а "Куски").
    WeldPiece piece(double lo, double hi) const {
        WeldPiece p;
        p.kind = kind;
        p.length = (hi - lo) * len;
        if (kind == WeldPiece::Kind::Line) {
            p.points = {at(lo), at(hi)};
        } else if (kind == WeldPiece::Kind::Arc) {
            p.center = center;
            p.axis = axis;
            p.radius = radius;
            p.sweep = (hi - lo) * sweep;
            const int n = std::max(1, int(std::ceil(std::abs(p.sweep) / (TWO_PI / ARC_SEGMENTS) - 1e-9)));
            for (int i = 0; i <= n; ++i) p.points.push_back(at(lo + (hi - lo) * i / n));
            if (p.sweep < 0) {  // clockwise: the same arc counterclockwise about −axis
                p.axis = -axis;
                p.sweep = -p.sweep;
            }
        } else {
            p.points.push_back(at(lo));
            for (size_t i = 1; i + 1 < pts.size(); ++i)
                if (cum[i] > lo * len && cum[i] < hi * len) p.points.push_back(pts[i]);
            p.points.push_back(at(hi));
        }
        return p;
    }
};

class Builder {
public:
    Builder(const kernel::Kernel& k, const regen::PartState& s) : k_(k), s_(s) {}

    const kernel::Shape& shape(int body) const { return s_.bodies.at(size_t(body)).shape; }

    const kernel::CanonicalTopology& topo(int body) {
        auto it = topo_.find(body);
        if (it == topo_.end()) it = topo_.emplace(body, kernel::canonicalTopology(k_, shape(body))).first;
        return it->second;
    }

    const BBox3& box(int body) {
        auto it = box_.find(body);
        if (it == box_.end()) it = box_.emplace(body, k_.bbox(shape(body))).first;
        return it->second;
    }

    // Names-table edge entry of a logical edge of a body (-1 — none).
    int edgeEntry(int body, int logical) {
        if (edgeIndex_.empty())
            for (size_t i = 0; i < s_.names.edges.size(); ++i) edgeIndex_[{s_.names.edges[i].body, s_.names.edges[i].logical}] = int(i);
        auto it = edgeIndex_.find({body, logical});
        return it == edgeIndex_.end() ? -1 : it->second;
    }

    // The curve of a logical edge (§6а "Кривая ребра"): a line, a circle or an arc, otherwise the tessellation polyline.
    std::optional<Curve> curve(int body, int logical) {
        const kernel::Shape& sh = shape(body);
        const auto ci = k_.edgeCurve(sh, logical);
        Curve c;
        if (ci.kind == kernel::CurveInfo::Kind::Line) {
            c.kind = WeldPiece::Kind::Line;
            c.p1 = ci.p1, c.p2 = ci.p2;
            c.len = (ci.p2 - ci.p1).norm();
            return c.len > 0 ? std::optional(c) : std::nullopt;
        }
        if (ci.kind == kernel::CurveInfo::Kind::Circle) {
            c.kind = WeldPiece::Kind::Arc;
            c.center = ci.center;
            c.axis = ci.axis.normalized();
            std::tie(c.e1, c.e2) = orthoPair(c.axis);
            c.radius = ci.radius;
            auto angleOf = [&](const Point3& p) { return std::atan2((p - c.center).dot(c.e2), (p - c.center).dot(c.e1)); };
            if (ci.closed) {
                c.closed = true;
                c.sweep = TWO_PI;
            } else {
                const auto& ends = topo(body).logicalEdges.at(size_t(logical)).ends;
                const int ei = edgeEntry(body, logical);
                if (ends.size() != 2 || ei < 0) return std::nullopt;
                const Point3 A = k_.vertexPoint(sh, ends[0]), B = k_.vertexPoint(sh, ends[1]);
                c.a0 = angleOf(A);
                c.sweep = geom2::ccwSweep(c.a0, angleOf(B));
                if (c.sweep == 0) c.sweep = TWO_PI;
                if (geom2::ccwSweep(c.a0, angleOf(s_.names.edges[size_t(ei)].key)) > c.sweep) c.sweep -= TWO_PI;  // the key is on the other way
            }
            c.len = std::abs(c.sweep) * c.radius;
            return c.len > 0 ? std::optional(c) : std::nullopt;
        }
        auto tit = tess_.find(body);
        if (tit == tess_.end()) {
            auto t = k_.tessellate(sh, {WELD_TESS_CHORD_GEOM, 0.2});
            tit = tess_.emplace(body, t ? std::move(*t) : kernel::Tessellation{}).first;
        }
        c.kind = WeldPiece::Kind::Polyline;
        for (const auto& em : tit->second.edges) {
            if (em.edge != logical || em.polylines.empty()) continue;
            const auto& f = em.polylines.front();
            for (size_t i = 0; i + 2 < f.size(); i += 3) c.pts.emplace_back(f[i], f[i + 1], f[i + 2]);
            break;
        }
        if (c.pts.size() < 2) return std::nullopt;
        c.cum.push_back(0);
        for (size_t i = 1; i < c.pts.size(); ++i) c.cum.push_back(c.cum.back() + (c.pts[i] - c.pts[i - 1]).norm());
        c.len = c.cum.back();
        return c.len > 0 ? std::optional(c) : std::nullopt;
    }

    double dist(const Point3& p, int faceEntry) const {
        const auto& fe = s_.names.faces.at(size_t(faceEntry));
        return k_.distanceToFace(shape(fe.body), fe.face, p);
    }
    bool onFace(const Point3& p, int faceEntry) const { return dist(p, faceEntry) <= WELD_CONTACT_TOL; }

    // Pieces of curve c lying on face entry b (§6а "Куски").
    std::vector<WeldPiece> contact(const Curve& c, int b) const {
        const int n = std::max(WELD_CONTACT_MIN_SAMPLES, int(std::ceil(c.len / WELD_CONTACT_STEP)));
        std::vector<char> on(size_t(n) + 1);
        for (int i = 0; i <= n; ++i) on[size_t(i)] = onFace(c.at(double(i) / n), b);
        std::vector<WeldPiece> out;
        if (std::all_of(on.begin(), on.end(), [](char x) { return x; })) {
            out.push_back(c.piece(0, 1));
            return out;
        }
        // The boundary between a sample off and a sample on: bisection with the tolerance of the sample on raised by
        // WELD_REFINE_TOL (exact contact — the boundary within it, not WELD_CONTACT_TOL beyond).
        auto refine = [&](double off, double in) {
            const double tol = std::min(WELD_CONTACT_TOL, dist(c.at(in), b) + WELD_REFINE_TOL);
            for (int i = 0; i < WELD_CONTACT_REFINE; ++i) {
                const double m = (off + in) / 2;
                (dist(c.at(m), b) <= tol ? in : off) = m;
            }
            return in;
        };
        // A closed curve is walked from its first sample off the face: a run never wraps.
        int start = 0;
        if (c.closed)
            while (on[size_t(start)]) ++start;
        const int count = c.closed ? n : n + 1;
        auto idx = [&](int j) { return (start + j) % (c.closed ? n : n + 1); };
        auto uOf = [&](int j) { return double(start + j) / n; };  // may exceed 1 on a closed curve (periodic)
        for (int j = 0; j < count;) {
            if (!on[size_t(idx(j))]) {
                ++j;
                continue;
            }
            int e = j;
            while (e + 1 < count && on[size_t(idx(e + 1))]) ++e;
            const bool openEnd = c.closed && e + 1 == count;  // the run reaches the start sample again
            const double lo = j == 0 && !c.closed ? 0.0 : refine(uOf(j - 1), uOf(j));
            const double hi = !c.closed && e == count - 1 ? 1.0 : refine(openEnd ? uOf(count) : uOf(e + 1), uOf(e));
            if ((hi - lo) * c.len >= WELD_MIN_PIECE) out.push_back(c.piece(lo, hi));
            j = e + 1;
        }
        return out;
    }

    const kernel::Kernel& k_;
    const regen::PartState& s_;
    std::map<int, kernel::CanonicalTopology> topo_;
    std::map<int, kernel::Tessellation> tess_;
    std::map<std::pair<int, int>, int> edgeIndex_;
    std::map<int, BBox3> box_;
    static constexpr double WELD_TESS_CHORD_GEOM = 0.05;  // as WELD_TESS_CHORD of the drawing (drawing.md §6.19)
};

double distToPolyline(const Point3& p, const std::vector<Point3>& pl) {
    double best = 1e300;
    for (size_t i = 0; i + 1 < pl.size(); ++i) {
        const Vec3 d = pl[i + 1] - pl[i];
        const double l2 = d.squaredNorm();
        const double t = l2 > 0 ? std::clamp((p - pl[i]).dot(d) / l2, 0.0, 1.0) : 0.0;
        best = std::min(best, (p - (pl[i] + t * d)).norm());
    }
    if (pl.size() == 1) best = (p - pl[0]).norm();
    return best;
}

// Segment a–b meets the box enlarged by WELD_CONTACT_TOL.
bool segmentNearBox(const Point3& a, const Point3& b, const BBox3& box) {
    if (box.isEmpty()) return false;
    double t0 = 0, t1 = 1;
    for (int i = 0; i < 3; ++i) {
        const double lo = box.min()[i] - WELD_CONTACT_TOL, hi = box.max()[i] + WELD_CONTACT_TOL, d = b[i] - a[i];
        if (std::abs(d) < 1e-15) {
            if (a[i] < lo || a[i] > hi) return false;
            continue;
        }
        double u0 = (lo - a[i]) / d, u1 = (hi - a[i]) / d;
        if (u0 > u1) std::swap(u0, u1);
        t0 = std::max(t0, u0), t1 = std::min(t1, u1);
        if (t0 > t1) return false;
    }
    return true;
}

Point3 midOf(const WeldPiece& p) {
    if (p.kind == WeldPiece::Kind::Arc) {
        const Vec3 r = p.points.front() - p.center;
        const Vec3 t = p.axis.cross(r);
        const double a = p.sweep / 2;
        return p.center + std::cos(a) * r + std::sin(a) * t;
    }
    double half = 0;
    for (size_t i = 0; i + 1 < p.points.size(); ++i) half += (p.points[i + 1] - p.points[i]).norm();
    half /= 2;
    for (size_t i = 0; i + 1 < p.points.size(); ++i) {
        const double l = (p.points[i + 1] - p.points[i]).norm();
        if (half <= l && l > 0) return p.points[i] + (p.points[i + 1] - p.points[i]) * (half / l);
        half -= l;
    }
    return p.points.front();
}

}  // namespace

WeldPath weldPath(const kernel::Kernel& k, const regen::PartState& s, const doc::Json& weld) {
    WeldPath out;
    Builder b(k, s);
    if (weld.contains("faces")) {
        // §6а: faces of different parts — the edges of one lying on the other.
        std::vector<std::pair<int, std::string>> faces;  // names entry, part
        for (const auto& r : weld["faces"]) {
            const auto es = resolvedEntries(s.names, r.get<std::string>(), naming::EntityKind::Face);
            if (es.empty()) {
                out.error = Error("WELD_FACE_LOST").with("ref", r.get<std::string>());
                return out;
            }
            for (int e : es) faces.emplace_back(e, partOf(s, s.bodies.at(size_t(s.names.faces[size_t(e)].body)).id));
        }
        for (size_t i = 0; i < faces.size(); ++i)
            for (size_t j = i + 1; j < faces.size(); ++j) {
                if (faces[i].second == faces[j].second) continue;
                for (const auto& [a, bb] : {std::pair{faces[i].first, faces[j].first}, std::pair{faces[j].first, faces[i].first}}) {
                    const auto& fa = s.names.faces[size_t(a)];
                    const auto& fb = s.names.faces[size_t(bb)];
                    const BBox3 boxB = b.box(fb.body);
                    const auto& t = b.topo(fa.body);
                    for (size_t le = 0; le < t.logicalEdges.size(); ++le) {
                        const auto& L = t.logicalEdges[le];
                        if (L.faces.first != fa.face && L.faces.second != fa.face) continue;
                        const auto c = b.curve(fa.body, int(le));
                        if (!c) continue;
                        // Quick rejection: a line that misses the box of the body of B (slab test).
                        if (c->kind == WeldPiece::Kind::Line && !segmentNearBox(c->p1, c->p2, boxB)) continue;
                        for (auto& p : b.contact(*c, bb)) {
                            const Point3 m = midOf(p);
                            const bool dup = std::any_of(out.pieces.begin(), out.pieces.end(),
                                                         [&](const WeldPiece& q) { return distToPolyline(m, q.points) <= WELD_CONTACT_TOL; });
                            if (dup) continue;
                            p.faceA = a;
                            p.faceB = bb;
                            out.pieces.push_back(std::move(p));
                        }
                    }
                }
            }
        if (out.pieces.empty()) out.error = Error("WELD_LINE_NOT_FOUND");
        return out;
    }
    // Edges (S5-WELD-001): the whole edge; B — a face of another part under the edge middle, A — the edge face least
    // parallel to B (for the body, §6б).
    for (const auto& r : weld.value("edges", doc::Json::array())) {
        for (int ei : resolvedEntries(s.names, r.get<std::string>(), naming::EntityKind::Edge)) {
            const auto& ee = s.names.edges[size_t(ei)];
            const auto c = b.curve(ee.body, ee.logical);
            if (!c) continue;
            WeldPiece p = c->piece(0, 1);
            const Point3 m = c->at(0.5);
            const std::string part = partOf(s, s.bodies.at(size_t(ee.body)).id);
            for (size_t fi = 0; fi < s.names.faces.size() && p.faceB < 0; ++fi) {
                const auto& fe = s.names.faces[fi];
                if (partOf(s, s.bodies.at(size_t(fe.body)).id) == part) continue;
                const BBox3 box = b.box(fe.body);
                const Vec3 lo = box.min() - Vec3::Constant(WELD_CONTACT_TOL), hi = box.max() + Vec3::Constant(WELD_CONTACT_TOL);
                if (!((m.array() >= lo.array()).all() && (m.array() <= hi.array()).all())) continue;
                if (k.distanceToFace(b.shape(fe.body), fe.face, m) <= WELD_CONTACT_TOL) p.faceB = int(fi);
            }
            if (p.faceB >= 0) {
                const auto& fb = s.names.faces[size_t(p.faceB)];
                const Vec3 nB = k.faceNormalAt(b.shape(fb.body), fb.face, m).normalized();
                double best = 2;
                for (int f : {ee.f1, ee.f2}) {
                    if (f < 0 || size_t(f) >= s.names.faces.size()) continue;
                    const auto& fa = s.names.faces[size_t(f)];
                    const double c2 = std::abs(k.faceNormalAt(b.shape(fa.body), fa.face, m).normalized().dot(nB));
                    if (c2 < best) best = c2, p.faceA = f;
                }
            }
            out.pieces.push_back(std::move(p));
        }
    }
    return out;
}

namespace {

// Closed triangle loop (q0, q1, q2) counterclockwise about n.
kernel::Loop3 triangle(Point3 q0, Point3 q1, Point3 q2, const Vec3& n) {
    if ((q1 - q0).cross(q2 - q0).dot(n) < 0) std::swap(q1, q2);
    kernel::Loop3 l;
    const Point3 q[3] = {q0, q1, q2};
    for (int i = 0; i < 3; ++i) {
        kernel::Segment3 s;
        s.id = "s" + std::to_string(i + 1);
        s.kind = kernel::Segment3::Kind::Line;
        s.p1 = q[i];
        s.p2 = q[(i + 1) % 3];
        l.segments.push_back(s);
    }
    return l;
}

// Bead loop (§6б): chord L–R and the arc through T, counterclockwise about n.
kernel::Loop3 bead(Point3 L, Point3 R, const Point3& T, const Vec3& n, const Point3& center, double radius) {
    if ((R - L).cross(T - L).dot(n) < 0) std::swap(L, R);
    kernel::Loop3 l;
    kernel::Segment3 chord;
    chord.id = "s1";
    chord.kind = kernel::Segment3::Kind::Line;
    chord.p1 = L;
    chord.p2 = R;
    kernel::Segment3 arc;
    arc.id = "s2";
    arc.kind = kernel::Segment3::Kind::Arc;
    arc.center = center;
    arc.radius = radius;
    arc.start = R;
    arc.end = L;
    l.segments = {chord, arc};
    return l;
}

}  // namespace

WeldBody weldBody(const kernel::Kernel& k, const regen::PartState& s, const WeldPath& path, const std::string& joint,
                  std::optional<double> leg) {
    WeldBody out;
    std::vector<kernel::Shape> solids;
    for (const auto& p : path.pieces) {
        if (p.faceA < 0 || p.faceB < 0 || p.points.size() < 2) continue;
        const Point3 P = p.points.front();
        Vec3 t = p.kind == WeldPiece::Kind::Arc ? Vec3(p.axis.cross(P - p.center)) : Vec3(p.points[1] - p.points[0]);
        if (t.norm() < 1e-12) continue;
        t.normalize();
        const auto& fa = s.names.faces[size_t(p.faceA)];
        const auto& fb = s.names.faces[size_t(p.faceB)];
        const Vec3 nA = k.faceNormalAt(s.bodies.at(size_t(fa.body)).shape, fa.face, P).normalized();
        const Vec3 nB = k.faceNormalAt(s.bodies.at(size_t(fb.body)).shape, fb.face, P).normalized();
        kernel::Profile pr;
        pr.plane.origin = P;
        pr.plane.normal = t;
        std::tie(pr.plane.x, pr.plane.y) = orthoPair(t);
        const bool parallel = nA.cross(nB).norm() < WELD_PARALLEL_SIN;
        if (joint == "butt" || parallel) {
            if (nA.dot(nB) <= 0) continue;  // faces facing each other: no side to show the bead on
            const Vec3 n = (nA + nB).normalized();
            const Vec3 side = t.cross(n).normalized();
            const double b = WELD_BEAD_HALF_MM, g = WELD_BEAD_HEIGHT_MM;
            const double rho = (b * b + g * g) / (2 * g);
            pr.outer = bead(P - b * side, P + b * side, P + g * n, t, P + (g - rho) * n, rho);
        } else {
            const double kk = leg.value_or(WELD_DISPLAY_LEG_MM);
            const Vec3 dB = (nA - nA.dot(nB) * nB - nA.dot(t) * t).normalized();
            const Vec3 dA = (nB - nB.dot(nA) * nA - nB.dot(t) * t).normalized();
            pr.outer = triangle(P, P + kk * dB, P + kk * dA, t);
        }
        Result<kernel::OpResult> r = Error("KERNEL_OPERATION_FAILED");
        if (p.kind == WeldPiece::Kind::Line) {
            kernel::Extent e;
            e.kind = kernel::Extent::Kind::Blind;
            e.depth = p.length;
            r = k.extrude(pr, e, false);
        } else if (p.kind == WeldPiece::Kind::Arc) {
            kernel::RevolveExtent e;
            if (p.sweep >= TWO_PI - 1e-9) e.kind = kernel::RevolveExtent::Kind::Full;
            else e.kind = kernel::RevolveExtent::Kind::Angle, e.value = p.sweep;
            r = k.revolve(pr, kernel::Axis{p.center, p.axis.normalized()}, e);
        } else {
            std::vector<kernel::PathSegment> segs;
            for (size_t i = 0; i + 1 < p.points.size(); ++i) {
                if ((p.points[i + 1] - p.points[i]).norm() < 1e-9) continue;
                kernel::PathSegment ps;
                ps.id = "p" + std::to_string(i + 1);
                ps.a = p.points[i];
                ps.b = p.points[i + 1];
                segs.push_back(ps);
            }
            if (!segs.empty()) r = k.pipeSweep(pr, segs);
        }
        if (r) solids.push_back(r->shape);  // a piece that does not build is shown by its line only (§6б)
    }
    if (solids.empty()) return out;
    out.shape = solids.size() == 1 ? solids.front() : k.combine(solids).shape;
    out.volume = k.massProperties(out.shape).volume;
    return out;
}

}  // namespace construe::drawing
