// Sheet metal part: sheet model, folding, feature builders, piece bodies and flat pattern (spec/sheet.md).
#pragma once

#include "base/cancel.h"
#include "base/error.h"
#include "base/math.h"
#include "base/spline.h"
#include "kernel/kernel.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace construe::sheet {

// ---------------------------------------------------------------- constants (§5)

inline constexpr double SHEET_DEFAULT_K = 0.44;
inline constexpr double SHEET_RELIEF_WIDTH_FACTOR = 1;
inline constexpr double SHEET_RELIEF_DEPTH_FACTOR = 1;
inline constexpr double SHEET_DEFAULT_GAP = 0.5;      // mm
inline constexpr double SHEET_MIN_THICKNESS = 1e-3;   // mm
inline constexpr double SHEET_MIN_LENGTH = 1e-3;      // mm
inline constexpr double SHEET_MIN_BA = 1e-3;          // mm
inline constexpr double SHEET_ANGLE_TOL = 1e-6;       // rad
inline constexpr double SHEET_TANGENT_TOL = 1e-6;     // rad
inline constexpr double SHEET_JOIN_TOL = 1e-6;        // mm
inline constexpr double SHEET_MATCH_TOL = 1e-5;       // mm
inline constexpr double SHEET_PARALLEL_TOL = 1e-9;    // 1 − |n1·n2|
inline constexpr double SHEET_TOOL_MARGIN = 0.01;     // mm
inline constexpr int SHEET_BEND_CUT_SLICES = 16;
inline constexpr double SHEET_AREA_REL_TOL = 1e-6;
inline constexpr double SHEET_OVERLAP_TOL = 1e-3;     // mm²
inline constexpr int SHEET_TAB_PROBE = 8;             // probe grid of a tab overlap (§4.6б)
inline constexpr double SHEET_SPLINE_FLAT_TOL = 1e-4; // mm: polyline of a spline segment in point queries (§2.2)
inline constexpr double SHEET_REC_TOL = 1e-4;         // mm: recognition distances (§4.9)
inline constexpr double SHEET_REC_ANGLE = 1e-6;       // recognition: parallel / perpendicular (§4.9)
inline constexpr double SHEET_REC_MATCH = 1e-6;       // recognition: relative volume and area match (§4.9 item 12)

// ---------------------------------------------------------------- model (§2)

enum class Relief { Rect, Round, None };
enum class Position { Inside, Outside, Edge };
enum class Side { Inside, Outside, Mid };

struct Params {
    double thickness = 0;  // T
    double radius = 0;     // R
    double kFactor = SHEET_DEFAULT_K;
    Relief relief = Relief::Rect;
    double reliefWidth = 0, reliefDepth = 0;  // ≤ 0: defaults (§2.1)
    double widthOfRelief() const { return reliefWidth > 0 ? reliefWidth : SHEET_RELIEF_WIDTH_FACTOR * thickness; }
    double depthOfRelief() const { return reliefDepth > 0 ? reliefDepth : SHEET_RELIEF_DEPTH_FACTOR * thickness; }
};

// Segment of a flat contour (§2.2); label — base name of the side face it generates.
struct Seg {
    enum class Kind { Line, Arc, Circle, Spline };
    Kind kind = Kind::Line;
    Point2 a = Point2::Zero(), b = Point2::Zero();  // Line: a → b; Arc: start, end (counterclockwise about +z); Spline: ends
    Point2 c = Point2::Zero();                      // Arc, Circle
    double r = 0;
    bool rev = false;  // Arc, Spline: the loop traverses it from b to a (against the natural direction)
    std::string label;
    spline::Curve curve;  // Spline: open clamped NURBS in flat coordinates (FX7-SH-004)
};
using Loop = std::vector<Seg>;
struct Region {
    Loop outer;
    std::vector<Loop> holes;
};

struct PieceOp {
    enum class Kind { Subtract, Intersect, Add };
    Kind kind = Kind::Subtract;
    Region region;
    std::vector<Region> minus;  // Add: material = region minus these (FX7-SH-004, §2.3)
};

struct Piece {
    enum class Kind { Wall, Bend };
    Kind kind = Kind::Wall;
    std::string feature;
    int parent = -1;
    Region region;
    std::vector<PieceOp> ops;
    std::string faceA, faceB;
    // Bend zone (§2.3).
    Point2 origin = Point2::Zero();
    Vec2 axis = Vec2::UnitY(), out = Vec2::UnitX();
    double w0 = 0, w1 = 0, ba = 0, angle = 0, radius = 0;
    bool up = true;
    std::string faceIn, faceOut, faceL, faceR, faceS, faceE;
};

struct Cut {
    Region region;
    std::string feature;
    int wall = -1;  // wall whose fold mapped the region to flat coordinates (§2.3); -1 — unknown
};

// Fillet or chamfer of a corner through the thickness (§4.6г, FX8-SH-004).
struct Corner {
    enum class Kind { Fillet, Chamfer };
    int piece = -1;                  // wall
    Point2 at = Point2::Zero();      // corner: projection of the edge on z = 0
    Kind kind = Kind::Fillet;
    double size = 0, size2 = 0, angle = 0;
    bool flip = false;
    Vec2 ref = Vec2::UnitX();        // normal of the chamfer reference face in flat coordinates
    std::string label, edge, feature;  // base of the rounded face; stable edge name (messages); feature ID
};

// Flange edge on a wall in flat coordinates (§4.4 item 1).
struct EdgeOnWall {
    int piece = -1;
    bool onB = false;
    Point2 q1 = Point2::Zero(), q2 = Point2::Zero();  // ordered along axis = out rotated by +90°
    Vec2 out = Vec2::UnitX();
    std::string name;  // stable edge name (for messages)
};
struct FlangeSpec {
    double length = 0, angle = 0, radius = 0, offsetStart = 0, offsetEnd = 0, gap = SHEET_DEFAULT_GAP;
    Position position = Position::Inside;
    bool reverse = false;
    Relief relief = Relief::Rect;
};
// Flange as built (§2.3): the source of pattern and mirror copies (§4.6д, FX8-SH-005).
struct FlangeRec {
    std::string feature;
    FlangeSpec spec;
    std::vector<EdgeOnWall> edges;
};

struct Model {
    Params params;
    Transform3 root = Transform3::Identity();
    std::vector<Piece> pieces;
    std::vector<Cut> cuts;
    std::vector<Corner> corners;      // §4.6г
    std::vector<FlangeRec> flanges;   // flanges as built — sources of pattern and mirror copies (§4.6д)
};

struct BendLine {
    Point2 a = Point2::Zero(), b = Point2::Zero();
    bool up = true;
    double angle = 0, radius = 0;
    std::string feature;
};

struct FlatPattern {
    std::vector<kernel::Curve2> curves;
    std::vector<BendLine> bends;
    double area = 0;
};

// ---------------------------------------------------------------- formulas (§4.1, §4.2)

double bendAllowance(double angle, double radius, const Params& p);
double setback(Position pos, double angle, double radius, double thickness);
Transform3 bendMotion(const Piece& bend, double thickness);
Transform3 fold(const Model& m, int piece);
// Rectangle of a bend zone: s ∈ [s0, s1] from the zone origin along out, w ∈ [w0, w1] along axis, labels by side.
Region bendRect(const Point2& origin, const Vec2& axis, const Vec2& out, double w0, double w1, double s0, double s1,
                const std::string& atS0, const std::string& atS1, const std::string& atW0, const std::string& atW1);

// ---------------------------------------------------------------- regions

// Profile in world coordinates → flat region via toFlat; label(segment id) — base of the side face. nullopt — an
// ellipse segment (not supported in a sheet).
std::optional<Region> regionFromProfile(const kernel::Profile& p, const Transform3& toFlat,
                                        const std::function<std::string(const std::string&)>& label);
// Point inside the region (or on its boundary within tol).
bool insideRegion(const Region& r, const Point2& p, double tol);
// Point inside the piece: its contour changed by the ops in order (subtract, intersect, add).
bool insidePiece(const Piece& piece, const Point2& p, double tol);
// Parameters w where the line origin + w·dir crosses the region boundary, sorted (§4.7 item 2).
std::vector<double> lineCrossings(const Region& r, const Point2& origin, const Vec2& dir);

// ---------------------------------------------------------------- builders (§4.3–§4.6)

// Base wall from a closed region (profile in world coordinates on the sketch plane frame).
Status addPlate(Model& m, const kernel::Profile& profile, const Plane& frame, bool reverse, const std::string& feature);

// Curve of an open base wall profile in sketch coordinates; arc — counterclockwise from a to b.
struct ChainCurve {
    std::string id;
    bool arc = false;
    Point2 a = Point2::Zero(), b = Point2::Zero(), c = Point2::Zero();
    double r = 0;
};
// Element of an ordered chain: traversed from a to b; arc sweep signed (counterclockwise +).
struct ChainElem {
    std::string id;
    bool arc = false;
    Point2 a = Point2::Zero(), b = Point2::Zero(), c = Point2::Zero();
    double r = 0, sweep = 0;
};
Result<std::vector<ChainElem>> orderChain(const std::vector<ChainCurve>& curves);
Status addOpenWall(Model& m, const std::vector<ChainElem>& chain, const Plane& frame, Side side, double width, bool reverse,
                   const std::string& feature);

// Edge between the faces with bases baseA, baseB: endpoints p1, p2 and the side face normal at the midpoint, in world.
std::optional<EdgeOnWall> edgeOnWall(const Model& m, const std::string& baseA, const std::string& baseB, const Point3& p1,
                                     const Point3& p2, const Vec3& sideNormalA, const Vec3& sideNormalB);
Status addFlanges(Model& m, const std::vector<EdgeOnWall>& edges, const FlangeSpec& spec, const std::string& feature);

// Wall whose A or B plane coincides with the plane and whose contour contains the probe (§4.5 item 1).
std::optional<int> wallOnPlane(const Model& m, const Plane& plane, const Point3& probe);
// Wall parallel to the normal whose contour contains the probe projection, nearest along the normal (§4.6).
std::optional<int> wallUnder(const Model& m, const Vec3& normal, const Point3& probe);
// Walls parallel to the normal whose contour contains the probe projection and whose middle lies at a signed offset
// along the normal within [lo, hi] (extrusion cut in a sheet part, §4.6а, FX7-SH-001); by piece index.
std::vector<int> wallsAlong(const Model& m, const Vec3& normal, const Point3& probe, double lo, double hi);
// Tab (§4.6б, FX7-SH-002): a wall piece in the flat coordinates of wall `wall`, folded with it (parent — the wall),
// sides A/B — those of the wall.
void addTab(Model& m, int wall, Region region, const std::string& feature);
// Sketch bend (§4.5): line q + t·a in flat coordinates of wall `wall`.
Status addLineBend(const kernel::Kernel& k, Model& m, int wall, const Point2& q, const Vec2& a, bool fixedLeft, bool up,
                   double angle, double radius, const std::string& feature);
void addCut(Model& m, Region region, const std::string& feature, int wall = -1);
// Corner through the thickness (§4.6г item 1, FX8-SH-004): the wall and the corner point of a straight edge p1–p2 (world);
// nullopt — the edge is not through the thickness of a wall.
struct CornerAt {
    int piece = -1;
    Point2 at = Point2::Zero();
};
std::optional<CornerAt> cornerOfEdge(const Model& m, const Point3& p1, const Point3& p2);
// Pattern and mirror copy (§4.6д, FX8-SH-005): the contribution of feature `source` in m0 moved by X (rigid motion or
// reflection) into m; relabel maps every label of the contribution; F — the copying feature, instance — its label (for
// errors). Errors: SHEET_PATTERN_SOURCE, SHEET_PATTERN_NO_WALL, REGEN_INPUT_ERROR (no contribution).
Status copyContribution(Model& m, const Model& m0, const std::string& source, const Transform3& X,
                        const std::function<std::string(const std::string&)>& relabel, const std::string& F,
                        const std::string& instance);
// Gear rim on a wall end (§4.6в, FX7-SH-004): the wall whose normal is parallel to the axis and whose end holds the face
// point (world); nullopt — none.
std::optional<int> wallOfRim(const Model& m, const Vec3& axis, const Point3& facePoint);
// «Лист из тела» ("Sheet from body") (§4.9, S5-SHEET-006): the sheet model of body S (face bases by FaceIdx); base — K and relief; the fields
// thickness / radius, if given. Error SHEET_RECOGNIZE: detail, face — FaceIdx of the place (if any), actual.
Result<Model> recognize(const kernel::Kernel& k, const kernel::Shape& S, const std::vector<std::string>& bases, const Params& base,
                        std::optional<double> thicknessField, std::optional<double> radiusField, const std::string& feature);

// ---------------------------------------------------------------- bodies (§4.7) and flat pattern (§4.8)

// Bases of a boolean result from the operands' bases (naming rule — provided by regen).
using BooleanBases = std::function<std::vector<std::string>(const kernel::OpResult&, const std::vector<std::vector<std::string>>&)>;
struct Built {
    kernel::Shape shape;
    std::vector<std::string> bases;
    double volume = 0;
};
// Bodies of the pieces: folded (world) or flat pattern (fold F₀). Pieces without a body (zero zone) are skipped.
// missedCorners — indices of corners whose edges were not found on their piece (§4.6г item 3).
Result<std::vector<Built>> buildPieces(const kernel::Kernel& k, const Model& m, bool flat, const BooleanBases& boolBases,
                                       const Cancel* cancel = nullptr, std::vector<int>* missedCorners = nullptr);
std::vector<BendLine> bendLines(const Model& m);
// Contour of the flat pattern body (world coordinates, fold F₀) and bend lines.
Result<FlatPattern> flatPattern(const kernel::Kernel& k, const Model& m, const kernel::Shape& flatBody);

}  // namespace construe::sheet
