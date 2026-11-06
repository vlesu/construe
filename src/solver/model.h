// Internal model of one public call: parameters and equations (spec/solver.md §1.4, layers 1–2).
// The equation layer knows nothing about the dimension of the space.
#pragma once

#include "base/spline.h"
#include "solver/problem.h"

#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <vector>

namespace construe::solver {

// Constants §5.
inline constexpr double L_MIN = 1e-6;
inline constexpr double COORD_MAX = 1e9;
inline constexpr double TOL_CONV = 1e-10;
inline constexpr double ETA_REG = 1e-9;
inline constexpr double EPS_LEN = 1e-8;
inline constexpr double SCALED_MAX = 1e6;
inline constexpr double ARC_SWEEP_JUMP = 3.14159265358979323846;
inline constexpr double TAU_ZERO = 1e-12;
inline constexpr double TAU_RANK = 1e-9;
inline constexpr double TAU_GROUP = 1e-6;
inline constexpr double TAU_FREE = 1e-6;
inline constexpr double TOL_GRAD = 1e-13;
inline constexpr double EPS_STEP = 1e-14;
inline constexpr double DL_DELTA0 = 0.1;
inline constexpr double DL_GROW = 3.0;
inline constexpr double DL_SHRINK = 0.5;
inline constexpr double RHO_LOW = 0.25;
inline constexpr double RHO_HIGH = 0.75;
inline constexpr int DL_MAX_ITER = 100;
inline constexpr double LM_TAU = 1e-3;
inline constexpr double LM_NU0 = 2.0;
inline constexpr int LM_MAX_ITER = 100;
inline constexpr double HOMOTOPY_REL = 0.1;
inline constexpr double HOMOTOPY_ANGLE = 3.14159265358979323846 / 18;
inline constexpr int HOMOTOPY_MAX_N0 = 64;
inline constexpr double HOMOTOPY_MIN_DT = 1.0 / 256;
inline constexpr int HOMOTOPY_MAX_HALVINGS = 4;
inline constexpr double HOMOTOPY_TOL_MID = 1e-6;
inline constexpr double SHAPE_WEIGHT = 1e-3;
inline constexpr int SHAPE_MAX_ITER = 50;
inline constexpr int SHAPE_BACKTRACK = 8;
inline constexpr double SHAPE_STEP_MAX = 0.5;
inline constexpr double SHAPE_TOL = 1e-9;
inline constexpr double HOMOTOPY_MOVE_MAX = 1.0;
inline constexpr double SOLVE_MOVE_MAX = 10.0;
inline constexpr int DRAG_INIT_CORR = 10;
inline constexpr int DRAG_MAX_OUTER = 25;
inline constexpr int DRAG_MAX_CORR = 5;
inline constexpr double DRAG_STEP_CAP = 0.25;
inline constexpr int DRAG_BACKTRACK = 8;
inline constexpr double DRAG_EPS_STEP = 1e-10;
inline constexpr double DRAG_ARMIJO = 1e-4;
inline constexpr double TOL_REACH = 1e-9;
inline constexpr int DENSE_WARN = 1000;

enum class EqKind { Subst, Fixation, General };

// Residual function of a General equation: value and gradient w.r.t. its own parameters.
struct GenFn {
    virtual ~GenFn() = default;
    virtual double value(const double* x) const = 0;
    virtual void gradient(const double* x, double* g) const = 0;  // dr̃/dx per slot
};

struct Equation {
    int group = 1;           // 0 — internal arc equation, 1 — constraint
    uint64_t owner = 0;      // ID of the arc or constraint
    int k = 0;               // equation number within the constraint
    EqKind kind = EqKind::General;
    int i = -1, j = -1;      // Subst: x_i − x_j = c; Fixation: x_i = c (i — parameter index)
    double c = 0.0;
    std::vector<int> params; // General: parameter indices per function slot
    std::shared_ptr<const GenFn> fn;

    // Dimensionless residual r̃ at values x (§4.1).
    double residual(const std::vector<double>& x, double L) const;
    // Gradient dr̃/dx (by parameter index, repeats are summed).
    void gradient(const std::vector<double>& x, double L, std::vector<std::pair<int, double>>& out) const;
    // All parameters of the equation.
    std::vector<int> allParams() const;
};

struct ModelEntity {
    EntityId id = 0;
    EntityKind kind = EntityKind::Point;
    int px = -1, py = -1;  // Point
    int pr = -1;           // Circle: r
    int pux = -1, puy = -1, pb = -1;  // Ellipse: ux, uy, b (§2.2)
    EntityId p1 = 0, p2 = 0, center = 0, start = 0, end = 0;
    // BSpline: pole points and the expanded curve (pole coordinates — placeholders, §2.3).
    std::vector<EntityId> poles;
    std::shared_ptr<const spline::Flat> curve;
};

// Overrides of dimension values (homotopy §4.9).
struct ValueOverrides {
    std::map<ConstraintId, double> value;
    std::map<ConstraintId, double> phi;               // angle: target oriented angle
    std::map<ConstraintId, std::vector<double>> target;  // fix
};

struct Model {
    double L = L_MIN;
    std::vector<ParamKey> keys;     // ascending by ParamKey
    std::vector<double> x0;
    std::vector<bool> fixed;
    std::map<ParamKey, int> index;
    std::map<EntityId, ModelEntity> entities;
    std::vector<Equation> equations;  // driving, in canonical order (§2.5)
    // Classes of points linked by coincident constraints (root — smallest ID); for tangency at a shared point (§4.2.5).
    std::map<EntityId, EntityId> coincidentRoot;
    // Hidden curve parameter of each driving point-on-spline constraint: index of key (CURVE_PARAM_ID, k) (§2.2).
    std::map<ConstraintId, int> curveParam;
    EntityId rootOf(EntityId e) const {
        auto it = coincidentRoot.find(e);
        return it == coincidentRoot.end() ? e : it->second;
    }

    int param(EntityId e, int slot) const { return index.at({e, slot}); }
    // Point parameters (x, y).
    std::pair<int, int> pointParams(EntityId e) const { return {param(e, 0), param(e, 1)}; }
};

// Building the model from a problem; L — §4.1 from the current values.
// lengthScale — characteristic length L of the call (§4.1); defaults to computeL(p). Set before building
// the equations: residual functions capture L.
Model buildModel(const Problem& p, const ValueOverrides* overrides = nullptr, std::optional<double> lengthScale = std::nullopt);
// Characteristic length §4.1.
double computeL(const Problem& p);
// Equations of one constraint (for constraintResidual, inferBranch, measure).
std::vector<Equation> constraintEquations(const Model& m, const ConstraintSpec& c, const ValueOverrides* ov = nullptr);
// measure §4.14 at values x.
double measureAt(const Model& m, const ConstraintSpec& c, const std::vector<double>& x);
// Step guards §4.10: true — the step is acceptable.
bool guardsAccept(const Model& m, const Problem& p, const std::vector<double>& xOld, const std::vector<double>& xNew);
// Degenerate entities (segments and curves of small length/radius).
std::vector<EntityId> degenerateEntities(const Model& m, const std::vector<double>& x);
// Entities that have free parameters.
std::vector<EntityId> underconstrained(const Model& m, const std::vector<bool>& paramFree);

}  // namespace construe::solver
