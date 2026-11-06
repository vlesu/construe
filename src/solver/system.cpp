// General system of equations (solver/system.h, spec/solver.md §3.8).
#include "solver/system.h"

#include "solver/algo.h"
#include "solver/problem.h"

#include <algorithm>
#include <map>
#include <set>

namespace construe::solver {

namespace {

// Wrapper turning the caller's residual into a layer-2 function.
struct Wrap final : GenFn {
    std::shared_ptr<const SystemFn> f;
    explicit Wrap(std::shared_ptr<const SystemFn> fn) : f(std::move(fn)) {}
    double value(const double* x) const override { return f->value(x); }
    void gradient(const double* x, double* g) const override { f->gradient(x, g); }
};

// Partial redundancy (§3.8): a redundancy group is dropped when every owner in it removes at least one degree of freedom —
// the rank without all of that owner's equations is lower than the full rank.
void dropPartialRedundancy(const Model& m, const std::vector<int>& eqs, const std::vector<double>& x, int rank, Diagnosis& d) {
    std::map<uint64_t, bool> contributes;
    auto contributesOf = [&](uint64_t owner) {
        if (auto it = contributes.find(owner); it != contributes.end()) return it->second;
        std::vector<int> rest;
        for (int e : eqs)
            if (m.equations[size_t(e)].owner != owner) rest.push_back(e);  // auxiliary equations of the owner too
        return contributes[owner] = fullRank(m, rest, x).rank < rank;
    };
    std::vector<Group> kept;
    for (const Group& g : d.groups) {
        bool removable = g.kind != GroupKind::Redundant;
        for (ConstraintId c : g.constraints) removable = removable || !contributesOf(c);
        if (removable) kept.push_back(g);
    }
    d.groups = std::move(kept);
    std::set<ConstraintId> suggested, redundant;
    for (const Group& g : d.groups) {
        if (g.suggested) suggested.insert(g.suggested);
        if (g.kind == GroupKind::Redundant) redundant.insert(g.constraints.begin(), g.constraints.end());
    }
    d.suggestedRemoval.assign(suggested.begin(), suggested.end());
    d.redundant.assign(redundant.begin(), redundant.end());
}

}  // namespace

SystemResult solveSystem(const System& s, const CancelToken* cancel) {
    Model m;
    m.L = s.L > 0 ? s.L : 1.0;
    for (size_t i = 0; i < s.x0.size(); ++i) {
        m.keys.push_back({EntityId(i + 1), 0});
        m.x0.push_back(s.x0[i]);
        m.fixed.push_back(i < s.fixed.size() && s.fixed[i]);
        m.index[m.keys.back()] = int(i);
    }
    std::map<uint64_t, int> k;
    for (const auto& e : s.equations) {
        Equation q;
        q.group = e.aux ? 0 : 1;
        q.owner = e.owner;
        q.k = k[e.owner]++;
        q.kind = EqKind::General;
        q.params = e.params;
        q.fn = std::make_shared<Wrap>(e.fn);
        m.equations.push_back(std::move(q));
    }
    std::vector<int> eqs(m.equations.size());
    for (size_t i = 0; i < eqs.size(); ++i) eqs[i] = int(i);
    const Problem empty;  // no entities: step guards — only G0 (§4.10)
    SystemResult out;
    out.x = m.x0;
    if (eqs.empty()) {
        out.dof = int(std::count(m.fixed.begin(), m.fixed.end(), false));
        out.freeParam.assign(m.x0.size(), true);
        for (size_t i = 0; i < m.fixed.size(); ++i) out.freeParam[i] = !m.fixed[i];
        return out;
    }
    const CoreResult core = solveCore(empty, m, eqs, m.x0, cancel, false, false);
    if (core.cancelled) {
        out.status = SystemStatus::Cancelled;
        return out;
    }
    out.x = core.converged ? core.x1 : (core.xBest.empty() ? core.x1 : core.xBest);
    out.maxResidual = maxResidual(m, eqs, out.x);
    const RankInfo ri = fullRank(m, eqs, out.x);
    out.dof = ri.dof;
    out.freeParam = ri.freeParam;
    const bool dependent = ri.rank < int(eqs.size());
    if ((!core.converged || dependent) && s.skipDiagnosis) {
        out.status = !core.converged ? SystemStatus::NotConverged : SystemStatus::Redundant;
        return out;
    }
    if (!core.converged || dependent) {
        bool cancelled = false;
        out.diagnosis = diagnoseAt(empty, m, out.x, cancel, nullptr, cancelled);
        if (cancelled) {
            out.status = SystemStatus::Cancelled;
            return out;
        }
        if (!core.converged) {
            out.status = out.diagnosis->groups.empty() ? SystemStatus::NotConverged : SystemStatus::Conflict;
        } else {
            dropPartialRedundancy(m, eqs, out.x, ri.rank, *out.diagnosis);
            out.status = out.diagnosis->groups.empty() ? SystemStatus::Converged : SystemStatus::Redundant;
            if (out.diagnosis->groups.empty()) out.diagnosis.reset();
        }
    }
    return out;
}

}  // namespace construe::solver
