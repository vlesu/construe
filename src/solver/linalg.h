// Solver linear algebra (spec/solver.md §4.11.1, §8.1): ORRQR, minimum-norm solve.
#pragma once

#include <Eigen/Dense>

#include <vector>

namespace construe::solver {

// Ordered rank-revealing QR over the columns of A (n × m): columns are equation gradients.
struct OrrqrResult {
    int rank = 0;
    std::vector<int> independent;  // column indices
    struct Dependent {
        int column;
        std::vector<double> coeffs;  // over the preceding independent columns (in the order of independent)
    };
    std::vector<Dependent> dependent;
    Eigen::MatrixXd Q;  // n × n
};

OrrqrResult orrqr(const Eigen::MatrixXd& A);

// h = −(D J)⁺ (D r): minimum-norm least-squares solution with row equilibration (§4.7).
Eigen::VectorXd minNormStep(const Eigen::MatrixXd& J, const Eigen::VectorXd& r);

// Minimum norm for an arbitrary system A y ≈ b (without equilibration).
Eigen::VectorXd minNormSolve(const Eigen::MatrixXd& A, const Eigen::VectorXd& b);

}  // namespace construe::solver
