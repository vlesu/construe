// Solver problem: building, modification, solving (spec/solver.md §3).
#pragma once

#include "solver/types.h"

#include <array>
#include <map>
#include <vector>

namespace construe::solver {

struct EntityData {
    EntityKind kind = EntityKind::Point;
    // Point
    double x = 0, y = 0;
    bool fixed = false;
    // Line: p1, p2; Circle: center (+ r); Arc, EllipseArc: center, start, end
    EntityId p1 = 0, p2 = 0, center = 0, start = 0, end = 0;
    double r = 0;
    bool radiusFixed = false;
    // Ellipse (§2.3, S4-SKT-004): center, semi-axis a vector (ux, uy), semi-axis b; shapeFixed — all three constant.
    double ux = 0, uy = 0, b = 0;
    bool shapeFixed = false;
    // BSpline (§2.3, S4A-SPL-001): pole points, degree, distinct knots with multiplicities, weights (constants).
    std::vector<EntityId> poles;
    int degree = 0;
    std::vector<double> knots, weights;
    std::vector<int> mults;
    bool periodic = false;
};

class Problem {
public:
    // Building and modification (§3.2). On error the problem is not changed.
    Status addPoint(EntityId id, double x, double y, bool fixed = false);
    Status addLine(EntityId id, EntityId p1, EntityId p2);
    Status addCircle(EntityId id, EntityId center, double r, bool radiusFixed = false);
    Status addArc(EntityId id, EntityId center, EntityId start, EntityId end);
    Status addEllipse(EntityId id, EntityId center, double ux, double uy, double b, bool shapeFixed = false);
    // Ellipse arc (§2.3, S4-SKT-007): counterclockwise by the parameter from start to end; the ends lie on the ellipse.
    Status addEllipseArc(EntityId id, EntityId center, EntityId start, EntityId end, double ux, double uy, double b);
    // B-spline / NURBS curve (§2.3, S4A-SPL-001): poles — distinct points; weights are constants (empty — all 1).
    Status addBSpline(EntityId id, const std::vector<EntityId>& poles, int degree, const std::vector<double>& knots,
                      const std::vector<int>& mults, const std::vector<double>& weights, bool periodic);
    Result<std::optional<Branch>> addConstraint(const ConstraintSpec& spec);
    Status remove(EntityId id);
    Status removeConstraint(ConstraintId id);
    Status setPoint(EntityId id, double x, double y);
    Status setRadius(EntityId id, double r);
    Status setFixed(EntityId id, bool fixed);
    Status setValue(ConstraintId id, double value);
    Status setTarget(ConstraintId id, const std::vector<double>& target);
    Status setDriving(ConstraintId id, bool driving);
    Status setBranch(ConstraintId id, const Branch& branch);

    // Reading (§3.3).
    bool hasEntity(EntityId id) const { return entities_.count(id) != 0; }
    const EntityData* entity(EntityId id) const;
    const ConstraintSpec* constraint(ConstraintId id) const;
    std::pair<double, double> point(EntityId id) const;
    double radius(EntityId id) const;
    // Ellipse: (ux, uy, b) (§3.3).
    std::array<double, 3> ellipse(EntityId id) const;
    double constraintResidual(ConstraintId id) const;
    double measure(ConstraintId id) const;
    Result<Branch> inferBranch(const ConstraintSpec& spec) const;
    double characteristicLength() const;

    // Solving (§3.4–§3.6).
    SolveResult solve(const SolveOptions& options = {});
    Diagnosis diagnose(const CancelToken* cancel = nullptr);
    // Selecting independent candidates (§4.11.4; weak sketch dimensions, S4-WEAK-001): candidates are accepted in order
    // if all their equations are linearly independent of the problem's driving equations and of previously accepted
    // candidates at the current values; selection stops when all parameters of the targets entities are determined.
    // Does not change the problem. Returns indices of accepted candidates ascending; equations — their equation count.
    std::vector<int> selectIndependent(const std::vector<ConstraintSpec>& candidates, const std::vector<EntityId>& targets,
                                       int* equations = nullptr) const;
    Result<DragResult> drag(const std::vector<DragTarget>& targets, const CancelToken* cancel = nullptr);

    const std::map<EntityId, EntityData>& entities() const { return entities_; }
    const std::map<ConstraintId, ConstraintSpec>& constraints() const { return constraints_; }

    // Internal: writing parameter values back (used by the algorithms).
    void setParamValue(const ParamKey& key, double v);
    double paramValue(const ParamKey& key) const;

private:
    Status validateConstraint(const ConstraintSpec& spec, bool checkId) const;

    std::map<EntityId, EntityData> entities_;
    std::map<ConstraintId, ConstraintSpec> constraints_;
};

}  // namespace construe::solver
