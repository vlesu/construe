// Screen geometry of the open sketch (docs/dev/client.md §4.3а): data, solved state, conversion of
// sketch coordinates to screen and back, screen polylines and distances. A value object: the sketcher builds it
// for the duration of a computation; it is used by snaps and hit testing (sketch_picker), labels (sketch_annotations)
// and the sketcher itself. The shared types Snap / Hit / Glyph live here too.
#pragma once

#include "api/session.h"
#include "app/camera.h"

#include <QLineF>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

// Point position snap (app-ux §6.4 items 1–3, 7).
struct Snap {
    // Center — center of a projection circle/circular edge (§6.4 item 1в); Intersection — of two curves (item 1г).
    enum class Kind { None, Point, Midpoint, Curve, Center, Intersection };
    Kind kind = Kind::None;
    std::string target;  // point ID (or "origin") / segment ID (Midpoint) / curve ID (Curve)
    // Target is a body vertex/edge in the sketch plane (§6.4, vision item 7): stable name; target is empty.
    // On placement, a construction projection of the target and a constraint to it are added.
    std::string source;
    std::string target2, source2;  // second intersection curve (Intersection): sketch curve ID or stable edge name
    Point2 at = Point2::Zero();
    // Alignment lines with points (§6.4 item 7): the coordinate is snapped; the constraint is vertical (by x) /
    // horizontal (by y) between the points (М9).
    std::optional<Point2> alignX, alignY;  // points whose x / y we aligned to
    std::string alignXId, alignYId;        // their IDs ("origin" — origin) or a segment ID (aligned with its midpoint)
    std::string alignXSource, alignYSource;  // target is a body vertex/straight edge (stable name); IDs are empty
    // Segment extension (§6.4 item 3в): a point on the segment's line outside it; constraint pointOnObject.
    std::string extLine;
};

// The third click of «Arc by 3 points» (app-ux §6.3, FX10-SK-004): it only shapes the arc and gets a constraint only
// from a point snap (pointOnObject), so only that snap acts; any other snap (curve, extension, alignment) would move
// the click without a constraint — the point stays at the cursor.
Snap arcThroughPointSnap(const Snap& sn, const Point2& cursor);

// Sketch object under the cursor (§3.5): label (dimension or constraint icon), point, curve.
struct Hit {
    enum class Kind { None, Label, Point, Curve };
    Kind kind = Kind::None;
    std::string id;
};

// Constraint icon (§6.6): screen rectangle and symbol.
struct Glyph {
    std::string constraint;
    QRectF rect;
    QString symbol;
    // Alignment of a point with a segment midpoint (FX8-SK-003): dashed leader from the point to the midpoint.
    std::optional<QLineF> leader;
};

class SketchGeometry {
public:
    // base — solved sketch state (coordinate system, projections); active — the state used to draw
    // geometry (intermediate while dragging, otherwise base).
    SketchGeometry(const sketch::SketchData* data, const api::SketchState* base, const api::SketchState* active, const Camera& camera)
        : data_(data), base_(base), active_(active ? active : base), camera_(camera) {}

    bool valid() const { return data_ && base_; }
    const sketch::SketchData* data() const { return data_; }
    const api::SketchState& base() const { return *base_; }
    const api::SketchState& active() const { return *active_; }

    Point2 pointPos(const std::string& id) const;     // "origin" — origin; unknown — (0, 0)
    double radius(const sketch::Entity& e) const;     // circle — solved radius, arc — |start − center|
    // Segment or projection-segment ends (sketch coordinates); otherwise empty (dimensions to projections, §6.9).
    std::optional<std::pair<Point2, Point2>> lineEnds(const std::string& id) const;
    // Center and radius of a circle, arc or projection circle/arc; otherwise empty.
    std::optional<std::pair<Point2, double>> circleOf(const std::string& id) const;
    // Center and shape of an ellipse (S4-SKT-004) in the active state; otherwise empty.
    // Ellipse or ellipse arc (S4-SKT-007): center and shape of its ellipse.
    std::optional<std::pair<Point2, sketch::SolvedState::EllipseShape>> ellipseOf(const std::string& id) const;
    // Polyline of an ellipse or an ellipse arc (sketch coordinates), n segments.
    std::vector<Point2> ellipseEntityPolyline(const sketch::Entity& e, int n) const;
    // Polyline of a spline (S4A-SPL-001) by its poles in the active state (sketch coordinates); invalid — empty.
    std::vector<Point2> splineEntityPolyline(const sketch::Entity& e) const;
    QPointF toScreen(const Point2& local) const;
    std::optional<Point2> toLocal(const QPointF& screen) const;  // intersection of the ray with the sketch plane

    // Projection (§6.11а): screen polyline (a point — a single point; no geometry — empty).
    std::vector<QPointF> projectedScreen(const std::string& id) const;
    // Screen distance from s to a curve (segment, circle, arc, projection curve); otherwise 1e18.
    double distToEntity(const sketch::Entity& e, const QPointF& s) const;
    // Screen polyline of an entity for box selection (§6.3): a curve — its polyline points, a standalone point —
    // a single point; curve ends and centers — empty.
    std::vector<Point2> screenPolyline(const sketch::Entity& e) const;
    // Curve under the cursor for trimming (§6.11): the nearest within PICK_CURVE_PX, on a tie — the smaller ID.
    std::string trimCurveAt(const QPointF& screen) const;

private:
    const sketch::SketchData* data_;
    const api::SketchState* base_;
    const api::SketchState* active_;
    const Camera& camera_;
};

// Screen distance from p to segment a–b.
double segDist(const QPointF& p, const QPointF& a, const QPointF& b);

// Closed ellipse polyline (sketch coordinates): n + 1 points at parameter t = 2πi/n, the last equals the first.
std::vector<Point2> ellipsePolyline(const Point2& c, const sketch::SolvedState::EllipseShape& sh, int n);
// Ellipse arc from start to end counterclockwise by the parameter (S4-SKT-007), n + 1 points.
std::vector<Point2> ellipseArcPolyline(const Point2& c, const sketch::SolvedState::EllipseShape& sh, const Point2& start,
                                       const Point2& end, int n);
// Parameter of a point of the ellipse (S4-SKT-007).
double ellipseParam(const Point2& c, const sketch::SolvedState::EllipseShape& sh, const Point2& p);
// Spline polyline (S4A-SPL-001): its flattening (sketch.md §2.7.6) with tolerance 1e-3 of the pole box size; the
// poles — by pos; weights — the entity's. Invalid curve — empty.
std::vector<Point2> splinePolyline(const sketch::Entity& e, const std::function<Point2(const std::string&)>& pos);
// Ellipse point at parameter t.
Point2 ellipsePoint(const Point2& c, const sketch::SolvedState::EllipseShape& sh, double t);

// New sketcher elements are construction ones (app-ux §6.3): curves — curves (`line`, `circle`, `arc`; centerline and
// «Draw as construction»), texts — texts (`text`; same toggle, FX2-SK-001). Other elements
// (points, projections, constraints) are not changed.
void markNewConstruction(doc::Json& items, bool curves, bool texts);

// «Dimension» tool (app-ux §6.9, FX2-SK-002): clicking an object adds it to the selection; an already
// selected object does not change the selection; an object after two selected ones starts the selection anew.
std::vector<std::string> addDimensionPick(std::vector<std::string> picks, const std::string& pick);
// Can the selected objects be dimensioned by clicking a place (§6.9): two entities — yes; one — segment, circle,
// arc, projection curve (via d.projections), body edge or sketch axis (label "srce:"); a point — no (needs a second).
bool dimensionPlaceable(const sketch::SketchData& d, const std::vector<std::string>& picks);

}  // namespace construe::app
