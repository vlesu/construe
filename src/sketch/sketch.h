// Sketch: solving, statuses, dragging, profiles (spec/sketch.md).
#pragma once

#include "base/error.h"
#include "base/math.h"
#include "base/spline.h"
#include "sketch/data.h"

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::sketch {

struct SolvedState {
    std::map<std::string, Point2> points;  // all points (mm, local coordinates)
    std::map<std::string, double> radii;   // all circles
    // Ellipses (S4-SKT-004): semi-axes rx, ry and rotation of the rx axis (rad), as in the document (sketch.md §2.3).
    struct EllipseShape {
        double rx = 0, ry = 0, angle = 0;
        bool operator==(const EllipseShape&) const = default;
    };
    std::map<std::string, EllipseShape> ellipses;
    // Splines (S4A-SPL-002): effective pole weights (empty — all 1); driving weight dimensions set them (§2.7.8).
    std::map<std::string, std::vector<double>> weights;
    bool operator==(const SolvedState&) const = default;
};

enum class SketchStatus { UnderConstrained, FullyConstrained, OverConstrainedRedundant, Conflict, NotConverged, Error };
const char* sketchStatusName(SketchStatus s);

enum class EntityState { Free, Fixed, Conflict };

struct ConflictGroup {
    std::vector<std::string> ids;
    std::string suggested;
    bool undetermined = false;
};

// Profile segment geometry (local coordinates).
struct Segment {
    enum class Kind { Line, Arc, Circle, Ellipse, EllipseArc, Spline };  // EllipseArc — S4-SKT-007, Spline — S4A-SPL-003
    std::string id;
    Kind kind = Kind::Line;
    Point2 p1 = Point2::Zero(), p2 = Point2::Zero();          // Line
    Point2 center = Point2::Zero();                           // Arc, Circle, Ellipse, EllipseArc
    double radius = 0;                                         // Arc, Circle; Ellipse, EllipseArc — semi-axis rx
    double minorRadius = 0, angle = 0;                         // Ellipse, EllipseArc: semi-axis ry, rotation of the rx axis (rad)
    Point2 start = Point2::Zero(), end = Point2::Zero();      // Arc, EllipseArc: counterclockwise from start to end
    spline::Curve curve;                                       // Spline: open (clamped) or periodic, positive direction — by t
    bool reversed = false;
};

struct Loop {
    std::vector<Segment> segments;
};

struct Region {
    std::string regionId;
    int depth = 0;
    Loop outer;
    std::vector<Loop> holes;
    double area = 0;
    Point2 interiorPoint = Point2::Zero();
};

struct ProfileSet {
    std::vector<Region> regions;
    std::vector<Error> messages;
    const Region* find(const std::string& regionId) const {
        for (const auto& r : regions)
            if (r.regionId == regionId) return &r;
        return nullptr;
    }
};

struct RegionTriangles {
    std::vector<Point2> vertices;
    std::vector<std::array<int, 3>> triangles;
};

struct SketchResult {
    SketchStatus status = SketchStatus::UnderConstrained;
    std::optional<int> dof;
    std::map<std::string, EntityState> entityState;
    std::vector<ConflictGroup> conflicts;
    std::vector<std::string> redundant;
    std::vector<std::string> unsatisfied;
    SolvedState state;
    std::map<std::string, double> reference;
    std::vector<Error> errors;
    std::vector<Error> messages;
    ProfileSet profiles;
};

// Values of driving dimensions in internal units (mm/rad), or evaluation errors.
using Values = std::map<std::string, Result<double>>;

SolvedState stateOf(const SketchData& s);
// Curve of a bezier/bspline entity (§2.7.1): poles from state.points (missing — from the entity data of s, else 0),
// weights — state.weights if present, else the entity's.
spline::Curve splineCurve(const Entity& e, const SolvedState& state, const SketchData* s = nullptr);
SketchData applySolution(SketchData s, const SolvedState& state);

SketchResult solve(const SketchData& s, const Values& values);
// value — the driving dimension value (if evaluated): the branch with the smallest residual at it (§4).
Result<Branch> inferBranch(const SketchData& s, const Constraint& c, std::optional<double> value = std::nullopt);
// Dimension value from the current sketch coordinates without solving (§4: measure).
Result<double> measure(const SketchData& s, const std::string& constraintId);

struct DragPlan {
    struct Target {
        bool radius = false;
        std::string entity;
        Point2 start = Point2::Zero();
    };
    std::vector<Target> targets;
    Point2 grab = Point2::Zero();
};

struct SketchDragResult {
    enum class Status { Reached, Constrained, Failed };
    Status status = Status::Failed;
    SolvedState state;
    std::map<std::string, double> reference;
};

Result<DragPlan> dragBegin(const SketchData& s, const SolvedState& state, const std::string& entity, const Point2& grab);
SketchDragResult drag(const SketchData& s, const Values& values, const SolvedState& state, const DragPlan& plan,
                      const Point2& cursor);

ProfileSet regions(const SketchData& s, const SolvedState& state);
RegionTriangles triangulateRegion(const Region& region, double chordTol);

// Weak dimensions (§4.4, S4-WEAK-001): dimensions and relations "inferred" from the solved state that close the
// remaining degrees of freedom of the main geometry. Not written to the document.
struct WeakSet {
    std::vector<Constraint> items;          // IDs "w1", "w2", … in selection order; dimensions — driving, without value
    // Implicit items (§4.4 item 7, FX7-SK-011): coordinates of axis (construction line) ends — they hold the ends
    // where they were and close their freedom, but are not shown and never become dimensions. IDs continue items'.
    std::vector<Constraint> implicit;
    std::map<std::string, double> values;   // dimension values of the set (mm / rad), measured in state
    int dof = 0;                            // number of closed degrees of freedom (equations of items and implicit)
};
// state — solved state s (values of user dimensions — values).
WeakSet weakConstraints(const SketchData& s, const SolvedState& state, const Values& values);
// Dimension values of the weak set in state `state` (dragging, §4.4 item 4).
std::map<std::string, double> measureWeak(const SketchData& s, const SolvedState& state, const std::vector<Constraint>& weak);
// Value of a distanceX/Y, diameter, semiMajor/semiMinor/ellipseAngle dimension from the state (without sketch data).
std::optional<double> weakValue(const Constraint& c, const SolvedState& state);
bool isWeakId(const std::string& id);  // "w<N>"
// Axes of the sketch (§4.4 item 2.4, S7F-SK-001): construction lines except contour ones (both ends coincident with
// points of other non-projected curves — sides and diagonals of a construction rectangle). IDs by entity order.
std::set<std::string> axisLines(const SketchData& s);
// A line in sketch coordinates that can serve as an axis of a diametral dimension (§2.2, FX9-SK-001).
struct AxisLine {
    Point2 p, dir;
};
// Diametral point–segment dimension (§2.2, FX13-DIM-001): where its axis is. True — the axis runs through the point
// parallel to the segment (the point lies on an axis parallel to the segment, the segment itself does not lie on an
// axis — the segment is then the radius side, e.g. a contour edge on the axis was picked first); false — the segment
// is the axis (as before). Axes: `axes` (revolve axes from the caller), axis lines (axisLines) and projected segments of
// s; none — the sketch axes through the origin. Positions — from st when given, otherwise document coordinates.
bool diametralAxisAtPoint(const SketchData& s, const Constraint& c, const SolvedState* st = nullptr, const std::vector<AxisLine>& axes = {});
// Solve held by weak dimensions (§4.4 item 3): the weak set from the document coordinates s (stateOf) is added to the
// constraints; if not solved — a plain solve. The result is for writing coordinates (status and DOF include the set).
SketchResult solveHeld(const SketchData& s, const Values& values);

// Normalization of refs order by the type signature (§2.2). nullopt — none matches.
std::optional<std::vector<std::string>> normalizeRefs(const SketchData& s, const Constraint& c);

// Built-in planes and axes of a part (spec/document.md §2.5) — the single table for all core modules (sketch.md §6а):
// IDs in the order front, top, right and x, y, z.
inline constexpr std::array<const char*, 3> DATUM_PLANE_IDS = {"plane:front", "plane:top", "plane:right"};
inline constexpr std::array<const char*, 3> DATUM_AXIS_IDS = {"axis:x", "axis:y", "axis:z"};
// Plane coordinate system of a built-in plane; nullopt — not a built-in plane ID.
std::optional<construe::Plane> datumPlane(const std::string& datumId);
// Direction of a built-in axis (+X, +Y, +Z); nullopt — not a built-in axis ID.
std::optional<Vec3> datumAxis(const std::string& axisId);
// Built-in planes with their normals and axes with their directions, in the order of the ID lists above.
struct DatumDirection {
    const char* id;
    Vec3 dir;
};
const std::array<DatumDirection, 3>& datumPlaneNormals();
const std::array<DatumDirection, 3>& datumAxisDirections();
// Sketch coordinate system on a planar face (§6).
construe::Plane faceFrame(const Point3& pointOnPlane, const Vec3& outwardNormal);

// Constants §7.
inline constexpr double PROFILE_MERGE_TOL = 1e-6;
inline constexpr double PROFILE_ANGLE_TOL = 1e-9;
inline constexpr double FRAME_DEGENERATE_TOL = 1e-9;
inline constexpr double REGION_MIN_CHORD = 1e-4;
inline constexpr double REGION_MAX_CHORD = 1e3;
inline constexpr double REGION_ARC_MAX_STEP = 0.78539816339744830962;
inline constexpr int REGION_ARC_MAX_SEGMENTS = 1024;
inline constexpr int REGION_ROW_DEPTH = 6;
inline constexpr double REGION_EAR_EPS = 1e-12;
inline constexpr int ELLIPSE_CROSS_SAMPLES = 256;  // curve samples when checking ellipse tangency (§3.2 item 0)
inline constexpr int ELLIPSE_REFINE_ITER = 60;     // golden-section iterations for the distance minimum (§3.2 item 0)
inline constexpr double WEAK_ALIGN_TOL = 1e-9;     // relative tolerance for "axis-aligned" / "coordinate 0" (§4.4)

}  // namespace construe::sketch
