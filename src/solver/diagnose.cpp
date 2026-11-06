// Diagnosis: groups, removal candidates, Redundant/Conflicting (spec/solver.md §4.12).
#include "solver/algo.h"

#include <algorithm>
#include <map>
#include <set>

namespace construe::solver {

Diagnosis diagnoseAt(const Problem& p, const Model& m, const std::vector<double>& x, const CancelToken* cancel,
                     std::vector<double>* xK, bool& cancelled) {
    cancelled = false;
    Diagnosis d;
    std::vector<int> E(m.equations.size());
    for (size_t i = 0; i < E.size(); ++i) E[i] = static_cast<int>(i);

    const RankInfo ri = fullRank(m, E, x);
    d.dof = ri.dof;
    d.rank = ri.rank;
    d.freeParams = freeKeys(m, ri.freeParam);
    d.underconstrainedEntities = underconstrained(m, ri.freeParam);

    // Groups §4.12.3.
    struct RawGroup {
        std::vector<ConstraintId> constraints;
        std::set<EntityId> arcs;
    };
    std::vector<RawGroup> groups;
    std::set<EntityId> degenerateArcs;
    for (const auto& dep : ri.qr.dependent) {
        std::vector<int> members{dep.column};
        double cmax = 0.0;
        for (double c : dep.coeffs) cmax = std::max(cmax, std::abs(c));
        const double thr = TAU_GROUP * std::max(cmax, 1.0);
        for (size_t t = 0; t < dep.coeffs.size(); ++t)
            if (std::abs(dep.coeffs[t]) >= thr && cmax > 0) members.push_back(ri.qr.independent[t]);
        std::set<ConstraintId> cs;
        std::set<EntityId> arcs;
        for (int col : members) {
            const Equation& q = m.equations[E[col]];
            if (q.group == 1) cs.insert(q.owner);
            else arcs.insert(q.owner);
        }
        if (cs.empty()) {
            degenerateArcs.insert(arcs.begin(), arcs.end());
            continue;
        }
        std::vector<ConstraintId> cv(cs.begin(), cs.end());
        bool merged = false;
        for (auto& g : groups)
            if (g.constraints == cv) {
                g.arcs.insert(arcs.begin(), arcs.end());
                merged = true;
                break;
            }
        if (!merged) groups.push_back({cv, arcs});
    }
    d.degenerateArcs.assign(degenerateArcs.begin(), degenerateArcs.end());

    // Choice of K §4.12.4.
    std::map<ConstraintId, int> eqCount;
    for (const Equation& q : m.equations)
        if (q.group == 1) ++eqCount[q.owner];
    std::vector<ConstraintId> suggested(groups.size(), 0);
    std::vector<bool> covered(groups.size(), false);
    std::set<ConstraintId> K;
    while (true) {
        std::map<ConstraintId, int> cover;
        for (size_t g = 0; g < groups.size(); ++g)
            if (!covered[g])
                for (ConstraintId q : groups[g].constraints) ++cover[q];
        if (cover.empty()) break;
        ConstraintId best = 0;
        bool have = false;
        for (const auto& [q, cnt] : cover) {
            if (!have) {
                best = q;
                have = true;
                continue;
            }
            const int cb = cover[best];
            if (cnt != cb) {
                if (cnt > cb) best = q;
                continue;
            }
            if (eqCount[q] != eqCount[best]) {
                if (eqCount[q] < eqCount[best]) best = q;
                continue;
            }
            if (q > best) best = q;
        }
        K.insert(best);
        for (size_t g = 0; g < groups.size(); ++g)
            if (!covered[g] && std::find(groups[g].constraints.begin(), groups[g].constraints.end(), best) !=
                                   groups[g].constraints.end()) {
                covered[g] = true;
                suggested[g] = best;
            }
    }
    d.suggestedRemoval.assign(K.begin(), K.end());

    // Redundant / Conflicting §4.12.5.
    std::vector<GroupKind> kinds(groups.size(), GroupKind::Redundant);
    if (!groups.empty() && maxResidual(m, E, x) > TOL_CONV) {
        std::vector<int> E2;
        for (int e : E) {
            const Equation& q = m.equations[e];
            if (q.group == 1 && K.count(q.owner)) continue;
            E2.push_back(e);
        }
        CoreResult cr = solveCore(p, m, E2, x, cancel, true, false);
        if (cr.cancelled) {
            cancelled = true;
            return d;
        }
        d.resolveConverged = cr.converged;
        if (cr.converged) {
            if (xK) *xK = cr.x1;
            for (size_t g = 0; g < groups.size(); ++g) {
                bool ok = true;
                for (ConstraintId q : groups[g].constraints) {
                    if (!K.count(q)) continue;
                    for (int e : E) {
                        const Equation& eq = m.equations[e];
                        if (eq.group == 1 && eq.owner == q && std::abs(eq.residual(cr.x1, m.L)) > TOL_CONV) ok = false;
                    }
                }
                kinds[g] = ok ? GroupKind::Redundant : GroupKind::Conflicting;
            }
        } else {
            std::fill(kinds.begin(), kinds.end(), GroupKind::Undetermined);
        }
    }

    std::set<ConstraintId> conflicting, redundant;
    for (size_t g = 0; g < groups.size(); ++g) {
        Group out;
        out.kind = kinds[g];
        out.constraints = groups[g].constraints;
        out.arcs.assign(groups[g].arcs.begin(), groups[g].arcs.end());
        out.suggested = suggested[g];
        d.groups.push_back(out);
        if (kinds[g] == GroupKind::Conflicting) conflicting.insert(out.constraints.begin(), out.constraints.end());
    }
    for (const Group& g : d.groups)
        if (g.kind == GroupKind::Redundant)
            for (ConstraintId q : g.constraints)
                if (!conflicting.count(q)) redundant.insert(q);
    d.conflicting.assign(conflicting.begin(), conflicting.end());
    d.redundant.assign(redundant.begin(), redundant.end());
    return d;
}

}  // namespace construe::solver
