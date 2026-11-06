// General system of equations (spec/solver.md §3.8): input for problems other than a planar sketch (assembly mates).
// The caller provides parameters and equations; the algorithms are the same as for the sketch.
#pragma once

#include "solver/types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace construe::solver {

// Equation residual: dimensionless (lengths divided by L by the caller) and its gradient w.r.t. its own parameters.
struct SystemFn {
    virtual ~SystemFn() = default;
    virtual double value(const double* x) const = 0;
    virtual void gradient(const double* x, double* g) const = 0;
};

struct System {
    double L = 1;                // characteristic length, mm
    std::vector<double> x0;      // parameters, mm
    std::vector<bool> fixed;
    struct Eq {
        uint64_t owner = 0;
        std::vector<int> params;  // no more than 12
        std::shared_ptr<const SystemFn> fn;
        bool aux = false;  // auxiliary (group 0): dependency only among such equations is not redundancy
    };
    std::vector<Eq> equations;
    bool skipDiagnosis = false;  // no diagnosis on dependency or non-convergence (inner solves of the assembly "pull")
};

enum class SystemStatus { Converged, NotConverged, Conflict, Redundant, Cancelled };

struct SystemResult {
    SystemStatus status = SystemStatus::Converged;
    std::vector<double> x;
    int dof = 0;
    std::vector<bool> freeParam;
    std::optional<Diagnosis> diagnosis;  // constraint IDs — owners of equations
    double maxResidual = 0;
};

SystemResult solveSystem(const System& s, const CancelToken* cancel = nullptr);

}  // namespace construe::solver
