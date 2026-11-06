// Lettering: extruded or cut font outlines, many small closed rings of walls between two levels of one
// direction (spec/reconstruction.md §4.7; S4-REC-016). The lettering is removed from the body in one operation and
// becomes a sketch of the letter outlines and a single extrusion or cut.
#include "rec/rec_internal.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>

namespace construe::rec::detail {

namespace {

using SK = kernel::SurfaceInfo::Kind;
using CK = kernel::CurveInfo::Kind;

// Ring of walls (§4.7 item 2).
struct Ring {
    std::vector<int> faces;
    double zlo = 0, zhi = 0;
    double extent = 0;     // diagonal of the bounding box in the plane ⊥ d
    double perimeter = 0;  // length of the edges at the top level (ring signature)
};

bool near(double a, double b) { return std::abs(a - b) <= REC_LIN_TOL * 10; }

}  // namespace

std::optional<TextRemoval> removeText(const kernel::Kernel& k, const kernel::Shape& body, std::vector<Note>& notes) {
    Topo t(k, body);
    const double diag = k.bbox(body).diagonal().norm();
    const double v0 = k.massProperties(body).volume;
    // Direction candidates: normals of planar faces (canonical direction, no repeats, in face order).
    std::vector<Vec3> dirs;
    for (int f = 0; f < t.faces(); ++f) {
        if (t.surf[size_t(f)].kind != SK::Plane) continue;
        const Vec3 d = canonicalAxis(t.surf[size_t(f)].dir);
        bool seen = false;
        for (const auto& x : dirs) seen = seen || parallel(x, d);
        if (!seen) dirs.push_back(d);
    }
    for (const Vec3& d : dirs) {
        // Walls: planes ∥ d, cylinders with axis ∥ d and spline walls along d (letters of fonts in other programs'
        // files, S4A-SPL-004).
        std::vector<bool> wall(size_t(t.faces()), false);
        int nWalls = 0;
        for (int f = 0; f < t.faces(); ++f) {
            const auto& si = t.surf[size_t(f)];
            wall[size_t(f)] = (si.kind == SK::Plane && perpendicular(si.dir, d)) || (si.kind == SK::Cylinder && parallel(si.dir, d)) ||
                              t.ruledAlong(f, d);
            nWalls += wall[size_t(f)];
        }
        if (nWalls < 2 * TEXT_MIN_LOOPS) continue;
        // Vertical edges: lines ∥ d between two walls.
        auto vertical = [&](int le) {
            const auto [a, b] = t.ct.logicalEdges[size_t(le)].faces;
            const auto& c = t.curve[size_t(le)];
            return a != b && wall[size_t(a)] && wall[size_t(b)] && c.kind == CK::Line && parallel(c.p2 - c.p1, d);
        };
        std::vector<int> parent(size_t(t.faces()));
        std::iota(parent.begin(), parent.end(), 0);
        std::function<int(int)> find = [&](int x) { return parent[size_t(x)] == x ? x : parent[size_t(x)] = find(parent[size_t(x)]); };
        for (size_t le = 0; le < t.ct.logicalEdges.size(); ++le)
            if (vertical(int(le))) {
                const auto [a, b] = t.ct.logicalEdges[le].faces;
                parent[size_t(find(a))] = find(b);
            }
        std::map<int, std::vector<int>> byRoot;
        for (int f = 0; f < t.faces(); ++f)
            if (wall[size_t(f)]) byRoot[find(f)].push_back(f);
        const Plane P0 = planeFrom(Point3::Zero(), d);
        std::vector<Ring> rings;
        for (auto& [root, faces] : byRoot) {
            if (faces.size() < 2) continue;  // a single face (wall of a hole or boss) is not a letter outline
            // Pieces of one cylinder (a hole or boss wall split into halves by another program) are not a letter either.
            const auto& s0 = t.surf[size_t(faces[0])];
            if (s0.kind == SK::Cylinder &&
                std::all_of(faces.begin(), faces.end(), [&](int f) { return samePiece(s0, t.surf[size_t(f)]); }))
                continue;
            Ring r;
            r.faces = faces;
            std::vector<double> zs;
            Eigen::AlignedBox2d box;
            bool ok = true;
            for (int f : faces) {
                for (int le : t.faceLE[size_t(f)]) {
                    if (vertical(le)) continue;
                    // A non-vertical wall edge to another wall: not a ring (the wall is not bounded by two levels).
                    if (wall[size_t(t.other(le, f))] && t.other(le, f) != f) {
                        ok = false;
                        break;
                    }
                    std::vector<Point3> pts{t.edgePoint(le)};
                    for (int v : t.ct.logicalEdges[size_t(le)].ends) pts.push_back(k.vertexPoint(body, v));
                    for (const auto& p : pts) {
                        zs.push_back(p.dot(d));
                        box.extend(P0.toLocal(p));
                    }
                }
                if (!ok) break;
            }
            if (!ok || zs.empty()) continue;
            r.zlo = *std::min_element(zs.begin(), zs.end());
            r.zhi = *std::max_element(zs.begin(), zs.end());
            if (!(r.zhi - r.zlo > REC_LIN_TOL * 10)) continue;
            for (double z : zs) ok = ok && (near(z, r.zlo) || near(z, r.zhi));
            // Each wall reaches both levels.
            for (int f : faces) {
                bool lo = false, hi = false;
                for (int le : t.faceLE[size_t(f)]) {
                    if (vertical(le)) continue;
                    const double z = t.edgePoint(le).dot(d);
                    lo = lo || near(z, r.zlo);
                    hi = hi || near(z, r.zhi);
                    if (near(z, r.zhi)) {
                        const auto L = t.ct.logicalEdges[size_t(le)];
                        for (int e : L.edges) r.perimeter += k.edgeLength(body, e);
                    }
                }
                ok = ok && lo && hi;
            }
            if (!ok) continue;
            r.extent = box.diagonal().norm();
            if (r.extent > TEXT_MAX_LOOP_REL * diag) continue;  // a large loop (outer loop of the body) is not a letter
            rings.push_back(r);
        }
        // Ring groups by level pair (§4.7 item 3).
        std::vector<std::vector<const Ring*>> groups;
        for (const auto& r : rings) {
            bool put = false;
            for (auto& g : groups)
                if (!put && near(g[0]->zlo, r.zlo) && near(g[0]->zhi, r.zhi)) g.push_back(&r), put = true;
            if (!put) groups.push_back({&r});
        }
        for (const auto& g : groups) {
            if (int(g.size()) < TEXT_MIN_LOOPS) continue;
            // Not all rings are identical (identical ones are a feature pattern, §4.8).
            double pmax = 0;
            for (const auto* r : g) pmax = std::max(pmax, r->perimeter);
            bool distinct = false;
            for (const auto* r : g) distinct = distinct || std::abs(r->perimeter - g[0]->perimeter) > TEXT_DISTINCT_REL * pmax;
            if (!distinct) continue;
            const double zlo = g[0]->zlo, zhi = g[0]->zhi;
            std::set<int> walls;
            for (const auto* r : g) walls.insert(r->faces.begin(), r->faces.end());
            // Caps: planes ⊥ d at level zlo or zhi, adjacent to the group's walls, whose neighbors are all walls of the group
            // (top of an extruded letter, bottom of a cut one).
            std::set<int> caps;
            bool capLo = false, capHi = false;
            for (int f : walls)
                for (int le : t.faceLE[size_t(f)]) {
                    const int c = t.other(le, f);
                    if (walls.count(c) || caps.count(c)) continue;
                    const auto& sc = t.surf[size_t(c)];
                    if (sc.kind != SK::Plane || !parallel(sc.dir, d)) continue;
                    bool bounded = true;
                    for (int le2 : t.faceLE[size_t(c)]) bounded = bounded && walls.count(t.other(le2, c)) > 0;
                    if (!bounded) continue;
                    const double z = sc.point.dot(d);
                    if (near(z, zlo)) capLo = true;
                    else if (near(z, zhi)) capHi = true;
                    else continue;
                    caps.insert(c);
                }
            // Caps at both levels: letters with counters («А», «В») — the counters (plate inside a raised letter,
            // islands inside a cut one) are bounded by the group's walls too. The letters are the level with the larger
            // cap area; the caps of the other level stay in the body.
            if (capLo && capHi) {
                double aLo = 0, aHi = 0;
                for (int c : caps) (near(t.surf[size_t(c)].point.dot(d), zlo) ? aLo : aHi) += k.faceArea(body, c);
                const bool keepHi = aHi > aLo;
                for (auto it = caps.begin(); it != caps.end();)
                    it = near(t.surf[size_t(*it)].point.dot(d), keepHi ? zhi : zlo) ? std::next(it) : caps.erase(it);
                capLo = !keepHi, capHi = keepHi;
            }
            std::vector<int> faces(walls.begin(), walls.end());
            faces.insert(faces.end(), caps.begin(), caps.end());
            auto res = removeChecked(k, body, faces);
            if (!res) {
                notes.push_back({"text", std::to_string(g.size()) + " loops: not removed"});
                continue;
            }
            const bool cut = k.massProperties(res->shape).volume > v0;
            // Sketch plane: extruded — the base level (without caps), normal towards the caps; cut — the open
            // level (without bottom), normal outward; through — the top level, normal +d.
            double zBase;
            Vec3 n;
            if (cut) {
                zBase = capLo ? zhi : capHi ? zlo : zhi;
                n = zBase == zhi ? d : Vec3(-d);
            } else {
                if (!capLo && !capHi) continue;
                zBase = capHi ? zlo : zhi;
                n = zBase == zlo ? d : Vec3(-d);
            }
            const Plane P = planeFrom(zBase * d, n);
            Op op;
            op.kind = Op::Kind::Extrude;
            op.plane = P;
            op.depth = zhi - zlo;
            op.cut = cut;
            op.reverse = cut;
            std::set<int> done;
            bool ok = true;
            for (int f : walls)
                for (int le : t.faceLE[size_t(f)]) {
                    if (vertical(le) || !near(t.edgePoint(le).dot(d), zBase) || !done.insert(le).second) continue;
                    auto c2 = k.projectEdge(body, le, P);
                    if (!c2) ok = false;
                    else op.curves.push_back(*c2);
                }
            if (!ok || op.curves.empty()) {
                notes.push_back({"text", "contour projection failed"});
                continue;
            }
            op.curves = mergePieces(op.curves);
            // An editable sketch text (§4.7а); not matched — the letter outlines stay (with a note for the report).
            op.text = fitText(op.curves);
            if (!op.text) notes.push_back({"text", "letters not matched to a product font: outlines"});
            return TextRemoval{res->shape, op};
        }
    }
    return std::nullopt;
}

}  // namespace construe::rec::detail
