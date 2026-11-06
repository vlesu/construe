// Spur involute gears: basic rack, gear geometry, tooth outline of the cutting tool section (spec/gear.md).
// Pure functions: no document, kernel or regeneration knowledge.
#pragma once

#include "base/error.h"
#include "base/math.h"
#include "base/spline.h"

#include <string>
#include <vector>

namespace construe::gear {

inline constexpr int GEAR_MIN_TEETH = 5;            // external gear, fewer — GEAR_BAD_VALUE (§5)
inline constexpr int GEAR_MIN_TEETH_INTERNAL = 20;  // internal gear (§5)
inline constexpr int GEAR_MAX_TEETH = 1000;         // §5
inline constexpr double GEAR_MAX_SHIFT = 0.8;       // |x| at most (§5)
inline constexpr double GEAR_MIN_TIP_FACTOR = 0.2;  // tooth tip thickness below 0.2·m — GEAR_THIN_TIP warning (§5)
inline constexpr int GEAR_BEZIER_DEGREE = 9;        // flank and fillet curves: single Bezier, starting degree (§4.4)
inline constexpr int GEAR_BEZIER_MAX_DEGREE = 15;   // highest degree tried (§4.4)
inline constexpr int GEAR_FIT_SAMPLES = 200;        // deviation check samples per curve (§4.4)
inline constexpr double GEAR_FIT_TOL = 1e-8;        // × m, mm: target deviation of the fitted curve (§4.4)
inline constexpr int GEAR_SCAN_POINTS = 400;        // samples of the generating curves when looking for undercut (§4.2)
inline constexpr double GEAR_JOIN_TOL = 1e-10;      // rad: fillet inside the involute space (§4.2)
inline constexpr double GEAR_ANGLE_TOL = 1e-9;      // rad: teeth count in a sector, full turn (§4.5)
inline constexpr double GEAR_FILLET_SHRINK = 0.7;   // internal gear: fillet radius step for a narrow root (§4.3)
inline constexpr int GEAR_FILLET_STEPS = 8;         // internal gear: radius reductions at most (§4.3)
inline constexpr int GEAR_TIP_SHIFT_HALVINGS = 6;   // tool tip shift halvings before dropping it (§4.5)

// Basic rack (§2.1): ISO 53 profile A = GOST 13755-2015. Coefficients are multiples of the module.
struct BasicRack {
    double alpha;  // profile angle, rad
    double ha;     // addendum coefficient ha*
    double c;      // bottom clearance coefficient c*
    double rho;    // root fillet radius coefficient ρf*
    const char* designation;  // for the parameter table (drawing.md §6.20)
};
const BasicRack& basicRack();

// inv a = tan a − a.
double involute(double a);

struct Params {
    double m = 1;      // module, mm
    int z = 20;        // number of teeth
    double x = 0;      // profile shift coefficient
    bool internal = false;
};

// Gear geometry (§3.1). Diameters and arcs in mm.
struct Geometry {
    double d = 0;    // reference (pitch) diameter m·z
    double db = 0;   // base diameter d·cos α
    double da = 0;   // tip diameter
    double df = 0;   // root diameter
    double p = 0;    // pitch π·m
    double s = 0;    // tooth thickness on the reference circle (arc)
    double e = 0;    // space width on the reference circle (arc)
    double sa = 0;   // tooth thickness on the tip circle (arc)
    double rForm = 0;  // radius where the involute meets the fillet (§4.2, §4.3)
    double zMin = 0;   // classical undercut limit 2(ha* − x)/sin²α (external)
    bool undercut = false;
    std::vector<Error> warnings;  // GEAR_UNDERCUT, GEAR_THIN_TIP, GEAR_TIP_BELOW_BASE, GEAR_FILLET_REDUCED (§7)
};
Result<Geometry> geometry(const Params& p);

// Half-angle of the involute side at radius R ≥ rb (§3.2): external — angle from the space centre to the flank
// (the space widens outward); internal — the same for the space of the internal gear (narrows outward).
double flankAngle(const Params& p, double R);

// Outline segment in the gear plane (§4.5): traversal from a to b; arcs — about center, counterclockwise if ccw;
// splines — natural start at a.
struct Seg {
    enum class Kind { Line, Arc, Circle, Spline };
    Kind kind = Kind::Line;
    std::string id;  // segment ID (kernel-interface.md §2.5): r<k>, fp<k>, ip<k>, t<k>, … (§4.5)
    Point2 a = Point2::Zero(), b = Point2::Zero();
    Point2 center = Point2::Zero();
    double radius = 0;
    bool ccw = true;
    spline::Curve curve;
};
using Loop = std::vector<Seg>;

// Layout of the teeth (§4.5): full gear — tooth 1 centred at angle phase; sector — from start, sweep angle.
struct Layout {
    double phase = 0;
    bool sector = false;
    double start = 0, angle = 0;  // rad
};

// Section of the cutting tool (§4.5): the region removed from the blank, between the tooth outline and the auxiliary
// circle of radius aux (external: aux > ra; internal: aux < ra). teeth — teeth formed.
struct Section {
    Loop outer;
    std::vector<Loop> holes;
    int teeth = 0;
};
// tipShift ≥ 0: the flanks continue past the tip circle by tipShift (external — outward, internal — inward), so the
// tool does not run along the blank surface when the face diameter equals da (§4.5); halved while the tooth is pointed.
Result<Section> toolSection(const Params& p, const Layout& layout, double aux, double tipShift = 0);

// Closed outline of a full gear (§4.5, the hole of the external tool / outer loop of the internal tool).
Result<Loop> outline(const Params& p, double phase = 0, double tipShift = 0);

// Signed area of a loop by Green's formula (positive — counterclockwise traversal), mm².
double loopArea(const Loop& loop);
// Point of a segment at parameter u ∈ [0, 1] along the traversal (splines — by curve parameter).
Point2 segPoint(const Seg& s, double u);

// Teeth in a sector of the given sweep (§4.5): floor(angle·z/2π + GEAR_ANGLE_TOL).
int sectorTeeth(int z, double angle);

}  // namespace construe::gear
