// Substitution, components, DogLeg, LM, homotopy, rank (spec/solver.md §4.4–§4.11).
#include "solver/algo.h"

#include "base/log.h"
#include "solver/dual.h"
#include "base/geom2.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <string>

namespace construe::solver {

// ---------------------------------------------------------------- substitution §4.5

namespace {

struct UnionFind {
    std::vector<int> parent;
    std::vector<double> off;  // x_u = x_parent + off[u]

    explicit UnionFind(int n) : parent(n), off(n, 0.0) {
        for (int i = 0; i < n; ++i) parent[i] = i;
    }

    // Root and offset u relative to the root (with path compression).
    std::pair<int, double> find(int u) {
        if (parent[u] == u) return {u, 0.0};
        auto [r, o] = find(parent[u]);
        off[u] += o;
        parent[u] = r;
        return {r, off[u]};
    }
};

}  // namespace

Substitution substitute(const Model& m, const std::vector<int>& eqs, const std::vector<double>& x0,
                        const std::vector<int>& dragParams) {
    const int P = static_cast<int>(m.keys.size());
    const int G = P;
    UnionFind uf(P + 1);
    Substitution s;
    std::vector<bool> isDrag(P + 1, false);
    for (int d : dragParams) isDrag[d] = true;

    // Root priority: G > dragged > smaller ParamKey.
    auto better = [&](int a, int b) {  // true if a has priority over b
        if (a == G || b == G) return a == G;
        if (isDrag[a] != isDrag[b]) return static_cast<bool>(isDrag[a]);
        return a < b;
    };
    auto link = [&](int i, int j, double c) {  // x_i − x_j = c
        auto [ri, oi] = uf.find(i);
        auto [rj, oj] = uf.find(j);
        if (ri != rj) {
            const double k = c + oj - oi;  // x_ri = x_rj + k
            if (better(rj, ri)) {
                uf.parent[ri] = rj;
                uf.off[ri] = k;
            } else {
                uf.parent[rj] = ri;
                uf.off[rj] = -k;
            }
        } else {
            s.depsFlag = true;  // dependency (consistent or not) — the equation is checked at the end
        }
    };

    for (int p = 0; p < P; ++p)
        if (m.fixed[p]) link(p, G, x0[p]);
    for (int e : eqs) {
        const Equation& q = m.equations[e];
        if (q.kind == EqKind::Subst) link(q.i, q.j, q.c);
        else if (q.kind == EqKind::Fixation) link(q.i, G, q.c);
    }

    s.unk.assign(P, -1);
    s.off.assign(P, 0.0);
    std::vector<int> rootToUnk(P + 1, -1);
    for (int p = 0; p < P; ++p) {
        auto [r, o] = uf.find(p);
        s.off[p] = o;
        if (r == G) continue;
        if (rootToUnk[r] < 0) {
            rootToUnk[r] = static_cast<int>(s.rootParam.size());
            s.rootParam.push_back(r);
            s.members.emplace_back();
        }
        s.unk[p] = rootToUnk[r];
        s.members[rootToUnk[r]].push_back(p);
    }
    // Unknowns are ordered by root key.
    {
        std::vector<int> order(s.rootParam.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return s.rootParam[a] < s.rootParam[b]; });
        std::vector<int> inv(order.size());
        for (size_t i = 0; i < order.size(); ++i) inv[order[i]] = static_cast<int>(i);
        std::vector<int> roots(order.size());
        std::vector<std::vector<int>> mem(order.size());
        for (size_t i = 0; i < order.size(); ++i) {
            roots[i] = s.rootParam[order[i]];
            mem[i] = std::move(s.members[order[i]]);
        }
        s.rootParam = std::move(roots);
        s.members = std::move(mem);
        for (int p = 0; p < P; ++p)
            if (s.unk[p] >= 0) s.unk[p] = inv[s.unk[p]];
    }
    s.z0.resize(s.rootParam.size());
    for (size_t u = 0; u < s.members.size(); ++u) {
        double sum = 0.0;
        for (int p : s.members[u]) sum += x0[p] - s.off[p];
        s.z0[u] = sum / static_cast<double>(s.members[u].size());
    }
    for (int e : eqs) {
        const Equation& q = m.equations[e];
        if (q.kind != EqKind::General) continue;
        bool allConst = true;
        for (int p : q.params)
            if (s.unk[p] >= 0) allConst = false;
        if (allConst) {
            s.constantEqs.push_back(e);
            s.depsFlag = true;
        } else {
            s.generalEqs.push_back(e);
        }
    }
    return s;
}

std::vector<Component> components(const Model& m, const Substitution& s) {
    const int U = static_cast<int>(s.rootParam.size());
    std::vector<int> parent(U);
    for (int i = 0; i < U; ++i) parent[i] = i;
    std::function<int(int)> find = [&](int u) { return parent[u] == u ? u : parent[u] = find(parent[u]); };
    std::vector<int> eqFirstUnk;
    for (int e : s.generalEqs) {
        int first = -1;
        for (int p : m.equations[e].params) {
            const int u = s.unk[p];
            if (u < 0) continue;
            if (first < 0) first = u;
            else {
                const int a = find(first), b = find(u);
                if (a != b) parent[std::max(a, b)] = std::min(a, b);
            }
        }
        eqFirstUnk.push_back(first);
    }
    std::vector<int> compOf(U, -1);
    std::vector<Component> comps;
    std::vector<bool> hasEq(U, false);
    for (int f : eqFirstUnk) hasEq[find(f)] = true;
    for (int u = 0; u < U; ++u) {
        const int r = find(u);
        if (!hasEq[r]) continue;
        if (compOf[r] < 0) {
            compOf[r] = static_cast<int>(comps.size());
            comps.emplace_back();
        }
        comps[compOf[r]].unknowns.push_back(u);
    }
    for (size_t i = 0; i < s.generalEqs.size(); ++i) comps[compOf[find(eqFirstUnk[i])]].eqs.push_back(s.generalEqs[i]);
    // Component order — by smallest root key (unknowns are already in ascending root order).
    std::sort(comps.begin(), comps.end(), [](const Component& a, const Component& b) { return a.unknowns[0] < b.unknowns[0]; });
    for (auto& c : comps) std::sort(c.eqs.begin(), c.eqs.end());
    return comps;
}

std::vector<double> unfold(const Substitution& s, const std::vector<double>& z, const std::vector<double>& base) {
    std::vector<double> x = base;
    for (size_t p = 0; p < x.size(); ++p) {
        if (s.unk[p] >= 0) x[p] = z[s.unk[p]] + s.off[p];
        else x[p] = s.off[p];
    }
    return x;
}

void evalSystem(const Model& m, const Substitution& s, const std::vector<int>& eqs, const std::vector<int>& unknowns,
                const std::vector<double>& x, Eigen::VectorXd& r, Eigen::MatrixXd* J) {
    const int rows = static_cast<int>(eqs.size());
    const int cols = static_cast<int>(unknowns.size());
    r.resize(rows);
    if (J) J->setZero(rows, cols);
    std::vector<int> col(s.rootParam.size(), -1);
    for (int c = 0; c < cols; ++c) col[unknowns[c]] = c;
    std::vector<std::pair<int, double>> g;
    for (int i = 0; i < rows; ++i) {
        const Equation& q = m.equations[eqs[i]];
        r[i] = q.residual(x, m.L);
        if (!J) continue;
        q.gradient(x, m.L, g);
        for (auto [p, v] : g) {
            const int u = s.unk[p];
            if (u >= 0 && col[u] >= 0) (*J)(i, col[u]) += v * m.L;
        }
    }
}

double maxResidual(const Model& m, const std::vector<int>& eqs, const std::vector<double>& x) {
    double mx = 0.0;
    for (int e : eqs) mx = std::max(mx, std::abs(m.equations[e].residual(x, m.L)));
    return mx;
}

// ---------------------------------------------------------------- DogLeg §4.7 and LM §4.8

namespace {

struct CompContext {
    const Problem& p;
    const Model& m;
    const Substitution& s;
    const Component& comp;
    std::vector<double> xBase;  // full vector onto which the component unknowns are overlaid
    const CancelToken* cancel;

    std::vector<double> apply(const Eigen::VectorXd& zt) const {
        std::vector<double> x = xBase;
        for (size_t c = 0; c < comp.unknowns.size(); ++c) {
            const int u = comp.unknowns[c];
            for (int q : s.members[u]) x[q] = zt[static_cast<Eigen::Index>(c)] * m.L + s.off[q];
        }
        return x;
    }
    void eval(const Eigen::VectorXd& zt, Eigen::VectorXd& r, Eigen::MatrixXd* J) const {
        evalSystem(m, s, comp.eqs, comp.unknowns, apply(zt), r, J);
    }
    bool finite(const Eigen::VectorXd& r) const { return r.allFinite(); }
};

struct IterResult {
    IterOutcome outcome;
    Eigen::VectorXd z;
    int iterations;
};

IterResult dogLeg(const CompContext& ctx, const Eigen::VectorXd& z0) {
    Eigen::VectorXd z = z0, r, rNew;
    Eigen::MatrixXd J;
    double delta = DL_DELTA0;
    ctx.eval(z, r, &J);
    for (int k = 0;; ++k) {
        if (r.lpNorm<Eigen::Infinity>() <= TOL_CONV) return {IterOutcome::Converged, z, k};
        const Eigen::VectorXd g = J.transpose() * r;
        if (g.lpNorm<Eigen::Infinity>() <= TOL_GRAD) return {IterOutcome::Stationary, z, k};
        if (k >= DL_MAX_ITER) return {IterOutcome::IterLimit, z, k};
        if (isCancelled(ctx.cancel)) return {IterOutcome::Cancelled, z, k};

        const Eigen::VectorXd hgn = minNormStep(J, r);
        const Eigen::VectorXd Jg = J * g;
        const double alpha = g.squaredNorm() / std::max(Jg.squaredNorm(), std::numeric_limits<double>::min());
        const Eigen::VectorXd hsd = -alpha * g;
        Eigen::VectorXd h;
        if (hgn.norm() <= delta) {
            h = hgn;
        } else if (hsd.norm() >= delta) {
            h = -(delta / g.norm()) * g;
        } else {
            const Eigen::VectorXd a = hsd, w = hgn - hsd;
            const double c = a.dot(w), q = delta * delta - a.squaredNorm(), ww = w.squaredNorm();
            const double disc = std::sqrt(c * c + ww * q);
            const double beta = c <= 0 ? (-c + disc) / ww : q / (c + disc);
            h = a + beta * w;
        }
        if (h.norm() <= EPS_STEP * (z.norm() + EPS_STEP)) return {IterOutcome::SmallStep, z, k};

        const Eigen::VectorXd zNew = z + h;
        double rho;
        if (!guardsAccept(ctx.m, ctx.p, ctx.apply(z), ctx.apply(zNew))) {
            rho = -1;
        } else {
            ctx.eval(zNew, rNew, nullptr);
            if (!rNew.allFinite()) {
                rho = -1;
            } else {
                const double actual = 0.5 * r.squaredNorm() - 0.5 * rNew.squaredNorm();
                const double pred = -(g.dot(h) + 0.5 * (J * h).squaredNorm());
                rho = pred > 0 ? actual / pred : -1;
            }
        }
        if (rho > 0) {
            z = zNew;
            ctx.eval(z, r, &J);
        }
        if (rho > RHO_HIGH) delta = std::max(delta, DL_GROW * h.norm());
        else if (rho < RHO_LOW) delta = DL_SHRINK * std::min(delta, h.norm());
        if (delta <= EPS_STEP * (z.norm() + EPS_STEP)) return {IterOutcome::SmallStep, z, k + 1};
    }
}

IterResult levenbergMarquardt(const CompContext& ctx, const Eigen::VectorXd& z0) {
    Eigen::VectorXd z = z0, r, rNew;
    Eigen::MatrixXd J;
    ctx.eval(z, r, &J);
    double nu = LM_NU0;
    double mu = LM_TAU * (J.transpose() * J).diagonal().maxCoeff();
    if (!(mu > 0)) mu = LM_TAU;
    const Eigen::Index n = z.size();
    for (int k = 0;; ++k) {
        if (r.lpNorm<Eigen::Infinity>() <= TOL_CONV) return {IterOutcome::Converged, z, k};
        const Eigen::VectorXd g = J.transpose() * r;
        if (g.lpNorm<Eigen::Infinity>() <= TOL_GRAD) return {IterOutcome::Stationary, z, k};
        if (k >= LM_MAX_ITER) return {IterOutcome::IterLimit, z, k};
        if (isCancelled(ctx.cancel)) return {IterOutcome::Cancelled, z, k};

        Eigen::MatrixXd A(J.rows() + n, n);
        A << J, std::sqrt(mu) * Eigen::MatrixXd::Identity(n, n);
        Eigen::VectorXd b(J.rows() + n);
        b << -r, Eigen::VectorXd::Zero(n);
        const Eigen::VectorXd h = A.householderQr().solve(b);
        if (h.norm() <= EPS_STEP * (z.norm() + EPS_STEP)) return {IterOutcome::SmallStep, z, k};

        const Eigen::VectorXd zNew = z + h;
        double rho = -1;
        if (guardsAccept(ctx.m, ctx.p, ctx.apply(z), ctx.apply(zNew))) {
            ctx.eval(zNew, rNew, nullptr);
            if (rNew.allFinite()) {
                const double denom = 0.5 * h.dot(mu * h - g);
                rho = denom > 0 ? (0.5 * r.squaredNorm() - 0.5 * rNew.squaredNorm()) / denom : -1;
            }
        }
        if (rho > 0) {
            z = zNew;
            ctx.eval(z, r, &J);
            mu *= std::max(1.0 / 3.0, 1.0 - std::pow(2.0 * rho - 1.0, 3));
            nu = LM_NU0;
        } else {
            mu *= nu;
            nu *= 2;
        }
    }
}

struct StepsResult {
    bool converged = false;
    bool cancelled = false;
    std::vector<double> x;
    int iterations = 0;
    bool usedLM = false;
    bool anyComp = false;
};

// Steps 1–4 of solveCore.
StepsResult solveSteps(const Problem& p, const Model& m, const std::vector<int>& eqs, const std::vector<double>& x0,
                       const CancelToken* cancel) {
    StepsResult res;
    const Substitution s = substitute(m, eqs, x0);
    const std::vector<Component> comps = components(m, s);
    std::vector<double> x = unfold(s, s.z0, x0);
    for (const Component& comp : comps) {
        res.anyComp = true;
        if (static_cast<int>(comp.unknowns.size()) > DENSE_WARN)
            log::warn("solver: dense component with " + std::to_string(comp.unknowns.size()) + " unknowns");
        CompContext ctx{p, m, s, comp, x, cancel};
        Eigen::VectorXd zt0(static_cast<Eigen::Index>(comp.unknowns.size()));
        for (size_t c = 0; c < comp.unknowns.size(); ++c) zt0[static_cast<Eigen::Index>(c)] = s.z0[comp.unknowns[c]] / m.L;
        IterResult dl = dogLeg(ctx, zt0);
        res.iterations += dl.iterations;
        IterResult best = dl;
        if (dl.outcome == IterOutcome::Cancelled) {
            res.cancelled = true;
            return res;
        }
        if (dl.outcome != IterOutcome::Converged) {
            IterResult lm = levenbergMarquardt(ctx, zt0);
            res.iterations += lm.iterations;
            if (lm.outcome == IterOutcome::Cancelled) {
                res.cancelled = true;
                return res;
            }
            res.usedLM = true;
            Eigen::VectorXd rd, rl;
            ctx.eval(dl.z, rd, nullptr);
            ctx.eval(lm.z, rl, nullptr);
            if (lm.outcome == IterOutcome::Converged || rl.squaredNorm() < rd.squaredNorm()) best = lm;
        }
        x = ctx.apply(best.z);
    }
    res.x = x;
    res.converged = maxResidual(m, eqs, x) <= TOL_CONV;
    return res;
}

// Shape preservation (§4.9.1): driving equations are solved exactly (minimum-norm step), and the freedom of
// their Jacobian's null space is spent on keeping segment lengths and radii as they were in xPrev.
struct ShapeContext {
    const Problem& p;
    const Model& m;
    const std::vector<int>& eqs;
    std::vector<double> xBase;
    std::vector<int> free;  // indices of free parameters
    const CancelToken* cancel;
    struct Soft {
        int kind;  // 0 — distance between two points (segment, arc radius), 1 — circle radius or semi-axis b
                   // of an ellipse, 2 — length of vector (ax, ay) — semi-axis a of an ellipse (§4.9.1)
        int ax, ay, bx, by, pr;
        double len0;
    };
    std::vector<Soft> soft;
    std::vector<int> col;  // column of the free parameter, or −1

    std::vector<double> apply(const Eigen::VectorXd& z) const {
        std::vector<double> x = xBase;
        for (size_t c = 0; c < free.size(); ++c) x[free[c]] = z[static_cast<Eigen::Index>(c)] * m.L;
        return x;
    }
    // Driving equations: r̃ and Jacobian w.r.t. scaled z.
    void hard(const std::vector<double>& x, Eigen::VectorXd& r, Eigen::MatrixXd* J) const {
        r.resize(static_cast<Eigen::Index>(eqs.size()));
        if (J) J->setZero(static_cast<Eigen::Index>(eqs.size()), static_cast<Eigen::Index>(free.size()));
        std::vector<std::pair<int, double>> g;
        for (size_t i = 0; i < eqs.size(); ++i) {
            const Equation& q = m.equations[eqs[i]];
            r[static_cast<Eigen::Index>(i)] = q.residual(x, m.L);
            if (!J) continue;
            q.gradient(x, m.L, g);
            for (auto [pi, v] : g)
                if (col[pi] >= 0) (*J)(static_cast<Eigen::Index>(i), col[pi]) += v * m.L;
        }
    }
    // Soft terms q_k = (size − size in xPrev) / L and their Jacobian w.r.t. z.
    void shape(const std::vector<double>& x, Eigen::VectorXd& r, Eigen::MatrixXd* J) const {
        r.resize(static_cast<Eigen::Index>(soft.size()));
        if (J) J->setZero(static_cast<Eigen::Index>(soft.size()), static_cast<Eigen::Index>(free.size()));
        for (size_t k = 0; k < soft.size(); ++k) {
            const Soft& s = soft[k];
            const auto row = static_cast<Eigen::Index>(k);
            if (s.kind == 1) {
                r[row] = (x[s.pr] - s.len0) / m.L;
                if (J && col[s.pr] >= 0) (*J)(row, col[s.pr]) += 1;
                continue;
            }
            if (s.kind == 2) {
                const double d = std::hypot(x[s.ax], x[s.ay]);
                r[row] = (d - s.len0) / m.L;
                if (!J || d <= 0) continue;
                if (col[s.ax] >= 0) (*J)(row, col[s.ax]) += x[s.ax] / d;
                if (col[s.ay] >= 0) (*J)(row, col[s.ay]) += x[s.ay] / d;
                continue;
            }
            const double dx = x[s.bx] - x[s.ax], dy = x[s.by] - x[s.ay], d = std::hypot(dx, dy);
            r[row] = (d - s.len0) / m.L;
            if (!J || d <= 0) continue;
            const double ux = dx / d, uy = dy / d;
            if (col[s.bx] >= 0) (*J)(row, col[s.bx]) += ux;
            if (col[s.by] >= 0) (*J)(row, col[s.by]) += uy;
            if (col[s.ax] >= 0) (*J)(row, col[s.ax]) -= ux;
            if (col[s.ay] >= 0) (*J)(row, col[s.ay]) -= uy;
        }
    }
};

// Largest change of a segment length, arc or circle radius (except changing) between xa and xb.
double shapeChange(const Model& m, const std::vector<double>& xa, const std::vector<double>& xb, const std::set<EntityId>& changing) {
    double mx = 0;
    auto len = [&](const std::vector<double>& x, EntityId a, EntityId b) {
        auto [ax, ay] = m.pointParams(a);
        auto [bx, by] = m.pointParams(b);
        return std::hypot(x[bx] - x[ax], x[by] - x[ay]);
    };
    for (const auto& [id, e] : m.entities) {
        if (changing.count(id)) continue;
        if (e.kind == EntityKind::Line) mx = std::max(mx, std::abs(len(xa, e.p1, e.p2) - len(xb, e.p1, e.p2)));
        else if (e.kind == EntityKind::Arc) mx = std::max(mx, std::abs(len(xa, e.center, e.start) - len(xb, e.center, e.start)));
        else if (e.kind == EntityKind::Circle) mx = std::max(mx, std::abs(xa[e.pr] - xb[e.pr]));
        else if (isEllipseKind(e.kind))
            mx = std::max({mx, std::abs(std::hypot(xa[e.pux], xa[e.puy]) - std::hypot(xb[e.pux], xb[e.puy])),
                           std::abs(xa[e.pb] - xb[e.pb])});
    }
    return mx;
}

std::vector<double> shapeHold(const Problem& p, const Model& m, const std::vector<int>& eqs, const std::vector<double>& x,
                              const std::set<EntityId>& changing, const CancelToken* cancel, bool& cancelled) {
    ShapeContext ctx{p, m, eqs, x, {}, cancel, {}, std::vector<int>(m.keys.size(), -1)};
    for (size_t i = 0; i < m.keys.size(); ++i)
        if (!m.fixed[i]) {
            ctx.col[i] = static_cast<int>(ctx.free.size());
            ctx.free.push_back(static_cast<int>(i));
        }
    if (ctx.free.empty()) return x;
    const double eps = EPS_LEN * m.L;
    for (const auto& [id, e] : m.entities) {
        if (changing.count(id)) continue;  // the size of this entity changes intentionally
        if (e.kind == EntityKind::Line || e.kind == EntityKind::Arc) {
            auto [ax, ay] = m.pointParams(e.kind == EntityKind::Line ? e.p1 : e.center);
            auto [bx, by] = m.pointParams(e.kind == EntityKind::Line ? e.p2 : e.start);
            const double len0 = std::hypot(x[bx] - x[ax], x[by] - x[ay]);
            if (len0 >= eps) ctx.soft.push_back({0, ax, ay, bx, by, -1, len0});
        } else if (e.kind == EntityKind::Circle) {
            if (x[e.pr] >= eps) ctx.soft.push_back({1, -1, -1, -1, -1, e.pr, x[e.pr]});
        } else if (isEllipseKind(e.kind)) {  // two soft links: ‖u‖ and b (§4.9.1)
            const double a0 = std::hypot(x[e.pux], x[e.puy]);
            if (a0 >= eps) ctx.soft.push_back({2, e.pux, e.puy, -1, -1, -1, a0});
            if (x[e.pb] >= eps) ctx.soft.push_back({1, -1, -1, -1, -1, e.pb, x[e.pb]});
        }
    }
    const auto n = static_cast<Eigen::Index>(ctx.free.size());
    auto merit = [&](const std::vector<double>& xv) {
        Eigen::VectorXd rh, rs;
        ctx.hard(xv, rh, nullptr);
        ctx.shape(xv, rs, nullptr);
        return rh.squaredNorm() + SHAPE_WEIGHT * SHAPE_WEIGHT * rs.squaredNorm();
    };
  // One least-squares pass from x with the current set of soft terms.
  auto run = [&]() -> std::vector<double> {
    Eigen::VectorXd z(n);
    for (Eigen::Index c = 0; c < n; ++c) z[c] = x[ctx.free[c]] / m.L;
    std::vector<double> xc = ctx.apply(z);
    double phi = merit(xc);
    for (int k = 0; k < SHAPE_MAX_ITER; ++k) {
        if (isCancelled(cancel)) {
            cancelled = true;
            return x;
        }
        Eigen::VectorXd rh, rs;
        Eigen::MatrixXd Jh, Js;
        ctx.hard(xc, rh, &Jh);
        ctx.shape(xc, rs, &Js);
        // h = h_p + N·y: h_p — minimum-norm step for driving eqs, N — basis of ker Jh, y — LSQ for the soft terms.
        Eigen::VectorXd h = eqs.empty() ? Eigen::VectorXd::Zero(n) : minNormStep(Jh, rh);
        if (!ctx.soft.empty()) {
            Eigen::MatrixXd N;
            if (eqs.empty()) {
                N = Eigen::MatrixXd::Identity(n, n);
            } else {
                const OrrqrResult q = orrqr(Jh.transpose());
                N = q.Q.rightCols(n - q.rank);
            }
            if (N.cols() > 0) h += N * minNormSolve(Js * N, -(rs + Js * h));
        }
        if (h.norm() > SHAPE_STEP_MAX) h *= SHAPE_STEP_MAX / h.norm();
        if (h.norm() <= EPS_STEP * (z.norm() + EPS_STEP)) break;
        bool accepted = false;
        for (int t = 0; t < SHAPE_BACKTRACK && !accepted; ++t) {
            const Eigen::VectorXd zn = z + std::ldexp(1.0, -t) * h;
            const std::vector<double> xn = ctx.apply(zn);
            if (!guardsAccept(m, p, xc, xn)) continue;
            const double phin = merit(xn);
            if (!(phin < phi)) continue;
            z = zn, xc = xn, phi = phin, accepted = true;
        }
        if (!accepted) break;
    }
    return xc;
  };
    // Two passes (§4.9.1): lengths and radii that the driving equations do not allow to keep (e.g. the slot lines
    // at a new center distance) are excluded — otherwise an unreachable soft condition pulls the sketch along free
    // directions (part 07: the slot drifted sideways).
    std::vector<double> xs = run();
    if (cancelled) return x;
    Eigen::VectorXd q;
    ctx.shape(xs, q, nullptr);
    std::vector<ShapeContext::Soft> reachable;
    for (size_t k = 0; k < ctx.soft.size(); ++k)
        if (std::abs(q[static_cast<Eigen::Index>(k)]) <= SHAPE_TOL) reachable.push_back(ctx.soft[k]);
    if (reachable.size() == ctx.soft.size()) return xs;
    ctx.soft = std::move(reachable);
    xs = run();
    return cancelled ? x : xs;
}

// Changed values for homotopy (§4.9): items and the initial number of steps N0.
struct HomotopyItem {
    ConstraintId id;
    ConstraintType type;
    double m0, v;               // for values
    double phi0, phi;           // for angle
    std::vector<double> t0, t;  // for fix
};

struct HomotopyPlan {
    std::vector<HomotopyItem> items;
    int n0 = 1;
};

HomotopyPlan homotopyPlan(const Problem& p, const Model& m, const std::vector<int>& eqs, const std::vector<double>& x0) {
    using Item = HomotopyItem;
    HomotopyPlan plan;
    std::vector<Item>& items = plan.items;
    // Which constraints are included in eqs.
    std::vector<ConstraintId> active;
    for (int e : eqs)
        if (m.equations[e].group == 1) active.push_back(m.equations[e].owner);
    std::sort(active.begin(), active.end());
    active.erase(std::unique(active.begin(), active.end()), active.end());
    for (ConstraintId cid : active) {
        const ConstraintSpec* cp = p.constraint(cid);
        if (!cp) continue;  // general system (§3.8): no sketch constraints — no homotopy over values
        const ConstraintSpec& c = *cp;
        if (c.type == ConstraintType::Fix) {
            const ModelEntity& e = m.entities.at(c.refs[0]);
            std::vector<double> cur;
            if (e.kind == EntityKind::Point) cur = {x0[e.px], x0[e.py]};
            else if (e.kind == EntityKind::Ellipse) {
                auto [ox, oy] = m.pointParams(e.center);
                cur = {x0[ox], x0[oy], x0[e.pux], x0[e.puy], x0[e.pb]};
            } else {
                auto [ox, oy] = m.pointParams(e.center);
                cur = {x0[ox], x0[oy], x0[e.pr]};
            }
            bool differs = false;
            for (size_t i = 0; i < cur.size(); ++i) differs = differs || std::abs(c.target[i] - cur[i]) > TOL_CONV * m.L;
            if (differs) items.push_back({cid, c.type, 0, 0, 0, 0, cur, c.target});
            continue;
        }
        if (!isDimension(c.type) || !c.driving || !c.value) continue;
        const double m0 = measureAt(m, c, x0);
        if (c.type == ConstraintType::Angle) {
            const Branch br = c.branch.value_or(Branch{});
            const double phi = br.sign * (br.supplementary ? PI - *c.value : *c.value);
            const ModelEntity& l1 = m.entities.at(c.refs[0]);
            const ModelEntity& l2 = m.entities.at(c.refs[1]);
            auto P = [&](EntityId id) {
                auto [ix, iy] = m.pointParams(id);
                return std::pair<double, double>{x0[ix], x0[iy]};
            };
            auto [a1x, a1y] = P(l1.p1);
            auto [b1x, b1y] = P(l1.p2);
            auto [a2x, a2y] = P(l2.p1);
            auto [b2x, b2y] = P(l2.p2);
            const double d1x = b1x - a1x, d1y = b1y - a1y, d2x = b2x - a2x, d2y = b2y - a2y;
            const double alpha0 = datan2(d1x * d2y - d1y * d2x, d1x * d2x + d1y * d2y);
            const double phi0 = phi + geom2::normalizeAngleSym(alpha0 - phi);
            if (std::abs(phi0 - phi) > TOL_CONV) items.push_back({cid, c.type, 0, 0, phi0, phi, {}, {}});
        } else if (c.type == ConstraintType::ArcAngle) {
            // Central angle — radians: threshold and step as for angle (§4.9).
            if (std::abs(*c.value - m0) > TOL_CONV) items.push_back({cid, c.type, m0, *c.value, 0, 0, {}, {}});
        } else if (c.type == ConstraintType::EllipseAngle) {
            // Ellipse rotation — modulo π: m0 is the one nearest to the value (§4.9, S4-SKT-004).
            double d = m0 - *c.value;
            while (d > PI / 2) d -= PI;
            while (d <= -PI / 2) d += PI;
            if (std::abs(d) > TOL_CONV) items.push_back({cid, ConstraintType::ArcAngle, *c.value + d, *c.value, 0, 0, {}, {}});
        } else if (std::abs(*c.value - m0) > TOL_CONV * m.L) {
            // radius/diameter/distance — m0 in the same units as value
            items.push_back({cid, c.type, m0, *c.value, 0, 0, {}, {}});
        }
    }
    double steps = 1;
    for (const Item& it : items) {
        double d = 0, s = 1;
        if (it.type == ConstraintType::Angle) {
            d = std::abs(it.phi - it.phi0), s = HOMOTOPY_ANGLE;
        } else if (it.type == ConstraintType::Fix) {
            for (size_t i = 0; i < it.t.size(); ++i) d = std::max(d, std::abs(it.t[i] - it.t0[i]));
            s = HOMOTOPY_REL * m.L;
        } else if (it.type == ConstraintType::ArcAngle) {
            d = std::abs(it.v - it.m0), s = HOMOTOPY_ANGLE;
        } else {
            d = std::abs(it.v - it.m0), s = HOMOTOPY_REL * std::max({std::abs(it.m0), std::abs(it.v), m.L});
        }
        steps = std::max(steps, std::ceil(d / s - 1e-12));
    }
    plan.n0 = static_cast<int>(std::min<double>(HOMOTOPY_MAX_N0, steps));
    return plan;
}

// Homotopy over dimension values with adaptive step (§4.9).
bool homotopy(const Problem& p, const Model& m, const std::vector<int>& eqs, const std::vector<double>& x0,
              const HomotopyPlan& plan, int n0, const CancelToken* cancel, std::vector<double>& xOut, int& iterations,
              bool& cancelled) {
    using Item = HomotopyItem;
    const std::vector<Item>& items = plan.items;
    if (items.empty()) return false;
    auto modelAt = [&](double t) {
        ValueOverrides ov;
        for (const Item& it : items) {
            if (it.type == ConstraintType::Angle) ov.phi[it.id] = it.phi0 + t * (it.phi - it.phi0);
            else if (it.type == ConstraintType::Fix) {
                std::vector<double> tk(it.t.size());
                for (size_t i = 0; i < tk.size(); ++i) tk[i] = it.t0[i] + t * (it.t[i] - it.t0[i]);
                ov.target[it.id] = tk;
            } else {
                ov.value[it.id] = it.m0 + t * (it.v - it.m0);
            }
        }
        Model mk = buildModel(p, &ov);
        mk.L = m.L;  // L is fixed for the whole call (§4.1); computeL does not depend on overrides
        return mk;
    };
    // Entities whose size the changed constraints alter: references and segments between two reference points.
    std::set<EntityId> changing;
    for (const Item& it : items) {
        const ConstraintSpec& c = *p.constraint(it.id);
        if (c.type == ConstraintType::EllipseAngle) continue;  // rotation keeps ellipse semi-axes (§4.9, S4-SKT-004)
        std::set<EntityId> pts;
        for (EntityId r : c.refs) {
            changing.insert(r);
            if (m.entities.at(r).kind == EntityKind::Point) pts.insert(r);
        }
        for (const auto& [id, e] : m.entities)
            if (e.kind == EntityKind::Line && pts.count(e.p1) && pts.count(e.p2)) changing.insert(id);
    }
    std::vector<double> x = x0;
    const double dtMax = 1.0 / n0;
    double t = 0, dt = dtMax;
    int halvings = 0;  // consecutive failures
    while (t < 1) {
        const double tn = t + dt >= 1 - 1e-12 ? 1.0 : t + dt;
        const Model mk = modelAt(tn);
        // An intermediate point (t' < 1) needs no full precision — it only leads to the solution (§4.9). A step moving
        // any value by more than HOMOTOPY_MOVE_MAX·L is an escape along a nearly degenerate "valley" (tangency), not a
        // smooth change: it is not accepted.
        auto reached = [&](const StepsResult& r) {
            for (size_t i = 0; i < x.size(); ++i)
                if (std::abs(r.x[i] - x[i]) > HOMOTOPY_MOVE_MAX * m.L) return false;
            return r.converged || (tn < 1 && maxResidual(mk, eqs, r.x) <= HOMOTOPY_TOL_MID);
        };
        // Exact solve from x; if it already kept the shape of unconstrained parts, shape preservation is not needed.
        StepsResult direct = solveSteps(p, mk, eqs, x, cancel);
        iterations += direct.iterations;
        if (direct.cancelled) {
            cancelled = true;
            return false;
        }
        StepsResult sr = direct;
        if (!reached(direct) || shapeChange(m, x, direct.x, changing) > SHAPE_TOL * m.L) {
            // Shape preservation, then exact solve from its result; fallback — exact solve from x.
            const std::vector<double> xs = shapeHold(p, mk, eqs, x, changing, cancel, cancelled);
            if (cancelled) return false;
            sr = solveSteps(p, mk, eqs, xs, cancel);
            iterations += sr.iterations;
            if (sr.cancelled) {
                cancelled = true;
                return false;
            }
            if (!reached(sr)) sr = direct;
        }
        if (reached(sr)) {
            x = sr.x;
            t = tn;
            dt = std::min(2 * dt, dtMax);
            halvings = 0;
        } else {
            dt /= 2;
            if (dt < HOMOTOPY_MIN_DT || ++halvings > HOMOTOPY_MAX_HALVINGS) {
                xOut = x;
                return false;
            }
        }
    }
    xOut = x;
    return maxResidual(m, eqs, x) <= TOL_CONV;
}

}  // namespace

bool withinMoveLimit(const Model& m, const std::vector<double>& x0, const std::vector<double>& x) {
    for (size_t i = 0; i < x0.size() && i < x.size(); ++i)
        if (std::abs(x[i] - x0[i]) > SOLVE_MOVE_MAX * m.L) return false;
    return true;
}

CoreResult solveCore(const Problem& p, const Model& m, const std::vector<int>& eqs, const std::vector<double>& x0,
                     const CancelToken* cancel, bool allowHomotopy, bool limitMove) {
    CoreResult res;
    HomotopyPlan plan;
    if (allowHomotopy) plan = homotopyPlan(p, m, eqs, x0);
    // Values are changed in small steps with shape preservation: a large step may jump to another branch
    // (§4.4.1 step 0, vision item 15). The direct pass is only a fallback if the steps did not get there.
    std::vector<double> xh;
    if (!plan.items.empty()) {
        bool cancelled = false;
        const bool ok = homotopy(p, m, eqs, x0, plan, plan.n0, cancel, xh, res.iterations, cancelled);
        if (cancelled) {
            res.cancelled = true;
            return res;
        }
        if (ok) {
            res.converged = true;
            res.x1 = xh;
            res.xBest = xh;
            res.method = SolveMethod::Homotopy;
            return res;
        }
    }
    StepsResult sr = solveSteps(p, m, eqs, x0, cancel);
    res.iterations += sr.iterations;
    if (sr.cancelled) {
        res.cancelled = true;
        return res;
    }
    // A solution far outside the sketch is an escape along a near-degenerate "valley", not a solution (§4.4.1 step 4).
    if (limitMove && sr.converged && !withinMoveLimit(m, x0, sr.x)) {
        sr.converged = false;
        res.xFar = sr.x;
    }
    res.x1 = sr.x;
    res.xBest = sr.x;
    if (sr.converged) {
        res.converged = true;
        res.method = !sr.anyComp ? SolveMethod::None : (sr.usedLM ? SolveMethod::LevenbergMarquardt : SolveMethod::DogLeg);
        return res;
    }
    if (!xh.empty() && maxResidual(m, eqs, xh) < maxResidual(m, eqs, sr.x)) res.xBest = xh;
    res.method = SolveMethod::None;
    return res;
}

// ---------------------------------------------------------------- rank §4.11

RankInfo fullRank(const Model& m, const std::vector<int>& eqs, const std::vector<double>& x) {
    RankInfo ri;
    const int P = static_cast<int>(m.keys.size());
    std::vector<int> row(P, -1);
    for (int p = 0; p < P; ++p)
        if (!m.fixed[p]) {
            row[p] = static_cast<int>(ri.freeIdx.size());
            ri.freeIdx.push_back(p);
        }
    const int n = static_cast<int>(ri.freeIdx.size());
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(n, static_cast<Eigen::Index>(eqs.size()));
    std::vector<std::pair<int, double>> g;
    for (size_t c = 0; c < eqs.size(); ++c) {
        m.equations[eqs[c]].gradient(x, m.L, g);
        for (auto [p, v] : g)
            if (row[p] >= 0) A(row[p], static_cast<Eigen::Index>(c)) += v * m.L;
    }
    ri.eqs = eqs;
    ri.qr = orrqr(A);
    ri.rank = ri.qr.rank;
    ri.dof = n - ri.rank;
    ri.freeParam.assign(P, false);
    for (int i = 0; i < n; ++i) {
        const double rn = n > ri.rank ? ri.qr.Q.row(i).tail(n - ri.rank).norm() : 0.0;
        ri.freeParam[ri.freeIdx[i]] = rn > TAU_FREE;
    }
    return ri;
}

RankInfo componentRank(const Model& m, const std::vector<int>& eqs, const std::vector<double>& x, bool& depsFlag) {
    RankInfo ri;
    const Substitution s = substitute(m, eqs, x);
    const std::vector<Component> comps = components(m, s);
    depsFlag = s.depsFlag;
    const int U = static_cast<int>(s.rootParam.size());
    std::vector<bool> freeUnk(U, true);  // isolated ones are free
    int rankSum = 0;
    for (const Component& c : comps) {
        Eigen::VectorXd r;
        Eigen::MatrixXd J;
        evalSystem(m, s, c.eqs, c.unknowns, x, r, &J);
        const OrrqrResult q = orrqr(J.transpose());
        rankSum += q.rank;
        if (q.rank < static_cast<int>(c.eqs.size())) depsFlag = true;
        const int n = static_cast<int>(c.unknowns.size());
        for (int i = 0; i < n; ++i) {
            const double rn = n > q.rank ? q.Q.row(i).tail(n - q.rank).norm() : 0.0;
            freeUnk[c.unknowns[i]] = rn > TAU_FREE;
        }
    }
    ri.rank = rankSum;
    ri.dof = U - rankSum;
    ri.freeParam.assign(m.keys.size(), false);
    for (size_t p = 0; p < m.keys.size(); ++p)
        if (s.unk[p] >= 0) ri.freeParam[p] = freeUnk[s.unk[p]];
    return ri;
}

std::vector<ParamKey> freeKeys(const Model& m, const std::vector<bool>& freeParam) {
    std::vector<ParamKey> out;
    for (size_t p = 0; p < m.keys.size(); ++p)
        if (freeParam[p]) out.push_back(m.keys[p]);
    return out;
}

}  // namespace construe::solver
