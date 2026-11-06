// Solver algorithms (layer 3, spec/solver.md §4.4–§4.13). Internal header.
#pragma once

#include "solver/linalg.h"
#include "solver/model.h"

#include <Eigen/Dense>

#include <vector>

namespace construe::solver {

// Result of equality substitution (§4.5).
struct Substitution {
    std::vector<int> unk;                 // parameter → reduced unknown, or −1 (constant class)
    std::vector<double> off;              // parameter → offset relative to the root (mm)
    std::vector<int> rootParam;           // unknown → root parameter
    std::vector<std::vector<int>> members;  // unknown → parameters of the class (ascending)
    std::vector<double> z0;               // initial values of the unknowns (mm)
    std::vector<int> generalEqs;          // General equations solved iteratively
    std::vector<int> constantEqs;         // General equations on constant classes
    bool depsFlag = false;
};

Substitution substitute(const Model& m, const std::vector<int>& eqs, const std::vector<double>& x0,
                        const std::vector<int>& dragParams = {});

struct Component {
    std::vector<int> unknowns;  // ascending by root key
    std::vector<int> eqs;       // in canonical order
};

std::vector<Component> components(const Model& m, const Substitution& s);

// Expand the values of the unknowns (mm) into the full parameter vector.
std::vector<double> unfold(const Substitution& s, const std::vector<double>& z, const std::vector<double>& base);

// Residuals and Jacobian (w.r.t. scaled unknowns z̃ = z/L) for a set of equations and unknowns.
void evalSystem(const Model& m, const Substitution& s, const std::vector<int>& eqs, const std::vector<int>& unknowns,
                const std::vector<double>& x, Eigen::VectorXd& r, Eigen::MatrixXd* J);

enum class IterOutcome { Converged, Stationary, SmallStep, IterLimit, Cancelled };

struct CoreResult {
    bool converged = false;
    bool cancelled = false;
    std::vector<double> x1;     // result (if converged) or the last point
    std::vector<double> xBest;  // best point by max |r̃|
    int iterations = 0;
    SolveMethod method = SolveMethod::None;
    std::vector<double> xFar;  // solution rejected by the SOLVE_MOVE_MAX limit (§4.4.1 step 4); empty — there was none
};

// Main pass of solveCore (§4.4.1): substitution, components, DogLeg → LM, homotopy.
// limitMove — a solution that moved from x0 farther than SOLVE_MOVE_MAX·L is not accepted (main pass); the
// diagnostic re-solve classifies groups and works without this limit (§4.12.5).
CoreResult solveCore(const Problem& p, const Model& m, const std::vector<int>& eqs, const std::vector<double>& x0,
                     const CancelToken* cancel, bool allowHomotopy = true, bool limitMove = true);

// No value of x moved from x0 farther than SOLVE_MOVE_MAX·L (§4.4.1 step 4).
bool withinMoveLimit(const Model& m, const std::vector<double>& x0, const std::vector<double>& x);

double maxResidual(const Model& m, const std::vector<int>& eqs, const std::vector<double>& x);

// DOF and freedom over the full system (§4.11.2) at point x.
struct RankInfo {
    int dof = 0;
    int rank = 0;
    std::vector<bool> freeParam;  // per model parameter
    OrrqrResult qr;
    std::vector<int> eqs;         // columns (equation indices)
    std::vector<int> freeIdx;     // rows (parameters)
};
RankInfo fullRank(const Model& m, const std::vector<int>& eqs, const std::vector<double>& x);

// DOF and freedom per component (§4.11.3) at solution x.
RankInfo componentRank(const Model& m, const std::vector<int>& eqs, const std::vector<double>& x, bool& depsFlag);

// Diagnosis (§4.12) at point x. xK — the solution without K, if the re-solve converged.
Diagnosis diagnoseAt(const Problem& p, const Model& m, const std::vector<double>& x, const CancelToken* cancel,
                     std::vector<double>* xK, bool& cancelled);

std::vector<ParamKey> freeKeys(const Model& m, const std::vector<bool>& freeParam);

inline bool isCancelled(const CancelToken* c) { return c && c->requested.load(std::memory_order_relaxed); }

}  // namespace construe::solver
