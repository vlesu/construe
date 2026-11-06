// 3D-print preparation of a body (spec/printprep.md; S6-PRINT-001, S6-PRINT-002): fitted cylindrical faces moved by the
// tolerance zone and the printer clearance, cosmetic threads turned into real turns. Export only — the model is not
// changed. Functions over the kernel interface, no state; targets are resolved by api (§5).
#pragma once

#include "base/cancel.h"
#include "base/error.h"
#include "base/math.h"
#include "kernel/kernel.h"

#include <string>
#include <vector>

namespace construe::printprep {

// §6
inline constexpr double PRINT_CLEARANCE_DEFAULT = 0.2;  // mm
inline constexpr double PRINT_MAX_CLEARANCE = 1.0;      // mm
inline constexpr double PRINT_AIR_MARGIN = 0.1;         // mm: tool overrun beyond the changed layer
inline constexpr double PRINT_MIN_CHANGE = 1e-6;        // mm
inline constexpr double PRINT_SAME_TOL = 1e-3;          // mm
inline constexpr int PRINT_THREAD_MAX_TURNS = 200;
inline constexpr double PRINT_THREAD_AIR_REL = 0.05;   // groove overrun into the air ≤ this × pitch (§4.2 item 4)

struct Options {
    double clearance = PRINT_CLEARANCE_DEFAULT;
    std::string externalThreadField = "6g";
    std::string internalThreadField = "6H";
};

// A resolved fit annotation of a cylindrical face: face index in the body, limit deviations, mm.
struct FitTarget {
    std::string id;
    int face = 0;
    double upper = 0, lower = 0;
};

// A thread record of the part state (regen.md §4.14 item 5).
struct ThreadTarget {
    std::string feature;
    bool internal = false;
    Point3 origin = Point3::Zero();
    Vec3 dir = Vec3::UnitZ();  // unit, into the face
    double radius = 0, d = 0, pitch = 0, length = 0;
    double height = 0;  // > 0: own profile height (free thread, FX7-PT-003)
};

struct Change {
    enum class Kind { Fit, Thread };
    Kind kind = Kind::Fit;
    std::string id;
    bool hole = false;
    double nominal = 0;
    std::vector<double> diameters;  // fit — [target]; thread — [major, pitch, minor]
};

struct Prepared {
    kernel::Shape shape;
    std::vector<Change> changes;
    std::vector<Error> warnings;
};

struct ThreadDiameters {
    double major = 0, pitch = 0, minor = 0;
};

struct ProfilePoint {
    double u = 0, rho = 0;
};

// §4.1: diameter of a fitted surface for printing.
double fitTargetDiameter(double nominal, double upper, double lower, bool hole, double clearance);
// §4.2 item 2: fundamental deviation of a thread tolerance field (es or EI), mm.
Result<double> threadFieldDeviation(const std::string& field, double pitch, bool internal);
// §4.2 item 1: basic diameters of the ISO 68-1 profile.
ThreadDiameters basicThreadDiameters(double d, double pitch);
// §4.2 item 3: printed diameters of the thread turns.
Result<ThreadDiameters> printThreadDiameters(double d, double pitch, bool internal, const Options& options);
// §4.2 item 4: groove overrun into the air, mm: min(PRINT_AIR_MARGIN, PRINT_THREAD_AIR_REL·pitch).
double threadAirMargin(double pitch);
// §4.2 item 4: groove polygon in the (u, ρ) half-plane; minor and major are diameters.
std::vector<ProfilePoint> grooveProfile(double pitch, double minor, double major, bool internal);
// §4.3: the print body.
Result<Prepared> prepareBody(const kernel::Kernel& k, const kernel::Shape& body, const std::vector<FitTarget>& fits,
                             const std::vector<ThreadTarget>& threads, const Options& options, const Cancel* cancel = nullptr);
// §4.4: whether the thread surface lies on the body.
bool threadOnBody(const kernel::Kernel& k, const kernel::Shape& body, const ThreadTarget& t);

}  // namespace construe::printprep
