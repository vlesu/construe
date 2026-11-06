#include "solver/linalg.h"

#include "solver/model.h"

#include <cmath>

namespace construe::solver {

OrrqrResult orrqr(const Eigen::MatrixXd& Ain) {
    const int n = static_cast<int>(Ain.rows());
    const int m = static_cast<int>(Ain.cols());
    OrrqrResult res;

    std::vector<Eigen::VectorXd> vs;  // reflection vectors (length n, zeros before k)
    std::vector<double> betas;
    Eigen::MatrixXd R = Eigen::MatrixXd::Zero(n, std::max(1, std::min(n, m)));

    auto applyAll = [&](Eigen::VectorXd& w) {
        for (size_t k = 0; k < vs.size(); ++k) {
            const double s = vs[k].dot(w);
            w -= betas[k] * s * vs[k];
        }
    };

    for (int j = 0; j < m; ++j) {
        Eigen::VectorXd a = Ain.col(j);
        const double na = a.norm();
        const bool zero = !(na > TAU_ZERO);
        if (!zero) a /= na;
        const int k = static_cast<int>(vs.size());
        Eigen::VectorXd w = zero ? Eigen::VectorXd::Zero(n) : a;
        if (!zero) applyAll(w);
        const double rho = k < n ? w.tail(n - k).norm() : 0.0;
        if (zero || rho <= TAU_RANK) {
            OrrqrResult::Dependent dep;
            dep.column = j;
            dep.coeffs.assign(k, 0.0);
            if (!zero && k > 0) {
                Eigen::VectorXd c = R.topLeftCorner(k, k).triangularView<Eigen::Upper>().solve(w.head(k));
                for (int t = 0; t < k; ++t) dep.coeffs[t] = c[t];
            }
            res.dependent.push_back(std::move(dep));
            continue;
        }
        // Reflection zeroing w[k+1..n-1].
        Eigen::VectorXd x = w.tail(n - k);
        const double alpha = x[0] >= 0 ? -rho : rho;
        Eigen::VectorXd v = Eigen::VectorXd::Zero(n);
        v.tail(n - k) = x;
        v[k] -= alpha;
        const double vv = v.squaredNorm();
        const double beta = vv > 0 ? 2.0 / vv : 0.0;
        vs.push_back(v);
        betas.push_back(beta);
        if (R.cols() <= k) R.conservativeResize(n, k + 1);
        R.col(k).setZero();
        R.col(k).head(k) = w.head(k);
        R(k, k) = alpha;
        res.independent.push_back(j);
    }
    res.rank = static_cast<int>(vs.size());

    // Q = H_1 · … · H_rank.
    res.Q = Eigen::MatrixXd::Identity(n, n);
    for (int k = res.rank - 1; k >= 0; --k) {
        // Q ← H_k Q
        const Eigen::RowVectorXd t = vs[k].transpose() * res.Q;
        res.Q -= betas[k] * vs[k] * t;
    }
    return res;
}

Eigen::VectorXd minNormSolve(const Eigen::MatrixXd& A, const Eigen::VectorXd& b) {
    if (A.rows() == 0 || A.cols() == 0) return Eigen::VectorXd::Zero(A.cols());
    Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> cod;
    cod.setThreshold(TAU_RANK);
    cod.compute(A);
    return cod.solve(b);
}

Eigen::VectorXd minNormStep(const Eigen::MatrixXd& J, const Eigen::VectorXd& r) {
    Eigen::MatrixXd DJ = J;
    Eigen::VectorXd Dr = r;
    for (Eigen::Index i = 0; i < J.rows(); ++i) {
        const double nrm = J.row(i).norm();
        const double d = nrm > TAU_ZERO ? 1.0 / nrm : 0.0;
        DJ.row(i) *= d;
        Dr[i] *= d;
    }
    return -minNormSolve(DJ, Dr);
}

}  // namespace construe::solver
