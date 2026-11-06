// Internal recognition helpers (spec/reconstruction.md §2.1).
#pragma once

#include "rec/recognize.h"
#include "base/constants.h"
#include "sketch/sketch.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace construe::rec::detail {

// Body topology and geometry, computed once.
struct Topo {
    const kernel::Kernel& k;
    kernel::Shape s;
    kernel::CanonicalTopology ct;
    std::vector<kernel::SurfaceInfo> surf;
    std::vector<std::vector<int>> faceLE;  // logical edges of the face (ascending)
    std::vector<kernel::CurveInfo> curve;  // per logical edge
    std::vector<bool> seam;                // the face has a seam

    Topo(const kernel::Kernel& kk, const kernel::Shape& sh);
    int faces() const { return int(surf.size()); }
    int other(int le, int f) const {
        const auto [a, b] = ct.logicalEdges[size_t(le)].faces;
        return a == f ? b : a;
    }
    Point3 edgePoint(int le) const;  // interior point of a logical edge (§2.1)
    bool smooth(int le) const;       // smooth edge (§2.1)
    EdgeKey key(int le) const;
    // Spline wall along d (§2.1, S4A-SPL-004): a free-form face (Other) whose normal is ⊥ d at its sample points.
    bool ruledAlong(int f, const Vec3& d) const;
};

inline bool parallel(const Vec3& a, const Vec3& b, double tol = REC_ANGLE_TOL) { return a.normalized().cross(b.normalized()).norm() <= tol; }
inline bool perpendicular(const Vec3& a, const Vec3& b, double tol = REC_ANGLE_TOL) { return std::abs(a.normalized().dot(b.normalized())) <= tol; }
inline double distToLine(const Point3& p, const Point3& o, const Vec3& a) { return (p - o).cross(a.normalized()).norm(); }
// Own tolerance of the body being recognized (largest entity tolerance, §2.1): set by recognize for its thread.
double& bodyTolerance();
// Axis distance tolerance (§2.1): REC_LIN_TOL·10, or REC_AXIS_TOL_FACTOR × the body's own tolerance if larger (a coarse
// STEP: axes of surfaces of one revolution differ by up to about that tolerance).
inline double axisTol() { return std::max(REC_LIN_TOL * 10, REC_AXIS_TOL_FACTOR * bodyTolerance()); }
// Radius tolerance of pieces of one surface (§4.4.1 item 2): REC_LIN_TOL, or the body's own tolerance if larger.
inline double radiusTol() { return std::max(REC_LIN_TOL, bodyTolerance()); }
inline bool coaxial(const Point3& o1, const Vec3& a1, const Point3& o2, const Vec3& a2) {
    return parallel(a1, a2) && distToLine(o2, o1, a1) <= axisTol();
}
// A cone on the axis (o, a) (§2.1): its apex comes from the half angle and the reference circle of the file, so its noise is
// many times the file's own: REC_CONE_AXIS_TOL for the apex, REC_CONE_ANGLE_TOL for the direction.
inline bool coneOnAxis(const Point3& o, const Vec3& a, const Point3& apex, const Vec3& dir) {
    return a.normalized().cross(dir.normalized()).norm() <= REC_CONE_ANGLE_TOL && distToLine(apex, o, a) <= REC_CONE_AXIS_TOL;
}
// Axis of the face's surface of revolution (cylinder, cone, torus; for a sphere, the center).
inline bool revolutionAxis(const kernel::SurfaceInfo& si, Point3& o, Vec3& a) {
    using K = kernel::SurfaceInfo::Kind;
    if (si.kind == K::Cylinder || si.kind == K::Cone || si.kind == K::Torus || (si.kind == K::Other && si.otherKind == "revolution")) {
        o = si.point;
        a = si.dir.normalized();
        return true;
    }
    return false;
}
// Pieces of one surface (a face split into halves in another program's file, §4.4.1 item 2): one kind; planes are one
// plane, cylinders and cones are coaxial with equal radius, angle, convexity (and apex for cones), tori — the same
// center, axis, radii and convexity, spheres — the same center and radius.
inline bool samePiece(const kernel::SurfaceInfo& a, const kernel::SurfaceInfo& b) {
    using K = kernel::SurfaceInfo::Kind;
    if (a.kind != b.kind) return false;
    if (a.kind == K::Plane) return parallel(a.dir, b.dir) && std::abs((b.point - a.point).dot(a.dir.normalized())) <= axisTol();
    if (a.kind == K::Cylinder || a.kind == K::Cone)
        return coaxial(a.point, a.dir, b.point, b.dir) && std::abs(b.radius - a.radius) <= radiusTol() &&
               std::abs(b.halfAngle - a.halfAngle) <= 1e-9 && b.convex == a.convex &&
               (a.kind != K::Cone || (b.point - a.point).norm() <= axisTol());
    if (a.kind == K::Torus)
        return parallel(a.dir, b.dir) && (b.point - a.point).norm() <= axisTol() &&
               std::abs(b.radius - a.radius) <= radiusTol() &&
               std::abs(b.minorRadius - a.minorRadius) <= radiusTol() && b.convex == a.convex;
    if (a.kind == K::Sphere)
        return (b.point - a.point).norm() <= axisTol() && std::abs(b.radius - a.radius) <= radiusTol();
    return false;
}
// Concave logical edge (§4.6 item 2).
bool concaveEdge(const Topo& t, int le);
// Plane with a basis from normal n and origin o (deterministic choice of the x axis, §5.3 "frame").
Plane planeFrom(const Point3& o, const Vec3& n);
// Ends of a curve with ends (line, arc, spline); empty for a point or circle.
inline std::vector<Point2> curveEnds(const kernel::Curve2& c) {
    using C2 = kernel::Curve2::Kind;
    if (c.kind == C2::Line || c.kind == C2::Spline) return {c.p1, c.p2};
    if (c.kind == C2::Arc) return {c.start, c.end};
    return {};
}
// Match of section curves (ends, center, radius; spline — degree, poles, weights and knot spacing in either direction)
// within tolerance tol.
bool sameCurve(const kernel::Curve2& a, const kernel::Curve2& b, double tol);

// Work limits of one recognition (§3, S4-REC-023), per thread, set by recognize: attempts of face removal and hole filling
// spent (at most REC_MAX_REMOVALS) and the cancellation flag of the caller.
struct Budget {
    int removals = 0;
    bool exhausted = false;
    const Cancel* cancel = nullptr;
};
Budget& budget();
// The next removal attempt is allowed (counts it); false — the budget is spent or recognition is cancelled.
bool spendRemoval();
inline bool stopped() { return budget().exhausted || cancelled(budget().cancel); }
// Face removal with a check (§4.1): a single solid, volume > 0 and changed; counts against the budget.
std::optional<kernel::OpResult> removeChecked(const kernel::Kernel& k, const kernel::Shape& s, const std::vector<int>& faces);
// Section difference (§4.6 item 4): boundary of the region between section a of the larger body and section b of the
// smaller one (curves of each, split at the other's ends, whose middle is not on the other), glued (mergePieces).
std::vector<kernel::Curve2> sectionDifference(const std::vector<kernel::Curve2>& a, const std::vector<kernel::Curve2>& b);
// Gluing pieces of one section line or circle (§4.5).
std::vector<kernel::Curve2> mergePieces(std::vector<kernel::Curve2> cs);

// Base (rec_base.cpp, §4.5): base operations; empty means not decomposed. simple: only probes 1–2 (single extrusion,
// revolution), otherwise only probes 3–4 (layers, the "not decomposed" note).
std::vector<Op> decomposeBase(const kernel::Kernel& k, const kernel::Shape& core, std::string& kind, std::vector<Note>& notes, bool simple);
// The body after an extrusion or revolution of the plan built by the kernel (§4.10 item 4): tools, not features; nullopt — the
// tool was not built or the operation is of another kind.
std::optional<kernel::Shape> applyOp(const kernel::Kernel& k, const kernel::Shape& body, const Op& op);
// Revolution (§4.5 item 2); nullopt means the body is not a body of revolution.
std::optional<Op> revolveBase(const kernel::Kernel& k, const kernel::Shape& s);
// Half profile of the section of s by plane p through the axis (p.y — the axis, §4.5 item 2): curves in x ≥ 0 closed
// by segments along the axis; nullopt — no section or an odd number of points on the axis.
std::optional<std::vector<kernel::Curve2>> halfProfile(const kernel::Kernel& k, const kernel::Shape& s, const Plane& p);
// Kernel profile of closed loops of segments, arcs and circles in the plane's local coordinates: the loop with the largest
// bounding box is the outer one, the others are holes; nullopt — a chain is not closed or there are other curves.
std::optional<kernel::Profile> profileFromCurves(const Plane& p, const std::vector<kernel::Curve2>& curves);
// The solid of a full revolution of a closed profile (one loop of segments, arcs or a circle in the plane's local
// coordinates) about the axis (origin, dir); nullopt — not one closed loop or the kernel failed.
std::optional<kernel::Shape> revolveSolid(const kernel::Kernel& k, const Plane& p, const std::vector<kernel::Curve2>& curves,
                                          const Point3& origin, const Vec3& dir);

// Cuts on bodies of revolution (rec_cuts.cpp, §4.6): the body of revolution after cut removal and the cuts in removal
// order (each with the body before its removal).
struct RevolveCuts {
    kernel::Shape restored;
    Op revolve;
    std::vector<std::pair<Op, kernel::Shape>> cuts;
    Point3 axisOrigin = Point3::Zero();  // main axis (§4.6 item 1)
    Vec3 axisDir = Vec3::UnitY();
};
std::optional<RevolveCuts> removeRevolveCuts(const kernel::Kernel& k, const kernel::Shape& core, std::vector<Note>& notes);
// The same by a section (§4.6а, S4-REC-023), when face removal does not free the body: the profile is the section by a
// half-plane through the main axis that meets no cut, the cuts are the solids of the difference of the body of
// revolution and the core.
std::optional<RevolveCuts> revolveCutsBySection(const kernel::Kernel& k, const kernel::Shape& core, std::vector<Note>& notes);
// Patterns and mirrors of cuts on a body of revolution (§4.6 item 5): blocks in construction order (reverse removal
// order) — a single cut, or a seed cut with Pattern circular / Mirror (target 0) — each with the body after the block.
std::vector<std::pair<std::vector<Op>, kernel::Shape>> groupRevolveCuts(const RevolveCuts& rc);

// Keys of logical edges, connected pieces of one curve merged (§4.2 item 3).
std::vector<EdgeKey> edgeKeys(const Topo& t, const std::vector<int>& les);

// A finishing face of one removal step (§4.2, §4.3): its size (fillet radius, chamfer distance), the two faces it is
// tangent to or lies between (none for a corner sphere) and its edges to them (rails, sides).
struct BlendFace {
    int face = 0;
    double size = 0;
    int g1 = -1, g2 = -1;
    int rail1 = -1, rail2 = -1;
};
// Fillet faces that can be removed now (§4.2 items 1–2): cylinders, tori and corner spheres, chains of them included; a
// fillet another one runs along is removed after it.
std::vector<BlendFace> blendFaces(const Topo& t);
// Own removal by filling (rec_blend.cpp, §4.2а, FX10-REC-006): the body with the corners of the faces restored — the
// corner triangle of each face extruded or revolved, united with the body (convex) or cut from it (concave); nullopt —
// a fill was not built or the result is not one solid.
// expected: the volume change the corners give (NaN — unknown: a circle trace, a corner sphere).
std::optional<kernel::Shape> fillBlends(const Topo& t, const std::vector<BlendFace>& faces, double* expected = nullptr);
// Size of a symmetric chamfer from its corner (§4.2а item 4): nullopt — not symmetric or not a chamfer of two planes or a
// cylinder and a plane.
std::optional<double> chamferSize(const Topo& t, const BlendFace& b);
// A step removed by filling (§4.2а): the sharp body and the operation, accepted only when the kernel's fillet (chamfer)
// of the new edges gives the body back (§6); counts against the removal budget.
// chains: by connected chains of the faces (§4.2а item 6) instead of all at once.
std::optional<std::pair<kernel::Shape, Op>> sharpenStep(const Topo& t, const std::vector<BlendFace>& faces, bool chamfer, double size, bool chains = false);
// One group of removed finishing faces (§4.2, §4.3).
struct Step {
    std::vector<Op> ops;   // in construction order within the step
    kernel::Shape before;  // body before the step is removed (= after its operations are built)
};
// The obvious removals from a body (§4.1): lettering, fillets, chamfers, holes, a loft; core — what remains.
struct Peel {
    kernel::Shape core;
    std::vector<std::pair<Op, kernel::Shape>> texts;  // operation and the body before its removal
    std::vector<Step> steps;
    std::vector<Op> holes;
    kernel::Shape beforeHoles;
    std::optional<std::pair<Op, kernel::Shape>> loft;
};
Peel peelBody(const kernel::Kernel& k, const kernel::Shape& body, std::vector<Note>& notes);
// A core decomposed into a base (§4.5) with cuts on a body of revolution (§4.6): core — the body after the base; base
// empty and kind "none" — not decomposed; layered — the base is several layers (§4.5 item 3).
struct CoreSplit {
    kernel::Shape core;
    std::vector<Op> base;
    std::string kind = "none";
    std::vector<std::pair<std::vector<Op>, kernel::Shape>> cutBlocks;  // construction order (§4.6 item 5)
    bool layered = false;
};
CoreSplit splitCore(const kernel::Kernel& k, const kernel::Shape& core, std::vector<Note>& notes);

// Hypothesis tree (rec_search.cpp, §4.10). One level: a body peeled, then the operations of the hypothesis that freed its
// core (empty at the deepest level); ops are built on the deeper body and give peel.core.
struct SearchLevel {
    Peel peel;
    std::vector<Op> ops;
    kernel::Shape restored;  // the body the hypothesis leaves (the source of the next level)
};
struct SearchResult {
    std::vector<SearchLevel> levels;  // from the source body inwards
    CoreSplit core;                   // of the deepest level
    std::vector<Note> notes;
};
// Plan cost (§4.10 item 5): the operations of a peeled body and a base.
double planCost(const Peel& peel, const std::vector<Op>& base);
// CONSTRUE_REC_TRACE: the plan to stderr (§12).
void tracePlan(const kernel::Kernel& k, const Plan& plan);
// The search starts when the obvious way ends without a simple base (§4.10 item 1).
bool needsSearch(const CoreSplit& split);
// root: the peeled source body and its split; nullopt — nothing better than them was found.
std::optional<SearchResult> searchHistory(const kernel::Kernel& k, const Peel& root, const CoreSplit& split, std::vector<Note>& notes);

// Lettering (rec_text.cpp, §4.7): the body without the lettering and the lettering operation (extrusion or cut of
// letter outlines); nullopt means there is no lettering.
struct TextRemoval {
    kernel::Shape restored;
    Op op;
};
std::optional<TextRemoval> removeText(const kernel::Kernel& k, const kernel::Shape& body, std::vector<Note>& notes);
// Loft added onto the body (rec_loft.cpp, §4.9): the body without the loft and the loft operation; nullopt — none.
struct LoftRemoval {
    kernel::Shape restored;
    Op op;
};
std::optional<LoftRemoval> removeLoft(const kernel::Kernel& k, const kernel::Shape& body, std::vector<Note>& notes);
// Text fit (rec_text_fit.cpp, §4.7а): text, font, height, anchor and angle whose layout reproduces the letter outlines
// (curves in plane local coordinates); nullopt — no product font reproduces them within the tolerance.
std::optional<Op::Text> fitText(const std::vector<kernel::Curve2>& curves);

// Feature patterns (rec_pattern.cpp, §4.8). Instance motion from the seed: translation or rotation about the axis.
struct Motion {
    bool rotate = false;
    double angle = 0;
    Vec3 shift = Vec3::Zero();
};
struct PatternFit {
    size_t seed = 0;            // index of the seed point (lexicographically first)
    bool circular = false;      // circular: axis origin/axis, count; otherwise linear
    Point3 origin = Point3::Zero();
    Vec3 axis = Vec3::UnitY();
    int count = 0;
    std::vector<Op::Direction> linear;
    std::vector<Motion> motion;  // per entry point: motion from the initial position (zero for the seed)
};
// Pattern from instance points (rules of §4.4.4 items 1–2): circular in the plane ⊥ n or a grid along axes ⊥ n.
std::optional<PatternFit> fitPattern(const std::vector<Point3>& pts, const Vec3& n);
// Block operations in which identical extrusion loops are arranged as a pattern or mirrored about a built-in plane →
// the rest, feature and pattern or mirror (§4.8).
std::vector<Op> splitPatterns(const std::vector<Op>& block);
// One sketch for operations of one level (§4.11, FX10-REC-004): plain round cuts become extrusions, extrusions with the same
// plane, direction, operation and end are merged; plan indices, blocks and bodies are kept consistent.
void mergeLevelOps(Plan& plan);
// Built-in mirror planes (§4.4.4 item 3) are sketch::datumPlaneNormals(): front, top, right; all pass through the origin.
// Reflection of point p across the plane through the origin with unit normal n.
inline Point3 reflect(const Point3& p, const Vec3& n) { return p - 2 * p.dot(n) * n; }
// Rotation of vector v around the unit axis ax by angle th (Rodrigues' formula).
inline Vec3 rotateAbout(const Vec3& v, const Vec3& ax, double th) {
    return v * std::cos(th) + ax.cross(v) * std::sin(th) + ax * ax.dot(v) * (1 - std::cos(th));
}

}  // namespace construe::rec::detail
