// Dragging: prioritized least-squares problem (spec/solver.md §4.13).
#include "solver/algo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace construe::solver {

namespace {

struct DragContext {
    const Problem& p;
    const Model& m;
    const Substitution& s;
    std::vector<int> unknowns;  // active unknowns
    std::vector<int> eqs;       // active equations
    std::vector<double> xStart;
    std::vector<DragTarget> targets;
    std::vector<double> rhoT;   // for Radius targets: target radius computed at the start of the frame

    std::vector<double> apply(const Eigen::VectorXd& zt) const {
        std::vector<double> x = xStart;
        for (size_t c = 0; c < unknowns.size(); ++c)
            for (int q : s.members[unknowns[c]]) x[q] = zt[static_cast<Eigen::Index>(c)] * m.L + s.off[q];
        return x;
    }

    void hard(const Eigen::VectorXd& zt, Eigen::VectorXd& r, Eigen::MatrixXd& J) const {
        evalSystem(m, s, eqs, unknowns, apply(zt), r, &J);
    }

    // Soft residuals s̃ and their Jacobian w.r.t. z̃.
    void soft(const Eigen::VectorXd& zt, Eigen::VectorXd& sv, Eigen::MatrixXd& S) const {
        const std::vector<double> x = apply(zt);
        std::vector<int> rows;
        std::vector<double> vals;
        std::vector<int> params;
        for (size_t t = 0; t < targets.size(); ++t) {
            const DragTarget& tg = targets[t];
            const ModelEntity& e = m.entities.at(tg.entity);
            if (tg.kind == DragTarget::Kind::Point) {
                vals.push_back((x[e.px] - tg.x) / m.L);
                params.push_back(e.px);
                vals.push_back((x[e.py] - tg.y) / m.L);
                params.push_back(e.py);
            } else {
                vals.push_back((x[e.pr] - rhoT[t]) / m.L);
                params.push_back(e.pr);
            }
        }
        const Eigen::Index k = static_cast<Eigen::Index>(vals.size());
        sv.resize(k);
        S.setZero(k, static_cast<Eigen::Index>(unknowns.size()));
        std::vector<int> col(s.rootParam.size(), -1);
        for (size_t c = 0; c < unknowns.size(); ++c) col[unknowns[c]] = static_cast<int>(c);
        for (Eigen::Index i = 0; i < k; ++i) {
            sv[i] = vals[static_cast<size_t>(i)];
            const int u = s.unk[params[static_cast<size_t>(i)]];
            if (u >= 0 && col[u] >= 0) S(i, col[u]) = 1.0;
        }
    }

    // Corrector: z ← z + minNormStep, with guards. true — converged.
    bool correct(Eigen::VectorXd& z, int maxSteps, int& iterations) const {
        Eigen::VectorXd r;
        Eigen::MatrixXd J;
        for (int k = 0;; ++k) {
            hard(z, r, J);
            if (r.size() == 0 || r.lpNorm<Eigen::Infinity>() <= TOL_CONV) return true;
            if (k >= maxSteps) return false;
            const Eigen::VectorXd zNew = z + minNormStep(J, r);
            ++iterations;
            if (!guardsAccept(m, p, apply(z), apply(zNew))) return false;
            z = zNew;
        }
    }
};

}  // namespace

Result<DragResult> Problem::drag(const std::vector<DragTarget>& targets, const CancelToken* cancel) {
    // Checks (§3.6).
    if (targets.empty()) return Error("SOLVER_INVALID_VALUE").with("name", std::string("targets"));
    std::set<EntityId> seen;
    for (const DragTarget& t : targets) {
        auto it = entities_.find(t.entity);
        if (it == entities_.end())
            return Error("SOLVER_UNKNOWN_ENTITY").with("id", static_cast<double>(t.entity));
        if (!seen.insert(t.entity).second)
            return Error("SOLVER_INVALID_VALUE").with("id", static_cast<double>(t.entity)).with("name", std::string("targets"));
        if (!std::isfinite(t.x) || std::abs(t.x) > COORD_MAX)
            return Error("SOLVER_INVALID_VALUE").with("id", static_cast<double>(t.entity)).with("name", std::string("x"));
        if (!std::isfinite(t.y) || std::abs(t.y) > COORD_MAX)
            return Error("SOLVER_INVALID_VALUE").with("id", static_cast<double>(t.entity)).with("name", std::string("y"));
        if (t.kind == DragTarget::Kind::Point && it->second.kind != EntityKind::Point)
            return Error("SOLVER_WRONG_KIND").with("id", static_cast<double>(t.entity)).with("expected", std::string("Point"));
        if (t.kind == DragTarget::Kind::Radius && it->second.kind != EntityKind::Circle)
            return Error("SOLVER_WRONG_KIND").with("id", static_cast<double>(t.entity)).with("expected", std::string("Circle"));
    }

    // L for drag: the bounding size includes the drag targets (§4.1); set before building the equations.
    double dragL = computeL(*this);
    {
        double minx = std::numeric_limits<double>::infinity(), miny = minx, maxx = -minx, maxy = -minx;
        auto add = [&](double x, double y) {
            minx = std::min(minx, x), maxx = std::max(maxx, x);
            miny = std::min(miny, y), maxy = std::max(maxy, y);
        };
        for (const auto& [id, e] : entities_) {
            if (e.kind == EntityKind::Point) add(e.x, e.y);
            if (e.kind == EntityKind::Circle) {
                const auto& c = entities_.at(e.center);
                add(c.x + e.r, c.y), add(c.x - e.r, c.y), add(c.x, c.y + e.r), add(c.x, c.y - e.r);
            }
        }
        for (const DragTarget& t : targets) {
            if (t.kind == DragTarget::Kind::Point) {
                add(t.x, t.y);
            } else {
                const auto& circle = entities_.at(t.entity);
                const auto& c = entities_.at(circle.center);
                const double rho = std::hypot(t.x - c.x, t.y - c.y);
                add(c.x + rho, c.y), add(c.x - rho, c.y), add(c.x, c.y + rho), add(c.x, c.y - rho);
            }
        }
        if (minx <= maxx) dragL = std::max(dragL, std::hypot(maxx - minx, maxy - miny));
    }
    const Model m = buildModel(*this, nullptr, dragL);
    std::vector<int> E(m.equations.size());
    for (size_t i = 0; i < E.size(); ++i) E[i] = static_cast<int>(i);

    std::vector<int> dragParams;
    for (const DragTarget& t : targets) {
        const ModelEntity& e = m.entities.at(t.entity);
        if (t.kind == DragTarget::Kind::Point) {
            dragParams.push_back(e.px);
            dragParams.push_back(e.py);
        } else {
            dragParams.push_back(e.pr);
        }
    }
    const Substitution s = substitute(m, E, m.x0, dragParams);
    const std::vector<Component> comps = components(m, s);

    // Active part (§4.13.2).
    std::set<int> targetUnk;
    for (int p : dragParams)
        if (s.unk[p] >= 0) targetUnk.insert(s.unk[p]);
    std::set<int> activeUnk;
    std::vector<int> activeEqs;
    for (const Component& c : comps) {
        bool active = false;
        for (int u : c.unknowns) active = active || targetUnk.count(u);
        if (!active) continue;
        activeUnk.insert(c.unknowns.begin(), c.unknowns.end());
        activeEqs.insert(activeEqs.end(), c.eqs.begin(), c.eqs.end());
    }
    activeUnk.insert(targetUnk.begin(), targetUnk.end());  // isolated unknowns of the targets
    std::sort(activeEqs.begin(), activeEqs.end());

    DragContext ctx{*this, m, s, std::vector<int>(activeUnk.begin(), activeUnk.end()), activeEqs, m.x0, targets, {}};
    ctx.rhoT.assign(targets.size(), 0.0);
    for (size_t t = 0; t < targets.size(); ++t) {
        if (targets[t].kind != DragTarget::Kind::Radius) continue;
        const ModelEntity& e = m.entities.at(targets[t].entity);
        auto [ox, oy] = m.pointParams(e.center);
        ctx.rhoT[t] = std::hypot(targets[t].x - m.x0[ox], targets[t].y - m.x0[oy]);
    }

    DragResult res;
    const Eigen::Index n = static_cast<Eigen::Index>(ctx.unknowns.size());
    Eigen::VectorXd z(n);
    for (Eigen::Index c = 0; c < n; ++c) z[c] = s.z0[ctx.unknowns[static_cast<size_t>(c)]] / m.L;

    // 0. Corrector from the initial state.
    if (!ctx.correct(z, DRAG_INIT_CORR, res.iterations)) {
        res.status = DragStatus::Failed;
        res.maxResidual = maxResidual(m, E, m.x0);
        return res;
    }

    Eigen::VectorXd r, sv, sNew;
    Eigen::MatrixXd J, S, Snew;
    for (int k = 1; k <= DRAG_MAX_OUTER && n > 0; ++k) {
        if (isCancelled(cancel)) {
            res.status = DragStatus::Cancelled;
            return res;
        }
        ++res.iterations;
        ctx.hard(z, r, J);
        ctx.soft(z, sv, S);
        Eigen::VectorXd hp = Eigen::VectorXd::Zero(n);
        Eigen::MatrixXd N = Eigen::MatrixXd::Identity(n, n);
        if (r.size() > 0) {
            hp = minNormStep(J, r);
            const OrrqrResult q = orrqr(J.transpose());
            N = q.Q.rightCols(n - q.rank);
        }
        Eigen::VectorXd h = hp;
        if (N.cols() > 0) {
            const Eigen::VectorXd y = -minNormSolve(S * N, sv + S * hp);
            h += N * y;
        }
        if (h.norm() > DRAG_STEP_CAP) h *= DRAG_STEP_CAP / h.norm();
        const double slope = 2.0 * sv.dot(S * h);
        if (slope >= 0) break;
        bool accepted = false;
        double t = 1.0, tAccepted = 0.0;
        // Candidate step t: guards, corrector, Armijo condition; ‖s̃‖² via out.
        auto candidate = [&](double tt, Eigen::VectorXd& zc, double& err) {
            zc = z + tt * h;
            if (!guardsAccept(m, *this, ctx.apply(z), ctx.apply(zc))) return false;
            if (!ctx.correct(zc, DRAG_MAX_CORR, res.iterations)) return false;
            ctx.soft(zc, sNew, Snew);
            err = sNew.squaredNorm();
            return err <= sv.squaredNorm() + DRAG_ARMIJO * tt * slope;
        };
        for (int b = 0; b < DRAG_BACKTRACK; ++b, t *= 0.5) {
            Eigen::VectorXd z1;
            double e1 = 0;
            if (!candidate(t, z1, e1)) continue;
            if (b == 0) {
                Eigen::VectorXd zh;
                double eh = 0;
                if (candidate(0.5, zh, eh) && eh < e1) {
                    z1 = zh;
                    t = 0.5;
                }
            }
            z = z1;
            accepted = true;
            tAccepted = t;
            break;
        }
        if (!accepted) break;
        ctx.soft(z, sv, S);
        if (sv.lpNorm<Eigen::Infinity>() <= TOL_REACH || (tAccepted * h).norm() <= DRAG_EPS_STEP) break;
    }

    ctx.soft(z, sv, S);
    const std::vector<double> x = ctx.apply(z);
    for (size_t p = 0; p < m.keys.size(); ++p) setParamValue(m.keys[p], x[p]);
    res.targetDistance = sv.size() ? sv.lpNorm<Eigen::Infinity>() : 0.0;
    res.status = res.targetDistance <= TOL_REACH ? DragStatus::Reached : DragStatus::Constrained;
    res.maxResidual = maxResidual(m, E, x);
    return res;
}

}  // namespace construe::solver
