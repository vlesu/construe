// Solver public API types (spec/solver.md §2–§3).
#pragma once

#include "base/error.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace construe::solver {

using EntityId = uint64_t;
using ConstraintId = uint64_t;

inline constexpr EntityId ORIGIN_ID = 0xFFFFFFFFFFFFFFFFull;   // §2.1
inline constexpr EntityId PROJ_ID_BASE = 0x8000000000000000ull;
// Hidden curve parameters of point-on-spline constraints: parameter keys (CURVE_PARAM_ID, k) (§2.2, S4A-SPL-002).
inline constexpr EntityId CURVE_PARAM_ID = 0xFFFFFFFFFFFFFFFEull;

struct ParamKey {
    EntityId entity = 0;
    int slot = 0;
    auto operator<=>(const ParamKey&) const = default;
};

enum class EntityKind { Point, Line, Circle, Arc, Ellipse, EllipseArc, BSpline };  // Ellipse — S4-SKT-004, EllipseArc — S4-SKT-007, BSpline — S4A-SPL-001
// Ellipse or ellipse arc: in constraints an ellipse arc is its ellipse (§2.3, S4-SKT-007).
inline bool isEllipseKind(EntityKind k) { return k == EntityKind::Ellipse || k == EntityKind::EllipseArc; }

enum class ConstraintType {
    Coincident,
    Horizontal,
    Vertical,
    Parallel,
    Perpendicular,
    Tangent,
    Equal,
    Concentric,
    PointOnObject,
    Midpoint,
    Symmetric,
    Fix,
    Collinear,
    ArcLength,
    ArcAngle,
    PathLength,
    Distance,
    DistanceX,
    DistanceY,
    Length,
    Radius,
    Diameter,
    Angle,
    SemiMajor,     // ellipse (spec/solver.md §4.2.21, S4-SKT-004)
    SemiMinor,
    EllipseAngle,
    Translate,  // copy relations (§4.2.21, S4-SKT-001)
    Rotate,     // (§4.2.21, S4-SKT-002)
    Curvature,  // equal curvature at a joint with a spline end (§4.2.23, S4A-SPL-002)
};

const char* constraintTypeName(ConstraintType t);
std::optional<ConstraintType> parseConstraintType(const std::string& s);
bool isDimension(ConstraintType t);

enum class Contact { External, Internal1, Internal2 };

// Solution branch (§4.2.20). kind is determined by the type and the kind of refs.
struct Branch {
    enum class Kind { Side, Contact, Angle };
    Kind kind = Kind::Side;
    int side = 1;                    // Side: +1 / -1
    Contact contact = Contact::External;
    int sign = 1;                    // Angle
    bool supplementary = false;      // Angle
    bool operator==(const Branch&) const = default;
};

struct ConstraintSpec {
    ConstraintId id = 0;
    ConstraintType type = ConstraintType::Coincident;
    std::vector<EntityId> refs;
    std::optional<double> value;
    std::vector<double> target;  // fix; translate — [vx, vy]; rotate — [φ] (§4.2.21)
    bool driving = true;
    std::optional<Branch> branch;
    bool far = false;  // distance to a circle/arc: farthest distance, otherwise nearest (§4.2.15а)
};

enum class SolveStatus { Converged, ConvergedRedundant, Inconsistent, NotConverged, Cancelled };
enum class SolveMethod { DogLeg, LevenbergMarquardt, Homotopy, None };
enum class GroupKind { Redundant, Conflicting, Undetermined };

const char* solveStatusName(SolveStatus s);
const char* groupKindName(GroupKind k);

struct Group {
    GroupKind kind = GroupKind::Redundant;
    std::vector<ConstraintId> constraints;
    std::vector<EntityId> arcs;
    ConstraintId suggested = 0;
};

struct Diagnosis {
    int dof = 0;
    int rank = 0;
    std::vector<Group> groups;
    std::vector<ConstraintId> conflicting;
    std::vector<ConstraintId> redundant;
    std::vector<ConstraintId> suggestedRemoval;
    std::vector<ParamKey> freeParams;
    std::vector<EntityId> underconstrainedEntities;
    std::vector<EntityId> degenerateArcs;
    std::optional<bool> resolveConverged;
};

struct SolveResult {
    SolveStatus status = SolveStatus::Converged;
    int dof = 0;
    double maxResidual = 0;
    double bestResidual = 0;
    int iterations = 0;
    SolveMethod method = SolveMethod::None;
    std::vector<ConstraintId> unsatisfied;
    std::vector<EntityId> unsatisfiedArcs;
    std::vector<ParamKey> freeParams;
    std::vector<EntityId> underconstrainedEntities;
    std::vector<EntityId> degenerateEntities;
    std::map<ConstraintId, double> referenceValues;
    std::optional<Diagnosis> diagnosis;
};

// Atomic cancellation flag (§3.7) — the only object modified from another thread.
struct CancelToken {
    std::atomic<bool> requested{false};
};

struct SolveOptions {
    bool diagnosis = true;  // Auto = true, Never = false
    const CancelToken* cancel = nullptr;
};

struct DragTarget {
    enum class Kind { Point, Radius };
    Kind kind = Kind::Point;
    EntityId entity = 0;
    double x = 0, y = 0;
};

enum class DragStatus { Reached, Constrained, Failed, Cancelled };
const char* dragStatusName(DragStatus s);

struct DragResult {
    DragStatus status = DragStatus::Failed;
    int iterations = 0;
    double maxResidual = 0;
    double targetDistance = 0;
};

}  // namespace construe::solver
