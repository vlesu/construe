// Mesh preparation and segmentation into single-surface regions (spec/mesh-fit.md §4.0, §4.4).
#include "meshfit/meshfit_internal.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <numeric>
#include <set>

#include <Eigen/Eigenvalues>

namespace construe::meshfit {

// ---------------------------------------------------------------- preparation (§4.0)

double meshVolume(const Mesh& m) {
    double v = 0;
    for (const auto& t : m.triangles)
        v += m.vertices[t[0]].dot(m.vertices[t[1]].cross(m.vertices[t[2]]));
    return v / 6;
}

Result<std::vector<Mesh>> splitShells(const Mesh& in) {
    // 1. Welding by exact coordinate match; collapsed and duplicate triangles are removed.
    std::map<std::array<double, 3>, uint32_t> idx;
    std::vector<Point3> verts;
    std::vector<uint32_t> remap(in.vertices.size());
    for (size_t i = 0; i < in.vertices.size(); ++i) {
        const Point3& p = in.vertices[i];
        std::array<double, 3> k{p.x() == 0 ? 0.0 : p.x(), p.y() == 0 ? 0.0 : p.y(), p.z() == 0 ? 0.0 : p.z()};
        auto [it, ins] = idx.emplace(k, uint32_t(verts.size()));
        if (ins) verts.push_back({k[0], k[1], k[2]});
        remap[i] = it->second;
    }
    std::vector<std::array<uint32_t, 3>> tris;
    std::set<std::array<uint32_t, 3>> seen;
    for (const auto& t0 : in.triangles) {
        std::array<uint32_t, 3> t{remap[t0[0]], remap[t0[1]], remap[t0[2]]};
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        std::array<uint32_t, 3> key = t;
        std::sort(key.begin(), key.end());
        if (!seen.insert(key).second) continue;
        tris.push_back(t);
    }
    // 2. Edges: each has exactly two triangles.
    std::map<std::pair<uint32_t, uint32_t>, std::vector<std::pair<int, int>>> edges;  // {min,max} → (triangle, k)
    for (size_t t = 0; t < tris.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = tris[t][size_t(k)], b = tris[t][size_t((k + 1) % 3)];
            edges[{std::min(a, b), std::max(a, b)}].push_back({int(t), k});
        }
    size_t open = 0, nonManifold = 0;
    for (const auto& [e, l] : edges) {
        if (l.size() == 1) ++open;
        if (l.size() > 2) ++nonManifold;
    }
    if (open || nonManifold)
        return Error("MESHFIT_NOT_CLOSED").with("open", double(open)).with("nonManifold", double(nonManifold));
    // 3. Connected shells and consistent orientation (BFS; a neighbor with the same directed edge is flipped).
    std::vector<int> comp(tris.size(), -1);
    std::vector<Mesh> out;
    for (size_t s = 0; s < tris.size(); ++s) {
        if (comp[s] >= 0) continue;
        const int ci = int(out.size());
        std::vector<int> members{int(s)};
        comp[s] = ci;
        for (size_t q = 0; q < members.size(); ++q) {
            const int t = members[q];
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = tris[size_t(t)][size_t(k)], b = tris[size_t(t)][size_t((k + 1) % 3)];
                for (const auto& [u, ku] : edges[{std::min(a, b), std::max(a, b)}]) {
                    if (u == t || comp[size_t(u)] >= 0) continue;
                    // Consistent: in the neighbor the edge goes b → a.
                    if (tris[size_t(u)][size_t(ku)] == a) std::swap(tris[size_t(u)][1], tris[size_t(u)][2]);
                    comp[size_t(u)] = ci;
                    members.push_back(u);
                }
            }
        }
        std::sort(members.begin(), members.end());
        Mesh m;
        std::map<uint32_t, uint32_t> local;
        for (int t : members) {
            std::array<uint32_t, 3> lt{};
            for (int k = 0; k < 3; ++k) {
                const uint32_t g = tris[size_t(t)][size_t(k)];
                auto [it, ins] = local.emplace(g, uint32_t(m.vertices.size()));
                if (ins) m.vertices.push_back(verts[g]);
                lt[size_t(k)] = it->second;
            }
            m.triangles.push_back(lt);
        }
        if (meshVolume(m) < 0)
            for (auto& t : m.triangles) std::swap(t[1], t[2]);
        out.push_back(std::move(m));
    }
    // Orientation is checked after the flips: each directed edge occurs exactly once (otherwise non-orientable).
    for (const auto& m : out) {
        std::set<std::pair<uint32_t, uint32_t>> dir;
        for (const auto& t : m.triangles)
            for (int k = 0; k < 3; ++k)
                if (!dir.insert({t[size_t(k)], t[size_t((k + 1) % 3)]}).second)
                    return Error("MESHFIT_NOT_CLOSED").with("open", 0.0).with("nonManifold", 0.0);
    }
    return out;
}

Topo buildTopo(const Mesh& m) {
    Topo t;
    t.mesh = &m;
    const size_t n = m.triangles.size();
    t.nbr.assign(n, {-1, -1, -1});
    t.normal.resize(n);
    t.area.resize(n);
    t.centroid.resize(n);
    std::map<std::pair<uint32_t, uint32_t>, std::pair<int, int>> dir;
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) dir[{m.triangles[i][size_t(k)], m.triangles[i][size_t((k + 1) % 3)]}] = {int(i), k};
    BBox3 bb;
    for (const auto& p : m.vertices) {
        bb.extend(p);
        t.maxCoord = std::max(t.maxCoord, p.cwiseAbs().maxCoeff());
    }
    t.diag = bb.isEmpty() ? 0 : bb.diagonal().norm();
    for (size_t i = 0; i < n; ++i) {
        const auto& tr = m.triangles[i];
        const Point3 &a = m.vertices[tr[0]], &b = m.vertices[tr[1]], &c = m.vertices[tr[2]];
        const Vec3 cr = (b - a).cross(c - a);
        t.area[i] = cr.norm() / 2;
        t.normal[i] = cr.norm() > 0 ? Vec3(cr.normalized()) : Vec3::Zero();
        t.centroid[i] = (a + b + c) / 3;
        for (int k = 0; k < 3; ++k) {
            auto it = dir.find({tr[size_t((k + 1) % 3)], tr[size_t(k)]});
            if (it != dir.end()) t.nbr[i][size_t(k)] = it->second.first;
        }
    }
    return t;
}

// ---------------------------------------------------------------- triangle-surface consistency (§4.4.1)

int facetSign(const FitContext& c, const SurfaceSpec& s, int t) {
    return c.topo->normal[size_t(t)].dot(surfaceNormal(s, c.topo->centroid[size_t(t)])) >= 0 ? 1 : -1;
}

bool plausible(const FitContext& c, const SurfaceSpec& s) {
    const double lim = MESHFIT_MAX_RADIUS_REL * std::max(c.topo->diag, 1e-9);
    switch (s.kind) {
        case Kind::Cylinder:
        case Kind::Sphere: return s.radius <= lim;
        case Kind::Torus: return s.radius <= lim && s.minorRadius <= lim;
        case Kind::Cone: return (s.point - c.topo->centroid.front()).norm() <= lim;
        default: return true;
    }
}

bool consistent(const FitContext& c, const SurfaceSpec& s, int sign, int t) {
    const Topo& tp = *c.topo;
    const auto& tr = tp.mesh->triangles[size_t(t)];
    for (uint32_t v : tr)
        if (!(std::abs(signedDistance(s, tp.mesh->vertices[v])) <= c.eps)) return false;
    const Point3& ce = tp.centroid[size_t(t)];
    if (!(std::abs(signedDistance(s, ce)) <= c.sagitta)) return false;
    if (tp.area[size_t(t)] <= 0) return true;  // degenerate triangle: no normal
    return sign * tp.normal[size_t(t)].dot(surfaceNormal(s, ce)) >= c.cosFacet;
}

bool smoothStep(const FitContext& c, const SurfaceSpec& s, int a, int b) {
    if (s.kind == Kind::Plane) return true;
    const Topo& t = *c.topo;
    if (t.area[size_t(a)] <= 0 || t.area[size_t(b)] <= 0) return true;
    return t.normal[size_t(a)].dot(t.normal[size_t(b)]) >= std::cos(MESHFIT_MAX_STEP_ANGLE);
}

bool regionConsistent(const FitContext& c, const SurfaceSpec& s, int sign, const std::vector<int>& tris) {
    const Topo& t = *c.topo;
    std::vector<char> in(t.nbr.size(), 0);
    for (int i : tris) in[size_t(i)] = 1;
    for (int i : tris) {
        if (!consistent(c, s, sign, i)) return false;
        for (int nb : t.nbr[size_t(i)])
            if (nb >= 0 && in[size_t(nb)] && !smoothStep(c, s, i, nb)) return false;
    }
    return true;
}

int simplicity(Kind k) {
    switch (k) {
        case Kind::Plane: return 0;
        case Kind::Cylinder: return 1;
        case Kind::Cone: return 2;
        case Kind::Sphere: return 3;
        default: return 4;
    }
}

double facetDeviation(const FitContext& c, const SurfaceSpec& s, const std::vector<int>& tris) {
    double sum = 0;
    for (int i : tris) sum += std::abs(signedDistance(s, c.topo->centroid[size_t(i)]));
    return tris.empty() ? 0 : sum / double(tris.size());
}

std::vector<int> regionVertices(const Topo& t, const std::vector<int>& tris) {
    std::vector<int> v;
    for (int i : tris)
        for (uint32_t k : t.mesh->triangles[size_t(i)]) v.push_back(int(k));
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

namespace {

// Extrusion strip (§4.4 item 3а): normals of all triangles ⊥ a common direction d, vertices at two levels along d
// (an extrusion wall split by the mesh into edge-to-edge strips). Spheres, tori and cones also pass through such points
// (two equal circles lie on a sphere); only an extrusion surface is plausible: a plane ∥ d, a cylinder with
// axis ∥ d.
bool stripAllows(const FitContext& c, const std::vector<int>& tris, const SurfaceSpec& s) {
    if (tris.size() < 2 || s.kind == Kind::Plane) return true;
    const Topo& t = *c.topo;
    Mat3 m = Mat3::Zero();
    for (int i : tris) m += t.area[size_t(i)] * t.normal[size_t(i)] * t.normal[size_t(i)].transpose();
    Eigen::SelfAdjointEigenSolver<Mat3> es(m);
    const Vec3 d = es.eigenvectors().col(0).normalized();
    for (int i : tris)
        if (t.area[size_t(i)] > 0 && std::abs(t.normal[size_t(i)].dot(d)) > std::sin(MESHFIT_SNAP_ANGLE)) return true;  // not a strip
    std::vector<double> levels;
    for (int v : regionVertices(t, tris)) {
        const double z = t.mesh->vertices[size_t(v)].dot(d);
        bool seen = false;
        for (double l : levels) seen = seen || std::abs(l - z) <= c.eps;
        if (!seen) levels.push_back(z);
        if (levels.size() > 2) return true;  // not a strip
    }
    if (levels.size() != 2) return true;
    return s.kind == Kind::Cylinder && std::abs(s.dir.normalized().dot(d)) >= std::cos(MESHFIT_SNAP_ANGLE);
}

std::vector<Point3> pointsOf(const Topo& t, const std::vector<int>& tris) {
    std::vector<Point3> p;
    for (int v : regionVertices(t, tris)) p.push_back(t.mesh->vertices[size_t(v)]);
    return p;
}

// Points and normals of the region's vertices (vertex normal: area-weighted sum of its region triangles' normals).
void orientedPoints(const Topo& t, const std::vector<int>& tris, std::vector<Point3>* pts, std::vector<Vec3>* nrm) {
    std::map<int, Vec3> acc;
    for (int i : tris)
        for (uint32_t k : t.mesh->triangles[size_t(i)]) {
            auto [it, ins] = acc.emplace(int(k), Vec3::Zero());
            it->second += t.area[size_t(i)] * t.normal[size_t(i)];
        }
    for (const auto& [v, n] : acc) {
        if (n.norm() <= 0) continue;
        pts->push_back(t.mesh->vertices[size_t(v)]);
        nrm->push_back(n.normalized());
    }
}

// Region grown from a seed (§4.4.2).
struct Grown {
    Seg seg;
    double area = 0;
    double dev = 0;  // mean |δ| of triangle centers (§4.4.4)
    int seed = 0;
    bool ok = false;
};

double areaOf(const Topo& t, const std::vector<int>& tris) {
    double a = 0;
    for (int i : tris) a += t.area[size_t(i)];
    return a;
}

// Connected part of the set tris containing start (traversal by adjacency within the set).
std::vector<int> componentOf(const Topo& t, const std::vector<char>& in, int start) {
    std::vector<int> out{start};
    std::vector<char> vis(in.size(), 0);
    vis[size_t(start)] = 1;
    for (size_t q = 0; q < out.size(); ++q)
        for (int nb : t.nbr[size_t(out[q])])
            if (nb >= 0 && in[size_t(nb)] && !vis[size_t(nb)]) {
                vis[size_t(nb)] = 1;
                out.push_back(nb);
            }
    std::sort(out.begin(), out.end());
    return out;
}

// Growing a region from seed over the allowed triangles with surface refinement (§4.4.2).
Grown grow(const FitContext& c, SurfaceSpec s, int seed, const std::vector<char>& allowed) {
    const Topo& t = *c.topo;
    Grown g;
    g.seed = seed;
    const int sign = facetSign(c, s, seed);
    if (!allowed[size_t(seed)] || !plausible(c, s) || !consistent(c, s, sign, seed)) return g;
    std::vector<char> in(t.nbr.size(), 0);
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<int> region;
        std::deque<int> q;
        std::fill(in.begin(), in.end(), 0);
        in[size_t(seed)] = 1;
        q.push_back(seed);
        region.push_back(seed);
        size_t nextRefit = 8;
        while (!q.empty()) {
            const int cur = q.front();
            q.pop_front();
            for (int nb : t.nbr[size_t(cur)]) {
                if (nb < 0 || in[size_t(nb)] || !allowed[size_t(nb)]) continue;
                if (!consistent(c, s, sign, nb) || !smoothStep(c, s, cur, nb)) continue;
                in[size_t(nb)] = 1;
                region.push_back(nb);
                q.push_back(nb);
            }
            if (region.size() >= nextRefit) {
                nextRefit *= 2;
                auto f = refineSurface(s, pointsOf(t, region));
                if (f.ok && f.maxDev <= c.eps) s = f.surface;
            }
        }
        std::sort(region.begin(), region.end());
        auto f = refineSurface(s, pointsOf(t, region));
        const bool changed = f.ok && f.maxDev <= c.eps;
        if (changed) s = f.surface;
        // Filter by the final surface and the seed's connected part.
        std::fill(in.begin(), in.end(), 0);
        for (int i : region)
            if (consistent(c, s, sign, i)) in[size_t(i)] = 1;
        if (!in[size_t(seed)]) return g;
        g.seg.tris = componentOf(t, in, seed);
        g.seg.surface = s;
        g.seg.sign = sign;
        if (!changed || g.seg.tris.size() == region.size()) break;
    }
    if (!stripAllows(c, g.seg.tris, g.seg.surface)) return g;
    g.area = areaOf(t, g.seg.tris);
    g.dev = facetDeviation(c, g.seg.surface, g.seg.tris);
    g.ok = true;
    return g;
}

// Seed neighborhood: BFS over the allowed triangles (§4.4.3).
std::vector<int> neighborhood(const Topo& t, int seed, const std::vector<char>& allowed) {
    std::vector<int> out{seed};
    std::set<int> vis{seed};
    for (size_t q = 0; q < out.size() && out.size() < MESHFIT_NEIGHBORHOOD; ++q)
        for (int nb : t.nbr[size_t(out[q])])
            if (nb >= 0 && allowed[size_t(nb)] && vis.insert(nb).second && out.size() < MESHFIT_NEIGHBORHOOD) out.push_back(nb);
    return out;
}

// Curved surface candidates from pairs and triples of neighborhood triangles (§4.4.3).
std::vector<SurfaceSpec> curvedCandidates(const FitContext& c, const std::vector<int>& nb) {
    const Topo& t = *c.topo;
    const Mesh& m = *t.mesh;
    std::vector<SurfaceSpec> out;
    const size_t K = std::min(nb.size(), MESHFIT_RANSAC_PAIRS);
    auto verts = [&](std::initializer_list<int> ts) {
        std::vector<Point3> p;
        for (int i : ts)
            for (uint32_t v : m.triangles[size_t(i)]) {
                const Point3& q = m.vertices[v];
                if (std::none_of(p.begin(), p.end(), [&](const Point3& x) { return (x - q).norm() < 1e-12; })) p.push_back(q);
            }
        return p;
    };
    for (size_t i = 0; i < K; ++i)
        for (size_t j = i + 1; j < nb.size() && j < 2 * K; ++j) {
            const int a = nb[i], b = nb[j];
            const Vec3 na = t.normal[size_t(a)], nbv = t.normal[size_t(b)];
            const Vec3 ax = na.cross(nbv);
            const auto pts = verts({a, b});
            if (ax.norm() > 0.017) {  // ≥ 1°: the cylinder axis is ⊥ both normals
                if (auto f = initialSurface(Kind::Cylinder, pts, {na, nbv})) out.push_back(*f);
            }
            if (pts.size() >= 4) {
                if (auto f = initialSurface(Kind::Sphere, pts, {}); f && f->radius < 10 * t.diag) out.push_back(*f);
            }
            for (size_t k = j + 1; k < nb.size() && k < 2 * K && i < MESHFIT_RANSAC_CONE_FIRST; ++k) {
                const int d = nb[k];
                std::vector<Point3> cp{t.centroid[size_t(a)], t.centroid[size_t(b)], t.centroid[size_t(d)]};
                std::vector<Vec3> cn{na, nbv, t.normal[size_t(d)]};
                auto f = initialSurface(Kind::Cone, cp, cn);
                if (!f) continue;
                // Angle from the vertices of the three triangles (tangent planes give apex and axis exactly, but the angle
                // is offset by the strip width).
                double sum = 0;
                int cnt = 0;
                for (const auto& q : verts({a, b, d})) {
                    const Vec3 w = q - f->point;
                    const double tt = w.dot(f->dir);
                    if (tt <= 0) continue;
                    sum += std::atan2((w - tt * f->dir).norm(), tt);
                    ++cnt;
                }
                if (cnt) f->halfAngle = sum / cnt;
                if (f->halfAngle > 0.02 && f->halfAngle < 1.55) out.push_back(*f);
            }
        }
    return out;
}

// Best seed hypotheses: the triangle's plane and curved surfaces (§4.4.3).
std::vector<Grown> hypotheses(const FitContext& c, int seed, const std::vector<char>& allowed) {
    const Topo& t = *c.topo;
    std::vector<Grown> out;
    if (t.area[size_t(seed)] <= 0) return out;
    SurfaceSpec pl;
    pl.kind = Kind::Plane;
    pl.point = t.centroid[size_t(seed)];
    pl.dir = t.normal[size_t(seed)];
    if (auto g = grow(c, pl, seed, allowed); g.ok) out.push_back(std::move(g));
    const auto nb = neighborhood(t, seed, allowed);
    if (nb.size() < 2) return out;
    // Best candidate of each kind: by the area of consistent triangles in the neighborhood containing the seed.
    std::map<int, std::pair<double, SurfaceSpec>> best;
    for (const auto& s : curvedCandidates(c, nb)) {
        const int sign = facetSign(c, s, seed);
        if (!consistent(c, s, sign, seed)) continue;
        double a = 0;
        for (int i : nb)
            if (consistent(c, s, sign, i)) a += t.area[size_t(i)];
        auto& b = best[int(s.kind)];
        if (a > b.first) b = {a, s};
    }
    // Torus (FX10-STL-001): no pair or triple of triangles defines it, so it is fitted to the centers and normals of
    // the leading neighborhood triangles (§4.4 item 2) — fillets of revolved edges, grooves, tooth flanks.
    {
        std::vector<Point3> pts;
        std::vector<Vec3> nrm;
        for (size_t i = 0; i < nb.size() && i < 2 * MESHFIT_RANSAC_PAIRS; ++i)
            if (t.area[size_t(nb[i])] > 0) {
                pts.push_back(t.centroid[size_t(nb[i])]);
                nrm.push_back(t.normal[size_t(nb[i])]);
            }
        if (pts.size() >= size_t(parameterCount(Kind::Torus) + MESHFIT_MIN_EXTRA_VERTICES)) {
            // Triangle centers lie inside the surface by the chord sagitta: the fit is refined on their vertices.
            std::vector<int> lead(nb.begin(), nb.begin() + std::ptrdiff_t(std::min(nb.size(), 2 * MESHFIT_RANSAC_PAIRS)));
            auto f = fitSurface(Kind::Torus, pts, nrm);
            if (f.ok) f = refineSurface(f.surface, pointsOf(t, lead));
            if (f.ok && plausible(c, f.surface)) {
                const int sign = facetSign(c, f.surface, seed);
                if (consistent(c, f.surface, sign, seed)) {
                    double a = 0;
                    for (int i : nb)
                        if (consistent(c, f.surface, sign, i)) a += t.area[size_t(i)];
                    auto& b = best[int(Kind::Torus)];
                    if (a > b.first) b = {a, f.surface};
                }
            }
        }
    }
    for (const auto& [k, v] : best) {
        if (auto g = grow(c, v.second, seed, allowed); g.ok && g.seg.tris.size() >= 2) out.push_back(std::move(g));
    }
    return out;
}

// Simplest kind consistent with all triangles of the set (§4.4.4).
std::optional<Seg> fitAll(const FitContext& c, const std::vector<int>& tris, int maxRank = 4) {
    const Topo& t = *c.topo;
    std::vector<Point3> pts;
    std::vector<Vec3> nrm;
    orientedPoints(t, tris, &pts, &nrm);
    // Among consistent kinds, the one with the smallest mean deviation of triangle centers (mesh triangles are chords
    // of their surface); equal within ε: fewer parameters wins.
    std::optional<Seg> best;
    double bestDev = 0;
    for (Kind k : {Kind::Plane, Kind::Cylinder, Kind::Cone, Kind::Sphere, Kind::Torus}) {
        if (simplicity(k) > maxRank) break;
        auto f = fitSurface(k, pts, nrm);
        if (!f.ok || f.maxDev > c.eps || !plausible(c, f.surface) || !stripAllows(c, tris, f.surface)) continue;
        const int sign = facetSign(c, f.surface, tris.front());
        if (!regionConsistent(c, f.surface, sign, tris)) continue;
        const double dev = facetDeviation(c, f.surface, tris);
        if (best && dev >= bestDev - c.eps) continue;
        best = Seg{f.surface, tris, sign};
        bestDev = dev;
    }
    return best;
}

}  // namespace

Result<std::vector<Seg>> segment(const FitContext& c, const Cancel* cancel) {
    const Topo& t = *c.topo;
    const int n = int(t.nbr.size());
    std::vector<int> order = std::vector<int>(size_t(n));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return t.area[size_t(a)] > t.area[size_t(b)]; });
    const std::vector<char> all(size_t(n), 1);
    // 1. Hypotheses from seeds; triangles covered by a grown region do not become seeds.
    std::vector<Grown> cands;
    std::vector<char> covered(size_t(n), 0);
    size_t seeds = 0;
    for (int s : order) {
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        if (covered[size_t(s)]) continue;
        covered[size_t(s)] = 1;
        // Seed count limit (§4.4 item 1): a free-form mesh yields a seed for almost every triangle; the rest are left
        // to the whole-part fitting and to mesh faces (item 8).
        if (++seeds > MESHFIT_MAX_SEEDS) break;
        for (auto& g : hypotheses(c, s, all)) {
            for (int i : g.seg.tris) covered[size_t(i)] = 1;
            cands.push_back(std::move(g));
        }
    }
    // 2. Selection by decreasing triangle count, then area (on a tie: fewer parameters, earlier seed):
    // a curved surface of many narrow strips beats a large planar face with a couple of strips of an adjacent fillet.
    std::stable_sort(cands.begin(), cands.end(), [&](const Grown& a, const Grown& b) {
        if (a.seg.tris.size() != b.seg.tris.size()) return a.seg.tris.size() > b.seg.tris.size();
        if (std::abs(a.area - b.area) > 1e-9 * std::max(a.area, b.area)) return a.area > b.area;
        if (std::abs(a.dev - b.dev) > c.eps) return a.dev < b.dev;
        return simplicity(a.seg.surface.kind) < simplicity(b.seg.surface.kind);
    });
    std::vector<int> regionOf(size_t(n), -1);
    std::vector<Seg> regions;
    std::vector<char> free(size_t(n), 1);
    for (auto& g : cands) {
        std::vector<int> rest;
        for (int i : g.seg.tris)
            if (regionOf[size_t(i)] < 0) rest.push_back(i);
        if (rest.empty()) continue;
        Seg s = g.seg;
        if (rest.size() < g.seg.tris.size()) {
            // Intersection with accepted ones: regrown from the remaining seed over free triangles.
            int seed = rest.front();
            double best = -1;
            for (int i : rest)
                if (t.area[size_t(i)] > best) best = t.area[size_t(i)], seed = i;
            auto g2 = grow(c, g.seg.surface, seed, free);
            if (!g2.ok || g2.seg.tris.size() < 2) continue;
            s = g2.seg;
        }
        const int id = int(regions.size());
        for (int i : s.tris) regionOf[size_t(i)] = id, free[size_t(i)] = 0;
        regions.push_back(std::move(s));
    }
    // 3. Unlabeled triangles: connected parts, each fitted as a whole (§4.4.4).
    {
        std::vector<char> un(size_t(n), 0), seen(size_t(n), 0);
        for (int i = 0; i < n; ++i) un[size_t(i)] = regionOf[size_t(i)] < 0;
        for (int s = 0; s < n; ++s) {
            if (!un[size_t(s)] || seen[size_t(s)]) continue;
            if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
            const auto comp = componentOf(t, un, s);
            for (int i : comp) seen[size_t(i)] = 1;
            if (auto seg = fitAll(c, comp)) {
                const int id = int(regions.size());
                for (int i : seg->tris) regionOf[size_t(i)] = id;
                regions.push_back(std::move(*seg));
            }
        }
    }
    auto rebuild = [&]() {
        for (auto& r : regions) r.tris.clear();
        for (int i = 0; i < n; ++i)
            if (regionOf[size_t(i)] >= 0) regions[size_t(regionOf[size_t(i)])].tris.push_back(i);
    };
    // 4. Attaching the remaining triangles and small regions to an adjacent region whose surface fits them.
    auto mergePass = [&]() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < n; ++i) {
                if (regionOf[size_t(i)] >= 0) continue;
                for (int nb : t.nbr[size_t(i)]) {
                    if (nb < 0 || regionOf[size_t(nb)] < 0) continue;
                    const Seg& r = regions[size_t(regionOf[size_t(nb)])];
                    if (consistent(c, r.surface, r.sign, i)) {
                        regionOf[size_t(i)] = regionOf[size_t(nb)];
                        changed = true;
                        break;
                    }
                }
            }
            rebuild();
            // The region is fully consistent with an adjacent one (of no smaller area): merge.
            for (size_t a = 0; a < regions.size(); ++a) {
                if (regions[a].tris.empty()) continue;
                std::set<int> nbs;
                for (int i : regions[a].tris)
                    for (int nb : t.nbr[size_t(i)])
                        if (nb >= 0 && regionOf[size_t(nb)] >= 0 && regionOf[size_t(nb)] != int(a)) nbs.insert(regionOf[size_t(nb)]);
                const double areaA = areaOf(t, regions[a].tris);
                for (int b : nbs) {
                    Seg& rb = regions[size_t(b)];
                    if (rb.tris.empty() || areaOf(t, rb.tris) < areaA) continue;
                    std::vector<int> uni = regions[a].tris;
                    uni.insert(uni.end(), rb.tris.begin(), rb.tris.end());
                    if (regionConsistent(c, rb.surface, rb.sign, uni)) {
                        for (int i : regions[a].tris) regionOf[size_t(i)] = b;
                        rb.tris.insert(rb.tris.end(), regions[a].tris.begin(), regions[a].tris.end());
                        std::sort(rb.tris.begin(), rb.tris.end());
                        regions[a].tris.clear();
                        changed = true;
                        break;
                    }
                }
            }
            rebuild();
        }
    };
    mergePass();
    // 5. Strips of planar faces (no interior vertices) smoothly adjacent to each other form a curved surface (§4.4.5).
    {
        std::vector<int> stripOf(regions.size(), 0);
        for (size_t r = 0; r < regions.size(); ++r) {
            if (regions[r].tris.empty() || regions[r].surface.kind != Kind::Plane) continue;
            bool interior = false;
            std::map<int, int> cnt;  // vertex → number of region boundary edges
            for (int i : regions[r].tris)
                for (int k = 0; k < 3; ++k) {
                    const int nb = t.nbr[size_t(i)][size_t(k)];
                    if (nb >= 0 && regionOf[size_t(nb)] == int(r)) continue;
                    cnt[int(t.mesh->triangles[size_t(i)][size_t(k)])]++;
                }
            for (int v : regionVertices(t, regions[r].tris)) interior = interior || !cnt.count(v);
            stripOf[r] = interior ? 0 : 1;
        }
        std::vector<char> done(regions.size(), 0);
        for (size_t r = 0; r < regions.size(); ++r) {
            if (!stripOf[r] || done[r]) continue;
            std::vector<int> cluster{int(r)};
            done[r] = 1;
            for (size_t q = 0; q < cluster.size(); ++q)
                for (int i : regions[size_t(cluster[q])].tris)
                    for (int nb : t.nbr[size_t(i)]) {
                        if (nb < 0) continue;
                        const int rb = regionOf[size_t(nb)];
                        if (rb < 0 || done[size_t(rb)] || !stripOf[size_t(rb)]) continue;
                        if (t.normal[size_t(i)].dot(t.normal[size_t(nb)]) < c.cosFacet) continue;
                        done[size_t(rb)] = 1;
                        cluster.push_back(rb);
                    }
            if (cluster.size() < 2) continue;
            std::vector<int> tris;
            for (int rr : cluster) tris.insert(tris.end(), regions[size_t(rr)].tris.begin(), regions[size_t(rr)].tris.end());
            std::sort(tris.begin(), tris.end());
            if (auto seg = fitAll(c, tris); seg && seg->surface.kind != Kind::Plane) {
                for (int rr : cluster) regions[size_t(rr)].tris.clear();
                const int id = int(regions.size());
                for (int i : seg->tris) regionOf[size_t(i)] = id;
                regions.push_back(std::move(*seg));
                stripOf.push_back(0);
                done.push_back(1);
            }
        }
    }
    // 5б. Adjacent curved regions whose union is consistent with one surface become one region (§4.4 item 7а):
    // cone or torus belts between adjacent mesh circles are each consistent with a sphere too.
    // A belt is a region without interior vertices (all vertices on its boundary): only belts are ambiguous.
    auto bandLike = [&](const std::vector<int>& tris) {
        std::set<int> bnd;
        std::vector<char> in(size_t(n), 0);
        for (int i : tris) in[size_t(i)] = 1;
        for (int i : tris)
            for (int k = 0; k < 3; ++k) {
                const int nb = t.nbr[size_t(i)][size_t(k)];
                if (nb >= 0 && in[size_t(nb)]) continue;
                bnd.insert(int(t.mesh->triangles[size_t(i)][size_t(k)]));
                bnd.insert(int(t.mesh->triangles[size_t(i)][size_t((k + 1) % 3)]));
            }
        for (int v : regionVertices(t, tris))
            if (!bnd.count(v)) return false;
        return true;
    };
    size_t tries = 0;
    for (bool again = true; again && tries < MESHFIT_MAX_UNION_TRIES;) {
        again = false;
        for (size_t a = 0; a < regions.size() && tries < MESHFIT_MAX_UNION_TRIES; ++a) {
            if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
            for (bool grew = true; grew && tries < MESHFIT_MAX_UNION_TRIES;) {
                grew = false;
                if (regions[a].tris.empty() || regions[a].surface.kind == Kind::Plane || !bandLike(regions[a].tris)) break;
                std::set<int> nbs;
                for (int i : regions[a].tris)
                    for (int nb : t.nbr[size_t(i)])
                        if (nb >= 0 && regionOf[size_t(nb)] >= 0 && regionOf[size_t(nb)] != int(a) &&
                            t.normal[size_t(i)].dot(t.normal[size_t(nb)]) >= std::cos(MESHFIT_MAX_STEP_ANGLE))
                            nbs.insert(regionOf[size_t(nb)]);
                for (int b : nbs) {
                    if (regions[size_t(b)].surface.kind == Kind::Plane || !bandLike(regions[size_t(b)].tris)) continue;
                    if (++tries > MESHFIT_MAX_UNION_TRIES) break;
                    std::vector<int> uni = regions[a].tris;
                    uni.insert(uni.end(), regions[size_t(b)].tris.begin(), regions[size_t(b)].tris.end());
                    std::sort(uni.begin(), uni.end());
                    if (auto seg = fitAll(c, uni)) {
                        regions[size_t(b)].tris.clear();
                        regions[a] = std::move(*seg);
                        for (int i : regions[a].tris) regionOf[size_t(i)] = int(a);
                        grew = again = true;
                        break;
                    }
                }
            }
        }
    }
    // 5в (§4.4 item 7б). Curved regions: triangles consistent with the surface of an adjacent region with more triangles
    // move to it (a planar slot wall with a strip of the tangent end is taken for a cylinder
    // of huge radius); the remainder is refitted.
    for (size_t a = 0; a < regions.size(); ++a) {
        Seg& r = regions[a];  // before the push_back below
        if (r.tris.empty() || r.surface.kind == Kind::Plane) continue;
        std::vector<int> keep;
        bool moved = false;
        for (int i : r.tris) {
            int to = -1;
            for (int nb : t.nbr[size_t(i)]) {
                const int q = nb >= 0 ? regionOf[size_t(nb)] : -1;
                if (q < 0 || q == int(a) || regions[size_t(q)].tris.size() <= r.tris.size()) continue;
                if (consistent(c, regions[size_t(q)].surface, regions[size_t(q)].sign, i)) {
                    to = q;
                    break;
                }
            }
            if (to < 0) {
                keep.push_back(i);
                continue;
            }
            regionOf[size_t(i)] = to;
            regions[size_t(to)].tris.push_back(i);
            std::sort(regions[size_t(to)].tris.begin(), regions[size_t(to)].tris.end());
            moved = true;
        }
        if (!moved) continue;
        if (keep.empty()) {
            r.tris.clear();
            continue;
        }
        // The remainder in connected parts; each is fitted as a whole, otherwise keeps the previous surface.
        std::vector<char> in(size_t(n), 0);
        for (int i : keep) in[size_t(i)] = 1;
        std::vector<Seg> parts;
        const SurfaceSpec old = r.surface;
        const int oldSign = r.sign;
        for (int i : keep) {
            if (!in[size_t(i)]) continue;
            auto comp = componentOf(t, in, i);
            for (int j : comp) in[size_t(j)] = 0;
            if (auto f = fitAll(c, comp)) parts.push_back(std::move(*f));
            else parts.push_back(Seg{old, comp, oldSign});
        }
        for (size_t pi = 0; pi < parts.size(); ++pi) {
            const int id = pi == 0 ? int(a) : int(regions.size());
            for (int j : parts[pi].tris) regionOf[size_t(j)] = id;
            if (pi == 0) regions[a] = std::move(parts[pi]);
            else regions.push_back(std::move(parts[pi]));
        }
    }
    // 5а (§4.4 item 7в). Simplest kind: a region consistent with a simpler surface gets it;
    // then attachment and merging again (planar regions of one plane are merged).
    for (auto& r : regions) {
        if (r.tris.empty() || simplicity(r.surface.kind) == 0) continue;
        if (auto simpler = fitAll(c, r.tris, simplicity(r.surface.kind) - 1);
            simpler && facetDeviation(c, simpler->surface, r.tris) <= facetDeviation(c, r.surface, r.tris) + c.eps)
            r = std::move(*simpler);
    }
    mergePass();
    // 5г-1 (§4.4 item 7г, S4-MESH-002). A curved region with too few vertices for its kind is not evidence of the
    // surface (a torus through 6 vertices fits any patch of a free-form surface): its triangles become unlabeled, as do
    // single-triangle planar regions adjacent to unlabeled ones; then attachment again.
    {
        bool any = false;
        for (auto& r : regions) {
            if (r.tris.empty() || r.surface.kind == Kind::Plane) continue;
            if (int(regionVertices(t, r.tris).size()) >= parameterCount(r.surface.kind) + MESHFIT_MIN_EXTRA_VERTICES) continue;
            for (int i : r.tris) regionOf[size_t(i)] = -1;
            r.tris.clear();
            any = true;
        }
        for (bool grew = any; grew;) {
            grew = false;
            for (auto& r : regions) {
                if (r.tris.size() != 1 || r.surface.kind != Kind::Plane) continue;
                const int i = r.tris.front();
                bool touches = false;
                for (int nb : t.nbr[size_t(i)]) touches = touches || (nb >= 0 && regionOf[size_t(nb)] < 0);
                if (!touches) continue;
                regionOf[size_t(i)] = -1;
                r.tris.clear();
                grew = true;
            }
        }
        if (any) mergePass();
        // 5г-2 (§4.4 item 7д, FX10-STL-001). Planes among the unlabeled triangles: a curved region dropped above may have
        // taken the strips of several planar faces at a small angle (three parallel edges always lie on a cylinder),
        // and those faces lost to it in the selection. A planar region of at least 2 triangles grown over unlabeled
        // ones (largest seed first) is a face again; then attachment again.
        if (any) {
            std::vector<int> order;
            for (int i = 0; i < n; ++i)
                if (regionOf[size_t(i)] < 0 && t.normal[size_t(i)].squaredNorm() > 0) order.push_back(i);
            std::sort(order.begin(), order.end(), [&](int a, int b) { return t.area[size_t(a)] != t.area[size_t(b)] ? t.area[size_t(a)] > t.area[size_t(b)] : a < b; });
            bool grown = false;
            for (int seed : order) {
                if (regionOf[size_t(seed)] >= 0) continue;
                SurfaceSpec pl;
                pl.kind = Kind::Plane;
                pl.point = t.centroid[size_t(seed)];
                pl.dir = t.normal[size_t(seed)];
                std::vector<int> tris{seed};
                std::set<int> in{seed};
                for (size_t q = 0; q < tris.size(); ++q)
                    for (int nb : t.nbr[size_t(tris[q])])
                        if (nb >= 0 && regionOf[size_t(nb)] < 0 && !in.count(nb) && consistent(c, pl, 1, nb)) {
                            in.insert(nb);
                            tris.push_back(nb);
                        }
                if (tris.size() < 2) continue;
                std::sort(tris.begin(), tris.end());
                Seg s;
                s.surface = pl;
                s.tris = tris;
                s.sign = 1;
                const int id = int(regions.size());
                for (int i : tris) regionOf[size_t(i)] = id;
                regions.push_back(std::move(s));
                grown = true;
            }
            if (grown) mergePass();
        }
    }
    // 5г-3 (§4.4 item 7е, FX10-STL-001). Known surfaces over unlabeled triangles: faces of patterns and of revolved
    // grooves lie on one surface (sprocket tooth flanks — one torus), and once it is recognized on one face it is the
    // hypothesis for the others, which no seed may have proposed (seeds propose no tori). Credible curved regions (enough
    // vertices, item 7г) by decreasing area; unlabeled seeds by decreasing area; a region of at least 2 triangles grown
    // over unlabeled ones consistent with the surface (by smooth steps) is a new region on that surface.
    {
        std::vector<int> cand;
        for (size_t r = 0; r < regions.size(); ++r)
            if (!regions[r].tris.empty() && regions[r].surface.kind != Kind::Plane &&
                int(regionVertices(t, regions[r].tris).size()) >= parameterCount(regions[r].surface.kind) + MESHFIT_MIN_EXTRA_VERTICES)
                cand.push_back(int(r));
        std::vector<double> candArea(regions.size(), 0.0);
        for (int r : cand) candArea[size_t(r)] = areaOf(t, regions[size_t(r)].tris);
        std::stable_sort(cand.begin(), cand.end(), [&](int a, int b) { return candArea[size_t(a)] > candArea[size_t(b)]; });
        std::vector<int> order;
        for (int i = 0; i < n; ++i)
            if (regionOf[size_t(i)] < 0 && t.normal[size_t(i)].squaredNorm() > 0) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return t.area[size_t(a)] != t.area[size_t(b)] ? t.area[size_t(a)] > t.area[size_t(b)] : a < b; });
        bool grown = false;
        for (int seed : order) {
            if (regionOf[size_t(seed)] >= 0) continue;
            for (int r : cand) {
                const SurfaceSpec s = regions[size_t(r)].surface;
                const int sign = facetSign(c, s, seed);
                if (!consistent(c, s, sign, seed)) continue;
                std::vector<int> tris{seed};
                std::set<int> in{seed};
                for (size_t q = 0; q < tris.size(); ++q)
                    for (int nb : t.nbr[size_t(tris[q])])
                        if (nb >= 0 && regionOf[size_t(nb)] < 0 && !in.count(nb) && consistent(c, s, sign, nb) && smoothStep(c, s, tris[q], nb)) {
                            in.insert(nb);
                            tris.push_back(nb);
                        }
                if (tris.size() < 2) continue;
                std::sort(tris.begin(), tris.end());
                Seg g;
                g.surface = s;
                g.tris = tris;
                g.sign = sign;
                const int id = int(regions.size());
                for (int i : tris) regionOf[size_t(i)] = id;
                regions.push_back(std::move(g));
                grown = true;
                break;
            }
        }
        if (grown) mergePass();
    }
    // 5г-4 (§4.4 item 7ж, FX10-STL-001). Slivers: a nearly degenerate triangle along a curved line of the surface (along
    // the tube section of a small fillet torus) has its vertices on the surface, but its normal is meaningless — its plane
    // turns about the long edge — and the facet angle test rejects it. An unlabeled triangle whose height is at most
    // MESHFIT_SLIVER_FACTOR × the largest deviation of an edge midpoint from the surface of an edge-adjacent region, with
    // all vertices within ε and the center within the sagitta tolerance, joins that region (neighbors in edge order),
    // until nothing changes.
    {
        const Mesh& m = *t.mesh;
        auto sliverOn = [&](const SurfaceSpec& S, int i) {
            const auto& tr = m.triangles[size_t(i)];
            double longest = 0, mid = 0;
            for (int k = 0; k < 3; ++k) {
                const Point3 &a = m.vertices[tr[size_t(k)]], &b = m.vertices[tr[size_t((k + 1) % 3)]];
                if (!(std::abs(signedDistance(S, a)) <= c.eps)) return false;
                longest = std::max(longest, (b - a).norm());
                mid = std::max(mid, std::abs(signedDistance(S, Point3((a + b) / 2))));
            }
            if (!(std::abs(signedDistance(S, t.centroid[size_t(i)])) <= c.sagitta) || longest <= 0) return false;
            if (2 * t.area[size_t(i)] / longest <= MESHFIT_SLIVER_FACTOR * mid) return true;
            // A chord triangle spanning a curved patch: the facet normal may differ from the normal at the center by the
            // angle the patch turns (the largest angle between the surface normals at its vertices).
            if (t.area[size_t(i)] <= 0) return true;
            double spread = 0;
            for (int a = 0; a < 3; ++a)
                for (int b = a + 1; b < 3; ++b)
                    spread = std::max(spread, std::acos(std::clamp(surfaceNormal(S, m.vertices[tr[size_t(a)]]).dot(surfaceNormal(S, m.vertices[tr[size_t(b)]])), -1.0, 1.0)));
            const double ang = std::acos(std::clamp(std::abs(t.normal[size_t(i)].dot(surfaceNormal(S, t.centroid[size_t(i)]))), 0.0, 1.0));
            return spread <= MESHFIT_MAX_STEP_ANGLE && ang <= MESHFIT_MAX_FACET_ANGLE + spread;
        };
        // Triangle i agrees with the region surface by the usual rule or as a sliver / chord of a curved patch.
        auto agrees = [&](const Seg& r, int i) { return consistent(c, r.surface, r.sign, i) || sliverOn(r.surface, i); };
        bool any = false;
        for (bool changed = true; changed;) {
            changed = false;
            for (int i = 0; i < n; ++i) {
                if (regionOf[size_t(i)] >= 0) continue;
                for (int nb : t.nbr[size_t(i)]) {
                    if (nb < 0 || regionOf[size_t(nb)] < 0) continue;
                    const Seg& r = regions[size_t(regionOf[size_t(nb)])];
                    if (r.mesh || !sliverOn(r.surface, i)) continue;
                    regionOf[size_t(i)] = regionOf[size_t(nb)];
                    changed = any = true;
                    break;
                }
            }
            if (changed) rebuild();
            // A small curved region inside a face (a few slivers fitted by a cone) whose triangles all agree with an
            // edge-adjacent region of a larger area merges into it; planar regions stay (a cap without interior vertices
            // has all its vertices on the circle of the adjacent cylinder).
            for (size_t a = 0; a < regions.size(); ++a) {
                if (regions[a].tris.empty() || regions[a].mesh || regions[a].surface.kind == Kind::Plane) continue;
                std::set<int> nbs;
                for (int i : regions[a].tris)
                    for (int nb : t.nbr[size_t(i)])
                        if (nb >= 0 && regionOf[size_t(nb)] >= 0 && regionOf[size_t(nb)] != int(a)) nbs.insert(regionOf[size_t(nb)]);
                const double areaA = areaOf(t, regions[a].tris);
                for (int b : nbs) {
                    Seg& rb = regions[size_t(b)];
                    if (rb.tris.empty() || rb.mesh || areaOf(t, rb.tris) <= areaA) continue;
                    if (!std::all_of(regions[a].tris.begin(), regions[a].tris.end(), [&](int i) { return agrees(rb, i); })) continue;
                    for (int i : regions[a].tris) regionOf[size_t(i)] = b;
                    rebuild();
                    changed = any = true;
                    break;
                }
            }
        }
        if (any) {
            rebuild();
            mergePass();
        }
    }
    // 5г (§4.4 item 8, S4-MESH-002). Unlabeled triangles (free-form parts: loft, spline contours) — mesh faces, each on
    // the plane of its triangle; there must be recognized surfaces and not too many mesh faces.
    const auto unlabeled = size_t(std::count(regionOf.begin(), regionOf.end(), -1));
    if (unlabeled > 0) {
        const bool recognized = std::any_of(regions.begin(), regions.end(), [](const Seg& r) { return !r.tris.empty(); });
        bool degenerate = false;  // a triangle without a normal has no plane
        for (int i = 0; i < n; ++i) degenerate = degenerate || (regionOf[size_t(i)] < 0 && t.normal[size_t(i)].squaredNorm() == 0);
        if (!recognized || degenerate || unlabeled > MESHFIT_MAX_MESH_FACES)
            return Error("MESHFIT_UNRECOGNIZED").with("triangles", double(unlabeled));
        for (int i = 0; i < n; ++i) {
            if (regionOf[size_t(i)] >= 0) continue;
            Seg s;
            s.surface.kind = Kind::Plane;
            s.surface.point = t.centroid[size_t(i)];
            s.surface.dir = t.normal[size_t(i)];
            s.tris = {i};
            s.mesh = true;
            regionOf[size_t(i)] = int(regions.size());
            regions.push_back(std::move(s));
        }
    }
    // 6. Final refinement of each region (a mesh face is exact on its plane); empty ones are removed.
    std::vector<Seg> out;
    for (auto& r : regions) {
        if (r.tris.empty()) continue;
        std::sort(r.tris.begin(), r.tris.end());
        if (r.mesh) {
            out.push_back(std::move(r));
            continue;
        }
        auto f = refineSurface(r.surface, pointsOf(t, r.tris));
        if (f.ok && f.maxDev <= c.eps) r.surface = f.surface;
        r.surface = canonical(r.surface);
        r.sign = facetSign(c, r.surface, r.tris.front());
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const Seg& a, const Seg& b) { return a.tris.front() < b.tris.front(); });
    return out;
}

}  // namespace construe::meshfit
