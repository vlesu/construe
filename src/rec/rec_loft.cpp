// Loft added onto the body (spec/reconstruction.md §4.9; S4-REC-022): a connected set of free-form side faces between
// a planar top cap and the planar outline on the body is removed in one operation and becomes a loft of two sections.
#include "rec/rec_internal.h"

#include <algorithm>
#include <functional>
#include <numeric>

namespace construe::rec::detail {

namespace {
using SK = kernel::SurfaceInfo::Kind;
bool near(double a, double b) { return std::abs(a - b) <= REC_LIN_TOL * 10; }
}  // namespace

std::optional<LoftRemoval> removeLoft(const kernel::Kernel& k, const kernel::Shape& body, std::vector<Note>& notes) {
    Topo t(k, body);
    const double v0 = k.massProperties(body).volume;
    // Side faces: free-form surfaces (extrusions and revolutions never produce them).
    std::vector<bool> side(size_t(t.faces()), false);
    for (int f = 0; f < t.faces(); ++f) side[size_t(f)] = t.surf[size_t(f)].kind == SK::Other && t.surf[size_t(f)].otherKind != "revolution";
    std::vector<int> parent(size_t(t.faces()));
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) { return parent[size_t(x)] == x ? x : parent[size_t(x)] = find(parent[size_t(x)]); };
    for (const auto& L : t.ct.logicalEdges) {
        const auto [a, b] = L.faces;
        if (a != b && side[size_t(a)] && side[size_t(b)]) parent[size_t(find(a))] = find(b);
    }
    std::map<int, std::vector<int>> comps;
    for (int f = 0; f < t.faces(); ++f)
        if (side[size_t(f)]) comps[find(f)].push_back(f);
    for (const auto& [root, faces] : comps) {
        const std::set<int> in(faces.begin(), faces.end());
        // The top cap: a plane adjacent to the side faces, all of whose neighbours are side faces of the component.
        std::vector<int> caps;
        std::set<int> seen;
        for (int f : faces)
            for (int le : t.faceLE[size_t(f)]) {
                const int c = t.other(le, f);
                if (in.count(c) || !seen.insert(c).second || t.surf[size_t(c)].kind != SK::Plane) continue;
                bool bounded = true;
                for (int le2 : t.faceLE[size_t(c)]) bounded = bounded && in.count(t.other(le2, c)) > 0;
                if (bounded) caps.push_back(c);
            }
        if (caps.size() != 1) continue;  // a standalone loft (both ends free) is a base, not an added loft — deferred
        const int cap = caps[0];
        const Vec3 up = t.surf[size_t(cap)].dir.normalized();  // outward normal of the cap: from the body to the top
        const double zt = t.surf[size_t(cap)].point.dot(up);
        // Spline walls along the cap normal are an extrusion of a spline outline, not a loft (S4A-SPL-004).
        if (std::all_of(faces.begin(), faces.end(), [&](int f) { return t.ruledAlong(f, up); })) continue;
        // The bottom outline: edges of the side faces to faces other than the cap and the component, on one level.
        std::vector<int> bottom;
        double zb = 0;
        bool ok = true, first = true;
        for (int f : faces)
            for (int le : t.faceLE[size_t(f)]) {
                const int o = t.other(le, f);
                if (in.count(o) || o == cap) continue;
                std::vector<Point3> pts{t.edgePoint(le)};
                for (int v : t.ct.logicalEdges[size_t(le)].ends) pts.push_back(k.vertexPoint(body, v));
                for (const auto& p : pts) {
                    const double z = p.dot(up);
                    if (first) zb = z, first = false;
                    ok = ok && near(z, zb);
                }
                if (std::find(bottom.begin(), bottom.end(), le) == bottom.end()) bottom.push_back(le);
            }
        if (!ok || bottom.empty() || !(zt - zb > REC_LIN_TOL * 10)) continue;
        std::vector<int> remove(faces.begin(), faces.end());
        remove.push_back(cap);
        auto res = removeChecked(k, body, remove);
        if (!res || !(k.massProperties(res->shape).volume < v0)) {
            notes.push_back({"loft", std::to_string(faces.size()) + " side faces: not removed"});
            continue;
        }
        Op op;
        op.kind = Op::Kind::Loft;
        op.plane = planeFrom(zb * up, up);
        op.plane2 = planeFrom(zt * up, up);
        for (int le : bottom) {
            auto c = k.projectEdge(body, le, op.plane);
            if (!c) ok = false;
            else op.curves.push_back(*c);
        }
        for (int le : t.faceLE[size_t(cap)]) {
            auto c = k.projectEdge(body, le, op.plane2);
            if (!c) ok = false;
            else op.curves2.push_back(*c);
        }
        if (!ok) {
            notes.push_back({"loft", "section projection failed"});
            continue;
        }
        op.curves = mergePieces(op.curves);
        op.curves2 = mergePieces(op.curves2);
        return LoftRemoval{res->shape, op};
    }
    return std::nullopt;
}

}  // namespace construe::rec::detail
