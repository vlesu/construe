// Geometric kernel interface (spec/kernel-interface.md). Types of a concrete kernel do not leak out.
#pragma once

#include "base/cancel.h"
#include "base/error.h"
#include "base/math.h"
#include "base/spline.h"

#include <array>
#include <optional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <stdexcept>
#include <vector>

namespace construe::kernel {

// ---------------------------------------------------------------- body (§2.2)

// Body data of a concrete implementation (immutable after creation).
struct ShapeData {
    virtual ~ShapeData() = default;
};

class Shape {
public:
    Shape() = default;
    explicit Shape(std::shared_ptr<const ShapeData> d) : d_(std::move(d)) {}
    const ShapeData* data() const { return d_.get(); }
    bool null() const { return !d_; }
    bool operator==(const Shape& o) const { return d_ == o.d_; }
    // Non-owning handle: lets caches memoize per-body values without keeping the body alive.
    std::weak_ptr<const ShapeData> weak() const { return d_; }

private:
    std::shared_ptr<const ShapeData> d_;
};

// ---------------------------------------------------------------- profile (§2.5)

using SegmentId = std::string;

struct Segment3 {
    enum class Kind { Line, Arc, Circle, Ellipse, EllipseArc, Spline };  // EllipseArc — S4-SKT-007, Spline — S4A-SPL-003
    SegmentId id;
    Kind kind = Kind::Line;
    Point3 p1 = Point3::Zero(), p2 = Point3::Zero();          // Line
    Point3 center = Point3::Zero();                           // Arc, Circle, Ellipse, EllipseArc
    double radius = 0;                                         // Ellipse, EllipseArc — semi-axis along xDir
    double minorRadius = 0;                                    // Ellipse, EllipseArc — second semi-axis (S4-SKT-004)
    Vec3 xDir = Vec3::UnitX();                                 // Ellipse, EllipseArc — unit direction of semi-axis radius in plane
    Point3 start = Point3::Zero(), end = Point3::Zero();      // Arc, EllipseArc: counterclockwise about plane.normal
    // Spline (§2.5, S4A-SPL-003): open clamped NURBS in the plane — poles (natural start — first, end — last), weights
    // (empty — all 1), distinct knots with multiplicities, degree.
    std::vector<Point3> poles;
    std::vector<double> weights, knots;
    std::vector<int> mults;
    int degree = 0;
    bool reversed = false;
};

struct Loop3 {
    std::vector<Segment3> segments;
};

struct Profile {
    Plane plane;
    Loop3 outer;
    std::vector<Loop3> holes;
    // loft only (§3.15, FX10-PT-004): the outer loop is traversed from its vertex nearest to this point; other
    // operations ignore it.
    std::optional<Point3> start;
};

struct ProfileInfo {
    double area = 0;
    bool outerCCW = true;
    std::vector<bool> holesCW;
};

// ---------------------------------------------------------------- history (§2.6)

enum class ProfileRole { Side, CapStart, CapEnd };

struct InputFace {
    int operand = 0;
    int face = 0;
    bool operator==(const InputFace&) const = default;
};

struct Origin {
    enum class Kind { Profile, FromFace, FromEdge, FromVertex, Offset };  // Offset — inner shell face (§3.13)
    Kind kind = Kind::Profile;
    ProfileRole role = ProfileRole::Side;  // Profile
    SegmentId segment;                     // Profile/Side
    InputFace input;                       // FromFace, Offset
    bool modified = false;                 // FromFace: MODIFIED / UNCHANGED
    int operand = 0;                       // FromEdge, FromVertex
    std::vector<int> edges;                // FromEdge: logical edges of the input
    int vertex = 0;                        // FromVertex
};

struct History {
    std::vector<std::vector<Origin>> faceOrigins;  // index = FaceIdx
    std::vector<InputFace> deletedFaces;
};

struct OpResult {
    Shape shape;
    History history;
    std::vector<Error> warnings;
};

// ---------------------------------------------------------------- operation parameters

struct Extent {
    enum class Kind { Blind, Symmetric, ThroughAll, ThroughAllBoth };  // ThroughAllBoth — kernel-interface §4.5
    Kind kind = Kind::Blind;
    double depth = 0;
    Shape bounds;  // ThroughAll (1б)
};

struct Axis {
    Point3 origin = Point3::Zero();
    Vec3 direction = Vec3::UnitZ();
};

// Segment of a pipe sweep path (§3.26, FX7-PT-002): a line a → b or an arc counterclockwise about axis from a to b.
struct PathSegment {
    std::string id;
    bool arc = false;
    Point3 a = Point3::Zero(), b = Point3::Zero(), center = Point3::Zero();
    Vec3 axis = Vec3::UnitZ();
};

struct RevolveExtent {
    enum class Kind { Angle, Symmetric, Full };
    Kind kind = Kind::Full;
    double value = 0;
};

enum class BoolKind { Union, Subtract, Intersect };

// Rigid motion (with reflection) for transform (§3.12): p → r·p + t, r — row-major.
struct Motion {
    std::array<double, 9> r{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Vec3 t = Vec3::Zero();
};

// STEP model (§3.16): products — parts (shape) and assemblies (occurrences).
struct StepComponent {
    int product = 0;
    Motion motion;
    std::string name;
};
struct StepProduct {
    std::string name;
    std::optional<std::string> color;       // #RRGGBB
    std::optional<Shape> shape;              // part
    std::map<int, std::string> faceColors;  // FaceIdx → #RRGGBB (part)
    std::vector<StepComponent> components;  // assembly
};
struct StepModel {
    std::vector<StepProduct> products;
    int root = 0;
};

struct EdgeRef {
    int edge = 0;        // LogicalEdgeIdx
    std::string label;   // caller's label (stable name)
    double size = 0;     // > 0: own radius/distance for this edge instead of the common one (§3.5, FX5-FEAT-001)
    // Chamfer only (§3.6, FX7-PT-010): > 0 — second distance (the first is measured on the reference face) or the angle
    // to the reference face (rad); the reference face — the first face of the logical edge, the second with flip.
    double size2 = 0;
    double angle = 0;
    bool flip = false;
};

// ---------------------------------------------------------------- tessellation (§2.7)

struct TessParams {
    double chordTol = 0.01;
    double angleTol = 0.2;
};

struct FaceMesh {
    int face = 0;
    std::vector<float> positions;   // 3·n
    std::vector<float> normals;     // 3·n
    std::vector<uint32_t> triangles;  // 3·m, indices local to the face
};

struct LogicalEdgeMesh {
    int edge = 0;
    std::vector<std::vector<float>> polylines;  // 3·k each
};

struct VertexPoint {
    int vertex = 0;
    std::array<float, 3> position{};
};

struct Tessellation {
    std::vector<FaceMesh> faces;
    std::vector<LogicalEdgeMesh> edges;
    std::vector<VertexPoint> vertices;
    std::vector<int> failedFaces;
};

// ---------------------------------------------------------------- queries (§3.8)

struct Counts {
    int solids = 0, faces = 0, edges = 0, vertices = 0;
};

struct LogicalEdge {
    std::vector<int> edges;
    std::pair<int, int> faces{0, 0};
    std::vector<int> ends;  // 0 or 2 principal vertices
};

struct CanonicalTopology {
    std::vector<LogicalEdge> logicalEdges;
    std::vector<int> essentialVertices;
    std::vector<int> seams;
    Counts counts;
};

struct MassProperties {
    double volume = 0, area = 0;
    Point3 centroid = Point3::Zero();
    Eigen::Matrix3d inertia = Eigen::Matrix3d::Zero();  // relative to centroid, density 1 (mm⁵), S2-PART-003
};

struct SurfaceInfo {
    enum class Kind { Plane, Cylinder, Cone, Sphere, Torus, Other };
    Kind kind = Kind::Other;
    Point3 point = Point3::Zero();  // plane: point; cylinder: axisPoint; cone: apex; sphere/torus: center
    Vec3 dir = Vec3::UnitZ();       // plane: normal; otherwise axisDir
    double radius = 0;              // cylinder, sphere; torus: major
    double minorRadius = 0;         // torus
    double halfAngle = 0;           // cone
    bool convex = true;
    std::string otherKind;
};

struct CurveInfo {
    enum class Kind { Line, Circle, Other };
    Kind kind = Kind::Other;
    Point3 p1 = Point3::Zero(), p2 = Point3::Zero();
    Point3 center = Point3::Zero();
    Vec3 axis = Vec3::UnitZ();
    double radius = 0;
    bool closed = false;
    std::string otherKind;
};

// Projection of a logical edge onto a plane (§3.9), in the plane's local coordinates.
struct Curve2 {
    enum class Kind { Point, Line, Arc, Circle, Spline };  // Spline — S4A-SPL-004
    Kind kind = Kind::Point;
    Point2 p = Point2::Zero();                      // Point
    Point2 p1 = Point2::Zero(), p2 = Point2::Zero();  // Line; Spline: its ends (first and last pole)
    Point2 center = Point2::Zero();                 // Arc, Circle
    double radius = 0;
    Point2 start = Point2::Zero(), end = Point2::Zero();  // Arc: counterclockwise about plane.Z
    spline::Curve spline;                           // Spline: open clamped NURBS (sketch.md §2.7.1), poles in plane coordinates
};

// Point set of an edge or a face meridian (§3.8, FX7-DRW-010): exactly one of edge (logical edge) and face is ≥ 0; for a
// face — its points in the half-plane {A + t·a + ρ·half, ρ ≥ 0} through the axis (A, a) of its surface of revolution.
struct CurveSet {
    int edge = -1;
    int face = -1;
    Vec3 half = Vec3::Zero();
};

// Projection with hidden line removal (§3.17).
struct PlacedShape {
    Shape shape;
    Motion motion;  // rotation only (det r = +1)
};
struct ViewDirection {
    Vec3 toViewer = Vec3::UnitZ();  // toward the viewer
    Vec3 up = Vec3::UnitY();        // up in the view; right = up × toViewer
};
struct HlrParams {
    double chordTol = 0.01;  // mm: polyline tolerance
};
struct ViewCurve {
    enum class Kind { Line, Circle, Arc, Polyline };
    Kind kind = Kind::Line;
    Point2 p1 = Point2::Zero(), p2 = Point2::Zero();  // Line
    Point2 center = Point2::Zero();                   // Circle, Arc
    double radius = 0;
    double start = 0, end = 0;  // Arc: rad, counterclockwise from start to end; start ∈ (−π, π], end ∈ (start, start + 2π)
    std::vector<Point2> points;  // Polyline
};
struct HlrProjection {
    std::vector<ViewCurve> visible, hidden, visibleSmooth, hiddenSmooth;
    std::vector<Error> warnings;  // KERNEL_HLR_APPROXIMATE
};
// The outer contour of a projection is visible (§3.17 "Outer contour", FX7-DRW-001): a hidden or smooth curve lying on
// the outer boundary of the projection where no visible curve runs moves to visible. Implementation independent.
void promoteOuterContour(HlrProjection& p);
inline constexpr int KERNEL_HLR_CONTOUR_RASTER = 1024;   // pixels along the larger side of the projection extent
inline constexpr double KERNEL_HLR_CONTOUR_SHARE = 0.5;  // share of a curve's samples that must be uncovered contour
// Sectioning bodies with a plane (§3.18).
struct SectionRegion {
    int item = 0;                                // body index in the input
    std::vector<std::vector<Point2>> loops;      // [0] — outer (counterclockwise), then holes (clockwise)
};
struct SectionCut {
    std::vector<PlacedShape> bodies;    // remaining parts of bodies, world coordinates, identity placement
    std::vector<SectionRegion> regions;
};

struct ValidityReport {
    bool valid = true;
    std::vector<std::string> issues;
};

// Shape healing (§3.24; S4-REC-013): reducing the entities' own tolerances.
struct HealResult {
    Shape shape;                 // healed shape; healed = false — the original
    bool healed = false;
    double toleranceBefore = 0;  // largest own tolerance of the input's entities, mm
    double toleranceAfter = 0;   // the same for the result
};

// Body from surfaces (§3.22; S4-MESH-001): face description — a surface, loops of shared edges.
struct SurfaceSpec {
    SurfaceInfo::Kind kind = SurfaceInfo::Kind::Plane;  // Plane, Cylinder, Cone, Sphere, Torus
    Point3 point = Point3::Zero();  // plane: a point; cylinder: a point on the axis; cone: apex; sphere/torus: center
    Vec3 dir = Vec3::UnitZ();       // plane: normal; otherwise axis direction (the cone widens along dir)
    double radius = 0;              // cylinder, sphere; torus: major
    double minorRadius = 0;         // torus
    double halfAngle = 0;           // cone, (0, π/2)
};
struct CurveSpec {
    enum class Kind { Line, Circle, Interp };
    Kind kind = Kind::Line;
    Point3 center = Point3::Zero();  // Circle
    Vec3 axis = Vec3::UnitZ();       // Circle: arc from v1 to v2 counterclockwise about axis; v1 = v2 — full circle
    double radius = 0;
    std::vector<Point3> points;      // Interp: points in order from v1 to v2 (ends included); closed — without repeating the first
    int surfaceA = -1, surfaceB = -1;  // Interp: surfaces whose intersection is the curve (−1 — unknown)
};
struct EdgeSpec {
    CurveSpec curve;
    int v1 = 0, v2 = 0;  // vertex indices; v1 = v2 — closed edge
};
struct LoopEdge {
    int edge = 0;
    bool reversed = false;
};
struct FaceSpec {
    int surface = 0;
    std::vector<std::vector<LoopEdge>> loops;  // counterclockwise about the outward normal (outer), clockwise (holes)
    Point3 inside = Point3::Zero();            // a point inside the face (on the surface)
    Vec3 outward = Vec3::UnitZ();              // outward normal of the body at point inside
};
struct SolidSpec {
    std::vector<Point3> vertices;
    std::vector<SurfaceSpec> surfaces;
    std::vector<EdgeSpec> edges;
    std::vector<FaceSpec> faces;
    double tolerance = 1e-5;  // mm: face sewing
};
// Closed triangle mesh (§3.23): triangles counterclockwise when viewed from outside.
struct MeshSpec {
    std::vector<Point3> vertices;
    std::vector<std::array<int, 3>> triangles;
};

struct KernelInfo {
    std::string name, version;
    int contract = 1;
};

inline constexpr int KERNEL_CONTRACT_VERSION = 25; // 2 — Extent::ThroughAllBoth (S1-FEAT-006); 3 — transform (S2-FEAT-002); 4 — shell; 5 — draft; 6 — loft; 7 — hiddenLineProjection (S3-DRW-003); 8 — sectionCut (S3-DRW-005); 9 — rayHitsShape (FX1-FEAT-003); 10 — shell of the whole body (FX4-FEAT-002); 11 — shapesToBlob/shapesFromBlob (S4-CACHE-001); 12 — removeFaces, planeSection, distanceToShape (S4-REC-002); 13 — solidFromFaces, solidFromMesh (S4-MESH-001); 14 — healShape (S4-REC-013); 15 — per-edge fillet/chamfer size, edgeMidpoint (FX5-FEAT-001); 16 — spline profile segments (S4A-SPL-003); 17 — planeSection fits splines to lines/circles, healShape intersection by points (S4-REC-023); 18 — helicalSweep (S6-PRINT-002); 19 — projectEdge of B-spline edges (S4A-SPL-004); 20 — KERNEL_TOLERANCE_GROWTH only on growth over the operands (FX7-UI-008); 21 — extremePoint, curvePoints, faceSurface Other "revolution" with axis (FX7-DRW-010); 22 — pipeSweep, asymmetric chamfer, boolean retries (FX7-PT-002, FX7-PT-005, FX7-PT-010); 23 — planeSection splines (FX7-SH-004)

// ---------------------------------------------------------------- constants (§5)

inline constexpr double KERNEL_PROFILE_GAP_TOL = 1e-5;
inline constexpr double KERNEL_MIN_SEGMENT = 1e-4;
inline constexpr double KERNEL_MIN_DIMENSION = 1e-4;
inline constexpr int KERNEL_ELLIPSE_CHECK_CHORDS = 256;  // ellipse chords when checking profile intersections (§4.1, S4-SKT-004)
inline constexpr double KERNEL_SPLINE_CHECK_TOL = 1e-3;  // spline flattening tolerance (mm) for profile intersection checks (§4.1, S4A-SPL-003)
inline constexpr double KERNEL_MIN_ANGLE = 1e-6;
inline constexpr double KERNEL_FULL_TURN_TOL = 1e-9;
inline constexpr double KERNEL_AXIS_ANGLE_TOL = 1e-9;
inline constexpr double KERNEL_TANGENT_EDGE_ANGLE = 1e-6;
inline constexpr double KERNEL_MAX_COORD = 1e5;
inline constexpr double KERNEL_THROUGH_ALL_MARGIN_ABS = 1.0;  // mm (§5)
inline constexpr double KERNEL_THROUGH_ALL_MARGIN_REL = 0.01;
inline constexpr double KERNEL_MAX_ENTITY_TOL = 1e-3;
inline constexpr double KERNEL_ENTITY_TOL_WARN = 1e-4;
// KERNEL_TOLERANCE_GROWTH (§9.6, FX7-UI-008): the boolean raised the tolerance — the result's largest entity tolerance is
// above the warning threshold and above the largest tolerance of the operands (an imported body with a large tolerance
// does not warn on every operation).
inline bool toleranceGrew(double result, double operands) { return result > KERNEL_ENTITY_TOL_WARN && result > operands; }
inline constexpr double KERNEL_BOOLEAN_FUZZY = 1e-5;
inline constexpr double KERNEL_BOOLEAN_FUZZY_MAX = 5e-5;  // mm: the third boolean attempt (§9.6, FX7-PT-005)
inline constexpr double KERNEL_SECTION_FIT_TOL = 1e-5;  // mm: a section spline on a line or circle is that curve (§3.20, §9.6е)
inline constexpr int KERNEL_SECTION_FIT_SAMPLES = 32;   // samples of a section spline checked against the fitted curve (§9.6е)
inline constexpr double KERNEL_MASSPROP_REL = 1e-9;
inline constexpr double KERNEL_HELIX_MAX_TURNS = 500;    // §3.25
inline constexpr double KERNEL_HELIX_VOLUME_REL = 1e-3;  // §3.25: helical faces are spline approximations
inline constexpr double KERNEL_BBOX_TOL = 1e-6;
inline constexpr double KERNEL_EXTREME_TOL = 1e-8;   // mm: value P·dir of the extremePoint result (§3.8)
inline constexpr int KERNEL_EXTREME_SAMPLES = 32;     // samples per continuity interval in extremePoint (§9.9)
inline constexpr double KERNEL_DIR_SIGN_TOL = 1e-9;
inline constexpr double KERNEL_POINT_ORDER_TOL = 1e-6;
inline constexpr double KERNEL_CHORD_FACTOR = 2;
inline constexpr double KERNEL_MIN_CHORD = 1e-4;
inline constexpr double KERNEL_MAX_CHORD = 1e3;
inline constexpr double KERNEL_MIN_ANGLE_TOL = 0.01;
inline constexpr double KERNEL_HLR_MIN_LENGTH = 1e-6;    // mm: a projection piece shorter than this is not output (§3.17)
inline constexpr double KERNEL_HLR_PARALLEL_TOL = 1e-9;  // 1 − |axis·toViewer|: circle in the view plane (§3.17)
inline constexpr double KERNEL_HLR_POLY_ANGLE = 0.1;     // rad: angular triangulation tolerance of the approximate algorithm (§9.9б)
inline constexpr double KERNEL_SHELL_RIM_TOL = 1e-6;  // mm: a shell rim face lies on the surface of the removed face (§3.13)
inline constexpr double KERNEL_SHELL_MATCH_TOL = 1e-6;  // mm: a point of a face of the "erosion" shell result lies on a face of the body or the tool (§9.6б)
inline constexpr double KERNEL_SHELL_CONCAVE_TOL = 1e-6;  // an edge is concave if nB·dA is greater (§9.6б)
inline constexpr size_t KERNEL_MESH_UNIFY_MAX = 20000;  // triangles: above this — body from the mesh without merging coplanar ones (§3.23)

// ---------------------------------------------------------------- interface

// Failure inside a kernel query (§3.0а, S4-QA-004): an exception of the implementation (OCCT) turned into a standard
// one with the query name and the original exception text; queries return values, so they report it by throwing.
struct KernelFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Kernel {
public:
    virtual ~Kernel() = default;

    virtual Result<OpResult> extrude(const Profile& p, const Extent& extent, bool reverse) const = 0;
    virtual Result<OpResult> revolve(const Profile& p, const Axis& axis, const RevolveExtent& extent) const = 0;
    virtual Result<OpResult> boolean(BoolKind kind, const Shape& target, const Shape& tool,
                                     const Cancel* cancel = nullptr) const = 0;
    virtual Result<OpResult> transform(const Shape& s, const Motion& m) const = 0;  // §3.12
    virtual Result<OpResult> shell(const Shape& body, const std::vector<int>& removeFaces, double thickness, bool outward,
                                   const Cancel* cancel = nullptr) const = 0;  // §3.13
    virtual Result<OpResult> draft(const Shape& body, const std::vector<int>& faces, const Point3& neutralPoint, const Vec3& pull,
                                   double angle, const Cancel* cancel = nullptr) const = 0;  // §3.14
    virtual Result<OpResult> loft(const std::vector<Profile>& sections, const Cancel* cancel = nullptr) const = 0;  // §3.15
    // Helical sweep of a profile about an axis in its plane: right-hand, pitch per turn, turns (§3.25, S6-PRINT-002).
    virtual Result<OpResult> helicalSweep(const Profile& p, const Axis& axis, double pitch, double turns,
                                          const Cancel* cancel = nullptr) const = 0;
    // Sweep of a profile along a G0 chain of lines and arcs with mitred corners (§3.26, FX7-PT-002); closed when the
    // last segment ends at the start of the first.
    virtual Result<OpResult> pipeSweep(const Profile& p, const std::vector<PathSegment>& path,
                                       const Cancel* cancel = nullptr) const = 0;
    virtual Result<OpResult> fillet(const Shape& body, const std::vector<EdgeRef>& edges, double radius,
                                    const Cancel* cancel = nullptr) const = 0;
    virtual Result<OpResult> chamfer(const Shape& body, const std::vector<EdgeRef>& edges, double distance,
                                     const Cancel* cancel = nullptr) const = 0;
    virtual Result<Tessellation> tessellate(const Shape& body, const TessParams& params,
                                            const Cancel* cancel = nullptr) const = 0;

    // "Raw" topology (§3.8).
    virtual Counts counts(const Shape& s) const = 0;
    virtual std::vector<int> solidFaces(const Shape& s, int solid) const = 0;
    virtual std::vector<int> faceEdges(const Shape& s, int face) const = 0;
    virtual std::pair<int, int> edgeFaces(const Shape& s, int edge) const = 0;
    virtual std::pair<int, int> edgeVertices(const Shape& s, int edge) const = 0;
    virtual std::vector<int> vertexFaces(const Shape& s, int vertex) const = 0;
    virtual std::vector<int> vertexEdges(const Shape& s, int vertex) const = 0;

    // Measurements.
    virtual MassProperties massProperties(const Shape& s) const = 0;
    virtual BBox3 bbox(const Shape& s) const = 0;
    virtual double faceArea(const Shape& s, int face) const = 0;
    virtual Point3 faceCentroid(const Shape& s, int face) const = 0;
    virtual double edgeLength(const Shape& s, int edge) const = 0;
    virtual Point3 edgeCentroid(const Shape& s, int edge) const = 0;
    virtual Point3 edgeMidpoint(const Shape& s, int edge) const = 0;  // §3.8: point of kernel edge at the middle of its parameter range
    virtual Point3 vertexPoint(const Shape& s, int vertex) const = 0;
    virtual Vec3 faceNormalAt(const Shape& s, int face, const Point3& p) const = 0;
    virtual double distanceToFace(const Shape& s, int face, const Point3& p) const = 0;
    virtual Point3 closestPointOnFace(const Shape& s, int face, const Point3& p) const = 0;  // §3.8 (reference point for annotations)
    virtual double distanceToEdge(const Shape& s, int edge, const Point3& p) const = 0;
    // Ray p + t·d, t > tMin, intersects or touches face of s (§3.8; FX1-FEAT-003).
    virtual bool rayHitsShape(const Shape& s, const Point3& p, const Vec3& d, double tMin) const = 0;
    virtual SurfaceInfo faceSurface(const Shape& s, int face) const = 0;
    virtual CurveInfo edgeCurve(const Shape& s, int logicalEdge) const = 0;
    virtual Result<Curve2> projectEdge(const Shape& s, int logicalEdge, const Plane& plane) const = 0;  // §3.9
    // §3.8 (FX7-DRW-010): the point of the set with the greatest P·dir; polylines of the set within deflection.
    virtual Result<Point3> extremePoint(const Shape& s, const CurveSet& q, const Vec3& dir) const = 0;
    virtual Result<std::vector<std::vector<Point3>>> curvePoints(const Shape& s, const CurveSet& q, double deflection) const = 0;
    // Distance from a point to the shape boundary (§3.21; S4-REC-002): the minimum over faces, ≥ 0.
    virtual double distanceToShape(const Shape& s, const Point3& p) const = 0;

    // Projection of a scene of bodies with hidden line removal (§3.17).
    virtual Result<HlrProjection> hiddenLineProjection(const std::vector<PlacedShape>& items, const ViewDirection& view,
                                                       const HlrParams& params, const Cancel* cancel = nullptr) const = 0;
    // Sectioning a scene of bodies with the plane through origin ⊥ view.toViewer; the viewer's side is removed (§3.18).
    virtual Result<SectionCut> sectionCut(const std::vector<PlacedShape>& items, const ViewDirection& view, const Point3& origin,
                                          const HlrParams& params, const Cancel* cancel = nullptr) const = 0;

    // Removing faces with extension of the adjacent ones (§3.19; S4-REC-002): fillets, chamfers, holes.
    virtual Result<OpResult> removeFaces(const Shape& body, const std::vector<int>& faces, const Cancel* cancel = nullptr) const = 0;
    // Section of a shape by a plane (§3.20; S4-REC-002): section curves in the plane's local coordinates; splines — B-spline
    // section curves as Spline instead of KERNEL_NOT_SUPPORTED (FX7-SH-004).
    virtual Result<std::vector<Curve2>> planeSection(const Shape& s, const Plane& plane, bool splines = false) const = 0;
    // Body from surfaces and edges (§3.22; S4-MESH-001) and a polyhedral body from a closed mesh (§3.23).
    virtual Result<Shape> solidFromFaces(const SolidSpec& spec, const Cancel* cancel = nullptr) const = 0;
    virtual Result<Shape> solidFromMesh(const MeshSpec& mesh, const Cancel* cancel = nullptr) const = 0;

    virtual ValidityReport checkValidity(const Shape& s) const = 0;
    // Healing a shape whose entity tolerances exceed KERNEL_MAX_ENTITY_TOL but not maxTolerance (§3.24; S4-REC-013).
    virtual Result<HealResult> healShape(const Shape& s, double maxTolerance) const = 0;
    // Faces of one surface and edges of one curve merged (§3.27, FX10-REC-003); optional: by default the shape as it is.
    virtual Result<Shape> unifyFaces(const Shape& s) const { return s; }
    virtual OpResult combine(const std::vector<Shape>& shapes) const = 0;
    virtual std::vector<std::pair<Shape, std::vector<int>>> components(const Shape& s) const = 0;
    virtual KernelInfo info() const = 0;

    // Exchange (§3.16).
    virtual Result<std::string> shapeToBrep(const Shape& s) const = 0;
    virtual Result<Shape> shapeFromBrep(const std::string& text) const = 0;
    // Dumping shapes for the disk cache (§3.16а): whole shapes, shared subshapes stay shared.
    virtual Result<std::string> shapesToBlob(const std::vector<Shape>& shapes) const = 0;
    virtual Result<std::vector<Shape>> shapesFromBlob(const std::string& blob) const = 0;
    virtual Status writeStep(const std::string& path, const StepModel& model) const = 0;
    virtual Result<StepModel> readStep(const std::string& path) const = 0;
};

// ---------------------------------------------------------------- common algorithms (kernel module, §4)

// Parameter origin (seam) of surfaces of revolution and circles in solidFromFaces (§3.22): the world axis least
// parallel to dir, orthogonalized against dir (on equal component magnitudes — the rule of §3.22).
Vec3 seamDirection(const Vec3& dir);

// Profile check (§4.1). On success normalized is the profile in canonical order and orientation.
Result<ProfileInfo> validateProfile(const Profile& p, Profile* normalized = nullptr);

// Canonical topology (§4.3) on top of the "raw" queries.
CanonicalTopology canonicalTopology(const Kernel& k, const Shape& s);

// "Through all" depth (§4.5): from a point of the profile plane along the extrude direction to the far corner of
// bbox(bounds) plus a margin; if the body lies entirely behind the plane — only the margin.
double throughAllDepth(const Kernel& k, const Shape& bounds, const Point3& origin, const Vec3& direction);

// Geometric key of a logical edge (§4.4).
Point3 logicalEdgeKey(const Kernel& k, const Shape& s, const LogicalEdge& e);

// Axial extent of a face (§4.6): the range [t0, t1] of (center − p)·a over the centers of the face's circular logical
// edges; nullopt — fewer than two such edges or t1 ≤ t0.
std::optional<std::pair<double, double>> axialRange(const Kernel& k, const Shape& sh, int face, const Point3& p, const Vec3& a);

}  // namespace construe::kernel
