// Sketch command packages — the domain part of the sketcher tools (app-ux §6.3, §6.8, §6.9; core-api.md §4.6.4).
// Pure stateless functions without Qt: from click geometry and sketch data they build sketchAdd items
// (with temporary IDs) or a dimension description. Screen rules (pixel tolerances, snaps) stay in the client:
// it adds snap constraints to points by their roles. The same functions are usable by agents and the network API.
#pragma once

#include "api/session.h"
#include "base/spline.h"
#include "sketch/groups.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::api::sketchops {

// sketchAdd items with temporary IDs t1, t2, … and roles: role → temporary ID (points to which the client
// will add snap constraints, and the created curves).
struct Batch {
    Json items = Json::array();
    std::map<std::string, std::string> roles;
};

// Center of the circle through three points (three-point arc, its preview); nothing — the points are collinear.
std::optional<Point2> circumcenter(const Point2& a, const Point2& b, const Point2& c);

// Rectangle by corners a and c (§6.3): 4 sides, each with its own endpoints; coincidence of adjacent sides' endpoints,
// horizontality of sides 0 and 2, verticality of 1 and 3. Roles: corner0…corner3 — the start of side i
// (corner0 = a, corner2 = c), side0…side3.
Batch rectangle(const Point2& a, const Point2& c);
// Center rectangle: like rectangle by corners center ∓ (corner − center), plus a center point,
// two construction diagonals with endpoints at the corners, and the center as midpoint of diagonal 0. Roles: center,
// corner0…corner3 (corner2 = corner), side0…side3.
Batch centerRectangle(const Point2& center, const Point2& corner);
// Regular polygon (app-ux §6.3, S1-SK-020): n sides (3…40), center center; vertices — pointOnObject to
// a construction circle through them, sides equal to side 0, coincident at vertices. inscribed — first is vertex 0;
// otherwise first is the midpoint of side 0, plus an inscribed construction circle tangent to side 0. Roles: center,
// circle (inscribed — the vertex circle, circumscribed — the inscribed circle), outerCircle (circumscribed), vertex0,
// side0…side{n−1}. Nothing — n out of range or first coincides with center.
std::optional<Batch> polygon(const Point2& center, const Point2& first, int n, bool inscribed);
// Ellipse (app-ux §6.3, S4-SKT-004): center c, end of the first semi-axis a (rx and rotation), b — the second semi-axis
// as the distance from b to the line of the first semi-axis. Roles: center, ellipse. Nothing — a zero semi-axis.
std::optional<Batch> ellipse(const Point2& c, const Point2& a, const Point2& b);

// Spline by control points (app-ux §6.3, S4A-SPL-001): n ≥ 2 poles; open — a Bezier when n ≤ 4 (degree n − 1), else a
// clamped uniform B-spline of degree 3; closed (n ≥ 3) — a periodic B-spline of degree min(3, n − 1) with knots 0…n.
// Roles: pole0…pole<n−1>, spline. Nothing — fewer poles, or two consecutive ones coincide.
std::optional<Batch> splineByPoles(const std::vector<Point2>& poles, bool closed);
// Spline through points (S4A-SPL-001): interpolation (sketch.md §2.7.7), stored as a B-spline with its poles. Roles as
// above; pole0 is at the first point, the last pole at the last one. Nothing — fewer than two points or coincident
// consecutive ones.
std::optional<Batch> splineThrough(const std::vector<Point2>& points);
// Circle through three points (app-ux §6.3). Roles: center, circle. Nothing — the points are collinear.
std::optional<Batch> circleThreePoints(const Point2& a, const Point2& b, const Point2& c);
// Three-point arc: start a, end b, point m on the arc. In the data the arc is counterclockwise: if m is not on
// the counterclockwise arc a→b, start and end are swapped. Roles: a, b — the click points a and b (after
// a possible swap), center, arc. Nothing — the points are collinear.
std::optional<Batch> arcThreePoints(const Point2& a, const Point2& b, const Point2& m);
// "Center — start — end" arc: ccw — traversal from start to end counterclockwise (otherwise start and end
// are swapped in the data). Roles: center, start, end — the click points, arc.
Batch arcCenterStartEnd(const Point2& center, const Point2& start, const Point2& end, bool ccw);
// Tangent arc from point p (existing point fromPoint) with tangent direction tangent to point end:
// the center is on the normal to the tangent; turning left — a CCW arc from p to end, right — from end to p.
// Coincidence with fromPoint and tangency to fromCurve (if not empty). Roles: end — the point at end, arc.
// Nothing — end lies on the tangent line (center at infinity).
std::optional<Batch> tangentArc(const Point2& p, const Point2& tangent, const Point2& end, const std::string& fromPoint,
                                const std::string& fromCurve);

// Constraints from the selection (§6.8, methods 1–2).
enum class ConstraintOutcome {
    Items,              // items — constraints to add
    PickEntities,       // the selection has no entities: the tool switches to "command → pick" mode
    FixArcUnsupported,  // "Fix" is unavailable for an arc
    AlreadyExists,      // all such constraints already exist
};
struct ConstraintPlan {
    ConstraintOutcome outcome = ConstraintOutcome::Items;
    Json items = Json::array();
};
// type — constraint type (`coincident` for a point and a curve becomes `pointOnObject`); multiple
// selection — by the rules of §6.8; already existing constraints (same type and set of refs) are not repeated.
ConstraintPlan constraintsForSelection(const sketch::SketchData& d, const std::string& type, const std::vector<std::string>& selected);
// Mirror (app-ux §6.3, S2-SK-009): copies of the selected curves and points, reflected about the line axis
// (a segment or a projected segment; a, b — its points), with symmetric relations. Nothing — nothing to reflect.
std::optional<Batch> mirror(const sketch::SketchData& d, const std::vector<std::string>& selected, const std::string& axis,
                            const Point2& a, const Point2& b);
// Corner for a sketch fillet / chamfer (core-api §4.6.2 sketchFillet "Corner", FX7-SK-006): point — any point of the
// corner; the corner is its coincidence cluster (points linked by coincident, transitively). The sides are the
// non-construction lines with an end in the cluster; construction curves meeting there do not matter. problem —
// empty when the corner is valid, otherwise "not a point", "no lines" (lines == 0), "one line" (lines == 1),
// "many lines" (lines > 2) or "not lines" (a non-construction arc/curve end is in the cluster as well).
struct CornerFind {
    std::string problem;
    int lines = 0;           // non-construction lines with an end in the cluster
    std::string p, q;        // ends of l1 and l2 in the cluster
    std::string l1, l2;      // l1 — the line whose end is point when it is a side end, else the first by entity order
    std::vector<std::string> cluster;  // cluster points in entity order (point included)
};
CornerFind findCorner(const sketch::SketchData& d, const std::string& point);
// Default radius of a sketch fillet at corner point (app-ux §6.3, FX8-SK-005): the radius of the arc of the same contour
// (non-construction lines and arcs connected through their ends or coincident points, from the corner's lines) whose
// middle is the nearest to the corner (state st); ties — the first by entity order. Nothing — not a valid corner or no arcs.
std::optional<double> neighborFilletRadius(const sketch::SketchData& d, const sketch::SolvedState& st, const std::string& point);

// Equality snap while drawing (app-ux §6.4 «Dimension equality», FX7-SK-003): the existing circle / arc (radius = true)
// or line (radius = false) of d whose radius / length in state st is the closest to value, if the difference is not
// more than absTol, mm (the client passes the screen snap tolerance SNAP_POINT_PX converted to mm, FX8-SK-006);
// construction curves count, projections do not. Ties — the first by entity order. Nothing — no such element.
struct EqualMatch {
    std::string id;
    double value = 0;  // its radius / length, mm
};
std::optional<EqualMatch> equalTarget(const sketch::SketchData& d, const sketch::SolvedState& st, bool radius, double value, double absTol,
                                      const std::set<std::string>& exclude = {});
// Tangency at a joint (app-ux §6.3 «Arc by 3 points», FX7-SK-002): the line or arc of d with an end at point (or
// coincident with it) whose direction there is parallel to dir within tolRad. Nothing — empty string.
std::string tangentAtJoin(const sketch::SketchData& d, const sketch::SolvedState& st, const std::string& point, const Point2& dir,
                          double tolRad);

// Closed contour of a curve for «Offset → Contour» (core-api §4.6.4, FX7-SK-007, S7F-SK-005): lines, arcs and projected
// lines / arcs (d.projections) of the same construction kind chained through joints (shared point, coincident cluster,
// same position), in traversal order from curve; a circle is a contour by itself. Empty — branches or does not close.
std::vector<std::string> contourOf(const sketch::SketchData& d, const std::string& curve);
// Sign of the distance of an `offset` group with sources at which curve is offset to side (+1 — left of its own
// direction; sketch.md §2.6.5, S7F-SK-005); 0 — curve is not a source or the sources are invalid.
int offsetSign(const sketch::SketchData& d, const std::vector<std::string>& sources, const std::string& curve, int side);

// The dimension can be a diameter one (S2-SK-003): distance point (or origin, or a projected point) — segment or
// projected segment.
bool diametralApplicable(const sketch::SketchData& d, const sketch::Constraint& c);
// Points owned by curves (sketch.md §2.1, sketch::ownedPoints); the rest are standalone (app-ux §6.5, FX9-SK-007).
std::set<std::string> ownedPoints(const sketch::SketchData& d);
// A line in sketch coordinates that can serve as an axis of a diameter dimension (sketch.md §2.2, FX9-SK-001).
struct DiametralAxis {
    Point2 p, dir;
};
inline constexpr double DIAMETRAL_AXIS_TOL = 1e-6;  // mm: a point lies on an axis; cosine: an axis is perpendicular
// Axis-equivalent diameter dimension (FX9-SK-001): a two-point dimension (distance point–point, distanceX, distanceY;
// points, the origin, projected points) one of whose points lies on an axis perpendicular to the dimension direction
// (distanceX — x, distanceY — y, distance — from the first point to the second). Axes: revolveAxes (axes of revolve
// features of this sketch, given by the caller), construction segments and projected segments of d; none of them —
// the sketch axes (horizontal and vertical through the origin). Result — the index (0, 1) of the ref on the axis;
// nothing — not applicable. Positions — from st.
std::optional<int> diametralAxisRef(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c,
                                    const std::vector<DiametralAxis>& revolveAxes = {});
// Point–segment diametral dimension: the axis runs through the point (sketch.diametralAxisAtPoint, sketch.md §2.2,
// FX13-DIM-001).
inline bool diametralAxisAtPoint(const sketch::SketchData& d, const sketch::Constraint& c, const sketch::SolvedState* st = nullptr) {
    return sketch::diametralAxisAtPoint(d, c, st);
}
// Either rule: point–segment (above) or axis-equivalent (diametralAxisRef).
bool diametralApplicable(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c,
                         const std::vector<DiametralAxis>& revolveAxes = {});

// Constraints allowed for the selection (app-ux §6.8, §7.2): type signatures and multiple-selection rules.
// Order — as in the table of §6.8 (coincident, horizontal, vertical, parallel, perpendicular, tangent, equal,
// concentric, midpoint, symmetric, fix). Projections are counted by their kind (point/segment/curve).
std::vector<std::string> applicableConstraints(const sketch::SketchData& d, const std::vector<std::string>& selected);

// Position of a sketch point by ID ("origin" — the origin) in the solved state.
using PointPos = std::function<Point2(const std::string&)>;
// Dimension kind (§6.9) for the selected entities and the placement point at. view maps a sketch point to the
// coordinates in which at is given (screen for the client): by them the rule "cursor outside the x/y range" picks
// distanceX/distanceY. parallelTolDeg — the "almost parallel" tolerance (PARALLEL_DIM_TOL_DEG). Nothing — the set
// does not yield a dimension.
std::optional<std::string> dimensionKind(const sketch::SketchData& d, const PointPos& pos, const std::vector<std::string>& ids,
                                         const Point2& at, const std::function<Point2(const Point2&)>& view,
                                         double parallelTolDeg);
// Refs and measured value of a dimension of kind kind (§6.9): length — mm; angle — rad (angle = true);
// distanceX/distanceY — absolute value, refs ordered so the difference is ≥ 0. radii — circle radii of solved state.
struct DimensionPlan {
    std::vector<std::string> refs;
    double value = 0;
    bool angle = false;
    bool number = false;  // dimensionless value (pole weight, S4A-SPL-002)
    std::string arc;  // "near" / "far" — dimension to a circle/arc (app-ux §6.9), otherwise empty
    int side = 0;     // side branch given explicitly (segment — curve on intersection, FX1-SKG-002); 0 — api infers it
};
// at — label position (sketch coordinates): for an angle it selects the sector (app-ux §6.9).
std::optional<DimensionPlan> dimensionFor(const sketch::SketchData& d, const PointPos& pos, const std::map<std::string, double>& radii,
                                          const std::vector<std::string>& ids, const std::string& kind,
                                          const std::optional<Point2>& at = std::nullopt);

// sketchUpdate changes for an entered value of dimension c (app-ux §6.9 "Entering a value", FX1-SKG-003): current —
// the current dimension value (for distanceX/distanceY — with the stored sign), text — the entered expression, entered
// its value. Positive — same side; negative — other side (distanceX/Y — refs in reverse order,
// distance with a side branch — the opposite branch), the absolute value is written; other types — value as is.
Json dimensionValueChanges(const sketch::Constraint& c, double current, const std::string& text, double entered);

// Label layout for "Fully define" (core-api §4.6.2 "Layout", FX4-SK-001). Model of the label and dimension lines in
// sketch coordinates: label rectangle, dimension and extension lines (as the client draws them, app-ux §6.6).
struct DimFootprint {
    Point2 lo, hi;  // label rectangle
    std::vector<std::pair<Point2, Point2>> dimension, extension;
};
// Dimension anchor (sketch.md §2.2 «labelOffset», FX8-SK-004): the point the label offset is measured from — the mean
// of the reference anchors (point — itself, origin — (0, 0); segment and projected segment — its middle; circle, arc,
// ellipse, ellipse arc, projected circle/arc — the center; spline — the mean of its poles; text — its anchor point;
// projected point — itself); for «weight» — the pole. Nothing — a reference without geometry.
std::optional<Point2> dimensionAnchor(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c);
// Label center of dimension c: anchor + labelOffset; nothing — no labelOffset or no anchor (default placement).
std::optional<Point2> dimensionLabelPoint(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c);
// labelOffset for the label center at `label` (absolute sketch coordinates); nothing — no anchor.
std::optional<std::array<double, 2>> labelOffsetFor(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c,
                                                    const Point2& label);
// Label size, substituted from the sketch extent for zero fields.
FullyDefineLayout resolvedLayout(const sketch::SketchData& d, const sketch::SolvedState& st, const FullyDefineLayout& m);
// Model of dimension c with its label at label; m — allowed label sizes; nothing — not a dimension or no ref geometry.
std::optional<DimFootprint> dimensionFootprint(const sketch::SketchData& d, const sketch::SolvedState& st, const sketch::Constraint& c,
                                               const Point2& label, const FullyDefineLayout& m);
// Segments of the sketch geometry (curves as polylines) in the solved state, including projections.
std::vector<std::pair<Point2, Point2>> geometrySegments(const sketch::SketchData& d, const sketch::SolvedState& st);
// Label position for each of added (new dimensions, not yet in d) — in the same order; d — sketch data with projections
// and existing dimensions, st — the solved state.
std::vector<Point2> layoutDimensionLabels(const sketch::SketchData& d, const sketch::SolvedState& st,
                                          const std::vector<sketch::Constraint>& added, const FullyDefineLayout& m);

// Linked group generator for preview (core-api.md §4.6.4 groupItems, sketch.md §2.6.2): items with refs
// "@role" over data d without the group; errors — as for sketchGroupCreate.
Result<std::vector<sketch::GroupItem>> groupItems(const sketch::SketchData& d, const std::string& type,
                                                  const std::vector<std::string>& sources, const Json& params,
                                                  const expr::DocumentUnits& units, const expr::Env& env = {});

}  // namespace construe::api::sketchops
