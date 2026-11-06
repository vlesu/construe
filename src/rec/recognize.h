// Recognition of the construction history of an imported body (spec/reconstruction.md §2–§4). A pure function over
// a kernel body: removes finishing operations from last to first and decomposes the core into extrusions or a revolution.
#pragma once

#include "base/cancel.h"
#include "base/error.h"
#include "base/math.h"
#include "kernel/kernel.h"

#include <optional>
#include <string>
#include <vector>

namespace construe::rec {

// Constants (reconstruction.md §7).
inline constexpr double REC_LIN_TOL = 1e-6;       // mm
inline constexpr double REC_ANGLE_TOL = 1e-6;     // rad
inline constexpr double REC_SMOOTH_ANGLE = 1e-3;  // rad: smooth edge
inline constexpr double REC_HOLE_TOL = 1e-3;      // mm: hole diameters and depths against the table
inline constexpr double REC_MATCH_REL = 1e-6;     // match check: volume and area (§6)
inline constexpr double REC_MATCH_DIST = 1e-3;    // mm: match check, point deviation (§6)
inline constexpr double REC_MATCH_SKIN = 1e-5;    // mm: match check, volume and area: faces may shift by this much (§6)
inline constexpr double REC_REMOVE_MIN_REL = 1e-9;  // volume fraction: removal leaving the body unchanged is no removal (§4.1)
inline constexpr double REC_FILL_GROW = 1e-3;     // mm: radial allowance of the hole-filling tool (§4.4.3)
inline constexpr double REC_AXIS_TOL_FACTOR = 2;  // axis comparisons: × the body's own tolerance (§2.1)
inline constexpr double REC_ROUND_SPREAD = 4;     // match check with rounded dimensions: deviation ≤ REC_MATCH_DIST + this·tolerance (§6)
inline constexpr int TEXT_MIN_LOOPS = 3;          // lettering: minimum number of letter outlines (§4.7)
inline constexpr double TEXT_MAX_LOOP_REL = 0.25; // lettering: max letter outline size, fraction of the body diagonal (§4.7)
inline constexpr double TEXT_DISTINCT_REL = 1e-3; // lettering: outline length spread, fraction of the largest length (§4.7)
inline constexpr double TEXT_FIT_TOL_REL = 2e-3;  // text fit: outline deviation, fraction of the letter height (§4.7а)
inline constexpr double TEXT_FIT_TOL_MIN = 1e-3;  // mm: text fit, the smallest allowed outline deviation (§4.7а)
inline constexpr double TEXT_MATCH_REL = 0.15;    // text fit: glyph contour centroid search radius, fraction of the height (§4.7а)
inline constexpr double TEXT_AREA_REL = 0.2;      // text fit: glyph contour area spread (§4.7а)
inline constexpr double TEXT_ERR_TIE = 0.02;      // text fit: glyph misfits closer than this are equal (§4.7а)
inline constexpr double TEXT_SNAP_REL = 0.25;     // text fit: round height/anchor/angle within this share of the tolerance (§4.7а)
inline constexpr int TEXT_MAX_SPACES = 3;         // text fit: consecutive spaces between letters (§4.7а)
inline constexpr double REC_DRAFT_MAX = 0.2618;   // rad (15°): the largest draft angle recognized as a draft (§4.5 item 1а)
inline constexpr double REC_RULED_TOL = 1e-6;     // spline wall: |normal · d| at the face samples (§2.1, S4A-SPL-004)
inline constexpr double REC_DUP_REL = 1e-5;       // identical bodies: volume, area, principal moments (§5.7 item 1)
inline constexpr double REC_DUP_DIST = 1e-3;      // mm: identical bodies, point and distance coincidence (§5.7 items 2–3)
inline constexpr int REC_DUP_MAX_CAND = 256;      // identical bodies: rotation candidates per pair (§5.7 item 2)
// Wall-time limits are written for an optimized build; a Debug build is about ten times slower and the same body must
// give the same plan there (§8).
#ifdef CONSTRUE_OPTIMIZED_BUILD
inline constexpr double REC_TIME_SCALE = 1;
#else
inline constexpr double REC_TIME_SCALE = 8;
#endif
inline constexpr double REC_REMOVE_MAX_SECONDS = 30;  // one kernel face removal: wall time, then it counts as not removed (§3)
inline constexpr int REC_HOLE_RAYS = 8;           // rays on the hole circle at 0.95·r, besides the axis, for a cavity beyond a through hole (§4.4.1 item 5)
inline constexpr int REC_MAX_REMOVALS = 600;      // face removal and hole filling attempts per body (§3, S4-REC-023)
inline constexpr int REC_REORDER_VARIANTS = 2;    // plans with another order of the finishing (§5.5 item 1a, FX10-REC-003)
inline constexpr double REC_CONE_AXIS_TOL = 1e-3;   // mm: a cone's apex off the axis of a revolution (§2.1), = REC_MATCH_DIST
inline constexpr double REC_CONE_ANGLE_TOL = 1e-4;  // rad: a cone's direction off the axis of a revolution (§2.1)
inline constexpr long REC_SEARCH_MAX_WORK = 60000;  // hypothesis tree: kernel attempts × faces per body (§4.10 item 7)
inline constexpr long REC_SEARCH_AFTER_PLAN = 2;    // hypothesis tree: work allowed after the first complete plan, in multiples of the work before it (§4.10 item 7)
inline constexpr int REC_SEARCH_MAX_FACES = 300;    // hypothesis tree: a body with more faces is not searched (§4.10 item 1)
inline constexpr double REC_SEARCH_MAX_SECONDS = 15;  // hypothesis tree: wall time per body, for bodies on which kernel calls are slow (§4.10 item 7)
inline constexpr int REC_SEARCH_MAX_NODES = 200;    // hypothesis tree: bodies expanded per body (§4.10 item 7)
inline constexpr int REC_SEARCH_MAX_GROUPS = 30;    // hypothesis tree: face groups tried per body (§4.10 item 2)
inline constexpr int REC_SEARCH_MAX_DIRS = 6;       // hypothesis tree: directions and axes tried per body (§4.10 item 2)
inline constexpr int REC_SEARCH_MAX_PIECES = 64;    // hypothesis tree: solids of one removed volume (§4.10 item 4)
inline constexpr int REC_SEARCH_ENVELOPE_ANGLES = 36;  // hypothesis tree: half-planes tried for a revolution envelope (§4.10 item 2 в)
inline constexpr double REC_SEARCH_ENVELOPE_AREA = 0.3;  // hypothesis tree: share of the body's area on an axis that may carry an envelope (§4.10 item 2 в)
inline constexpr int REC_SEARCH_MAX_FACE_TOOLS = 24;  // hypothesis tree: planar faces tried as a pocket bottom or a boss top (§4.10 item 2 д)
inline constexpr int REC_SEARCH_ENVELOPES = 4;      // hypothesis tree: extrusion envelopes per direction (§4.10 item 2 в)
inline constexpr int REC_SEARCH_MAX_RISE = 4;       // hypothesis tree: a child may be this much less simple than its parent (§4.10 item 6)
inline constexpr double REC_SEARCH_CURVE_COST = 0.1;  // hypothesis tree: plan cost of a sketch curve, an operation is 1 (§4.10 item 5)

// Geometric key of a logical edge (§2.2): key from kernel-interface §4.4, length, curve kind.
struct EdgeKey {
    Point3 key = Point3::Zero();
    double length = 0;
    kernel::CurveInfo::Kind kind = kernel::CurveInfo::Kind::Other;
    // Connected pieces of one curve merged into this key (§4.2 item 3), each with its own key; empty for a single edge.
    // The built part has the whole edge or, on a base body, the same pieces (§5.3).
    std::vector<EdgeKey> pieces;
};

struct Op {
    enum class Kind { Extrude, Revolve, Hole, CircleCut, Pattern, Mirror, Chamfer, Fillet, Draft, Loft };
    Kind kind = Kind::Extrude;
    // Extrude, Revolve, Hole, CircleCut: sketch plane (normal per §2.2), curves in its local coordinates;
    // extrusion Pattern (§4.8): target plane and loops of all instances.
    Plane plane;
    std::vector<kernel::Curve2> curves;
    // Extrude: depth, operation, direction.
    double depth = 0;
    bool cut = false;      // Extrude: cut (otherwise add)
    bool reverse = false;  // Extrude: against the plane normal
    // Revolve: axis (world); sector — the full angle of a sector around the sketch plane (§4.5 item 2), 0 — a full turn.
    Point3 axisOrigin = Point3::Zero();
    Vec3 axisDir = Vec3::UnitY();
    double sector = 0;
    // Hole, CircleCut: points (world) on the entry plane; material is against the plane normal.
    std::vector<Point3> points;
    std::string holeKind;  // simple | counterbore | countersink | tapped
    std::string size, fit;
    double pitch = 0;      // tapped: 0 means coarse pitch
    double radius = 0;     // CircleCut
    bool through = false;  // Hole, CircleCut; Extrude cut: through cut on a body of revolution (§4.6)
    bool both = false;     // CircleCut, Extrude cut: through in both directions from the plane
    // Pattern, Mirror.
    int target = -1;                // index of the Op in the plan
    struct Direction {
        Vec3 dir = Vec3::UnitX();   // unit vector along a built-in axis
        int count = 1;
        double spacing = 0;
    };
    std::vector<Direction> linear;  // Pattern linear
    bool circular = false;          // Pattern circular: axis axisOrigin/axisDir, count
    int count = 0;
    std::string mirrorPlane;        // Mirror: plane:front | plane:top | plane:right
    // Fillet, Chamfer.
    double size2 = 0;               // radius / distance
    std::vector<EdgeKey> edges;
    // Draft (§4.5 item 1а): plane — the neutral plane (normal = pull direction unless reverse); points/normals — a point
    // and the outward normal of each face before the draft (on the neutral plane); angle.
    std::vector<Vec3> normals;
    double angle = 0;
    // Loft (§4.9): the second section (plane and curves); the first is plane/curves.
    Plane plane2;
    std::vector<kernel::Curve2> curves2;
    // Extrude of a lettering (§4.7а): an editable sketch text instead of the letter outlines (curves stay as the fallback).
    struct Text {
        std::string text, font;
        double height = 0;                  // capital height, mm
        Point2 anchor = Point2::Zero();     // baseline start, plane local coordinates
        double angle = 0;                   // baseline direction in the plane, rad
    };
    std::optional<Text> text;
    // Extrude of one level with another depth (§4.11 item 5): sketchOf — index of the earlier extrusion whose sketch this
    // one is built on (plane and curves are then in that operation's coordinates), -1 — its own sketch; shared — curves of
    // the later extrusions drawn in this operation's sketch (each feature takes the regions inside its own curves).
    int sketchOf = -1;
    std::vector<kernel::Curve2> shared;
};

struct Note {
    std::string kind;    // fillet | chamfer | hole | text | base | multiSolid | limit | verify
    std::string detail;
};

struct Plan {
    std::vector<Op> ops;              // in construction order
    kernel::Shape core;               // body core after removal of finishing operations
    std::string baseKind = "none";    // none | extrude | revolve
    size_t baseOps = 0;               // number of leading ops forming the base
    // Blocks (§5.5): after[i] is the body after the block of operation i (base: the core; holes: the body before
    // hole removal; fillets/chamfers: the body before their step removal); resume[i]: first operation of the next block.
    std::vector<kernel::Shape> after;
    std::vector<size_t> resume;
    std::vector<Note> notes;
};

// §4.11 item 5: point q (plane coordinates) inside the closed loops of curves — even-odd rule for a ray along +x; arcs,
// circles and splines as polylines with chord tolerance REC_LIN_TOL·100.
bool insideCurves(const std::vector<kernel::Curve2>& curves, const Point2& q);

// §5.5 item 1a (FX10-REC-003): plans with another order of the finishing — the kernel builds fillets meeting at a corner in
// one order and not in the other. For the failed fillet or chamfer `failed` in a run of at least two fillets and chamfers:
// the run reversed, then the failed one first (the rest in order). At most REC_REORDER_VARIANTS plans; a plan whose pattern
// or shared-sketch reference would point forward is left out.
std::vector<Plan> finishingReordered(const Plan& plan, size_t failed);

// §3: recognition. body is the shape of the imported body; cancel interrupts it (the plan is then incomplete, the caller
// discards it; core-api §4.7з).
Plan recognize(const kernel::Kernel& k, const kernel::Shape& body, const Cancel* cancel = nullptr);

// §6: body match (volume, area, point deviation in both directions); roundTol > 0 widens the tolerances for rounding
// of dimension values (§5.4).
bool bodiesMatch(const kernel::Kernel& k, const kernel::Shape& a, const kernel::Shape& b, double roundTol = 0);

// §5.7 (FX7-IO-001): a proper rigid motion M with M(a) = b (mirror copies never match), or none.
std::optional<kernel::Motion> congruentMotion(const kernel::Kernel& k, const kernel::Shape& a, const kernel::Shape& b);

// §5.7: groups of identical shapes. For each shape in order: index of its group's representative (the first shape of
// the group) and the motion of the representative onto it (identity for the representative).
struct CongruenceEntry {
    int group = 0;
    kernel::Motion motion;
};
std::vector<CongruenceEntry> groupCongruent(const kernel::Kernel& k, const std::vector<kernel::Shape>& shapes);

// §5.4: the value rounded to the coarsest "round" step (1·10ⁿ or 5·10ⁿ) for which the shift ≤ tol; a nonzero
// value is never rounded to zero; tol ≤ 0 leaves the value unchanged.
double roundValue(double v, double tol);

// Canonical axis direction (§2.1).
Vec3 canonicalAxis(const Vec3& a);

}  // namespace construe::rec
