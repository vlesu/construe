// Dimensions from the model (spec/drawing.md §6а.10; FX1-DRW-003, FX2-DRW-001, FX3-DRW-004) and sketch dimension tolerances in
// drawing dimension labels (§6а.3, FX3-DRW-001): matching sketch and feature dimensions to model edges and vertices.
#include "drawing/dimension_parts.h"
#include "drawing/render.h"
#include "drawing/sheet_marks.h"

#include "expr/expr.h"
#include "regen/assembly_ref.h"
#include "regen/naming.h"
#include "sketch/offset.h"
#include "sketch/sketch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <numbers>
#include <set>
#include <utility>

namespace construe::drawing {

namespace {

using namespace dimdetail;

// Model geometry in world coordinates for matching with a sketch.
struct WorldTopo {
    struct Edge {
        std::string name;
        kernel::CurveInfo ci;
    };
    struct Vertex {
        std::string name;
        Point3 p;
    };
    struct Face {
        std::string name;
        kernel::SurfaceInfo si;
    };
    std::vector<Edge> edges;
    std::vector<Vertex> vertices;
    std::vector<Face> cylinders;  // cylindrical faces (silhouette fallbacks, §6а.10 items 3, 3а; FX8-DRW-002)
};

WorldTopo worldTopo(const regen::PartState& st, const kernel::Kernel& k) {
    WorldTopo t;
    for (const auto& e : st.names.edges) t.edges.push_back({e.full, k.edgeCurve(st.bodies.at(size_t(e.body)).shape, e.logical)});
    for (const auto& v : st.names.vertices) t.vertices.push_back({v.full, k.vertexPoint(st.bodies.at(size_t(v.body)).shape, v.vertex)});
    for (const auto& f : st.names.faces) {
        auto si = k.faceSurface(st.bodies.at(size_t(f.body)).shape, f.face);
        if (si.kind == kernel::SurfaceInfo::Kind::Cylinder) t.cylinders.push_back({f.full, si});
    }
    return t;
}

// Sheet dimension reference to a sketch entity: vertex, line edge, circle edge (center — a point), §6а.10.
struct Matched {
    doc::DimensionRef ref;
    enum class Kind { Point, Segment, Circle } kind = Kind::Point;
};

bool isClosedCircle(const kernel::CurveInfo& ci) { return ci.kind == kernel::CurveInfo::Kind::Circle && ci.closed; }

// In-plane length of v: the component along the sketch normal N (unit) is dropped (§6а.10 item 3, FX8-DRW-002: the
// matching is a projection along the sketch normal — model vertices of an extrusion with an offset or both ways lie off
// the sketch plane).
double planar(const Vec3& v, const Vec3& N) { return (v - v.dot(N) * N).norm(); }

std::optional<Matched> matchPoint(const WorldTopo& t, const Point3& P, const Vec3& Nraw) {
    const Vec3 N = Nraw.normalized();
    for (const auto& v : t.vertices)  // in the sketch plane first
        if ((v.p - P).norm() <= MODEL_DIM_MATCH_TOL) return Matched{{"vertex", v.name}, Matched::Kind::Point};
    for (const auto& v : t.vertices)  // then along the normal
        if (planar(v.p - P, N) <= MODEL_DIM_MATCH_TOL) return Matched{{"vertex", v.name}, Matched::Kind::Point};
    for (const auto& e : t.edges)  // center of a circle with axis ∥ sketch normal
        if (e.ci.kind == kernel::CurveInfo::Kind::Circle && planar(e.ci.center - P, N) <= MODEL_DIM_MATCH_TOL &&
            1 - std::abs(e.ci.axis.normalized().dot(N)) <= DIM_PARALLEL_TOL)
            return Matched{{"edge", e.name}, Matched::Kind::Circle};
    for (const auto& e : t.edges)  // center of a closed circle with any axis (sweep end face, FX3-DRW-004)
        if (isClosedCircle(e.ci) && (e.ci.center - P).norm() <= MODEL_DIM_MATCH_TOL) return Matched{{"edge", e.name}, Matched::Kind::Circle};
    return std::nullopt;
}

std::optional<Matched> matchLine(const WorldTopo& t, const Point3& A, const Point3& B, const Vec3& Nraw) {
    const double L = (B - A).norm();
    if (L <= MODEL_DIM_MATCH_TOL) return std::nullopt;
    const Vec3 N = Nraw.normalized();
    const Vec3 d = (B - A) / L;
    auto off = [&](const Point3& p) {  // distance from the plane through AB along N; height along N
        const Vec3 w = p - A;
        return std::pair{(w - w.dot(d) * d - w.dot(N) * N).norm(), w.dot(N)};
    };
    const WorldTopo::Edge *best = nullptr, *bestSame = nullptr;
    for (const auto& e : t.edges) {
        if (e.ci.kind != kernel::CurveInfo::Kind::Line) continue;
        const auto [o1, h1] = off(e.ci.p1);
        const auto [o2, h2] = off(e.ci.p2);
        if (o1 > MODEL_DIM_MATCH_TOL || o2 > MODEL_DIM_MATCH_TOL || std::abs(h1 - h2) > MODEL_DIM_MATCH_TOL) continue;  // ⊥ N, on AB
        const double s1 = (e.ci.p1 - A).dot(d), s2 = (e.ci.p2 - A).dot(d);
        const double lo = std::max(0.0, std::min(s1, s2)), hi = std::min(L, std::max(s1, s2));
        if (hi - lo <= MODEL_DIM_MATCH_TOL) continue;  // do not overlap
        const bool same = std::abs(std::min(s1, s2)) <= MODEL_DIM_MATCH_TOL && std::abs(std::max(s1, s2) - L) <= MODEL_DIM_MATCH_TOL;
        if (same && std::abs(h1) <= MODEL_DIM_MATCH_TOL) return Matched{{"edge", e.name}, Matched::Kind::Segment};  // in the plane
        if (same && !bestSame) bestSame = &e;
        if (!best) best = &e;
    }
    if (bestSame) return Matched{{"edge", bestSame->name}, Matched::Kind::Segment};
    if (best) return Matched{{"edge", best->name}, Matched::Kind::Segment};
    return std::nullopt;
}

std::optional<Matched> matchCircle(const WorldTopo& t, const Point3& C, double r, const Vec3& Nraw) {
    const Vec3 N = Nraw.normalized();
    const WorldTopo::Edge* off = nullptr;
    for (const auto& e : t.edges)
        if (e.ci.kind == kernel::CurveInfo::Kind::Circle && planar(e.ci.center - C, N) <= MODEL_DIM_MATCH_TOL &&
            std::abs(e.ci.radius - r) <= MODEL_DIM_MATCH_TOL && 1 - std::abs(e.ci.axis.normalized().dot(N)) <= DIM_PARALLEL_TOL) {
            if ((e.ci.center - C).norm() <= MODEL_DIM_MATCH_TOL) return Matched{{"edge", e.name}, Matched::Kind::Circle};
            if (!off) off = &e;
        }
    if (off) return Matched{{"edge", off->name}, Matched::Kind::Circle};
    return std::nullopt;
}

// Silhouette of a cylindrical face on a view looking along N (§6а.2: generatrix A + t·a + side·ρ·w, w = N × a) through
// both A and B along N (FX8-DRW-002: a sketch segment on a cylinder outline — no edge there). side is for a view with
// n = +N; a view with n = −N flips it (Candidate::silhouetteNormal).
std::optional<Matched> matchSilhouette(const WorldTopo& t, const Point3& A, const Point3& B, const Vec3& Nraw) {
    const Vec3 N = Nraw.normalized();
    if ((B - A).norm() <= MODEL_DIM_MATCH_TOL) return std::nullopt;
    const Vec3 d = (B - A).normalized();
    for (const auto& f : t.cylinders) {
        const Vec3 a = f.si.dir.normalized();
        if (std::abs(a.dot(N)) > DIM_PARALLEL_TOL || 1 - std::abs(a.dot(d)) > DIM_PARALLEL_TOL) continue;
        const Vec3 w = N.cross(a).normalized();
        for (const int side : {1, -1}) {
            const Point3 O = f.si.point + side * f.si.radius * w;
            auto onGen = [&](const Point3& X) {
                const Vec3 v = X - O;
                return (v - v.dot(a) * a - v.dot(N) * N).norm() <= MODEL_DIM_MATCH_TOL;
            };
            if (onGen(A) && onGen(B)) return Matched{{"silhouette", f.name, side}, Matched::Kind::Segment};
        }
    }
    return std::nullopt;
}

// Candidate sketch or depth dimension: kind, references and fields; measurement plane — normal normal (sketch) or
// direction along (depth).
struct Candidate {
    doc::SheetDimension dim;
    std::optional<Vec3> normal;   // a view fits if its n ∥ normal
    std::optional<Vec3> along;    // a view fits if its n ⊥ along
    std::optional<Vec3> xAxis;    // distanceX / distanceY: measurement direction in the world
    std::optional<double> value;  // expected value of the built dimension (mm or degrees), §6а.10 item 3
    std::vector<doc::SheetDimension> alts;  // fallback reference variants (sketch points — not vertices), in order
    std::string constraint;                 // sketch dimension ID (sketch dimension tolerance, §6а.3)
    // Silhouette references were matched looking along this sketch normal: a view with n = −N flips their sides (§6а.2).
    std::optional<Vec3> silhouetteNormal;
    // A diameter by an edge-on circle (linear on one closed circle edge) is also shown end-on (FX13-DRW-001, FX13-DRW-003):
    // on a view with n ∥ the circle axis — a diameter dimension on the same edge.
    std::optional<Vec3> endOnAxis;
};

// Dimension from the axis to a profile point of a body of revolution (§6а.10 item 3а): the circle obtained by revolving point P
// about the axis (point O, direction m — across the circle), centered on the axis — linear dimension on its edge-on segment (Ø).
std::optional<doc::DimensionRef> radialCircle(const WorldTopo& t, const Point3& P, const Point3& O, const Vec3& m, bool perpendicular) {
    const double tol = MODEL_DIM_MATCH_TOL;
    for (const auto& e : t.edges) {
        if (!isClosedCircle(e.ci)) continue;
        const Vec3 a = e.ci.axis.normalized();
        // perpendicular: circle axis ⊥ m (m — radius direction, distanceX/Y); otherwise axis ∥ m (m — revolution axis).
        if (perpendicular ? std::abs(a.dot(m)) > DIM_PARALLEL_TOL : 1 - std::abs(a.dot(m)) > DIM_PARALLEL_TOL) continue;
        if (std::abs((P - e.ci.center).dot(a)) > tol || std::abs((P - e.ci.center).norm() - e.ci.radius) > tol) continue;
        if ((O - e.ci.center).cross(a).norm() > tol) continue;
        return doc::DimensionRef{"edge", e.name};
    }
    return std::nullopt;
}

// Dimension between a profile point P and an axis point O of a body of revolution (§6а.10 item 3а), measured along m (in
// the sketch plane):
// - radial (across the revolution axis): the closed circle obtained by revolving P, centered on the axis through O, with
//   radius |(P − O)·m| — linear dimension on its edge-on segment (diameter "Ø", GOST 2.307 for bodies of revolution);
//   none (a cut opened the circle, FX8-DRW-002) — the cylindrical face of that radius about such an axis: linear on its
//   two silhouettes (Ø);
// - axial (along the axis, not with radialOnly): a circle through P with axis ∥ m and another circle with a parallel axis
//   whose plane passes through O — closed circles first, then arcs (a cut seam, FX8-DRW-002).
// radial = true in the result — the value is the doubled distance.
std::optional<Candidate> revolvedPair(const WorldTopo& t, const Point3& P, const Point3& O, const Vec3& mRaw, const Plane& fr, bool radialOnly,
                                      bool& radial) {
    const Vec3 m = mRaw.normalized();
    const double tol = MODEL_DIM_MATCH_TOL;
    auto through = [&](const WorldTopo::Edge& e, const Point3& X) {  // X on circle e
        const Vec3 a = e.ci.axis.normalized();
        return std::abs((X - e.ci.center).dot(a)) <= tol && std::abs((X - e.ci.center).norm() - e.ci.radius) <= tol;
    };
    Candidate cd;
    cd.normal = fr.normal;
    cd.xAxis = m;
    cd.dim.kind = "linear";
    cd.dim.orientation = "aligned";
    const double rho = std::abs((P - O).dot(m));
    for (const auto& e : t.edges) {  // radial
        if (!isClosedCircle(e.ci)) continue;
        const Vec3 a = e.ci.axis.normalized();
        if (std::abs(a.dot(m)) > DIM_PARALLEL_TOL || !through(e, P) || (O - e.ci.center).cross(a).norm() > tol) continue;
        if (std::abs(rho - e.ci.radius) > tol) continue;
        cd.along = a;
        cd.dim.refs.push_back({"edge", e.name});
        radial = true;
        return cd;
    }
    for (const auto& f : t.cylinders) {  // radial by the silhouettes of the cylinder (FX8-DRW-002)
        const Vec3 a = f.si.dir.normalized();
        if (std::abs(a.dot(m)) > DIM_PARALLEL_TOL || (O - f.si.point).cross(a).norm() > tol || std::abs(f.si.radius - rho) > tol) continue;
        if (std::abs((P - f.si.point).cross(a).norm() - rho) > tol) continue;
        cd.along = a;
        cd.dim.refs = {{"silhouette", f.name, 1}, {"silhouette", f.name, -1}};
        radial = true;
        return cd;
    }
    if (radialOnly) return std::nullopt;
    for (const bool closed : {true, false})  // axial
        for (const auto& e1 : t.edges) {
            if (e1.ci.kind != kernel::CurveInfo::Kind::Circle || e1.ci.closed != closed || !through(e1, P)) continue;
            const Vec3 a = e1.ci.axis.normalized();
            if (1 - std::abs(a.dot(m)) > DIM_PARALLEL_TOL) continue;
            for (const auto& e0 : t.edges) {
                if (e0.ci.kind != kernel::CurveInfo::Kind::Circle || (closed && !e0.ci.closed) || &e0 == &e1 ||
                    1 - std::abs(e0.ci.axis.normalized().dot(a)) > DIM_PARALLEL_TOL)
                    continue;
                if (std::abs((O - e0.ci.center).dot(a)) > tol) continue;
                cd.along = a;
                cd.dim.refs = {{"edge", e0.name}, {"edge", e1.name}};
                radial = false;
                return cd;
            }
        }
    return std::nullopt;
}

// Diametral dimension from an axis point P to a generatrix parallel to the axis direction a (§6а.10 item 3а, FX13-DRW-001):
// the first closed circle with axis ∥ a centered on the axis through P with radius rho — linear on its edge-on segment
// (Ø); none — the first cylindrical face of radius rho on that axis — linear on its two silhouettes (Ø).
std::optional<Candidate> generatrixDiameter(const WorldTopo& t, const Point3& P, const Vec3& aRaw, double rho, const Plane& fr) {
    const Vec3 a = aRaw.normalized();
    const double tol = MODEL_DIM_MATCH_TOL;
    Candidate cd;
    cd.normal = fr.normal;
    cd.along = a;
    cd.dim.kind = "linear";
    cd.dim.orientation = "aligned";
    for (const auto& e : t.edges) {
        if (!isClosedCircle(e.ci) || 1 - std::abs(e.ci.axis.normalized().dot(a)) > DIM_PARALLEL_TOL) continue;
        if ((e.ci.center - P).cross(a).norm() > tol || std::abs(e.ci.radius - rho) > tol) continue;
        cd.dim.refs = {{"edge", e.name}};
        return cd;
    }
    for (const auto& f : t.cylinders) {
        if (1 - std::abs(f.si.dir.normalized().dot(a)) > DIM_PARALLEL_TOL) continue;
        if ((f.si.point - P).cross(a).norm() > tol || std::abs(f.si.radius - rho) > tol) continue;
        cd.dim.refs = {{"silhouette", f.name, 1}, {"silhouette", f.name, -1}};
        return cd;
    }
    return std::nullopt;
}

// Body of revolution (§6а.10 item 3а): distanceX / distanceY between two sketch points (one of them — on the revolution
// axis: the origin or any point, FX8-DRW-002) and distance between a point and a segment lying on the axis (a center
// line, FX8-DRW-002 — radial only); want — the sketch value (radial — doubled).
template <class WorldPt>
std::optional<Candidate> revolvedCandidate(const WorldTopo& t, const sketch::SketchData& sk, const sketch::Constraint& c, const Plane& fr,
                                           WorldPt worldPt, std::optional<double> want) {
    if (c.refs.size() != 2) return std::nullopt;
    auto finish = [&](std::optional<Candidate> cd, bool radial) {
        if (cd && want) cd->value = radial ? 2 * *want : *want;
        return cd;
    };
    if (c.type == "distanceX" || c.type == "distanceY") {
        const Vec3 m = c.type == "distanceX" ? fr.x : fr.y;
        for (const int oi : {0, 1}) {  // O — the origin first (as before), then either point
            const auto O = worldPt(c.refs[size_t(oi)]), P = worldPt(c.refs[size_t(1 - oi)]);
            if (!O || !P) continue;
            bool radial = false;
            if (auto cd = revolvedPair(t, *P, *O, m, fr, false, radial)) return finish(std::move(cd), radial);
        }
        return std::nullopt;
    }
    if (c.type != "distance" || c.arc) return std::nullopt;
    for (const int li : {0, 1}) {
        const sketch::Entity* L = sk.findEntity(c.refs[size_t(li)]);
        if (!L || L->type != sketch::EntityType::Line) continue;
        const auto A = worldPt(L->p1), B = worldPt(L->p2), P = worldPt(c.refs[size_t(1 - li)]);
        if (!A || !B || !P || (*B - *A).norm() <= MODEL_DIM_MATCH_TOL) continue;
        const Vec3 l = (*B - *A).normalized();
        const Point3 O = *A + (*P - *A).dot(l) * l;  // foot of the perpendicular — on the axis
        if ((*P - O).norm() <= MODEL_DIM_MATCH_TOL) continue;
        bool radial = false;
        if (auto cd = revolvedPair(t, *P, O, *P - O, fr, true, radial)) {
            cd->xAxis.reset();  // measured across the axis line, not along a sketch axis
            return finish(std::move(cd), radial);
        }
    }
    return std::nullopt;
}

// Dimension candidates of one sketch (§6а.10 items 2–4): isDimensionType constraints with driving ≠ false in sketch order.
// skipped — dimensions that gave no candidate (nothing in the model matches them; unplaced report, FX8-DRW-002).
std::vector<Candidate> sketchCandidates(const doc::Document& md, const sketch::SketchData& sk, const regen::SketchEntry& se, const WorldTopo& topo,
                                        const regen::PartState* st = nullptr, std::vector<std::string>* skipped = nullptr) {
    std::vector<Candidate> cands;
    const auto vars = regen::documentVariables(md);
    const auto units = regen::documentUnits(md);
    const Plane& fr = se.frame;
    const auto& pts = se.result.state.points;
    auto worldPt = [&](const std::string& id) -> std::optional<Point3> {
        if (id == "origin") return fr.origin;
        const auto it = pts.find(id);
        if (it != pts.end()) return fr.toWorld(it->second);
        // A projected point (sketch.md §2.5): its geometry in the sketch plane (FX8-DRW-002).
        if (const auto pj = se.projections.find(id); pj != se.projections.end() && pj->second.kind == sketch::Projection::Kind::Point)
            return fr.toWorld(Point2(pj->second.px, pj->second.py));
        return std::nullopt;
    };
    auto radiusOf = [&](const sketch::Entity& e) {
        double r = e.r;
        if (const auto it = se.result.state.radii.find(e.id); it != se.result.state.radii.end()) r = it->second;
        if (e.type == sketch::EntityType::Arc)
            if (const auto S = worldPt(e.start), C = worldPt(e.center); S && C) r = (*S - *C).norm();
        return r;
    };
    // Sketch segment → model edge along the normal, else a cylinder silhouette (FX8-DRW-002).
    bool silhouette = false;  // the last matchRef gave a silhouette
    auto matchSegment = [&](const Point3& A, const Point3& B) -> std::optional<Matched> {
        if (auto m = matchLine(topo, A, B, fr.normal)) return m;
        auto m = matchSilhouette(topo, A, B, fr.normal);
        if (m) silhouette = true;
        return m;
    };
    // Sketch dimension reference → model entity.
    auto matchRef = [&](const std::string& id) -> std::optional<Matched> {
        if (id == "origin") return matchPoint(topo, fr.origin, fr.normal);
        const sketch::Entity* e = sk.findEntity(id);
        if (!e) return std::nullopt;
        switch (e->type) {
            case sketch::EntityType::Point: {
                const auto Pt = worldPt(id);
                return Pt ? matchPoint(topo, *Pt, fr.normal) : std::nullopt;
            }
            case sketch::EntityType::Line: {
                const auto A = worldPt(e->p1), B = worldPt(e->p2);
                return A && B ? matchSegment(*A, *B) : std::nullopt;
            }
            case sketch::EntityType::Circle:
            case sketch::EntityType::Arc: {
                const auto C = worldPt(e->center);
                return C ? matchCircle(topo, *C, radiusOf(*e), fr.normal) : std::nullopt;
            }
            case sketch::EntityType::Projected: {  // FX8-DRW-002: by the source name, else by the projected geometry
                if (st && !e->source.empty())
                    if (const auto it = st->names.byName.find(e->source); it != st->names.byName.end()) {
                        if (it->second.kind == naming::EntityKind::Vertex) return Matched{{"vertex", e->source}, Matched::Kind::Point};
                        if (it->second.kind == naming::EntityKind::Edge) {
                            const auto& ci = topo.edges.at(size_t(it->second.index)).ci;
                            if (ci.kind == kernel::CurveInfo::Kind::Line) return Matched{{"edge", e->source}, Matched::Kind::Segment};
                            if (ci.kind == kernel::CurveInfo::Kind::Circle) return Matched{{"edge", e->source}, Matched::Kind::Circle};
                        }
                    }
                const auto pj = se.projections.find(id);
                if (pj == se.projections.end()) return std::nullopt;
                const sketch::Projection& g = pj->second;
                switch (g.kind) {
                    case sketch::Projection::Kind::Point: return matchPoint(topo, fr.toWorld(Point2(g.px, g.py)), fr.normal);
                    case sketch::Projection::Kind::Line: return matchSegment(fr.toWorld(Point2(g.x1, g.y1)), fr.toWorld(Point2(g.x2, g.y2)));
                    case sketch::Projection::Kind::Arc:
                    case sketch::Projection::Kind::Circle: return matchCircle(topo, fr.toWorld(Point2(g.cx, g.cy)), g.r, fr.normal);
                }
                return std::nullopt;
            }
            default: return std::nullopt;
        }
    };
    // Sketch point for a dimension along dir: a vertex or "witnesses" — line edges ⊥ dir whose line passes through the
    // point (corner cut off by a fillet or chamfer, §6а.10 item 3).
    // Witnesses lie in planes parallel to the sketch (⊥ N); the point is matched along N (FX8-DRW-002).
    auto pointRefs = [&](const Point3& X, const Vec3& dir) {
        std::vector<doc::DimensionRef> out;
        const Vec3 N = fr.normal.normalized();
        if (auto m = matchPoint(topo, X, N)) out.push_back(m->ref);
        for (const auto& e : topo.edges) {
            if (e.ci.kind != kernel::CurveInfo::Kind::Line || out.size() >= MODEL_DIM_WITNESSES) continue;
            const Vec3 t = (e.ci.p2 - e.ci.p1).normalized();
            if (std::abs(t.dot(dir)) > DIM_PARALLEL_TOL || std::abs(t.dot(N)) > DIM_PARALLEL_TOL) continue;
            const Vec3 w = X - e.ci.p1;
            if ((w - w.dot(t) * t - w.dot(N) * N).norm() <= MODEL_DIM_MATCH_TOL) out.push_back({"edge", e.name});
        }
        // Body of revolution (FX13-DRW-001): closed circles with axis ∥ dir and ⊥ N whose plane holds X — edge-on on a
        // view along N, a segment ⊥ dir through X (a shaft step or end face whose corner is chamfered).
        for (const auto& e : topo.edges) {
            if (!isClosedCircle(e.ci) || out.size() >= MODEL_DIM_WITNESSES) continue;
            const Vec3 a = e.ci.axis.normalized();
            if (1 - std::abs(a.dot(dir)) > DIM_PARALLEL_TOL || std::abs(a.dot(N)) > DIM_PARALLEL_TOL) continue;
            if (std::abs((X - e.ci.center).dot(a)) <= MODEL_DIM_MATCH_TOL) out.push_back({"edge", e.name});
        }
        // Then cylinder silhouettes with axis ⊥ dir and ⊥ N whose generatrix passes through X along N.
        for (const auto& f : topo.cylinders) {
            const Vec3 a = f.si.dir.normalized();
            if (std::abs(a.dot(dir)) > DIM_PARALLEL_TOL || std::abs(a.dot(N)) > DIM_PARALLEL_TOL) continue;
            const Vec3 w = N.cross(a).normalized();
            for (const int side : {1, -1}) {
                if (out.size() >= MODEL_DIM_WITNESSES) break;
                const Vec3 v = X - (f.si.point + side * f.si.radius * w);
                if ((v - v.dot(a) * a - v.dot(N) * N).norm() > MODEL_DIM_MATCH_TOL) continue;
                out.push_back({"silhouette", f.name, side});
                silhouette = true;  // sides are for a view with n = +N (Candidate::silhouetteNormal)
            }
        }
        return out;
    };
    // Counterclockwise span of a sketch arc from start to end (rad) and its radius.
    auto arcSweep = [&](const std::string& id) -> std::optional<std::pair<double, double>> {
        const sketch::Entity* e = sk.findEntity(id);
        if (!e || e->type != sketch::EntityType::Arc) return std::nullopt;
        const auto c = pts.find(e->center), s = pts.find(e->start), t = pts.find(e->end);
        if (c == pts.end() || s == pts.end() || t == pts.end()) return std::nullopt;
        const Point2 vs = s->second - c->second, vt = t->second - c->second;
        return std::pair{sweepOf(geom2::angleOf(vs), geom2::angleOf(vt)), vs.norm()};
    };
    for (const auto& c : sk.constraints) {
        if (!sketch::isDimensionType(c.type) || c.driving == false) continue;  // reference ones are not transferred (FX3-DRW-006)
        if (sketch::derivedOffsetDimension(sk, c.id)) continue;  // one offset distance per group (FX12-SK-001)
        // At the end of the iteration: no candidate — skipped (FX8-DRW-002); silhouette references — the sketch normal.
        struct Done {
            std::vector<Candidate>& cands;
            size_t before;
            const std::string& id;
            std::vector<std::string>* skipped;
            bool& silhouette;
            const Vec3 N;
            ~Done() {
                if (cands.size() == before && skipped) skipped->push_back(id);
                if (cands.size() > before && silhouette) cands.back().silhouetteNormal = N;
                silhouette = false;
            }
        } done{cands, cands.size(), c.id, skipped, silhouette, fr.normal.normalized()};
        // Sketch dimension value (mm; angle — degrees): the built dimension must show it (item 3).
        std::optional<double> want;
        if (c.value)
            if (auto q = expr::evaluateField(*c.value, expr::Expected(sketch::isAngleDimension(c.type) ? expr::ANGLE : expr::LENGTH), vars.env(),
                                             units))
                want = std::abs(sketch::isAngleDimension(c.type) ? q->value * 180 / std::numbers::pi : q->value);
        Candidate cd;
        cd.normal = fr.normal;
        cd.value = want;
        cd.constraint = c.id;
        doc::SheetDimension& a = cd.dim;
        // Diametral dimension from an axis (FX3-DRW-004): point P and center line L — circle of revolving P about L, Ø.
        if (c.diametral) {
            if (c.refs.size() != 2) continue;
            const sketch::Entity* e0 = sk.findEntity(c.refs[0]);
            const bool lineFirst = e0 && e0->type == sketch::EntityType::Line;
            const sketch::Entity* L = sk.findEntity(c.refs[lineFirst ? 0 : 1]);
            auto P = worldPt(c.refs[lineFirst ? 1 : 0]);
            if (!L || L->type != sketch::EntityType::Line || !P) continue;
            auto A = worldPt(L->p1), B = worldPt(L->p2);
            if (!A || !B || (*B - *A).norm() <= MODEL_DIM_MATCH_TOL) continue;
            const Vec3 axis = (*B - *A).normalized();
            // The point lies on the axis and the segment is the radius side (FX13-DIM-001): the circle of revolving the
            // segment about the axis through the point (parallel to the segment).
            // The radius point — an end of the segment (a profile point: its circle is an edge of the body).
            if (!lineFirst && sketch::diametralAxisAtPoint(sk, c, &se.result.state)) {
                const Point3 O = *P, E1 = *A, E2 = *B;
                A = O;
                P = radialCircle(topo, E1, O, axis, false) || !radialCircle(topo, E2, O, axis, false) ? E1 : E2;
            }
            const auto ref = radialCircle(topo, *P, *A, axis, false);
            if (!ref) {  // the circle is opened by a cut — the cylinder silhouettes (FX8-DRW-002)
                const Point3 O = *A + (*P - *A).dot(axis) * axis;
                bool radial = false;
                auto rc = (*P - O).norm() > MODEL_DIM_MATCH_TOL ? revolvedPair(topo, *P, O, *P - O, fr, true, radial) : std::nullopt;
                // P on the revolution axis, L — a generatrix parallel to it (FX13-DRW-001: a shaft diameter from an axis
                // point to the cylinder line): the circle or cylinder of radius |P − O| about the axis through P along L.
                if (!rc && (*P - O).norm() > MODEL_DIM_MATCH_TOL) rc = generatrixDiameter(topo, *P, axis, (*P - O).norm(), fr);
                if (!rc) continue;
                rc->xAxis.reset();
                rc->value = want;
                rc->constraint = c.id;
                cands.push_back(std::move(*rc));
                continue;
            }
            a.kind = "linear";
            a.orientation = "aligned";
            a.refs = {*ref};
            cd.along = axis;
            cands.push_back(std::move(cd));
            continue;
        }
        // Path length (FX3-DRW-004): line and arc edges of the path — pathLength dimension.
        if (c.type == "pathLength") {
            bool ok = !c.refs.empty();
            for (const auto& r : c.refs) {
                const auto m = matchRef(r);
                ok = ok && m && m->kind != Matched::Kind::Point;
                if (ok) a.refs.push_back(m->ref);
            }
            if (!ok) continue;
            a.kind = "pathLength";
            cands.push_back(std::move(cd));
            continue;
        }
        std::vector<std::optional<Matched>> ms;
        bool ok = !c.refs.empty();
        for (const auto& r : c.refs) {
            ms.push_back(matchRef(r));
            ok = ok && ms.back();
        }
        // Distance to a circle "near" / "far" (FX3-DRW-004): for a circle reference — its nearest or farthest point.
        if (c.arc) {
            if (!ok || ms.size() != 2) continue;
            const int ci = ms[1]->kind == Matched::Kind::Circle ? 1 : ms[0]->kind == Matched::Kind::Circle ? 0 : -1;
            if (ci < 0) continue;
            a.kind = "linear";
            a.orientation = "aligned";
            a.refs = {ms[0]->ref, ms[1]->ref};
            a.refs[size_t(ci)].arc = *c.arc == "far" ? "far" : "near";
            cands.push_back(std::move(cd));
            continue;
        }
        const bool linearType = c.type == "length" || c.type == "distance" || c.type == "distanceX" || c.type == "distanceY";
        // Fallback references of a linear dimension by sketch points (item 3): endpoints of the length segment, distance points.
        if (linearType) {
            std::vector<Point3> X;
            if (c.type == "length" && c.refs.size() == 1)
                if (const sketch::Entity* e = sk.findEntity(c.refs[0]); e && e->type == sketch::EntityType::Line)
                    if (const auto A = worldPt(e->p1), B = worldPt(e->p2); A && B) X = {*A, *B};
            if (c.type != "length" && c.refs.size() == 2) {
                const auto A = worldPt(c.refs[0]), B = worldPt(c.refs[1]);
                const sketch::Entity* e0 = sk.findEntity(c.refs[0]);
                const sketch::Entity* e1 = sk.findEntity(c.refs[1]);
                const bool p0 = c.refs[0] == "origin" || (e0 && e0->type == sketch::EntityType::Point);
                const bool p1 = c.refs[1] == "origin" || (e1 && e1->type == sketch::EntityType::Point);
                if (A && B && p0 && p1) X = {*A, *B};
                // A point and a segment (FX13-DRW-001): the point and the foot of the perpendicular on the segment line.
                if (c.type == "distance" && !c.diametral && p0 != p1) {
                    const sketch::Entity* L = p0 ? e1 : e0;
                    const auto P = p0 ? A : B;
                    if (P && L && L->type == sketch::EntityType::Line)
                        if (const auto L1 = worldPt(L->p1), L2 = worldPt(L->p2); L1 && L2 && (*L2 - *L1).norm() > MODEL_DIM_MATCH_TOL) {
                            const Vec3 l = (*L2 - *L1).normalized();
                            X = {*P, *L1 + (*P - *L1).dot(l) * l};
                        }
                }
            }
            if (X.size() == 2 && (X[1] - X[0]).norm() > MODEL_DIM_MATCH_TOL) {
                const Vec3 dir = c.type == "distanceX" ? fr.x.normalized() : c.type == "distanceY" ? fr.y.normalized() : Vec3((X[1] - X[0]).normalized());
                const auto r0 = pointRefs(X[0], dir), r1 = pointRefs(X[1], dir);
                for (const auto& x0 : r0)
                    for (const auto& x1 : r1) {
                        doc::SheetDimension alt;
                        alt.kind = "linear";
                        alt.orientation = "aligned";
                        alt.refs = {x0, x1};
                        cd.alts.push_back(alt);
                    }
            }
            if (c.type != "length" && c.type != "distance") cd.xAxis = c.type == "distanceX" ? fr.x : fr.y;
        }
        if (!ok) {
            // Body of revolution (§6а.10 item 3а): the origin is not a model vertex; a dimension from the origin along the sketch
            // axis to a profile point is transferred to the circles obtained by revolution.
            if (auto rc = revolvedCandidate(topo, sk, c, fr, worldPt, want)) {  // radial — the value is the diameter
                rc->constraint = c.id;
                cands.push_back(std::move(*rc));
                continue;
            }
            if (!linearType || cd.alts.empty() || !want) continue;
            cd.dim = cd.alts.front();  // only fallback references (point — not a vertex)
            cd.alts.erase(cd.alts.begin());
            cands.push_back(std::move(cd));
            continue;
        }
        for (const auto& m : ms) a.refs.push_back(m->ref);
        if (c.type == "radius" || c.type == "diameter") {
            if (ms.size() != 1 || ms[0]->kind != Matched::Kind::Circle) continue;
            a.kind = c.type;
        } else if (c.type == "length") {
            if (ms.size() != 1 || ms[0]->kind != Matched::Kind::Segment) continue;
            a.kind = "linear";
            a.orientation = "aligned";
        } else if (c.type == "distance" || c.type == "distanceX" || c.type == "distanceY") {
            if (ms.size() != 2) continue;
            a.kind = "linear";
            a.orientation = "aligned";
        } else if (c.type == "angle") {
            if (ms.size() != 2 || ms[0]->kind != Matched::Kind::Segment || ms[1]->kind != Matched::Kind::Segment) continue;
            a.kind = "angle";
        } else if (c.type == "arcLength" || c.type == "arcAngle") {  // FX2-DRW-001
            const auto sw = ms.size() == 1 && ms[0]->kind == Matched::Kind::Circle ? arcSweep(c.refs[0]) : std::nullopt;
            if (!sw) continue;
            a.kind = c.type == "arcLength" ? "arcLength" : "angle";
            cd.value = c.type == "arcLength" ? sw->first * sw->second : sw->first * 180 / std::numbers::pi;
        } else {
            continue;
        }
        cands.push_back(std::move(cd));
    }
    for (auto& cd : cands)  // end-on variant of a diameter by an edge-on circle
        if (cd.dim.kind == "linear" && cd.dim.refs.size() == 1 && cd.dim.refs[0].kind == "edge")
            for (const auto& e : topo.edges)
                if (e.name == cd.dim.refs[0].name) {
                    if (isClosedCircle(e.ci)) cd.endOnAxis = e.ci.axis.normalized();
                    break;
                }
    return cands;
}

// Model view with a built placement (§6а.10 item 1): Ctx is built in place — references to the record fields.
struct ViewInfo {
    const doc::DrawingView* v = nullptr;
    Setup su;
};

// Room on the sheet (§6а.10 item 5в, FX13-DRW-003): the drawing field inside the frame without the title block, and the
// boxes of the views (by their named geometry, §6а.7) — a dimension fits a view when its primitives stay in the field,
// out of the title block and out of the other views.
struct Rect {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    void add(const Point2& p) { x0 = std::min(x0, p.x()), y0 = std::min(y0, p.y()), x1 = std::max(x1, p.x()), y1 = std::max(y1, p.y()); }
    bool empty() const { return x1 < x0; }
    bool inside(const Rect& o) const { return x0 >= o.x0 - DIM_EPS && x1 <= o.x1 + DIM_EPS && y0 >= o.y0 - DIM_EPS && y1 <= o.y1 + DIM_EPS; }
    bool meets(const Rect& o) const {
        return !empty() && !o.empty() && x0 < o.x1 - MODEL_DIM_OVERLAP_TOL && o.x0 < x1 - MODEL_DIM_OVERLAP_TOL && y0 < o.y1 - MODEL_DIM_OVERLAP_TOL &&
               o.y0 < y1 - MODEL_DIM_OVERLAP_TOL;
    }
};
struct Room {
    Rect field, titleBlock;
    std::map<std::string, Rect> views;  // view ID → box on the sheet
};

Rect primitivesBox(const std::vector<Primitive>& prims);

// Common §6а.10 flow for one model: candidates → the first view and variant where the dimension builds and passes the check.
struct Placer {
    const doc::Document& d;   // drawing document
    const doc::Document& md;  // model document (feature expressions)
    const doc::Element* drawingEl;
    const doc::Sheet& sheet;
    const ModelProvider& models;
    const regen::PartState& st;
    const std::vector<ViewInfo>& views;     // target views of the command
    const std::vector<ViewInfo>& allViews;  // main and projected views of every chain of the model (duplicate check, FX7-DRW-009)
    const std::vector<doc::SheetDimension>& existing;  // drawing dimensions on views of this model (item 6)
    std::vector<doc::SheetDimension>& out;
    std::map<std::string, int>& placedOnView;
    std::array<bool, 3> overallDone{false, false, false};  // world axes that already have an overall dimension on the drawing (items 5а, 6)
    // Part and body (item 5б, FX7-DRW-010): values and directions of dimensions of the model on the drawing, placed by the
    // command or buildable from its sources on some view — the axes they cover get no overall dimension.
    struct Note {
        double value = 0;
        Vec3 dir = Vec3::Zero();
        bool diameter = false;  // dir — the axis â: covers every direction ⊥ â
    };
    bool noting = false;
    std::vector<Note> notes;
    // Sources tried by place() and their outcome (unplaced report, FX8-DRW-002): placed (or already on the drawing),
    // not placed on the target views but buildable on another view of the model.
    int attempts = 0, successes = 0, elsewhere = 0;
    // Source of the feature dimension being placed (FX12-PMI-007): feature ID and parameter path; empty param — none
    // (sketch dimensions — their tolerance is matched by references, §6а.3).
    std::string srcFeature, srcParam;
    std::vector<UnplacedDimension>* unplaced = nullptr;
    // Target views (FX13-DRW-003): the first `preferred` are the selected ones — tried first and without the room check;
    // the rest — first where the dimension fits (room), then anywhere. room nullptr — no room check.
    size_t preferred = 0;
    const Room* room = nullptr;

    // The built dimension fits view `view` (item 5в).
    bool fits(const std::string& view, const DimensionPreview& r) const {
        if (!room) return true;
        const Rect b = primitivesBox(r.primitives);
        if (b.empty()) return true;
        if (!room->field.empty() && !b.inside(room->field)) return false;
        if (b.meets(room->titleBlock)) return false;
        for (const auto& [id, vb] : room->views)
            if (id != view && b.meets(vb)) return false;
        return true;
    }

    Ctx ctx(const ViewInfo& vi) const { return Ctx{vi.su.vp, stateOrEmpty(vi.su.state), models.kernel()}; }

    // Note of a built dimension a on sheet s (item 5б): linear — the world measurement direction, linear-diameter (§6а.3) and
    // diameter — the axis of the circle or surface.
    void note(const doc::Sheet& s, const doc::SheetDimension& a, const DimensionPreview& r) {
        if (!noting || (a.kind != "linear" && a.kind != "diameter")) return;
        bool viewError = false;
        auto su = setup(s, a.view, models, viewError);
        if (!su) return;
        const Ctx c{su->vp, stateOrEmpty(su->state), models.kernel()};
        const kernel::Kernel& k = models.kernel();
        Note nt;
        nt.value = r.value;
        if (a.kind == "diameter") {
            nt.diameter = true;
            nt.dir = c.vp.n;
            notes.push_back(nt);
            return;
        }
        std::vector<RefGeom> g;
        for (const auto& ref : a.refs) {
            auto rg = refGeom(c, ref);
            if (!rg) return;
            g.push_back(*rg);
        }
        if (g.size() == 1 && g[0].edgeOn) {
            const auto& e = c.st.names.edges.at(size_t(g[0].edgeEntry));
            nt.diameter = true;
            nt.dir = k.edgeCurve(c.st.bodies.at(size_t(e.body)).shape, e.logical).axis.normalized();
        } else if (g.size() == 2 && g[0].faceEntry >= 0 && g[0].faceEntry == g[1].faceEntry && g[0].side == -g[1].side && g[0].side != 0) {
            const auto& fe = c.st.names.faces.at(size_t(g[0].faceEntry));
            nt.diameter = true;
            nt.dir = k.faceSurface(c.st.bodies.at(size_t(fe.body)).shape, fe.face).dir.normalized();
        } else if (r.layout.valid) {
            nt.dir = r.layout.d.x() * c.vp.r + r.layout.d.y() * c.vp.u;
        } else {
            return;
        }
        notes.push_back(nt);
    }
    // The axis D with the extent `value` is covered by a note (item 5б).
    bool covered(const Vec3& D, double value) const {
        for (const auto& nt : notes) {
            if (std::abs(nt.value - value) > MODEL_DIM_VALUE_TOL) continue;
            if (nt.diameter ? std::abs(nt.dir.dot(D)) <= DIM_PARALLEL_TOL : 1 - std::abs(nt.dir.dot(D)) <= DIM_PARALLEL_TOL) return true;
        }
        return false;
    }

    // Item 6; linear dimensions on the same two references along different axes (horizontal and vertical — a corner to a
    // corner along X and along Y, FX8-DRW-002) are different dimensions.
    static bool same(const doc::SheetDimension& x, const doc::SheetDimension& a) {
        if (!sameRefs(x, a)) return false;
        const bool hv = (x.orientation == "horizontal" && a.orientation == "vertical") || (x.orientation == "vertical" && a.orientation == "horizontal");
        return !(x.kind == "linear" && hv);
    }
    bool duplicate(const doc::SheetDimension& a) const {
        for (const auto& x : existing)
            if (same(x, a)) return true;
        for (const auto& x : out)
            if (same(x, a)) return true;
        return false;
    }

    // Placement fields (item 5); for an angular dimension on two edges the angle field is set by the caller.
    void placement(doc::SheetDimension& a, const ViewInfo& vi) const {
        const int i = placedOnView.count(a.view) ? placedOnView.at(a.view) : 0;
        if (a.kind == "linear") {
            a.offset = MODEL_DIM_OFFSET_MM + MODEL_DIM_STEP_MM * i;  // rows — item 7
            return;
        }
        if (a.kind == "angle" && a.refs.size() == 2) {
            a.offset = MODEL_DIM_ANGLE_RADIUS_MM + MODEL_DIM_STEP_MM * i;
            return;
        }
        if (a.kind == "pathLength") {
            a.offset = MODEL_DIM_OFFSET_MM;
            a.angle = MODEL_DIM_CIRCLE_ANGLE;
            return;
        }
        const Ctx c = ctx(vi);
        auto g = refGeom(c, a.refs.front());
        if (!g) return;
        const double R = vi.su.vp.scale * g->radius;
        if (a.kind == "arcLength") {
            a.offset = MODEL_DIM_OFFSET_MM;
        } else if (a.kind == "angle") {  // central angle of an arc
            a.offset = R + MODEL_DIM_OFFSET_MM;
        } else {  // diameter, radius: label outside the circle; for an arc — at its middle (arrow on the arc)
            a.offset = R + MODEL_DIM_OFFSET_MM;
            a.angle = g->kind == RefGeom::Kind::Arc ? g->start + sweepOf(g->start, g->end) / 2 : MODEL_DIM_CIRCLE_ANGLE;
        }
    }

    // Builds a variant on view vi; accepts it (adds to out) on success and passing accept. Returns true on
    // acceptance or a duplicate (the dimension already exists — stop searching).
    bool tryVariant(doc::SheetDimension a, const ViewInfo& vi, const std::function<bool(double)>& accept, bool needRoom = false) {
        if (duplicate(a)) return true;
        a.view = vi.v->id;
        if (!srcParam.empty()) a.source = doc::Json{{"feature", srcFeature}, {"param", srcParam}};
        placement(a, vi);
        bool viewError = false;
        auto r = build(d, drawingEl, sheet, a, std::nullopt, models, viewError);
        if (!r || (accept && !accept(r->value))) return false;
        if (needRoom && !fits(a.view, *r)) return false;
        note(sheet, a, *r);
        // A parallel dimension whose measurement direction on the view is horizontal or vertical becomes horizontal
        // or vertical (§6а.10 item 4): dimensions go above the view and to its left (row layout, item 7).
        if (a.kind == "linear" && a.orientation == "aligned" && r->layout.valid) {
            if (std::abs(r->layout.d.y()) <= DIM_PARALLEL_TOL) a.orientation = "horizontal";
            else if (std::abs(r->layout.d.x()) <= DIM_PARALLEL_TOL) a.orientation = "vertical";
        }
        ++placedOnView[a.view];
        out.push_back(std::move(a));
        return true;
    }

    // The first view (in order) and the first variant on it (§6а.10 item 5); true — placed (or already exists).
    // A variant of the source already standing on any view of the model (also outside the targets) — the source is
    // done (FX7-DRW-009: a dimension is not repeated on another view).
    bool place(const std::function<std::vector<doc::SheetDimension>(const ViewInfo&)>& variants, const std::function<bool(double)>& accept = {}) {
        ++attempts;
        for (const auto& vi : allViews)
            for (const auto& a : variants(vi))
                if (duplicate(a)) {
                    noteVariant(a, vi, accept);
                    ++successes;
                    return true;
                }
        // The selected views first (FX13-DRW-003), then the others where there is room, then anywhere (item 5в).
        for (const bool roomPass : {true, false})
            for (size_t i = 0; i < views.size(); ++i) {
                const bool selected = i < preferred;
                if (selected && !roomPass) continue;
                const ViewInfo& vi = views[i];
                for (auto& a : variants(vi))
                    if (tryVariant(std::move(a), vi, accept, !selected && roomPass && room)) {
                        ++successes;
                        return true;
                    }
            }
        // Not placed on the target views: a source that builds on another view of the model still covers its axis (item 5б)
        // and is not reported as unplaced.
        for (const auto& vi : allViews) {
            if (std::any_of(views.begin(), views.end(), [&](const ViewInfo& x) { return x.v == vi.v; })) continue;
            for (const auto& a : variants(vi))
                if (noting ? noteVariant(a, vi, accept) : builds(a, vi, accept)) {
                    ++elsewhere;
                    return false;
                }
        }
        return false;
    }
    bool builds(doc::SheetDimension a, const ViewInfo& vi, const std::function<bool(double)>& accept) const {
        a.view = vi.v->id;
        placement(a, vi);
        bool viewError = false;
        auto r = build(d, drawingEl, sheet, a, std::nullopt, models, viewError);
        return r && (!accept || accept(r->value));
    }
    void report(const doc::Feature& f, const std::string& constraint, const std::string& value) {
        if (unplaced) unplaced->push_back({f.id, f.name, constraint, value});
    }
    // Builds variant a on view vi and notes it when it passes accept (item 5б).
    bool noteVariant(doc::SheetDimension a, const ViewInfo& vi, const std::function<bool(double)>& accept) {
        if (!noting) return false;
        a.view = vi.v->id;
        placement(a, vi);
        bool viewError = false;
        auto r = build(d, drawingEl, sheet, a, std::nullopt, models, viewError);
        if (!r || (accept && !accept(r->value))) return false;
        note(sheet, a, *r);
        return true;
    }
};

bool nearValue(double a, double b) { return std::abs(a - b) <= MODEL_DIM_VALUE_TOL; }
bool axisAlongView(const Vec3& axis, const Vec3& n) { return 1 - std::abs(axis.normalized().dot(n)) <= DIM_PARALLEL_TOL; }

// Feature expression value (mm or rad); error — nullopt (the source is skipped).
std::optional<double> fieldValue(const doc::Document& d, const std::string& text, expr::Expected expected) {
    auto q = expr::evaluateField(text, expected, regen::documentVariables(d).env(), regen::documentUnits(d));
    if (!q) return std::nullopt;
    return q->value;
}
std::optional<double> jsonValue(const doc::Document& d, const doc::Json& def, const char* key, expr::Expected expected) {
    if (!def.contains(key)) return std::nullopt;
    const auto& v = def[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) return fieldValue(d, v.get<std::string>(), expected);
    return std::nullopt;
}

// Table face indices with a base starting with prefix (exact — equal to it), in table order.
std::vector<int> facesWithBase(const regen::PartState& st, const std::string& prefix, bool exact = false) {
    std::vector<int> out;
    for (size_t i = 0; i < st.names.faces.size(); ++i) {
        const std::string& b = st.names.faces[i].base;
        if (exact ? b == prefix : b.rfind(prefix, 0) == 0) out.push_back(int(i));
    }
    return out;
}
// Table edge indices bounding face fi, in table order.
std::vector<int> edgesOfFace(const regen::PartState& st, int fi) {
    std::vector<int> out;
    for (size_t i = 0; i < st.names.edges.size(); ++i)
        if (st.names.edges[i].f1 == fi || st.names.edges[i].f2 == fi) out.push_back(int(i));
    return out;
}
std::vector<int> edgesOfFaces(const regen::PartState& st, const std::vector<int>& faces) {
    std::vector<int> out;
    for (size_t i = 0; i < st.names.edges.size(); ++i)
        for (int f : faces)
            if (st.names.edges[i].f1 == f || st.names.edges[i].f2 == f) {
                out.push_back(int(i));
                break;
            }
    return out;
}
// Faces adjacent to face fi via an edge, in face table order.
std::vector<int> adjacentFaces(const regen::PartState& st, int fi) {
    std::set<int> s;
    for (int e : edgesOfFace(st, fi)) s.insert(st.names.edges[size_t(e)].f1 == fi ? st.names.edges[size_t(e)].f2 : st.names.edges[size_t(e)].f1);
    return {s.begin(), s.end()};
}

doc::DimensionRef edgeRef(const regen::PartState& st, int i) { return {"edge", st.names.edges.at(size_t(i)).full}; }

// Planar faces lying in the plane (point O, normal N).
std::vector<int> facesInPlane(const regen::PartState& st, const kernel::Kernel& k, const Point3& O, const Vec3& N) {
    std::vector<int> out;
    for (size_t i = 0; i < st.names.faces.size(); ++i) {
        const auto& fe = st.names.faces[i];
        const auto si = k.faceSurface(st.bodies.at(size_t(fe.body)).shape, fe.face);
        if (si.kind != kernel::SurfaceInfo::Kind::Plane || si.dir.normalized().cross(N.normalized()).norm() > DIM_PARALLEL_TOL) continue;
        if (std::abs((si.point - O).dot(N.normalized())) <= MODEL_DIM_MATCH_TOL) out.push_back(int(i));
    }
    return out;
}

// Distance between parallel faces (§6а.10 item 3в): linear on a line edge of a face from fa and an edge of a face from fb
// parallel to it on the view at distance value (shell thickness, plane offset).
constexpr size_t MODEL_DIM_PAIR_VARIANTS = 8;
bool placeParallel(Placer& P, const std::vector<int>& fa, const std::vector<int>& fb, double value) {
    const regen::PartState& st = P.st;
    const std::vector<int> ea = edgesOfFaces(st, fa), eb = edgesOfFaces(st, fb);
    return P.place(
        [&](const ViewInfo& vi) {
            std::vector<doc::SheetDimension> v;
            const Ctx c = P.ctx(vi);
            std::vector<std::pair<int, RefGeom>> ga, gb;
            for (int i : ea)
                if (auto g = edgeGeom(c, i); g && g->kind == RefGeom::Kind::Segment) ga.push_back({i, *g});
            for (int i : eb)
                if (auto g = edgeGeom(c, i); g && g->kind == RefGeom::Kind::Segment) gb.push_back({i, *g});
            for (const auto& [i, g1] : ga)
                for (const auto& [j, g2] : gb) {
                    if (i == j || v.size() >= MODEL_DIM_PAIR_VARIANTS) continue;
                    const Point2 t1 = (g1.b - g1.a).normalized(), t2 = (g2.b - g2.a).normalized();
                    if (std::abs(cross2(t1, t2)) > DIM_PARALLEL_TOL) continue;
                    if (!nearValue(std::abs((g2.a - g1.a).dot(Point2(-t1.y(), t1.x()))), value)) continue;
                    doc::SheetDimension a;
                    a.kind = "linear";
                    a.orientation = "aligned";
                    a.refs = {edgeRef(st, i), edgeRef(st, j)};
                    v.push_back(a);
                }
            return v;
        },
        [&](double x) { return nearValue(x, value); });
}

// Thread designation (§6а.10 item 3г): dimension on a circle of the threaded face radius on the thread axis — first on a side
// view (edge-on circle, linear), otherwise end-on (diameter); text — thread designation, empty prefix. A dimension
// with the same references already placed by this command gets the designation instead of the number.
void threadDesignations(Placer& P, const std::string& featureId, const WorldTopo& topo) {
    for (const auto& t : P.st.threads) {
        if (t.feature != featureId) continue;
        std::string text = t.designation;
        for (size_t pos; (pos = text.find('x')) != std::string::npos;) text.replace(pos, 1, "\xC3\x97");  // «M8x1» → «M8×1»
        const Vec3 a = t.dir.normalized();
        std::vector<int> circles;
        for (size_t i = 0; i < topo.edges.size(); ++i) {
            const auto& ci = topo.edges[i].ci;
            if (!isClosedCircle(ci) || std::abs(ci.radius - t.radius) > MODEL_DIM_MATCH_TOL || 1 - std::abs(ci.axis.normalized().dot(a)) > DIM_PARALLEL_TOL) continue;
            if ((ci.center - t.origin).cross(a).norm() > MODEL_DIM_MATCH_TOL) continue;
            circles.push_back(int(i));
        }
        if (circles.empty()) continue;
        for (const bool side : {true, false}) {
            bool done = false;
            for (const auto& vi : P.views) {
                if (side ? std::abs(a.dot(vi.su.vp.n)) > DIM_PARALLEL_TOL : !axisAlongView(a, vi.su.vp.n)) continue;
                for (int i : circles) {
                    doc::SheetDimension x;
                    x.kind = side ? "linear" : "diameter";
                    if (side) x.orientation = "aligned";
                    x.refs = {edgeRef(P.st, i)};
                    x.prefix = std::string();
                    x.text = text;
                    // Already placed by this command (diameter of the same edge) — designation instead of the number.
                    auto same = std::find_if(P.out.begin(), P.out.end(), [&](const doc::SheetDimension& y) { return sameRefs(y, x); });
                    if (same != P.out.end()) {
                        same->prefix = x.prefix;
                        same->text = x.text;
                        done = true;
                        break;
                    }
                    if (P.tryVariant(x, vi, {})) {
                        done = true;
                        break;
                    }
                }
                if (done) break;
            }
            if (done) break;
        }
    }
}

// Part feature dimensions (§6а.10 item 3б, FX2-DRW-001, FX3-DRW-004).
void featureDimensions(Placer& P, const doc::Feature& f, const WorldTopo& topo) {
    const regen::PartState& st = P.st;
    const doc::Document& d = P.md;
    const kernel::Kernel& k = P.models.kernel();
    auto isArc = [&](int i) { return topo.edges[size_t(i)].ci.kind == kernel::CurveInfo::Kind::Circle && !topo.edges[size_t(i)].ci.closed; };
    auto isCircle = [&](int i) { return isClosedCircle(topo.edges[size_t(i)].ci); };
    auto isLine = [&](int i) { return topo.edges[size_t(i)].ci.kind == kernel::CurveInfo::Kind::Line; };
    P.srcFeature = f.id;
    if (f.type == doc::FeatureType::Fillet) {
        P.srcParam = "radius";
        const auto r = fieldValue(d, f.edgeFeature()->size, expr::Expected(expr::LENGTH));
        if (!r) return;
        std::vector<int> es;
        for (int i : edgesOfFaces(st, facesWithBase(st, f.id + ".fillet(")))
            if (isArc(i) && std::abs(topo.edges[size_t(i)].ci.radius - *r) <= MODEL_DIM_MATCH_TOL) es.push_back(i);
        P.place(
            [&](const ViewInfo& vi) {
                std::vector<doc::SheetDimension> v;
                for (int i : es) {
                    if (!axisAlongView(topo.edges[size_t(i)].ci.axis, vi.su.vp.n)) continue;
                    doc::SheetDimension a;
                    a.kind = "radius";
                    a.refs = {edgeRef(st, i)};
                    v.push_back(a);
                }
                return v;
            },
            [&](double value) { return nearValue(value, *r); });
        return;
    }
    if (f.type == doc::FeatureType::Chamfer) {
        P.srcParam = "distance";
        const auto dist = fieldValue(d, f.edgeFeature()->size, expr::Expected(expr::LENGTH));
        if (!dist) return;
        const std::vector<int> faces = facesWithBase(st, f.id + ".chamfer(");
        std::vector<int> cuts;  // (а) chamfer cuts on adjacent faces
        for (int i : edgesOfFaces(st, faces)) {
            const auto& ci = topo.edges[size_t(i)].ci;
            if (isLine(i) && std::abs((ci.p2 - ci.p1).norm() - *dist * std::numbers::sqrt2) <= MODEL_DIM_MATCH_TOL) cuts.push_back(i);
        }
        std::vector<std::pair<int, int>> rims;  // (б) circles of a conical chamfer
        if (cuts.empty())
            for (int fi : faces) {
                std::vector<int> cs;
                for (int i : edgesOfFace(st, fi))
                    if (isCircle(i)) cs.push_back(i);
                for (size_t a = 0; a < cs.size(); ++a)
                    for (size_t b = a + 1; b < cs.size(); ++b)
                        if (axisAlongView(topo.edges[size_t(cs[a])].ci.axis, topo.edges[size_t(cs[b])].ci.axis.normalized()))
                            rims.push_back({cs[a], cs[b]});
            }
        P.place(
            [&](const ViewInfo& vi) {
                std::vector<doc::SheetDimension> v;
                doc::SheetDimension a;
                a.kind = "linear";
                a.suffix = "\xC3\x97" "45\xC2\xB0";  // «×45°»
                for (int i : cuts) {
                    const auto& ci = topo.edges[size_t(i)].ci;
                    if (std::abs((ci.p2 - ci.p1).normalized().dot(vi.su.vp.n)) > DIM_PARALLEL_TOL) continue;  // cut — in the view plane
                    for (const char* o : {"horizontal", "vertical"}) {
                        a.orientation = o;
                        a.refs = {edgeRef(st, i)};
                        v.push_back(a);
                    }
                }
                for (const auto& [e0, e1] : rims) {
                    a.orientation = "aligned";
                    a.refs = {edgeRef(st, e0), edgeRef(st, e1)};
                    v.push_back(a);
                }
                return v;
            },
            [&](double value) { return nearValue(value, *dist); });
        return;
    }
    if (const doc::RevolveData* rv = f.revolve(); rv && rv->angleType != "full") {
        P.srcParam = "angle.value";
        const auto th = fieldValue(d, rv->angleValue, expr::Expected(expr::ANGLE));
        if (!th) return;
        const double deg = *th * 180 / std::numbers::pi;
        bool placed = false;
        const auto fs = facesWithBase(st, f.id + ".cap(start)", true), fe = facesWithBase(st, f.id + ".cap(end)", true);
        if (*th < std::numbers::pi - 1e-12 && !fs.empty() && !fe.empty()) {
            auto normalOf = [&](int fi) {
                const auto& e = st.names.faces.at(size_t(fi));
                return k.faceSurface(st.bodies.at(size_t(e.body)).shape, e.face).dir.normalized();
            };
            const Vec3 cr = normalOf(fs[0]).cross(normalOf(fe[0]));
            if (cr.norm() > DIM_PARALLEL_TOL) {
                const Vec3 axis = cr.normalized();
                auto radial = [&](int fi) -> int {  // first line edge of the end face ⊥ axis
                    for (int i : edgesOfFace(st, fi)) {
                        const auto& ci = topo.edges[size_t(i)].ci;
                        if (isLine(i) && std::abs((ci.p2 - ci.p1).normalized().dot(axis)) <= 1e-9) return i;
                    }
                    return -1;
                };
                const int es = radial(fs[0]), ee = radial(fe[0]);
                if (es >= 0 && ee >= 0)
                    placed = P.place(
                        [&](const ViewInfo& vi) {
                            std::vector<doc::SheetDimension> v;
                            if (!axisAlongView(axis, vi.su.vp.n)) return v;
                            // Body sector: from the intersection point of the straight edges toward the sum of directions to their midpoints.
                            const auto& c1 = topo.edges[size_t(es)].ci;
                            const auto& c2 = topo.edges[size_t(ee)].ci;
                            const Point2 a1 = vi.su.vp.toView(c1.p1), b1 = vi.su.vp.toView(c1.p2);
                            const Point2 a2 = vi.su.vp.toView(c2.p1), b2 = vi.su.vp.toView(c2.p2);
                            const Point2 t1 = (b1 - a1).normalized(), t2 = (b2 - a2).normalized();
                            const double crs = cross2(t1, t2);
                            if (std::abs(crs) <= DIM_PARALLEL_TOL) return v;
                            const Point2 O = a1 + cross2(a2 - a1, t2) / crs * t1;
                            const Point2 m1 = (a1 + b1) / 2 - O, m2 = (a2 + b2) / 2 - O;
                            if (m1.norm() <= DIM_EPS || m2.norm() <= DIM_EPS) return v;
                            doc::SheetDimension a;
                            a.kind = "angle";
                            a.refs = {edgeRef(st, es), edgeRef(st, ee)};
                            a.angle = geom2::angleOf(Point2(m1.normalized() + m2.normalized()));
                            v.push_back(a);
                            return v;
                        },
                        [&](double value) { return nearValue(value, deg); });
            }
        }
        // Angle from 180° (and angle without radial end-face edges, FX3-DRW-004): central angle of a side face arc — arc edges
        // with a span equal to the revolution angle.
        if (!placed) {
            std::vector<int> arcs;
            for (int i : edgesOfFaces(st, facesWithBase(st, f.id + ".side(")))
                if (isArc(i)) arcs.push_back(i);
            P.place(
                [&](const ViewInfo& vi) {
                    std::vector<doc::SheetDimension> v;
                    for (int i : arcs) {
                        if (!axisAlongView(topo.edges[size_t(i)].ci.axis, vi.su.vp.n)) continue;
                        doc::SheetDimension a;
                        a.kind = "angle";
                        a.refs = {edgeRef(st, i)};
                        v.push_back(a);
                    }
                    return v;
                },
                [&](double value) { return nearValue(value, deg); });
        }
        return;
    }
    if (f.type == doc::FeatureType::Hole) {
        const doc::Json& def = f.pattern()->def;
        const std::string kind = def.value("kind", std::string("simple"));
        if (kind == "tapped") {  // tapped hole — thread designation
            threadDesignations(P, f.id, topo);
            return;
        }
        std::vector<std::string> walls = {"s1"};
        if (kind == "counterbore") walls.push_back("s3");
        if (kind == "countersink") walls.push_back("s5");
        for (const auto& w : walls) {
            P.srcParam = w == "s1" ? "diameter" : "";  // the wall diameter (the face tolerance of s1 — the same, §6а.3)
            std::vector<int> cs;
            for (int i : edgesOfFaces(st, facesWithBase(st, f.id + ".side(" + w + "/")))
                if (isCircle(i)) cs.push_back(i);
            if (w == "s5")  // countersink: the largest circle of the cone
                std::stable_sort(cs.begin(), cs.end(), [&](int a, int b) { return topo.edges[size_t(a)].ci.radius > topo.edges[size_t(b)].ci.radius + MODEL_DIM_MATCH_TOL; });
            P.place([&](const ViewInfo& vi) {
                std::vector<doc::SheetDimension> v;
                for (int i : cs) {
                    if (!axisAlongView(topo.edges[size_t(i)].ci.axis, vi.su.vp.n)) continue;
                    if (w == "s5" && std::abs(topo.edges[size_t(i)].ci.radius - topo.edges[size_t(cs[0])].ci.radius) > MODEL_DIM_MATCH_TOL) continue;
                    doc::SheetDimension a;
                    a.kind = "diameter";
                    a.refs = {edgeRef(st, i)};
                    v.push_back(a);
                }
                return v;
            });
        }
        return;
    }
    // Hole of a connection cut in the part (FX8-DRW-002, regen.md §4.19): a threaded one — the thread designation; otherwise
    // a diameter per wall radius — a closed circle of a face F.side(…) with the axis ∥ n, else edge-on (linear, Ø).
    if (f.type == doc::FeatureType::ConnectionCut) {
        if (!f.pattern() || !f.pattern()->def.contains("revolve")) return;
        if (f.pattern()->def.contains("thread")) {
            threadDesignations(P, f.id, topo);
            return;
        }
        std::vector<std::vector<int>> byRadius;  // closed circles of the walls grouped by radius, in table order
        for (int i : edgesOfFaces(st, facesWithBase(st, f.id + ".side("))) {
            if (!isCircle(i)) continue;
            auto g = std::find_if(byRadius.begin(), byRadius.end(), [&](const std::vector<int>& x) {
                return std::abs(topo.edges[size_t(x[0])].ci.radius - topo.edges[size_t(i)].ci.radius) <= MODEL_DIM_MATCH_TOL;
            });
            if (g == byRadius.end()) byRadius.push_back({i});
            else g->push_back(i);
        }
        for (const auto& cs : byRadius)
            P.place([&](const ViewInfo& vi) {
                std::vector<doc::SheetDimension> v;
                for (const bool endOn : {true, false})
                    for (int i : cs) {
                        const bool along = axisAlongView(topo.edges[size_t(i)].ci.axis, vi.su.vp.n);
                        if (endOn != along || (!along && std::abs(topo.edges[size_t(i)].ci.axis.normalized().dot(vi.su.vp.n)) > DIM_PARALLEL_TOL)) continue;
                        doc::SheetDimension a;
                        a.kind = endOn ? "diameter" : "linear";
                        if (!endOn) a.orientation = "aligned";
                        a.refs = {edgeRef(st, i)};
                        v.push_back(a);
                    }
                return v;
            });
        return;
    }
    if (f.type == doc::FeatureType::Thread) {
        threadDesignations(P, f.id, topo);
        return;
    }
    if (f.type == doc::FeatureType::Shell) {  // wall thickness: inner face F.inner(g) and face g
        P.srcParam = "thickness";
        const doc::Json& def = f.pattern()->def;
        const auto t = jsonValue(d, def, "thickness", expr::Expected(expr::LENGTH));
        if (!t) return;
        const std::string pre = f.id + ".inner(";
        for (int fi : facesWithBase(st, pre)) {
            const std::string& b = st.names.faces[size_t(fi)].base;
            if (b.back() != ')') continue;
            const auto outer = facesWithBase(st, b.substr(pre.size(), b.size() - pre.size() - 1), true);
            if (!outer.empty() && placeParallel(P, {fi}, outer, *t)) return;
        }
        return;
    }
    if (f.type == doc::FeatureType::Draft) {
        P.srcParam = "angle";
        // Draft angle: an edge of the drafted face and an edge of the adjacent face with a common vertex (angle at the face edge) on a view
        // where both are in the view plane; value — 90° ± draft (the angle α to the extrusion direction needs a line of this
        // direction — deferred, §6а.9).
        const doc::Json& def = f.pattern()->def;
        const auto al = jsonValue(d, def, "angle", expr::Expected(expr::ANGLE));
        if (!al || !def.contains("faces")) return;
        const double deg = *al * 180 / std::numbers::pi;
        for (const auto& fn : def["faces"]) {
            const auto rr = naming::resolve(fn.get<std::string>(), naming::EntityKind::Face, st.names);
            if (rr.entities.empty() || (rr.status != naming::ResolveStatus::Exact && rr.status != naming::ResolveStatus::Remapped)) continue;
            const int fi = rr.entities.front().index;
            std::vector<int> own, adj;
            for (int i : edgesOfFace(st, fi))
                if (isLine(i)) own.push_back(i);
            for (int g : adjacentFaces(st, fi))
                for (int i : edgesOfFace(st, g))
                    if (isLine(i) && std::find(own.begin(), own.end(), i) == own.end() && std::find(adj.begin(), adj.end(), i) == adj.end()) adj.push_back(i);
            auto variants = [&](const ViewInfo& vi) {
                std::vector<doc::SheetDimension> v;
                const Ctx c = P.ctx(vi);
                for (int i : own) {
                    auto g1 = edgeGeom(c, i);
                    if (!g1 || g1->kind != RefGeom::Kind::Segment || std::abs(g1->dir3.dot(vi.su.vp.n)) > DIM_PARALLEL_TOL) continue;
                    for (int j : adj) {
                        auto g2 = edgeGeom(c, j);
                        if (!g2 || g2->kind != RefGeom::Kind::Segment || std::abs(g2->dir3.dot(vi.su.vp.n)) > DIM_PARALLEL_TOL) continue;
                        bool corner = false;  // common vertex
                        for (const Point3& x : {g1->p1, g1->p2})
                            for (const Point3& y : {g2->p1, g2->p2}) corner = corner || (x - y).norm() <= MODEL_DIM_MATCH_TOL;
                        if (!corner) continue;
                        const Point2 t1 = (g1->b - g1->a).normalized(), t2 = (g2->b - g2->a).normalized();
                        if (std::abs(cross2(t1, t2)) <= DIM_PARALLEL_TOL) continue;
                        doc::SheetDimension a;
                        a.kind = "angle";
                        a.refs = {edgeRef(st, i), edgeRef(st, j)};
                        for (const auto& [s1, s2] : {std::pair{1.0, 1.0}, {1.0, -1.0}, {-1.0, 1.0}, {-1.0, -1.0}}) {
                            const Point2 u = s1 * t1 + s2 * t2;
                            if (u.norm() <= DIM_EPS) continue;
                            a.angle = geom2::angleOf(u);
                            v.push_back(a);
                        }
                        if (v.size() >= 4 * MODEL_DIM_PAIR_VARIANTS) return v;
                    }
                }
                return v;
            };
            if (P.place(variants, [&](double x) { return nearValue(x, 90 + deg) || nearValue(x, 90 - deg); })) return;
        }
        return;
    }
    if (f.type == doc::FeatureType::Pattern) {
        // Linear pattern spacing (FX3-DRW-004): corresponding circles (otherwise vertices) of the source feature and the first
        // instance along the direction — a linear dimension equal to the spacing; with instance count n ≥ 3 — prefix «(n−1)×».
        const doc::Json& def = f.pattern()->def;
        if (def.value("mode", std::string()) != "linear" || !def.contains("directions") || !def.contains("features")) return;
        const auto& dirs = def["directions"];
        for (size_t j = 0; j < dirs.size() && j < 2; ++j) {
            P.srcParam = "directions." + std::to_string(j) + ".spacing";
            const auto sp = jsonValue(d, dirs[j], "spacing", expr::Expected(expr::LENGTH));
            const auto cnt = jsonValue(d, dirs[j], "count", expr::Expected(expr::DIMLESS));
            if (!sp || !cnt || *cnt < 2) continue;
            const std::string label = dirs.size() == 1 ? "1" : j == 0 ? "1_0" : "0_1";
            bool done = false;
            for (const auto& src : def["features"]) {
                if (done) break;
                const std::string sid = src.get<std::string>();
                std::vector<std::pair<int, int>> pairs;  // (source edge or vertex, instance one): edge ≥ 0, vertex — −1 − index
                for (int fo : facesWithBase(st, sid + ".")) {
                    const std::string& b = st.names.faces[size_t(fo)].base;
                    const auto inst = facesWithBase(st, f.id + ".copy(" + label + "," + b + ")", true);
                    if (inst.empty()) continue;
                    for (int eo : edgesOfFace(st, fo)) {
                        if (!isCircle(eo)) continue;
                        for (int ei : edgesOfFaces(st, inst))
                            if (isCircle(ei) && std::abs(topo.edges[size_t(ei)].ci.radius - topo.edges[size_t(eo)].ci.radius) <= MODEL_DIM_MATCH_TOL)
                                pairs.push_back({eo, ei});
                    }
                    for (size_t vo = 0; vo < st.names.vertices.size() && pairs.empty(); ++vo) {
                        const auto& fv = st.names.vertices[vo].faces;
                        if (std::find(fv.begin(), fv.end(), fo) == fv.end()) continue;
                        for (size_t vi = 0; vi < st.names.vertices.size(); ++vi) {
                            const auto& fw = st.names.vertices[vi].faces;
                            if (std::find(fw.begin(), fw.end(), inst[0]) != fw.end()) pairs.push_back({-1 - int(vo), -1 - int(vi)});
                        }
                    }
                }
                auto ref = [&](int x) {
                    return x >= 0 ? edgeRef(st, x) : doc::DimensionRef{"vertex", st.names.vertices.at(size_t(-1 - x)).full};
                };
                done = P.place(
                    [&](const ViewInfo&) {
                        std::vector<doc::SheetDimension> v;
                        for (const auto& [x, y] : pairs) {
                            if (v.size() >= 4 * MODEL_DIM_PAIR_VARIANTS) break;
                            doc::SheetDimension a;
                            a.kind = "linear";
                            a.orientation = "aligned";
                            a.refs = {ref(x), ref(y)};
                            if (*cnt >= 3) a.prefix = std::to_string(std::llround(*cnt) - 1) + "\xC3\x97";
                            v.push_back(a);
                        }
                        return v;
                    },
                    [&](double x) { return nearValue(x, *sp); });
            }
        }
        return;
    }
    if (f.type == doc::FeatureType::Plane) {
        P.srcParam = "distance";
        // Plane offset (FX3-DRW-004; loft sections, sketches on offset planes): faces in the offset plane and
        // in the original one — a linear dimension equal to the offset.
        const doc::Json& def = f.refGeom()->def;
        const auto it = st.planes.find(f.id);
        if (def.value("mode", std::string()) != "offset" || it == st.planes.end() || !def.contains("ref")) return;
        const auto dist = jsonValue(d, def, "distance", expr::Expected(expr::LENGTH));
        if (!dist || *dist <= MODEL_DIM_VALUE_TOL) return;
        const doc::Json& ref = def["ref"];
        std::optional<Plane> base;
        if (ref.contains("datum")) base = sketch::datumPlane(ref["datum"].get<std::string>());
        else if (ref.contains("feature")) {
            if (const auto b = st.planes.find(ref["feature"].get<std::string>()); b != st.planes.end()) base = b->second;
        } else if (ref.contains("face")) {
            const auto rr = naming::resolve(ref["face"].get<std::string>(), naming::EntityKind::Face, st.names);
            if (!rr.entities.empty()) {
                const auto& fe = st.names.faces.at(size_t(rr.entities.front().index));
                const auto si = k.faceSurface(st.bodies.at(size_t(fe.body)).shape, fe.face);
                if (si.kind == kernel::SurfaceInfo::Kind::Plane) {
                    Plane p;
                    p.origin = si.point;
                    p.normal = si.dir;
                    base = p;
                }
            }
        }
        if (!base) return;
        const auto fa = facesInPlane(st, k, it->second.origin, it->second.normal), fb = facesInPlane(st, k, base->origin, base->normal);
        if (!fa.empty() && !fb.empty()) placeParallel(P, fa, fb, *dist);
        return;
    }
}

// Value expression of sketch dimension id of feature f ("" — none): text of the unplaced report (FX8-DRW-002).
std::string constraintValue(const doc::Feature& f, const std::string& id) {
    if (const sketch::SketchData* sk = f.sketchData())
        for (const auto& c : sk->constraints)
            if (c.id == id && c.value) return *c.value;
    return {};
}

// Part and body dimensions (§6а.10 items 2–3г).
void partDimensions(Placer& P, const doc::Element& el) {
    const regen::PartState& st = P.st;
    const WorldTopo topo = worldTopo(st, P.models.kernel());
    // Sketch and depth candidates — in feature order; a candidate placed nowhere is reported (FX8-DRW-002).
    auto flush = [&](std::vector<Candidate>& cands, const doc::Feature& f) {
        for (auto& cd : cands) {
            const int elsewhere = P.elsewhere;
            const bool placed = P.place(
                [&](const ViewInfo& vi) {
                    std::vector<doc::SheetDimension> v;
                    const Vec3 n = vi.su.vp.n;
                    const bool inPlane = !(cd.normal && 1 - std::abs(cd.normal->normalized().dot(n)) > DIM_PARALLEL_TOL) &&
                                         !(cd.along && std::abs(cd.along->dot(n)) > DIM_PARALLEL_TOL);
                    if (!inPlane) {
                        // The circle seen end-on (FX13-DRW-003): the diameter on the same edge.
                        if (cd.endOnAxis && 1 - std::abs(cd.endOnAxis->dot(n)) <= DIM_PARALLEL_TOL) {
                            doc::SheetDimension a;
                            a.kind = "diameter";
                            a.refs = cd.dim.refs;
                            v.push_back(a);
                        }
                        return v;
                    }
                    std::vector<doc::SheetDimension> alts{cd.dim};
                    alts.insert(alts.end(), cd.alts.begin(), cd.alts.end());
                    for (doc::SheetDimension a : alts) {
                        if (cd.xAxis) {  // distanceX / distanceY — along a sketch axis: horizontal or vertical on the view
                            const Vec3 ax = cd.xAxis->normalized();
                            if (1 - std::abs(ax.dot(vi.su.vp.r)) <= DIM_PARALLEL_TOL) a.orientation = "horizontal";
                            else if (1 - std::abs(ax.dot(vi.su.vp.u)) <= DIM_PARALLEL_TOL) a.orientation = "vertical";
                        }
                        if (cd.silhouetteNormal && cd.silhouetteNormal->dot(n) < 0)  // §6а.2: side is relative to the view
                            for (auto& r : a.refs)
                                if (r.kind == "silhouette") r.side = -r.side;
                        if (a.kind != "angle" || a.refs.size() != 2) {
                            v.push_back(a);
                            continue;
                        }
                        // Angle between edges: the sector in which the value equals the sketch dimension (§6а.10 item 5).
                        const Ctx c = P.ctx(vi);
                        auto g1 = refGeom(c, a.refs[0]), g2 = refGeom(c, a.refs[1]);
                        if (!g1 || !g2 || g1->kind != RefGeom::Kind::Segment || g2->kind != RefGeom::Kind::Segment) continue;
                        const Point2 t1 = (g1->b - g1->a).normalized(), t2 = (g2->b - g2->a).normalized();
                        for (const auto& [s1, s2] : {std::pair{1.0, 1.0}, {1.0, -1.0}, {-1.0, 1.0}, {-1.0, -1.0}}) {
                            const Point2 u = s1 * t1 + s2 * t2;
                            if (u.norm() <= DIM_EPS) continue;
                            a.angle = geom2::angleOf(u);
                            v.push_back(a);
                        }
                    }
                    return v;
                },
                cd.value ? std::function<bool(double)>([w = *cd.value](double value) { return nearValue(value, w); }) : std::function<bool(double)>());
            if (!placed && P.elsewhere == elsewhere) P.report(f, cd.constraint, constraintValue(f, cd.constraint));
        }
        cands.clear();
    };
    const auto vars = regen::documentVariables(P.md);
    const auto units = regen::documentUnits(P.md);
    std::vector<Candidate> cands;
    for (const auto& fp : el.features) {
        const doc::Feature& f = *fp;
        if (f.suppressed) continue;
        const auto se = st.sketches.find(f.id);
        if (const sketch::SketchData* sk = f.sketchData(); sk && se != st.sketches.end()) {
            std::vector<std::string> skipped;
            cands = sketchCandidates(P.md, *sk, se->second, topo, &st, &skipped);
            for (const auto& id : skipped) P.report(f, id, constraintValue(f, id));
            flush(cands, f);
            continue;
        }
        // Extrusion depth "blind" and "symmetric" (§6а.10): a line edge along the sketch normal with length equal
        // to the depth, one endpoint (for symmetric — the middle) in the sketch plane.
        if (const doc::ExtrudeData* x = f.extrude(); x && (x->endType == "blind" || x->endType == "symmetric")) {
            const auto se2 = st.sketches.find(x->sketch);
            if (se2 == st.sketches.end()) continue;
            auto q = expr::evaluateField(x->depth, expr::Expected(expr::LENGTH), vars.env(), units);
            if (!q) continue;
            const double depth = std::abs(q->value);
            const Plane& fr = se2->second.frame;
            const Vec3 N = fr.normal.normalized();
            for (const auto& e : topo.edges) {
                if (e.ci.kind != kernel::CurveInfo::Kind::Line) continue;
                const Vec3 v = e.ci.p2 - e.ci.p1;
                if (std::abs(v.norm() - depth) > MODEL_DIM_MATCH_TOL || v.normalized().cross(N).norm() > DIM_PARALLEL_TOL) continue;
                const double h1 = (e.ci.p1 - fr.origin).dot(N), h2 = (e.ci.p2 - fr.origin).dot(N);
                const bool onPlane = x->endType == "blind" ? std::min(std::abs(h1), std::abs(h2)) <= MODEL_DIM_MATCH_TOL
                                                           : std::abs(h1 + h2) <= MODEL_DIM_MATCH_TOL;
                if (!onPlane) continue;
                Candidate cd;
                cd.along = N;
                cd.dim.kind = "linear";
                cd.dim.orientation = "aligned";
                cd.dim.refs.push_back({"edge", e.name});
                cands.push_back(std::move(cd));
                break;
            }
            if (cands.empty()) P.report(f, {}, x->depth);
            P.srcFeature = f.id;
            P.srcParam = "end.depth";
            flush(cands, f);
            P.srcParam.clear();
            continue;
        }
        const int attempts = P.attempts, successes = P.successes, elsewhere = P.elsewhere;
        featureDimensions(P, f, topo);
        P.srcParam.clear();
        if (P.attempts > attempts && P.successes == successes && P.elsewhere == elsewhere) P.report(f, {}, {});
    }
}

// An assembly state body belongs to a visible instance (§6а.10 item 5а): the top-level instance of the path is not hidden.
bool visibleBody(const regen::PartState& st, int body, const std::set<std::string>& hidden) {
    const std::string& id = st.bodies.at(size_t(body)).id;
    return !hidden.count(id.substr(0, id.find('/')));
}

// Overall reference dimensions (§6а.10 items 5а, 5б): along the world axes in the directions r, u of the model views, from
// the bounding box of the visible bodies; the extreme references — vertex, segment edge, silhouette segment or sphere
// outline, then curves (FX7-DRW-010: curve edge, silhouette of a surface of revolution). part — item 5б: an axis already
// covered by a dimension of the model gets none.
void overallDimensions(Placer& P, const std::function<bool(int)>& visible, bool part) {
    const regen::PartState& st = P.st;
    const kernel::Kernel& k = P.models.kernel();
    BBox3 box;
    for (size_t b = 0; b < st.bodies.size(); ++b)
        if (visible(int(b))) box.extend(k.bbox(st.bodies[b].shape));
    if (box.isEmpty()) return;
    std::array<bool, 3>& done = P.overallDone;
    for (const auto& vi : P.views) {
        const Ctx c = P.ctx(vi);
        for (const Vec3* dir : {&vi.su.vp.r, &vi.su.vp.u}) {
            int ax = -1;
            for (int i = 0; i < 3; ++i)
                if (1 - std::abs(dir->dot(Vec3::Unit(i))) <= DIM_PARALLEL_TOL) ax = i;
            if (ax < 0 || done[size_t(ax)]) continue;
            const Vec3 D = Vec3::Unit(ax);
            const double lo = box.min()[ax], hi = box.max()[ax];
            if (hi - lo <= DIM_EPS) continue;
            if (part && P.covered(D, hi - lo)) {
                done[size_t(ax)] = true;
                continue;
            }
            auto onLevel = [&](const Point3& p, double level) { return std::abs(p.dot(D) - level) <= MODEL_DIM_MATCH_TOL; };
            // Endpoints of the edge segment on the view in the world (§6а.2): a straight edge or an edge-on circle.
            auto segEnds = [&](int i) -> std::optional<std::pair<Point3, Point3>> {
                const auto& e = st.names.edges.at(size_t(i));
                if (!visible(e.body)) return std::nullopt;
                auto g = edgeGeom(c, i);
                if (!g || g->kind != RefGeom::Kind::Segment) return std::nullopt;
                return std::pair{g->p1, g->p2};
            };
            // A curve reaches the level with its extreme point toward it (FX7-DRW-010).
            auto curveAt = [&](const RefGeom& g, double level) {
                auto p = curveExtreme(c, g, level == hi ? D : Vec3(-D));
                return p && onLevel(*p, level);
            };
            // A reference that does not resolve on the view (FX13-DRW-004: an edge of a connection hole of the assembly with
            // a name the view cannot resolve) is skipped — the next entity at the level is taken.
            auto resolves = [&](const doc::DimensionRef& ref) { return bool(refGeom(c, ref)); };
            auto extreme = [&](double level) -> std::optional<doc::DimensionRef> {
                for (const auto& v : st.names.vertices)
                    if (visible(v.body) && onLevel(k.vertexPoint(st.bodies.at(size_t(v.body)).shape, v.vertex), level))
                        if (const doc::DimensionRef ref{"vertex", v.full}; resolves(ref)) return ref;
                for (size_t i = 0; i < st.names.edges.size(); ++i)
                    if (const auto se = segEnds(int(i)); se && onLevel(se->first, level) && onLevel(se->second, level) && resolves(edgeRef(st, int(i))))
                        return edgeRef(st, int(i));
                // Silhouettes (FX3-DRW-004): cylinder or cone generatrix seen from the side at this level; sphere — farthest outline point.
                for (size_t i = 0; i < st.names.faces.size(); ++i) {
                    const auto& fe = st.names.faces[i];
                    if (!visible(fe.body)) continue;
                    for (const int side : {1, -1}) {
                        auto g = faceLineGeom(c, int(i), false, side);
                        if (g && g->kind == RefGeom::Kind::Segment && onLevel(g->p1, level) && onLevel(g->p2, level))
                            if (const doc::DimensionRef ref{"silhouette", fe.full, side}; resolves(ref)) return ref;
                    }
                    const auto si = k.faceSurface(st.bodies.at(size_t(fe.body)).shape, fe.face);
                    if (si.kind == kernel::SurfaceInfo::Kind::Sphere && (onLevel(si.point + si.radius * D, level) || onLevel(si.point - si.radius * D, level)))
                        if (auto g = faceLineGeom(c, int(i), false, 0); g && g->kind == RefGeom::Kind::Circle)
                            if (const doc::DimensionRef ref{"silhouette", fe.full, 0, "far"}; resolves(ref)) return ref;
                }
                // Curves (FX7-DRW-010): curve edges, then silhouettes of surfaces of revolution — the far extreme point.
                for (size_t i = 0; i < st.names.edges.size(); ++i) {
                    if (!visible(st.names.edges[i].body)) continue;
                    auto g = edgeGeom(c, int(i));
                    if (const doc::DimensionRef ref{"edge", st.names.edges[i].full, 0, "far"}; g && g->kind == RefGeom::Kind::Curve && curveAt(*g, level) && resolves(ref))
                        return ref;
                }
                for (size_t i = 0; i < st.names.faces.size(); ++i) {
                    const auto& fe = st.names.faces[i];
                    if (!visible(fe.body)) continue;
                    for (const int side : {1, -1}) {
                        auto g = faceLineGeom(c, int(i), false, side);
                        if (const doc::DimensionRef ref{"silhouette", fe.full, side, "far"}; g && g->kind == RefGeom::Kind::Curve && curveAt(*g, level) && resolves(ref))
                            return ref;
                    }
                }
                return std::nullopt;
            };
            doc::SheetDimension a;
            a.kind = "linear";
            a.orientation = dir == &vi.su.vp.r ? "horizontal" : "vertical";
            a.prefix = "(";
            a.suffix = ")";
            const auto rl = extreme(lo), rh = extreme(hi);
            if (rl && rh) {
                a.refs = {*rl, *rh};
                // Silhouettes of one face on both sides — a diameter (FX7-DRW-010).
                if (rl->kind == "silhouette" && rh->kind == "silhouette" && rl->name == rh->name && rl->side == -rh->side && rl->side != 0)
                    a.prefix = "(\xC3\x98";
            } else {
                for (size_t i = 0; i < st.names.edges.size() && a.refs.empty(); ++i) {
                    const auto se = segEnds(int(i));
                    if (!se || !edgeGeom(c, int(i))->edgeOn) continue;
                    if ((onLevel(se->first, lo) && onLevel(se->second, hi)) || (onLevel(se->first, hi) && onLevel(se->second, lo))) {
                        a.refs = {edgeRef(st, int(i))};
                        a.prefix = "(\xC3\x98";
                    }
                }
            }
            if (P.tryVariant(a, vi, [&](double value) { return nearValue(value, hi - lo); })) done[size_t(ax)] = true;
        }
    }
}

// Assembly dimensions: "distance" and "angle" mates and overall reference dimensions (§6а.10 item 5а, FX2-DRW-001).
void assemblyDimensions(Placer& P, const doc::Element& el) {
    const regen::PartState& st = P.st;
    const kernel::Kernel& k = P.models.kernel();
    std::set<std::string> hidden;
    for (const auto& in : el.instances)
        if (in.hidden) hidden.insert(in.id);
    // Mate reference → dimension reference on the view (§6а.2); angle — edge ⊥ view direction.
    auto dimRefOn = [&](const std::string& name, const ViewInfo& vi, bool angle) -> std::optional<std::pair<doc::DimensionRef, RefGeom>> {
        const auto it = st.names.byName.find(name);
        if (it == st.names.byName.end()) return std::nullopt;
        const Ctx c = P.ctx(vi);
        const Vec3 n = vi.su.vp.n;
        switch (it->second.kind) {
            case naming::EntityKind::Face: {
                const auto& fe = st.names.faces.at(size_t(it->second.index));
                const auto si = k.faceSurface(st.bodies.at(size_t(fe.body)).shape, fe.face);
                const bool plane = si.kind == kernel::SurfaceInfo::Kind::Plane, cyl = si.kind == kernel::SurfaceInfo::Kind::Cylinder;
                if (plane && std::abs(si.dir.normalized().dot(n)) > DIM_PARALLEL_TOL) return std::nullopt;
                if (!plane && !cyl) return std::nullopt;
                for (int i : edgesOfFace(st, it->second.index)) {
                    auto g = edgeGeom(c, i);
                    if (!g) continue;
                    if (plane && g->kind == RefGeom::Kind::Segment && (!angle || std::abs(g->dir3.dot(n)) <= DIM_PARALLEL_TOL))
                        return std::pair{edgeRef(st, i), *g};
                    if (cyl && !angle && g->kind == RefGeom::Kind::Circle) return std::pair{edgeRef(st, i), *g};
                }
                return std::nullopt;
            }
            case naming::EntityKind::Edge: {
                auto g = edgeGeom(c, it->second.index);
                if (!g || (angle && (g->kind != RefGeom::Kind::Segment || std::abs(g->dir3.dot(n)) > DIM_PARALLEL_TOL))) return std::nullopt;
                return std::pair{edgeRef(st, it->second.index), *g};
            }
            case naming::EntityKind::Vertex: {
                if (angle) return std::nullopt;
                RefGeom g;
                return std::pair{doc::DimensionRef{"vertex", st.names.vertices.at(size_t(it->second.index)).full}, g};
            }
        }
        return std::nullopt;
    };
    for (const auto& m : el.mates) {
        if (m.suppressed || (m.type != "distance" && m.type != "angle") || !m.value) continue;
        if (regen::refTopInstance(m.refs[0]).empty() || regen::refTopInstance(m.refs[1]).empty()) continue;  // own geometry (assembly.md §3.1)
        const bool angle = m.type == "angle";
        const auto v = fieldValue(P.md, *m.value, expr::Expected(angle ? expr::ANGLE : expr::LENGTH));
        if (!v) continue;
        const double want = std::abs(angle ? *v * 180 / std::numbers::pi : *v);
        if (want <= MODEL_DIM_VALUE_TOL) continue;
        P.place(
            [&](const ViewInfo& vi) {
                std::vector<doc::SheetDimension> out;
                const auto r1 = dimRefOn(m.refs[0], vi, angle), r2 = dimRefOn(m.refs[1], vi, angle);
                if (!r1 || !r2) return out;
                doc::SheetDimension a;
                a.refs = {r1->first, r2->first};
                if (!angle) {
                    a.kind = "linear";
                    a.orientation = "aligned";
                    out.push_back(a);
                    return out;
                }
                a.kind = "angle";
                const Point2 t1 = (r1->second.b - r1->second.a).normalized(), t2 = (r2->second.b - r2->second.a).normalized();
                for (const auto& [s1, s2] : {std::pair{1.0, 1.0}, {1.0, -1.0}, {-1.0, 1.0}, {-1.0, -1.0}}) {
                    const Point2 u = s1 * t1 + s2 * t2;
                    if (u.norm() <= DIM_EPS) continue;
                    a.angle = geom2::angleOf(u);
                    out.push_back(a);
                }
                return out;
            },
            [&](double value) { return nearValue(value, want); });
    }
    overallDimensions(P, [&](int body) { return visibleBody(st, body, hidden); }, false);
}

// Row layout of the command's linear dimensions (§6а.10 item 7, GOST 2.307): on each view, dimensions of one offset
// direction — in ascending order of length into the nearest row without overlaps (chains — in one row, dimensions from a
// common datum — in outer rows); row r — at MODEL_DIM_OFFSET_MM + MODEL_DIM_STEP_MM·r from the view outline.
void layoutRows(const doc::Document& d, const doc::Element* drawingEl, const doc::Sheet& sheet, const ModelProvider& models,
                std::vector<doc::SheetDimension>& dims) {
    struct Item {
        size_t index;
        DimensionPreview::Layout lay;
    };
    std::map<std::pair<std::string, long long>, std::vector<Item>> groups;
    for (size_t i = 0; i < dims.size(); ++i) {
        if (dims[i].kind != "linear") continue;
        bool viewError = false;
        auto r = build(d, drawingEl, sheet, dims[i], std::nullopt, models, viewError);
        if (!r || !r->layout.valid || dims[i].offset < 0) continue;
        groups[{dims[i].view, std::llround(geom2::angleOf(r->layout.m) * 1e6)}].push_back({i, r->layout});
    }
    std::map<std::string, std::vector<ViewEntity>> entities;
    for (auto& [key, items] : groups) {
        const Point2 m = items.front().lay.m, dref(m.y(), -m.x());
        auto ent = entities.find(key.first);
        if (ent == entities.end()) {
            auto r = drawing::viewEntities(d, *drawingEl, sheet, key.first, models);
            ent = entities.emplace(key.first, r ? std::move(*r) : std::vector<ViewEntity>()).first;
        }
        double contour = -1e300;  // view edge along m — by the named view geometry (§6а.7)
        for (const auto& e : ent->second) {
            if (e.kind == ViewEntity::Kind::Point) {
                contour = std::max(contour, e.a.dot(m));
            } else if (e.kind == ViewEntity::Kind::Line) {
                contour = std::max({contour, e.a.dot(m), e.b.dot(m)});
            } else if (e.kind == ViewEntity::Kind::Curve) {
                for (const auto& pl : e.polylines)
                    for (const auto& q : pl) contour = std::max(contour, q.dot(m));
            } else {
                contour = std::max(contour, e.center.dot(m) + e.radius);
            }
        }
        for (const auto& it : items) contour = std::max(contour, it.lay.hmax);
        for (auto& it : items)
            if (it.lay.d.dot(dref) < 0) it.lay.lo = -std::exchange(it.lay.hi, -it.lay.lo);
        std::vector<size_t> order(items.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return items[a].lay.hi - items[a].lay.lo < items[b].lay.hi - items[b].lay.lo - 1e-9;
        });
        std::vector<std::vector<std::pair<double, double>>> rows;
        for (const size_t oi : order) {
            const Item& it = items[oi];
            // Overlap — intersection of extents greater than DIM_EPS: dimensions of a chain touch at a common extension line.
            const double lo = it.lay.lo, hi = it.lay.hi;
            size_t row = 0;
            for (; row < rows.size(); ++row)
                if (std::none_of(rows[row].begin(), rows[row].end(),
                                 [&](const auto& x) { return lo < x.second - DIM_EPS && x.first < hi - DIM_EPS; }))
                    break;
            if (row == rows.size()) rows.emplace_back();
            rows[row].push_back({lo, hi});
            dims[it.index].offset = contour + MODEL_DIM_OFFSET_MM + MODEL_DIM_STEP_MM * double(row) - it.lay.hmax;
        }
    }
}

// Footprint of a built dimension on the sheet (§6а.10 item 7а, FX13-DRW-002): text boxes (oriented rectangles) and
// dimension lines (line primitives parallel to a text of the dimension — extension lines are across it).
struct Footprint {
    std::vector<std::array<Point2, 4>> texts;
    std::vector<std::pair<Point2, Point2>> lines;
};

Footprint footprint(const DimensionPreview& r) {
    Footprint f;
    std::vector<Point2> dirs;
    for (const auto& p : r.primitives) {
        if (p.kind != PrimitiveKind::Text || p.text.empty()) continue;
        const double h = p.height;
        auto w = textWidth(p.text, h, p.font, p.styles);
        const double width = w ? *w : 0.6 * h * double(p.text.size());
        const double x0 = p.align.find("center") != std::string::npos ? -width / 2 : p.align.find("right") != std::string::npos ? -width : 0;
        const double y0 = p.align.find("middle") != std::string::npos ? -h / 2 : p.align.find("top") != std::string::npos ? -h : 0;
        const Point2 ex(std::cos(p.angle), std::sin(p.angle)), ey(-ex.y(), ex.x());
        auto at = [&](double x, double y) { return Point2(p.position + x * ex + y * ey); };
        f.texts.push_back({at(x0, y0), at(x0 + width, y0), at(x0 + width, y0 + h), at(x0, y0 + h)});
        dirs.push_back(ex);
    }
    for (const auto& p : r.primitives) {
        if (p.kind != PrimitiveKind::Line || p.points.size() != 2) continue;
        const Point2 v = p.points[1] - p.points[0];
        if (v.norm() <= MODEL_DIM_OVERLAP_TOL) continue;
        const Point2 t = v.normalized();
        if (std::any_of(dirs.begin(), dirs.end(), [&](const Point2& e) { return std::abs(cross2(t, e)) <= DIM_PARALLEL_TOL; }))
            f.lines.push_back({p.points[0], p.points[1]});
    }
    return f;
}

Rect primitivesBox(const std::vector<Primitive>& prims) {
    Rect b;
    DimensionPreview r;
    r.primitives = prims;
    for (const auto& t : footprint(r).texts)
        for (const auto& q : t) b.add(q);
    for (const auto& p : prims) {
        if (p.kind == PrimitiveKind::Text) continue;
        if (p.kind == PrimitiveKind::Arc || p.kind == PrimitiveKind::Circle) {
            b.add(p.center - Point2(p.radius, p.radius));
            b.add(p.center + Point2(p.radius, p.radius));
            continue;
        }
        for (const auto& q : p.points) b.add(q);
    }
    return b;
}

// Oriented rectangles overlap by more than the tolerance (separating axes).
bool boxesOverlap(const std::array<Point2, 4>& a, const std::array<Point2, 4>& b) {
    for (const auto* q : {&a, &b})
        for (size_t i = 0; i < 2; ++i) {
            const Point2 e = (*q)[i + 1] - (*q)[i];
            if (e.norm() <= DIM_EPS) continue;
            const Point2 n(-e.y() / e.norm(), e.x() / e.norm());
            double alo = 1e300, ahi = -1e300, blo = 1e300, bhi = -1e300;
            for (const auto& p : a) alo = std::min(alo, p.dot(n)), ahi = std::max(ahi, p.dot(n));
            for (const auto& p : b) blo = std::min(blo, p.dot(n)), bhi = std::max(bhi, p.dot(n));
            if (ahi - blo <= MODEL_DIM_OVERLAP_TOL || bhi - alo <= MODEL_DIM_OVERLAP_TOL) return false;
        }
    return true;
}

// Collinear segments sharing a stretch longer than the tolerance (dimension lines on one line; a chain only touches).
bool linesOverlap(const std::pair<Point2, Point2>& a, const std::pair<Point2, Point2>& b) {
    const Point2 t = (a.second - a.first).normalized();
    const Point2 u = b.second - b.first;
    if (std::abs(cross2(t, u.normalized())) > DIM_PARALLEL_TOL) return false;
    if (std::abs(cross2(t, b.first - a.first)) > MODEL_DIM_OVERLAP_TOL) return false;
    const double a1 = 0, a2 = (a.second - a.first).dot(t);
    const double b1 = (b.first - a.first).dot(t), b2 = (b.second - a.first).dot(t);
    return std::min(a2, std::max(b1, b2)) - std::max(a1, std::min(b1, b2)) > MODEL_DIM_OVERLAP_TOL;
}

int overlaps(const Footprint& f, const std::vector<Footprint>& others) {
    int n = 0;
    for (const auto& o : others) {
        bool hit = false;
        for (const auto& a : f.texts)
            for (const auto& b : o.texts) hit = hit || boxesOverlap(a, b);
        for (const auto& a : f.lines)
            for (const auto& b : o.lines) hit = hit || linesOverlap(a, b);
        n += hit ? 1 : 0;
    }
    return n;
}

// Spreading (§6а.10 item 7а, FX13-DRW-002): a dimension of the command whose text box or dimension line overlaps one
// of the dimensions already on its view (on the sheet or placed before it) is moved — linear and angular between edges
// one more row out, a diameter turned by MODEL_DIM_SPREAD_ANGLE, then further out; the first position free of overlaps,
// otherwise the one with the fewest (short of room the dimensions at least do not coincide).
void spreadOverlaps(const doc::Document& d, const doc::Element* drawingEl, const doc::Sheet& sheet, const ModelProvider& models,
                    std::vector<doc::SheetDimension>& dims) {
    std::map<std::string, std::vector<Footprint>> taken;  // view → footprints
    auto shape = [&](const doc::SheetDimension& a) -> std::optional<Footprint> {
        bool viewError = false;
        auto r = build(d, drawingEl, sheet, a, std::nullopt, models, viewError);
        if (!r) return std::nullopt;
        return footprint(*r);
    };
    for (const auto& a : sheet.dimensions)
        if (auto f = shape(a)) taken[a.view].push_back(std::move(*f));
    for (auto& a : dims) {
        auto& occupied = taken[a.view];
        auto f = shape(a);
        if (!f) continue;
        int best = overlaps(*f, occupied);
        if (best > 0) {
            doc::SheetDimension chosen = a;
            Footprint chosenShape = *f;
            const double angle0 = a.angle;
            const int turns = a.kind == "diameter" ? int(std::lround(2 * std::numbers::pi / MODEL_DIM_SPREAD_ANGLE)) - 1 : 0;
            for (int k = 1; k <= MODEL_DIM_SPREAD_TRIES && best > 0; ++k) {
                doc::SheetDimension v = a;
                if (k <= turns) {
                    v.angle = angle0 + k * MODEL_DIM_SPREAD_ANGLE;
                } else {
                    const int out = k - turns;
                    v.offset = a.offset + (a.offset < 0 ? -1 : 1) * MODEL_DIM_STEP_MM * out;
                }
                auto g = shape(v);
                if (!g) continue;
                const int n = overlaps(*g, occupied);
                if (n < best) {
                    best = n;
                    chosen = v;
                    chosenShape = std::move(*g);
                }
            }
            a = std::move(chosen);
            *f = std::move(chosenShape);
        }
        occupied.push_back(std::move(*f));
    }
}

// Room on the sheet (item 5в): the field inside the frame (GOST 2.301: 20 mm on the left, 5 mm elsewhere), the title
// block of form 1 (185 × 55 at the lower right corner) and the boxes of the built views.
Room sheetRoom(const doc::Document& d, const doc::Element& drawingEl, const doc::Sheet& sheet, const ModelProvider& models) {
    Room r;
    if (const auto sz = sheetSize(drawingEl, sheet)) {
        r.field.add({MODEL_DIM_FIELD_LEFT_MM, MODEL_DIM_FIELD_MM});
        r.field.add({sz->width - MODEL_DIM_FIELD_MM, sz->height - MODEL_DIM_FIELD_MM});
        r.titleBlock.add({sz->width - MODEL_DIM_FIELD_MM - MODEL_DIM_TITLE_W_MM, MODEL_DIM_FIELD_MM});
        r.titleBlock.add({sz->width - MODEL_DIM_FIELD_MM, MODEL_DIM_FIELD_MM + MODEL_DIM_TITLE_H_MM});
    }
    for (const auto& v : sheet.views) {
        auto ents = viewEntities(d, drawingEl, sheet, v.id, models);
        if (!ents) continue;
        Rect b;
        for (const auto& e : *ents) {
            if (e.kind == ViewEntity::Kind::Point) b.add(e.a);
            else if (e.kind == ViewEntity::Kind::Line) b.add(e.a), b.add(e.b);
            else if (e.kind == ViewEntity::Kind::Curve) {
                for (const auto& pl : e.polylines)
                    for (const auto& q : pl) b.add(q);
            } else {
                b.add(e.center - Point2(e.radius, e.radius));
                b.add(e.center + Point2(e.radius, e.radius));
            }
        }
        if (!b.empty()) r.views[v.id] = b;
    }
    return r;
}

}  // namespace

namespace dimdetail {

SketchFits sketchFits(const doc::Document& md, const doc::Element& el, const regen::PartState& st, const kernel::Kernel& k) {
    SketchFits out;
    std::map<std::pair<std::string, std::string>, const doc::Json*> fits;  // (sketch, dimension) → designation
    for (const auto& a : el.annotations)
        if (a.value("type", std::string()) == "fit" && a.contains("dimension"))
            fits[{a["dimension"].value("sketch", std::string()), a["dimension"].value("constraint", std::string())}] = &a;
    if (fits.empty()) return out;
    const WorldTopo topo = worldTopo(st, k);
    for (const auto& fp : el.features) {
        const sketch::SketchData* sk = fp->sketchData();
        const auto se = st.sketches.find(fp->id);
        if (!sk || fp->suppressed || se == st.sketches.end()) continue;
        bool any = false;
        for (const auto& [key, fit] : fits) any = any || key.first == fp->id;
        if (!any) continue;
        for (auto& cd : sketchCandidates(md, *sk, se->second, topo)) {
            const auto it = fits.find({fp->id, cd.constraint});
            if (it == fits.end()) continue;
            SketchFit f;
            f.fit = it->second;
            f.forms.push_back(cd.dim);
            for (auto& alt : cd.alts) f.forms.push_back(alt);
            out.push_back(std::move(f));
        }
    }
    return out;
}

}  // namespace dimdetail

std::vector<doc::SheetDimension> modelDimensions(const doc::Document& d, const doc::Element& drawingEl, const doc::Sheet& sheet,
                                                 const ModelProvider& models, const std::set<std::string>* onlyViews,
                                                 std::vector<UnplacedDimension>* unplaced) {
    std::vector<doc::SheetDimension> out;
    std::map<std::string, int> placedOnView;  // view → number of dimensions placed by the command (offset step)
    std::set<std::string> doneModels;
    std::optional<Room> room;  // item 5в — computed once, on the first model with views
    for (const auto& bv : sheet.views) {
        const std::string key = bv.refFile + "#" + bv.refElement;
        // Flat-pattern and exploded views — no model dimensions (§6.21, §6.24).
        if (bv.kind != "base" || bv.flat || bv.exploded || doneModels.count(key)) continue;
        doneModels.insert(key);
        // Model of another file (FX3-DRW-004) — via the linked file's document.
        const doc::Document* mdp = bv.refFile.empty() ? &d : models.linkedDocument(bv.refFile);
        if (!mdp) continue;
        const doc::Document& md = *mdp;
        const int ei = md.elementIndex(bv.refElement);
        if (ei < 0) continue;
        const doc::Element& el = md.elements[size_t(ei)];
        if (el.isDrawing() || el.isSpecification()) continue;
        auto stp = models.model(bv.refFile, bv.refElement);
        if (!stp || !*stp) continue;
        const regen::PartState& st = **stp;
        // Views of this model where dimensions can be placed: main and projected chains, in sheet order. With a view
        // filter (FX7-DRW-009) — the filtered views of every chain of this model.
        std::vector<ViewInfo> views, allViews;
        for (const auto& v : sheet.views) {
            if (v.kind != "base" && v.kind != "projected") continue;
            auto t = viewTriad(sheet, v.id);
            if (!t || !t->base || t->base->flat || t->base->exploded || t->base->refFile != bv.refFile || t->base->refElement != bv.refElement) continue;
            bool viewError = false;
            auto su = setup(sheet, v.id, models, viewError);
            if (!su) continue;
            allViews.push_back({&v, *su});
            if (onlyViews ? onlyViews->count(v.id) > 0 : t->base == &bv) views.push_back({&v, *su});
        }
        // FX13-DRW-003 (item 5в): the selected views first, then the other views of the model in sheet order; without a
        // selection — the main view first.
        size_t preferred = 0;
        if (onlyViews) {
            preferred = views.size();
            for (const auto& vi : allViews)
                if (!onlyViews->count(vi.v->id)) views.push_back(vi);
        } else {
            // Two-pass stable partition: std::stable_partition needs an over-aligned temporary buffer for this
            // Eigen-holding struct, which MSVC rejects (C2338, extended aligned_storage).
            std::vector<ViewInfo> first, rest;
            for (auto& vi : views) (vi.v->kind == "base" ? first : rest).push_back(std::move(vi));
            first.insert(first.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
            views = std::move(first);
        }
        // Drawing dimensions on views of the same model — on all sheets (item 6: each dimension once per whole drawing).
        // Overall dimension (reference linear "(…)" horizontal or vertical on the view) marks its world axis (item 5а).
        std::vector<doc::SheetDimension> existing;
        std::vector<const doc::Sheet*> existingSheets;
        std::array<bool, 3> overallDone{false, false, false};
        auto collect = [&](const doc::Sheet& s) {
            for (const auto& a : s.dimensions) {
                auto t = viewTriad(s, a.view);
                if (!t || !t->base || t->base->flat || t->base->exploded || t->base->refFile != bv.refFile || t->base->refElement != bv.refElement) continue;
                existing.push_back(a);
                existingSheets.push_back(&s);
                if (a.kind != "linear" || !a.prefix || a.prefix->rfind("(", 0) != 0) continue;
                const Vec3* dir = a.orientation == "horizontal" ? &t->r : a.orientation == "vertical" ? &t->u : nullptr;
                for (int i = 0; dir && i < 3; ++i)
                    if (1 - std::abs(dir->dot(Vec3::Unit(i))) <= DIM_PARALLEL_TOL) overallDone[size_t(i)] = true;
            }
        };
        bool sheetInDrawing = false;
        for (const auto& s : drawingEl.sheets) {
            if (s.id == sheet.id) {
                sheetInDrawing = true;
                collect(sheet);
            } else {
                collect(s);
            }
        }
        if (!sheetInDrawing) collect(sheet);
        if (views.empty()) continue;
        Placer P{d, md, &drawingEl, sheet, models, st, views, allViews, existing, out, placedOnView, overallDone};
        P.unplaced = unplaced;
        P.preferred = preferred;
        if (!room) room = sheetRoom(d, drawingEl, sheet, models);
        P.room = &*room;
        if (el.isAssembly()) {
            assemblyDimensions(P, el);
        } else {
            P.noting = true;
            for (size_t i = 0; i < existing.size(); ++i) {  // dimensions of the model already on the drawing (item 5б)
                bool viewError = false;
                if (auto r = build(d, &drawingEl, *existingSheets[i], existing[i], std::nullopt, models, viewError)) P.note(*existingSheets[i], existing[i], *r);
            }
            partDimensions(P, el);
            overallDimensions(P, [](int) { return true; }, true);  // item 5б (FX7-DRW-010)
        }
    }
    layoutRows(d, &drawingEl, sheet, models, out);
    spreadOverlaps(d, &drawingEl, sheet, models, out);  // FX13-DRW-002
    return out;
}


// Room of a built sheet for marks (§6.18 «Выбранный вид и место», FX14-DRW-005): the boxes of item 5в.
namespace {
SheetBox toBox(const Rect& r) { return r.empty() ? SheetBox{} : SheetBox{r.x0, r.y0, r.x1, r.y1}; }
Rect toRect(const SheetBox& b) {
    Rect r;
    if (!b.empty()) r = Rect{b.x0, b.y0, b.x1, b.y1};
    return r;
}
}  // namespace

SheetBox primitivesExtent(const std::vector<Primitive>& prims) { return toBox(primitivesBox(prims)); }

MarkRoom markRoom(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models) {
    MarkRoom out;
    out.graphics = composeSheet(d, drawing, sheet, models);
    const Room room = sheetRoom(d, drawing, sheet, models);
    out.field = toBox(room.field);
    if (!room.titleBlock.empty()) out.boxes.push_back({std::string(), toBox(room.titleBlock)});
    for (const auto& [id, b] : room.views) out.boxes.push_back({id, toBox(b)});
    // Dimensions — by their roles «dim:<id>»; marks — by the roles of their placements.
    std::map<std::string, std::vector<Primitive>> byRole;
    for (const auto& p : out.graphics.primitives)
        if (p.role.rfind("dim:", 0) == 0) byRole[p.role].push_back(p);
    for (const auto& [role, prims] : byRole) out.boxes.push_back({std::string(), primitivesExtent(prims)});
    for (const auto& m : out.graphics.marks) {
        std::vector<Primitive> prims;
        for (const auto& p : out.graphics.primitives)
            if (std::find(m.roles.begin(), m.roles.end(), p.role) != m.roles.end()) prims.push_back(p);
        if (!prims.empty()) out.boxes.push_back({std::string(), primitivesExtent(prims)});
    }
    return out;
}

int markInterference(const MarkRoom& room, const SheetBox& box, const std::string& view, const std::vector<SheetBox>& extra) {
    const Rect b = toRect(box);
    if (b.empty()) return 0;
    int n = 0;
    const Rect field = toRect(room.field);
    if (!field.empty() && !b.inside(field)) ++n;
    for (const auto& o : room.boxes)
        if ((o.view.empty() || o.view != view) && b.meets(toRect(o.box))) ++n;
    for (const auto& e : extra)
        if (b.meets(toRect(e))) ++n;
    return n;
}

}  // namespace construe::drawing
