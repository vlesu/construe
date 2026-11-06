// Canonical topology and geometric keys (spec/kernel-interface.md §4.3–§4.4).
#include "kernel/kernel.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>

namespace construe::kernel {

CanonicalTopology canonicalTopology(const Kernel& k, const Shape& s) {
    CanonicalTopology ct;
    const Counts raw = k.counts(s);
    std::vector<bool> seam(raw.edges, false), essential(raw.vertices, false);
    for (int e = 0; e < raw.edges; ++e) {
        const auto [f1, f2] = k.edgeFaces(s, e);
        if (f1 == f2) {
            seam[e] = true;
            ct.seams.push_back(e);
        }
    }
    for (int v = 0; v < raw.vertices; ++v)
        if (k.vertexFaces(s, v).size() >= 3) {
            essential[v] = true;
            ct.essentialVertices.push_back(v);
        }
    // Union-find over non-seam edges through non-principal vertices with the same pair of faces.
    std::vector<int> parent(raw.edges);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    for (int v = 0; v < raw.vertices; ++v) {
        if (essential[v]) continue;
        std::vector<int> es;
        for (int e : k.vertexEdges(s, v))
            if (!seam[e]) es.push_back(e);
        for (size_t i = 0; i + 1 < es.size(); ++i)
            for (size_t j = i + 1; j < es.size(); ++j)
                if (k.edgeFaces(s, es[i]) == k.edgeFaces(s, es[j])) {
                    const int a = find(es[i]), b = find(es[j]);
                    if (a != b) parent[std::max(a, b)] = std::min(a, b);
                }
    }
    std::map<int, LogicalEdge> groups;  // root (smallest EdgeIdx) → chain
    for (int e = 0; e < raw.edges; ++e) {
        if (seam[e]) continue;
        LogicalEdge& le = groups[find(e)];
        le.edges.push_back(e);
        le.faces = k.edgeFaces(s, e);
    }
    for (auto& [root, le] : groups) {
        std::sort(le.edges.begin(), le.edges.end());
        // Ends: principal vertices that occur once in the chain.
        std::map<int, int> count;
        for (int e : le.edges) {
            const auto [v1, v2] = k.edgeVertices(s, e);
            ++count[v1];
            ++count[v2];
        }
        for (const auto& [v, c] : count)
            if (essential[v] && c == 1) le.ends.push_back(v);
        if (le.ends.size() != 2) le.ends.clear();
        ct.logicalEdges.push_back(le);
    }
    std::sort(ct.logicalEdges.begin(), ct.logicalEdges.end(),
              [](const LogicalEdge& a, const LogicalEdge& b) { return a.edges.front() < b.edges.front(); });
    ct.counts = {raw.solids, raw.faces, static_cast<int>(ct.logicalEdges.size()),
                 static_cast<int>(ct.essentialVertices.size())};
    return ct;
}

Point3 logicalEdgeKey(const Kernel& k, const Shape& s, const LogicalEdge& e) {
    Point3 sum = Point3::Zero();
    double len = 0;
    for (int x : e.edges) {
        const double l = k.edgeLength(s, x);
        sum += l * k.edgeCentroid(s, x);
        len += l;
    }
    return len > 0 ? Point3(sum / len) : sum;
}

Vec3 seamDirection(const Vec3& d) {
    const Vec3 n = d.normalized();
    Vec3 a = Vec3::UnitX();
    if (std::abs(n.x()) > std::abs(n.y()) && std::abs(n.x()) > std::abs(n.z())) a = Vec3::UnitY();
    else if (std::abs(n.y()) >= std::abs(n.z()) && std::abs(n.y()) >= std::abs(n.x())) a = Vec3::UnitZ();
    return (a - a.dot(n) * n).normalized();
}

}  // namespace construe::kernel
