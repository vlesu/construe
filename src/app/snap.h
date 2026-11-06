#pragma once
// Snap and box-selection geometry of the sketcher (docs/dev/client.md §4.3а, app-ux §6.3, §6.4). Pure functions without
// Qt, covered by unit tests; candidate search and pixel thresholds are in SketchEditor.

#include "base/math.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::app::snap {

// Nearest point of segment a–b to p.
Point2 nearestOnSegment(const Point2& a, const Point2& b, const Point2& p);
// Nearest point of circle (c, r) to p; none if p is at the center.
std::optional<Point2> nearestOnCircle(const Point2& c, double r, const Point2& p);
// Nearest point of an arc (center c, counter-clockwise from start to end) to p, if the projection
// of p onto the circle falls within the arc; otherwise none.
std::optional<Point2> nearestOnArc(const Point2& c, const Point2& start, const Point2& end, const Point2& p);
// Nearest point of an ellipse (center c, semi-axes rx, ry, rotation angle) to p (S4-SKT-004): the best of
// SNAP_ELLIPSE_SAMPLES parameter samples, refined by golden-section search on adjacent intervals (app-ux §6.4 item 3).
Point2 nearestOnEllipse(const Point2& c, double rx, double ry, double angle, const Point2& p);

// Curve for intersections (§6.4 item 1г): segment a–b, circle (c, r) or arc (c, counter-clockwise from start to end).
struct Curve {
    enum class Kind { Segment, Circle, Arc };
    Kind kind = Kind::Segment;
    Point2 a = Point2::Zero(), b = Point2::Zero();          // Segment
    Point2 c = Point2::Zero();                               // Circle, Arc
    double r = 0;
    Point2 start = Point2::Zero(), end = Point2::Zero();    // Arc
};
// Intersection points of two curves (tangency — one point); coincident/parallel — no points.
std::vector<Point2> intersect(const Curve& u, const Curve& v);

// Body geometry in the sketch plane (§6.4, vision item 7): a point lies in the plane if its distance
// to the plane is ≤ tol; a polyline — if all its points lie in the plane (an empty one does not).
bool pointInPlane(const Plane& plane, const Point3& p, double tol);
bool polylineInPlane(const Plane& plane, const std::vector<Point3>& pts, double tol);
// Point of the polyline nearest to p (in sketch coordinates); empty polyline — none.
std::optional<Point2> nearestOnPolyline(const std::vector<Point2>& pts, const Point2& p);

// Direction snap of segment a→b (§6.4 items 4–6), in priority order:
// horizontal/vertical; parallel/perpendicular to the woken segment (direction woken);
// tangent to the arc continuation (continuation direction tangent). Tolerance — tolRad.
enum class Direction { None, Horizontal, Vertical, Parallel, Perpendicular, Tangent };
struct DirectionResult {
    Direction kind = Direction::None;
    Point2 end = Point2::Zero();  // snapped b: length is preserved (for H/V — projection onto the axis)
};
DirectionResult snapDirection(const Point2& a, const Point2& b, double tolRad,
                              const std::optional<Point2>& woken, const std::optional<Point2>& tangent);

// Tangent of the arc continuation at its end point q (center c): for the end point — the counter-clockwise
// direction, for the start point — the reverse.
Point2 arcContinuation(const Point2& c, const Point2& q, bool atStart);

// Tangency at a joint (app-ux §6.4, FX10-SK-004): direction dir goes on along t (dir · t > 0) and differs from it by
// no more than tolRad.
bool alongTangent(const Point2& dir, const Point2& t, double tolRad);

// Equality of an arc by 3 points (app-ux §6.4 item 8, FX10-SK-004): how much the radius of the arc through a, b and m
// changes per unit of shift of m along the radius (|dr/dm|); 0 — degenerate. The snap tolerance of the equality is
// measured at the third point: the radius tolerance is the point tolerance times max(1, this factor).
double arc3RadiusSensitivity(const Point2& a, const Point2& b, const Point2& m);

// Key of a direction hint of the «Line» tool (app-ux §6.4, FX10-SK-005): "dir:<type>:<other>"; a tangency is keyed by
// the curve — "tangent:<other>", as the arc tangency hints; an empty type — empty.
std::string directionHintKey(const std::string& type, const std::string& other);

// Auto-constraint hints dismissed by Esc (app-ux §6.13 item 0, FX10-SK-005). A hint is a key ("equal:<id>",
// "tangent:<id>", "dir:<type>:<other>"). Esc dismisses the shown hints; a dismissed hint stays dismissed while the
// cursor keeps offering it and is forgotten once it is not offered (the cursor left the target) or after the click.
class DismissedHints {
public:
    // Esc: the shown hints become dismissed; false — nothing was shown (Esc goes on down the ladder).
    bool dismiss(const std::vector<std::string>& shown);
    bool contains(const std::string& key) const { return keys_.count(key) > 0; }
    // The cursor moved: the hints offered now regardless of dismissal (raw); the others are forgotten.
    void retain(const std::vector<std::string>& offered);
    void clear() { keys_.clear(); }
    bool empty() const { return keys_.empty(); }
    const std::set<std::string>& keys() const { return keys_; }

private:
    std::set<std::string> keys_;
};

// Selection box (app-ux §6.3): the curve's screen polyline is entirely inside the rectangle / touched by it.
// The rectangle is given by corners (x0, y0)–(x1, y1) in any order.
bool polylineInside(const std::vector<Point2>& pts, const Point2& c0, const Point2& c1);
bool polylineTouches(const std::vector<Point2>& pts, const Point2& c0, const Point2& c1);

}  // namespace construe::app::snap
