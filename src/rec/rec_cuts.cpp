// Cuts on bodies of revolution: keyways, flats, cross slots, slots in bushings (spec/reconstruction.md §4.6;
// S4-REC-010). A core not decomposed by probes 1–2 of §4.5 is freed of "non-revolution" face groups around the main
// axis by face removal; each removed group is an extrusion cut, and the remainder must be a body of revolution.
#include "rec/rec_internal.h"
#include "base/geom2.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>

namespace construe::rec::detail {

using SK = kernel::SurfaceInfo::Kind;
using CK = kernel::CurveInfo::Kind;

// Concave edge (§4.6 item 2): face g rises from the edge above the outer side of face f. The direction into face g is
// perpendicular to the edge in the tangent plane of g, on the side where the point is closer to face g.
bool concaveEdge(const Topo& t, int le) {
    const auto [f, g] = t.ct.logicalEdges[size_t(le)].faces;
    if (f == g) return false;
    const auto& c = t.curve[size_t(le)];
    const Point3 p = t.edgePoint(le);
    Vec3 tan;
    const Vec3 nf = t.k.faceNormalAt(t.s, f, p).normalized(), ng = t.k.faceNormalAt(t.s, g, p).normalized();
    if (c.kind == CK::Line) tan = c.p2 - c.p1;
    else if (c.kind == CK::Circle) tan = c.axis.normalized().cross(p - c.center);
    else tan = nf.cross(ng);  // an intersection curve: tangent to both faces
    if (tan.norm() < 1e-9) return false;
    tan.normalize();
    Vec3 v = ng.cross(tan);
    if (v.norm() < 1e-12) return false;
    v.normalize();
    const double eps = 1e-2;
    if (t.k.distanceToFace(t.s, g, p - eps * v) < t.k.distanceToFace(t.s, g, p + eps * v)) v = -v;
    return v.dot(nf) > 1e-6;
}

namespace {

constexpr int REC_SECTION_ANGLES = 72;  // half-planes tried for the profile of a revolution by section (§4.6а)

struct MainAxis {
    Point3 o = Point3::Zero();
    Vec3 a = Vec3::UnitY();
};

// Main axis (§4.6 item 1): the axis of surfaces of revolution (cylinder, cone, torus) with the largest total area of
// coaxial faces; on a tie, the axis of the face with the smaller index.
std::optional<MainAxis> mainAxis(const Topo& t) {
    std::vector<std::pair<MainAxis, double>> axes;
    for (int f = 0; f < t.faces(); ++f) {
        Point3 o;
        Vec3 a;
        if (!revolutionAxis(t.surf[size_t(f)], o, a)) continue;
        const double area = t.k.faceArea(t.s, f);
        bool put = false;
        for (auto& [m, s] : axes)
            if (!put && coaxial(m.o, m.a, o, a)) s += area, put = true;
        if (!put) axes.push_back({{o, canonicalAxis(a)}, area});
    }
    if (axes.empty()) return std::nullopt;
    size_t best = 0;
    for (size_t i = 1; i < axes.size(); ++i)
        if (axes[i].second > axes[best].second * (1 + 1e-9)) best = i;
    return axes[best].first;
}

// Face compatible with revolution about the axis (§4.5 item 2): plane ⊥ axis, coaxial revolution surface, sphere centered on it.
bool compatible(const Topo& t, int f, const MainAxis& m) {
    const auto& si = t.surf[size_t(f)];
    Point3 o;
    Vec3 a;
    if (si.kind == SK::Plane) return parallel(si.dir, m.a);
    if (si.kind == SK::Sphere) return distToLine(si.point, m.o, m.a) <= axisTol();
    if (si.kind == SK::Cone) return coneOnAxis(m.o, m.a, si.point, si.dir);
    if (revolutionAxis(si, o, a)) return coaxial(m.o, m.a, o, a);
    return false;
}

// Outward normal of a planar face.
Vec3 planeNormal(const Topo& t, int f) {
    return t.k.faceNormalAt(t.s, f, t.k.closestPointOnFace(t.s, f, t.k.faceCentroid(t.s, f))).normalized();
}

// Projection of the coordinate origin onto the axis.
Point3 axisOrigin(const MainAxis& m) { return m.o - m.o.dot(m.a) * m.a; }

// Plane ⊥ d at level z (coordinate along d), origin on the axis.
Plane levelPlane(const MainAxis& m, const Vec3& d, double z) {
    const Point3 c0 = axisOrigin(m);
    return planeFrom(c0 + (z - c0.dot(d)) * d, d);
}

// ---- Section difference (§4.6 item 4, through cut): the profile of R \ S in plane P from the sections of R and S.

using C2 = kernel::Curve2::Kind;

double angleOf(const kernel::Curve2& c, const Point2& p) { return std::atan2(p.y() - c.center.y(), p.x() - c.center.x()); }
// Counterclockwise sweep from angle a to angle b in [0, 2π).
double sweepTo(double a, double b) { return geom2::ccwSweep(a, b); }

// Point p lies on curve c (within tol).
bool onCurve(const kernel::Curve2& c, const Point2& p, double tol) {
    if (c.kind == C2::Line) {
        const Vec2 u = c.p2 - c.p1;
        const double L2 = u.squaredNorm();
        const double t = L2 > 0 ? std::clamp((p - c.p1).dot(u) / L2, 0.0, 1.0) : 0.0;
        return (c.p1 + t * u - p).norm() <= tol;
    }
    if (std::abs((p - c.center).norm() - c.radius) > tol) return false;
    if (c.kind == C2::Circle) return true;
    if ((p - c.start).norm() <= tol || (p - c.end).norm() <= tol) return true;
    const double a0 = angleOf(c, c.start);
    return sweepTo(a0, angleOf(c, p)) <= sweepTo(a0, angleOf(c, c.end));
}

// Curve c split at the points of cuts lying on it (inside it); a circle with fewer than two points stays whole.
std::vector<kernel::Curve2> splitAt(const kernel::Curve2& c, const std::vector<Point2>& cuts, double tol) {
    std::vector<kernel::Curve2> out;
    if (c.kind == C2::Line) {
        const Vec2 u = c.p2 - c.p1;
        std::vector<double> ts{0, 1};
        for (const auto& p : cuts)
            if (onCurve(c, p, tol)) ts.push_back((p - c.p1).dot(u) / u.squaredNorm());
        std::sort(ts.begin(), ts.end());
        for (size_t i = 0; i + 1 < ts.size(); ++i) {
            if ((ts[i + 1] - ts[i]) * u.norm() <= tol) continue;
            kernel::Curve2 x = c;
            x.p1 = c.p1 + ts[i] * u, x.p2 = c.p1 + ts[i + 1] * u;
            out.push_back(x);
        }
        return out;
    }
    std::vector<double> as;  // angles of the cut points on the curve
    for (const auto& p : cuts)
        if (onCurve(c, p, tol)) as.push_back(angleOf(c, p));
    double a0 = 0, span = 2 * PI;
    if (c.kind == C2::Arc) {
        a0 = angleOf(c, c.start);
        span = sweepTo(a0, angleOf(c, c.end));
    } else {
        if (as.size() < 2) return {c};
        a0 = as[0];
    }
    std::vector<double> ss{0, span};
    for (double a : as) ss.push_back(std::min(sweepTo(a0, a), span));
    std::sort(ss.begin(), ss.end());
    for (size_t i = 0; i + 1 < ss.size(); ++i) {
        if ((ss[i + 1] - ss[i]) * c.radius <= tol) continue;
        kernel::Curve2 x = c;
        x.kind = C2::Arc;
        x.start = c.center + c.radius * Point2(std::cos(a0 + ss[i]), std::sin(a0 + ss[i]));
        x.end = c.center + c.radius * Point2(std::cos(a0 + ss[i + 1]), std::sin(a0 + ss[i + 1]));
        out.push_back(x);
    }
    return out;
}

Point2 midOf(const kernel::Curve2& c) {
    if (c.kind == C2::Line) return (c.p1 + c.p2) / 2;
    const double a0 = c.kind == C2::Arc ? angleOf(c, c.start) : 0;
    const double sp = c.kind == C2::Arc ? sweepTo(a0, angleOf(c, c.end)) : 2 * PI;
    return c.center + c.radius * Point2(std::cos(a0 + sp / 2), std::sin(a0 + sp / 2));
}

std::vector<Point2> endsOf(const std::vector<kernel::Curve2>& cs) {
    std::vector<Point2> v;
    for (const auto& c : cs) {
        if (c.kind == C2::Line) v.push_back(c.p1), v.push_back(c.p2);
        else if (c.kind == C2::Arc) v.push_back(c.start), v.push_back(c.end);
    }
    return v;
}

}  // namespace

// Boundary of the region between the sections a (of the larger body) and b (of the smaller one): the pieces of each
// curve, split at the ends of the other section's curves on it, whose middle is not on a curve of the other section.
std::vector<kernel::Curve2> sectionDifference(const std::vector<kernel::Curve2>& a, const std::vector<kernel::Curve2>& b) {
    const double tol = REC_LIN_TOL * 10;
    std::vector<kernel::Curve2> out;
    auto keep = [&](const std::vector<kernel::Curve2>& from, const std::vector<kernel::Curve2>& other) {
        const auto cuts = endsOf(other);
        for (const auto& c : from)
            for (const auto& x : splitAt(c, cuts, tol)) {
                const Point2 m = midOf(x);
                if (std::none_of(other.begin(), other.end(), [&](const kernel::Curve2& o) { return onCurve(o, m, tol); })) out.push_back(x);
            }
    };
    keep(a, b);
    keep(b, a);
    return mergePieces(out);
}

namespace {

// A body with a through cut in both directions applied; nullopt — the tool was not built.
std::optional<kernel::Shape> applyThrough(const kernel::Kernel& k, const kernel::Shape& body, const Op& op) {
    auto pr = profileFromCurves(op.plane, op.curves);
    if (!pr || !kernel::validateProfile(*pr)) return std::nullopt;
    const auto mp = k.massProperties(body);
    const double reach = (mp.centroid - op.plane.origin).norm() + std::sqrt(mp.area) * 4 + 10;
    auto tool = k.extrude(*pr, kernel::Extent{kernel::Extent::Kind::Symmetric, reach * 4, {}}, false);
    if (!tool) return std::nullopt;
    auto r = k.boolean(kernel::BoolKind::Subtract, body, tool->shape);
    if (!r) return std::nullopt;
    return r->shape;
}

// Cut for the removed group G (§4.6 item 4): direction, bottom, profile.
std::optional<Op> cutOp(const kernel::Kernel& k, const Topo& t, const std::vector<int>& G, const kernel::Shape& restored, const MainAxis& m,
                        bool verifyThrough = true) {
    const std::set<int> inG(G.begin(), G.end());
    std::vector<Vec3> cand;
    auto addCand = [&](const Vec3& v) {
        const Vec3 c = canonicalAxis(v);
        for (const auto& x : cand)
            if (parallel(x, c)) return;
        cand.push_back(c);
    };
    for (int f : G) {
        const auto& si = t.surf[size_t(f)];
        if (si.kind == SK::Plane || si.kind == SK::Cylinder) addCand(si.dir);
    }
    addCand(m.a);
    std::optional<Vec3> through;
    std::optional<Op> best;
    double bestArea = 0;
    for (const auto& c : cand)
        for (const double sgn : {1.0, -1.0}) {
            const Vec3 d = sgn * c;
            std::vector<int> floors;
            bool ok = true;
            for (int f : G) {
                const auto& si = t.surf[size_t(f)];
                if (si.kind == SK::Plane) {
                    const Vec3 n = planeNormal(t, f);
                    if (parallel(n, d)) {
                        if (n.dot(d) > 0) floors.push_back(f);
                        else ok = false;
                    } else if (!perpendicular(n, d)) {
                        ok = false;
                    }
                } else if (si.kind == SK::Cylinder) {
                    ok = ok && parallel(si.dir, d);
                } else {
                    ok = false;
                }
                if (!ok) break;
            }
            if (!ok) continue;
            if (floors.empty()) {
                if (!through) through = c;
                continue;
            }
            // Blind cut: the bottom is planes of one level.
            const double h = t.surf[size_t(floors[0])].point.dot(d);
            for (int f : floors) ok = ok && std::abs(t.surf[size_t(f)].point.dot(d) - h) <= REC_LIN_TOL * 10;
            if (!ok) continue;
            // Sketch level: tangent to the coaxial convex cylinder the cut exits onto (d ⊥ axis), or an end face ⊥ d
            // with outward normal +d, adjacent to the group.
            double top = -std::numeric_limits<double>::max();
            for (int f : G)
                for (int le : t.faceLE[size_t(f)]) {
                    const int g = t.other(le, f);
                    if (inG.count(g)) continue;
                    const auto& sg = t.surf[size_t(g)];
                    if (sg.kind == SK::Cylinder && sg.convex && coaxial(m.o, m.a, sg.point, sg.dir) && perpendicular(m.a, d))
                        top = std::max(top, sg.point.dot(d) + sg.radius);
                    else if (sg.kind == SK::Plane) {
                        const Vec3 n = planeNormal(t, g);
                        if (parallel(n, d) && n.dot(d) > 0) top = std::max(top, sg.point.dot(d));
                    }
                }
            if (!(top > h + REC_LIN_TOL)) continue;
            const Plane P = levelPlane(m, d, top);
            Op op;
            op.kind = Op::Kind::Extrude;
            op.plane = P;
            op.depth = top - h;
            op.cut = true;
            op.reverse = true;
            for (int f : floors)
                for (int le : t.faceLE[size_t(f)]) {
                    auto c2 = k.projectEdge(t.s, le, P);
                    if (!c2) ok = false;
                    else op.curves.push_back(*c2);
                }
            op.curves = mergePieces(op.curves);
            if (!ok) continue;
            // The floor outline swept to the level must give the body: a pocket whose walls change on the way is not this cut.
            if (auto cutBody = applyOp(k, restored, op))
                if (std::abs(k.massProperties(*cutBody).volume - k.massProperties(t.s).volume) > REC_MATCH_DIST * k.massProperties(restored).area) continue;
            // Of the suitable directions, the largest bottom area (a flat: from the side, not from the end along its wall).
            double area = 0;
            for (int f : floors) area += k.faceArea(t.s, f);
            if (!best || area > bestArea * (1 + 1e-9)) best = op, bestArea = area;
        }
    if (best) return best;
    if (!through) return std::nullopt;
    // Through cut: profile is the removed volume's section at the middle of its extent along d, through both ways.
    const Vec3 d = *through;
    auto tool = k.boolean(kernel::BoolKind::Subtract, restored, t.s);
    double lo = std::numeric_limits<double>::max(), hi = -lo;
    std::vector<kernel::Curve2> curves;
    if (tool && k.counts(tool->shape).solids >= 1) {
        const auto ct = kernel::canonicalTopology(k, tool->shape);
        for (int v : ct.essentialVertices) {
            const double z = k.vertexPoint(tool->shape, v).dot(d);
            lo = std::min(lo, z), hi = std::max(hi, z);
        }
        if (!(hi > lo + REC_LIN_TOL)) return std::nullopt;
        // A cut through a stepped bore: the section changes along d, the widest one is the profile of the cut.
        std::vector<double> levels;
        for (int v : ct.essentialVertices) levels.push_back(k.vertexPoint(tool->shape, v).dot(d));
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end(), [](double x, double y) { return std::abs(x - y) <= REC_LIN_TOL * 10; }), levels.end());
        double bestBox = -1;
        for (size_t i = 0; i + 1 < levels.size(); ++i) {
            auto sec = k.planeSection(tool->shape, levelPlane(m, d, (levels[i] + levels[i + 1]) / 2));
            if (!sec || sec->empty()) continue;
            Eigen::AlignedBox2d box;
            for (const auto& c : *sec) {
                if (c.kind == kernel::Curve2::Kind::Line) box.extend(c.p1), box.extend(c.p2);
                else if (c.kind == kernel::Curve2::Kind::Arc) box.extend(c.start), box.extend(c.end), box.extend(midOf(c));
                else box.extend(c.center - Point2(c.radius, c.radius)), box.extend(c.center + Point2(c.radius, c.radius));
            }
            const double area = box.isEmpty() ? 0 : box.volume();
            if (area > bestBox * (1 + 1e-9)) bestBox = area, curves = mergePieces(*sec);
        }
        if (curves.empty()) return std::nullopt;
    } else {
        // The removed volume is not a valid body (solids touching along edges — a keyway through a stepped bore): the
        // extent is that of the group's faces, the profile is the difference of the sections of R and S.
        for (int f : G)
            for (int le : t.faceLE[size_t(f)])
                for (int v : t.ct.logicalEdges[size_t(le)].ends) {
                    const double z = k.vertexPoint(t.s, v).dot(d);
                    lo = std::min(lo, z), hi = std::max(hi, z);
                }
        if (!(hi > lo + REC_LIN_TOL)) return std::nullopt;
        const Plane mid = levelPlane(m, d, (lo + hi) / 2);
        auto sr = k.planeSection(restored, mid), ss = k.planeSection(t.s, mid);
        if (!sr || !ss) return std::nullopt;
        curves = sectionDifference(mergePieces(*sr), mergePieces(*ss));
        if (curves.empty()) return std::nullopt;
    }
    const Plane P = levelPlane(m, d, (lo + hi) / 2);
    Op op;
    op.kind = Op::Kind::Extrude;
    op.plane = P;
    op.curves = curves;
    op.cut = true;
    op.through = true;
    op.both = true;
    // Through both ways the tool must take nothing but this cut: a cut that ends inside the body is not through.
    if (verifyThrough)
        if (auto cutBody = applyThrough(k, restored, op))
            if (std::abs(k.massProperties(*cutBody).volume - k.massProperties(t.s).volume) > REC_MATCH_DIST * k.massProperties(restored).area) return std::nullopt;
    return op;
}

// Cut face groups (§4.6 item 2): connected pieces of incompatible faces; withPlanes adds planes ⊥ axis adjacent to
// them along a concave edge (bottom of a cross slot, wall at the end of a flat), which join the pieces.
std::vector<std::vector<int>> cutGroups(const Topo& t, const MainAxis& m, bool withPlanes) {
    const int n = t.faces();
    std::vector<int> parent(static_cast<size_t>(n));
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) { return parent[size_t(x)] == x ? x : parent[size_t(x)] = find(parent[size_t(x)]); };
    auto unite = [&](int a, int b) { parent[size_t(find(a))] = find(b); };
    std::vector<bool> foreign(static_cast<size_t>(n)), member(static_cast<size_t>(n));
    for (int f = 0; f < n; ++f) member[size_t(f)] = foreign[size_t(f)] = !compatible(t, f, m);
    for (size_t le = 0; le < t.ct.logicalEdges.size(); ++le) {
        const auto [f, g] = t.ct.logicalEdges[le].faces;
        if (f == g) continue;
        if (foreign[size_t(f)] && foreign[size_t(g)]) {
            unite(f, g);
        } else if (withPlanes && (foreign[size_t(f)] || foreign[size_t(g)])) {
            const int pl = foreign[size_t(f)] ? g : f;
            if (t.surf[size_t(pl)].kind == SK::Plane && concaveEdge(t, int(le))) {
                unite(f, g);
                member[size_t(pl)] = true;
            }
        }
    }
    std::map<int, std::vector<int>> by;
    for (int f = 0; f < n; ++f)
        if (member[size_t(f)]) by[find(f)].push_back(f);
    std::vector<std::vector<int>> out;
    for (auto& [r, v] : by) out.push_back(v);
    // Groups on the same surfaces (each face of one is a piece of a surface of the other and back, §4.4.1 item 2) are
    // one cut interrupted by a groove or a bore step; opposite slots of a pattern differ by their bottoms.
    auto covers = [&](const std::vector<int>& a, const std::vector<int>& b) {
        return std::all_of(a.begin(), a.end(), [&](int f) {
            return std::any_of(b.begin(), b.end(), [&](int g) { return samePiece(t.surf[size_t(f)], t.surf[size_t(g)]); });
        });
    };
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.front() < b.front(); });
    for (size_t i = 0; i < out.size(); ++i)
        for (size_t j = i + 1; j < out.size();) {
            if (covers(out[i], out[j]) && covers(out[j], out[i])) {
                out[i].insert(out[i].end(), out[j].begin(), out[j].end());
                std::sort(out[i].begin(), out[i].end());
                out.erase(out.begin() + long(j));
            } else {
                ++j;
            }
        }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.front() < b.front(); });
    return out;
}

}  // namespace

std::optional<RevolveCuts> removeRevolveCuts(const kernel::Kernel& k, const kernel::Shape& core, std::vector<Note>& notes) {
    Topo t0(k, core);
    const auto m = mainAxis(t0);
    if (!m) return std::nullopt;
    // A body mostly of revolution: the area of compatible faces is not less than the area of the others (§4.6 item 1).
    double comp = 0, other = 0;
    for (int f = 0; f < t0.faces(); ++f) (compatible(t0, f, *m) ? comp : other) += k.faceArea(core, f);
    if (other <= 0 || comp < other) return std::nullopt;
    RevolveCuts rc;
    kernel::Shape s = core;
    for (int guard = 0; guard < 64; ++guard) {
        Topo t(k, s);
        bool removed = false;
        for (const bool withPlanes : {true, false}) {
            for (const auto& g : cutGroups(t, *m, withPlanes)) {
                auto r = removeChecked(k, s, g);
                // A cut: removal adds material; a group whose removal takes material away (rib, boss) is not a cut.
                if (!r || !(k.massProperties(r->shape).volume > k.massProperties(s).volume)) continue;
                auto op = cutOp(k, t, g, r->shape, *m);
                if (!op) continue;
                rc.cuts.push_back({*op, s});
                s = r->shape;
                removed = true;
                break;
            }
            if (removed) break;
        }
        if (!removed) break;
    }
    if (rc.cuts.empty()) return std::nullopt;
    auto rev = revolveBase(k, s);
    if (!rev) {
        // The rest still has cuts that face removal does not take (§4.6 item 3): they are found by a section of the rest
        // (§4.6а); the cuts removed so far come first in removal order (S4-REC-023).
        std::vector<Note> sectionNotes;
        if (auto bySection = revolveCutsBySection(k, s, sectionNotes)) {
            rc.cuts.insert(rc.cuts.end(), bySection->cuts.begin(), bySection->cuts.end());
            rc.restored = bySection->restored;
            rc.revolve = bySection->revolve;
            rc.axisOrigin = bySection->axisOrigin;
            rc.axisDir = bySection->axisDir;
            return rc;
        }
        notes.push_back({"base", std::to_string(rc.cuts.size()) + " cuts removed, rest is not a revolution body"});
        notes.insert(notes.end(), sectionNotes.begin(), sectionNotes.end());
        return std::nullopt;
    }
    rc.restored = s;
    rc.revolve = *rev;
    rc.axisOrigin = m->o;
    rc.axisDir = m->a;
    return rc;
}

std::optional<RevolveCuts> revolveCutsBySection(const kernel::Kernel& k, const kernel::Shape& core, std::vector<Note>& notes) {
    Topo t0(k, core);
    const auto m = mainAxis(t0);
    if (!m) return std::nullopt;
    // The same body as for §4.6 item 1: mostly of revolution.
    std::vector<int> compat;
    double comp = 0, other = 0;
    for (int f = 0; f < t0.faces(); ++f) {
        const bool c = compatible(t0, f, *m);
        (c ? comp : other) += k.faceArea(core, f);
        if (c) compat.push_back(f);
    }
    if (other <= 0 || comp < other) return std::nullopt;
    const Vec3 a = m->a;
    const Point3 oAxis = axisOrigin(*m);
    // Half-planes from the reference direction (the built-in axis most orthogonal to a, as §4.5 item 2) by equal steps.
    Vec3 e = Vec3::UnitX();
    double best = 2;
    for (const Vec3& c : {Vec3(Vec3::UnitX()), Vec3(Vec3::UnitY()), Vec3(Vec3::UnitZ())})
        if (std::abs(c.dot(a)) < best - 1e-12) best = std::abs(c.dot(a)), e = c;
    const Vec3 x0 = (e - e.dot(a) * a).normalized(), y0 = a.cross(x0).normalized();
    const double tol = REC_LIN_TOL * 10 + axisTol();
    for (int j = 0; j < REC_SECTION_ANGLES && !stopped(); ++j) {
        const double th = 2 * PI * double(j) / REC_SECTION_ANGLES;
        Plane p;
        p.origin = oAxis;
        p.y = a;
        p.x = (std::cos(th) * x0 + std::sin(th) * y0).normalized();
        p.normal = p.x.cross(p.y).normalized();
        auto half = halfProfile(k, core, p);
        if (!half) continue;
        // The half-plane meets no cut: every profile curve (except the closing segments on the axis) lies on faces
        // compatible with the revolution — its middle and ends.
        bool clean = true;
        for (const auto& c : *half) {
            if (c.kind == C2::Line && c.p1.x() == 0 && c.p2.x() == 0) continue;
            std::vector<Point2> pts = endsOf({c});
            pts.push_back(midOf(c));
            for (const auto& q : pts) {
                const Point3 w = p.toWorld(q);
                clean = clean && std::any_of(compat.begin(), compat.end(), [&](int f) { return k.distanceToFace(core, f, w) <= tol; });
            }
            if (!clean) break;
        }
        if (!clean) continue;
        // The body of revolution R and the removed volume T = R − core; volume check V(R) − V(T) = V(core).
        auto R = revolveSolid(k, p, *half, oAxis, a);
        if (!R) {
            notes.push_back({"base", "revolution by section: profile not revolved"});
            break;
        }
        auto T = k.boolean(kernel::BoolKind::Subtract, *R, core);
        if (!T || k.counts(T->shape).solids < 1) {
            notes.push_back({"base", "revolution by section: no removed volume"});
            break;
        }
        const auto mc = k.massProperties(core);
        const double vR = k.massProperties(*R).volume, vT = k.massProperties(T->shape).volume;
        if (std::abs(vR - vT - mc.volume) > REC_MATCH_REL * vR + mc.area * std::max(REC_LIN_TOL, bodyTolerance())) {
            notes.push_back({"base", "revolution by section: volume mismatch"});
            return std::nullopt;
        }
        std::vector<kernel::Shape> parts;
        if (k.counts(T->shape).solids == 1) parts.push_back(T->shape);
        else
            for (auto& [sol, faces] : k.components(T->shape)) parts.push_back(sol);
        // Each removed solid is a cut (§4.6 item 4) on the body R without it: the cut faces are those inside R.
        std::vector<Op> ops;
        for (size_t i = 0; i < parts.size(); ++i) {
            auto Si = k.boolean(kernel::BoolKind::Subtract, *R, parts[i]);
            if (!Si || k.counts(Si->shape).solids != 1) {
                notes.push_back({"base", "revolution by section: cut " + std::to_string(i + 1) + " not built"});
                return std::nullopt;
            }
            Topo ti(k, Si->shape);
            std::vector<int> G;
            for (int f = 0; f < ti.faces(); ++f)
                if (k.distanceToShape(*R, k.closestPointOnFace(Si->shape, f, k.faceCentroid(Si->shape, f))) > tol) G.push_back(f);
            auto op = G.empty() ? std::nullopt : cutOp(k, ti, G, *R, *m, false);  // the pieces of one cut are checked together below
            if (!op) {
                notes.push_back({"base", "revolution by section: removed solid " + std::to_string(i + 1) + " is not a cut"});
                return std::nullopt;
            }
            ops.push_back(*op);
        }
        // All the cuts together give the core: a through cut must end nowhere inside the body.
        {
            kernel::Shape all = *R;
            bool checked = true;
            for (const auto& op : ops) {
                if (!op.through) {
                    checked = false;
                    break;
                }
                if (auto r = applyThrough(k, all, op)) all = *r;
                else checked = false;
            }
            if (checked && std::abs(k.massProperties(all).volume - mc.volume) > REC_MATCH_DIST * mc.area) {
                notes.push_back({"base", "revolution by section: through cuts take too much"});
                return std::nullopt;
            }
        }
        // Cuts in removal order (reverse of construction), each with the body after it in construction: R minus the
        // removed solids up to it; the last one is the core itself.
        std::vector<kernel::Shape> after;
        kernel::Shape cur = *R;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i + 1 == parts.size()) {
                cur = core;
            } else {
                auto r = k.boolean(kernel::BoolKind::Subtract, cur, parts[i]);
                if (!r) return std::nullopt;
                cur = r->shape;
            }
            after.push_back(cur);
        }
        RevolveCuts rc;
        rc.restored = *R;
        rc.revolve.kind = Op::Kind::Revolve;
        rc.revolve.plane = p;
        rc.revolve.curves = *half;
        rc.revolve.axisOrigin = oAxis;
        rc.revolve.axisDir = a;
        for (size_t i = parts.size(); i-- > 0;) rc.cuts.push_back({ops[i], after[i]});
        rc.axisOrigin = m->o;
        rc.axisDir = m->a;
        return rc;
    }
    return std::nullopt;  // no half-plane free of cuts: not a body of revolution with cuts (no note)
}

namespace {

// Reference point of a cut (§4.6 item 5): the center of the bounding box of its curves in the sketch plane, world.
Point3 cutAnchor(const Op& op) {
    Eigen::AlignedBox2d box;
    for (const auto& c : op.curves) {
        if (c.kind == kernel::Curve2::Kind::Line) box.extend(c.p1), box.extend(c.p2);
        else if (c.kind == kernel::Curve2::Kind::Arc) box.extend(c.start), box.extend(c.end), box.extend(c.center);
        else box.extend(c.center - Point2(c.radius, c.radius)), box.extend(c.center + Point2(c.radius, c.radius));
    }
    return op.plane.toWorld(box.isEmpty() ? Point2(0, 0) : Point2(box.center()));
}

// Cut a, transformed by the point map f (normal map g; arcs swapped when the map reverses orientation), coincides with
// cut b (§4.6 item 5).
template <class F, class G>
bool sameCutUnder(const Op& a, const Op& b, F f, G g, bool swapArcs) {
    if (a.through != b.through || a.both != b.both || a.reverse != b.reverse || a.cut != b.cut || a.curves.size() != b.curves.size() ||
        std::abs(a.depth - b.depth) > REC_LIN_TOL * 100)
        return false;
    if ((f(a.plane.origin) - b.plane.origin).norm() > REC_LIN_TOL * 100) return false;
    if ((g(a.plane.normal) - b.plane.normal).norm() > REC_ANGLE_TOL * 100) return false;
    std::vector<bool> used(b.curves.size(), false);
    for (const auto& c : a.curves) {
        kernel::Curve2 m;
        m.kind = c.kind;
        m.radius = c.radius;
        auto map = [&](const Point2& q) { return b.plane.toLocal(f(a.plane.toWorld(q))); };
        if (c.kind == kernel::Curve2::Kind::Line) m.p1 = map(c.p1), m.p2 = map(c.p2);
        else if (c.kind == kernel::Curve2::Kind::Arc) {
            m.center = map(c.center);
            m.start = map(swapArcs ? c.end : c.start);
            m.end = map(swapArcs ? c.start : c.end);
        } else {
            m.center = map(c.center);
        }
        bool found = false;
        for (size_t j = 0; j < b.curves.size() && !found; ++j)
            if (!used[j] && sameCurve(m, b.curves[j], REC_LIN_TOL * 100)) used[j] = found = true;
        if (!found) return false;
    }
    return true;
}

}  // namespace

std::vector<std::pair<std::vector<Op>, kernel::Shape>> groupRevolveCuts(const RevolveCuts& rc) {
    const auto& cuts = rc.cuts;
    const size_t n = cuts.size();
    const Vec3 a = rc.axisDir.normalized();
    const Point3 o = rc.axisOrigin;
    auto radial = [&](const Op& op) {
        const Vec3 r = cutAnchor(op) - o;
        return Vec3(r - r.dot(a) * a);
    };
    auto rotateCut = [&](double th) {
        return std::make_pair([=](const Point3& p) { return Point3(o + rotateAbout(p - o, a, th)); },
                              [=](const Vec3& v) { return rotateAbout(v, a, th); });
    };
    std::vector<int> groupOf(n, -1);          // index of the seed cut, or -1
    std::vector<std::vector<Op>> seedOps(n);  // the block of the seed (removal index)
    for (size_t i = 0; i < n; ++i) {
        if (groupOf[i] >= 0) continue;
        const Op& seed = cuts[i].first;
        const Vec3 r0 = radial(seed);
        // Circular pattern: the seed rotated onto each next free cut.
        std::vector<size_t> members{i};
        std::vector<double> angles{0};
        if (r0.norm() > REC_LIN_TOL * 100)
            for (size_t j = i + 1; j < n; ++j) {
                if (groupOf[j] >= 0) continue;
                const Vec3 r1 = radial(cuts[j].first);
                if (r1.norm() <= REC_LIN_TOL * 100) continue;
                double th = std::atan2(a.dot(r0.cross(r1)), r0.dot(r1));
                if (th < 0) th += 2 * PI;
                const auto [f, g] = rotateCut(th);
                if (!sameCutUnder(seed, cuts[j].first, f, g, false)) continue;
                members.push_back(j);
                angles.push_back(th);
            }
        bool circular = members.size() >= 2;
        if (circular) {
            std::vector<double> sorted = angles;
            std::sort(sorted.begin(), sorted.end());
            const double N = double(members.size());
            for (size_t q = 0; q < sorted.size(); ++q) circular = circular && std::abs(sorted[q] - 2 * PI * double(q) / N) <= 1e-6;
        }
        if (circular) {
            for (size_t x : members) groupOf[x] = int(i);
            Op p;
            p.kind = Op::Kind::Pattern;
            p.target = 0;
            p.plane = seed.plane;
            p.circular = true;
            p.axisOrigin = o;
            p.axisDir = a;
            p.count = int(members.size());
            seedOps[i] = {seed, p};  // curves are empty: instances lie in different planes (§5.3)
            continue;
        }
        // Mirror: the first next free cut equal to the seed reflected about a built-in plane.
        for (size_t j = i + 1; j < n && groupOf[i] < 0; ++j) {
            if (groupOf[j] >= 0) continue;
            for (const auto& [name, pn] : sketch::datumPlaneNormals()) {
                const Vec3 nn = pn;
                auto f = [nn](const Point3& p) { return reflect(p, nn); };
                auto g = [nn](const Vec3& v) { return Vec3(v - 2 * v.dot(nn) * nn); };
                if (!sameCutUnder(seed, cuts[j].first, f, g, true)) continue;
                groupOf[i] = groupOf[j] = int(i);
                Op mi;
                mi.kind = Op::Kind::Mirror;
                mi.target = 0;
                mi.mirrorPlane = name;
                seedOps[i] = {seed, mi};
                break;
            }
        }
    }
    // Construction order — reverse removal order; a group stands at its seed (the smallest removal index): the body
    // after the block is the body before the seed's removal (§4.6 item 5).
    std::vector<std::pair<std::vector<Op>, kernel::Shape>> out;
    for (size_t q = n; q-- > 0;) {
        if (groupOf[q] >= 0 && groupOf[q] != int(q)) continue;
        out.push_back({groupOf[q] == int(q) ? seedOps[q] : std::vector<Op>{cuts[q].first}, cuts[q].second});
    }
    return out;
}

}  // namespace construe::rec::detail
