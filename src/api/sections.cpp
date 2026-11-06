// Part sections in the 3D view (api/sections.h, spec/sections.md).
#include "api/sections.h"

#include "base/ids.h"
#include "base/tolerance.h"
#include "expr/expr.h"
#include "regen/naming.h"
#include "regen/regen.h"
#include "sketch/sketch.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <map>
#include <optional>

namespace construe::api {

namespace {

Error sectionError(const char* code) { return Error(code); }

kernel::Segment3 lineSeg(const Point3& a, const Point3& b, int i) {
    kernel::Segment3 s;
    s.id = "s" + std::to_string(i);
    s.kind = kernel::Segment3::Kind::Line;
    s.p1 = a;
    s.p2 = b;
    return s;
}

// Closed polygon (points in plane pl, counterclockwise around pl.normal) → profile.
kernel::Profile polygonProfile(const Plane& pl, const std::vector<Point2>& pts) {
    kernel::Profile p;
    p.plane = pl;
    for (size_t i = 0; i < pts.size(); ++i)
        p.outer.segments.push_back(lineSeg(pl.toWorld(pts[i]), pl.toWorld(pts[(i + 1) % pts.size()]), int(i)));
    return p;
}

double signedArea(const std::vector<Point2>& pts) {
    double a = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Point2& p = pts[i];
        const Point2& q = pts[(i + 1) % pts.size()];
        a += p.x() * q.y() - q.x() * p.y();
    }
    return a / 2;
}

// Proper intersection of segments [a,b] and [c,d] (excluding shared ends of adjacent edges).
bool segmentsCross(const Point2& a, const Point2& b, const Point2& c, const Point2& d) {
    auto cross = [](const Point2& o, const Point2& p, const Point2& q) {
        return (p.x() - o.x()) * (q.y() - o.y()) - (p.y() - o.y()) * (q.x() - o.x());
    };
    const double d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

bool simplePolygon(const std::vector<Point2>& pts) {
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            if (j == i + 1 || (i == 0 && j == n - 1)) continue;  // adjacent edges
            if (segmentsCross(pts[i], pts[(i + 1) % n], pts[j], pts[(j + 1) % n])) return false;
        }
    return true;
}

Result<kernel::Shape> extrudeTool(const kernel::Kernel& k, const kernel::Profile& p, const kernel::Extent& e) {
    auto r = k.extrude(p, e, false);
    if (!r) return Error("SECTION_FAILED").with("details", r.error().code);
    return r->shape;
}

}  // namespace

SectionFrame sectionFrame(const kernel::Kernel& k, const regen::PartState& st) {
    SectionFrame f;
    f.all.setEmpty();
    for (const auto& b : st.bodies) f.all.extend(k.bbox(b.shape));
    f.L = f.all.isEmpty() ? 100.0 : 2 * f.all.diagonal().norm() + 10;
    return f;
}

Result<Plane> sectionPlane(const kernel::Kernel& k, const regen::PartState& st, const doc::Document& d, const doc::Section& s) {
    if (!s.plane) return sectionError("SECTION_BAD_REF").with("ref", std::string("plane"));
    const Json& ref = *s.plane;
    Plane pl;
    if (ref.contains("datum")) {
        auto p = sketch::datumPlane(ref["datum"].get<std::string>());
        if (!p) return sectionError("SECTION_REF_NOT_FOUND").with("ref", ref["datum"].get<std::string>());
        pl = *p;
    } else if (ref.contains("feature")) {
        const std::string id = ref["feature"].get<std::string>();
        auto it = st.planes.find(id);
        if (it == st.planes.end()) return sectionError("SECTION_REF_NOT_FOUND").with("ref", id);
        pl = it->second;
    } else {
        const std::string name = ref["face"].get<std::string>();
        const auto rr = naming::resolve(name, naming::EntityKind::Face, st.names);
        if (rr.error || rr.entities.size() != 1 ||
            (rr.status != naming::ResolveStatus::Exact && rr.status != naming::ResolveStatus::Remapped))
            return sectionError("SECTION_REF_NOT_FOUND").with("ref", name);
        const auto& fe = st.names.faces.at(rr.entities.front().index);
        const kernel::Shape& body = st.bodies.at(fe.body).shape;
        const auto si = k.faceSurface(body, fe.face);
        if (si.kind != kernel::SurfaceInfo::Kind::Plane) return sectionError("SECTION_BAD_REF").with("ref", name);
        pl.origin = k.faceCentroid(body, fe.face);
        pl.normal = k.faceNormalAt(body, fe.face, pl.origin).normalized();
        pl.x = pl.normal.unitOrthogonal();
        pl.y = pl.normal.cross(pl.x);
    }
    const auto vars = regen::documentVariables(d);
    auto off = expr::evaluateField(s.offset, expr::LENGTH, vars.env(), regen::documentUnits(d));
    if (!off) return off.error();
    pl.origin += off->value * pl.normal;
    if (s.flip) {
        pl.normal = -pl.normal;
        pl.y = -pl.y;
    }
    return pl;
}

Result<kernel::Shape> sectionTool(const kernel::Kernel& k, const regen::PartState& st, const doc::Document& d,
                                  const doc::Section& s, const SectionFrame& fr) {
    const double L = fr.L;
    if (s.plane) {
        auto pl = sectionPlane(k, st, d, s);
        if (!pl) return pl.error();
        const Point2 c = fr.all.isEmpty() ? Point2::Zero() : pl->toLocal(Point3(fr.all.center()));
        const std::vector<Point2> sq{c + Point2(-L, -L), c + Point2(L, -L), c + Point2(L, L), c + Point2(-L, L)};
        return extrudeTool(k, polygonProfile(*pl, sq), kernel::Extent{kernel::Extent::Kind::Blind, L, {}});
    }
    // Polyline (stepped) section along a sketch (§3.2).
    const auto& el = d.active();
    const auto f = el.find(s.sketch);
    if (!f) return sectionError("SECTION_REF_NOT_FOUND").with("ref", s.sketch);
    const auto* data = f->sketchData();
    if (!data) return sectionError("SECTION_BAD_REF").with("ref", s.sketch);
    auto se = st.sketches.find(s.sketch);
    if (se == st.sketches.end()) return sectionError("SECTION_REF_NOT_FOUND").with("ref", s.sketch);
    // Section by a closed contour (§3.3): regions with material - prisms all the way through.
    {
        const Plane& frame = se->second.frame;
        std::optional<kernel::Shape> contour;
        for (const auto& r : se->second.result.profiles.regions) {
            if (r.depth % 2 != 0) continue;
            auto prism = extrudeTool(k, regen::toKernelProfile(r, frame), kernel::Extent{kernel::Extent::Kind::Symmetric, 2 * L, {}});
            if (!prism) return prism.error();
            if (!contour) {
                contour = *prism;
                continue;
            }
            auto u = k.boolean(kernel::BoolKind::Union, *contour, *prism);
            if (!u) return Error("SECTION_FAILED").with("details", u.error().code);
            contour = u->shape;
        }
        if (contour) {
            if (!s.flip) return *contour;
            const Point2 c = fr.all.isEmpty() ? Point2::Zero() : frame.toLocal(Point3(fr.all.center()));
            const std::vector<Point2> sq{c + Point2(-L, -L), c + Point2(L, -L), c + Point2(L, L), c + Point2(-L, L)};
            auto box = extrudeTool(k, polygonProfile(frame, sq), kernel::Extent{kernel::Extent::Kind::Symmetric, 2 * L, {}});
            if (!box) return box.error();
            auto out = k.boolean(kernel::BoolKind::Subtract, *box, *contour);  // all outside the contour is removed
            if (!out) return Error("SECTION_FAILED").with("details", out.error().code);
            return out->shape;
        }
    }
    const auto& pts = se->second.result.state.points;
    auto bad = [&] { return sectionError("SECTION_BAD_SKETCH").with("sketch", s.sketch); };
    struct L2 {
        std::string id;
        Point2 a, b;
    };
    std::vector<L2> lines;
    for (const auto& e : data->entities) {
        if (e.construction) continue;
        if (e.type == sketch::EntityType::Line) {
            auto pa = pts.find(e.p1), pb = pts.find(e.p2);
            if (pa == pts.end() || pb == pts.end()) return bad();
            lines.push_back({e.id, pa->second, pb->second});
        } else if (e.type == sketch::EntityType::Circle || e.type == sketch::EntityType::Arc) {
            return bad();
        }
    }
    if (lines.empty()) return bad();
    // Chain nodes - by coincidence of coordinates (LINEAR_TOL).
    std::vector<Point2> nodes;
    auto nodeOf = [&](const Point2& p) {
        for (size_t i = 0; i < nodes.size(); ++i)
            if ((nodes[i] - p).norm() <= LINEAR_TOL) return int(i);
        nodes.push_back(p);
        return int(nodes.size() - 1);
    };
    std::vector<std::pair<int, int>> ends;
    for (const auto& l : lines) ends.push_back({nodeOf(l.a), nodeOf(l.b)});
    std::map<int, std::vector<int>> at;  // node → segments
    for (size_t i = 0; i < ends.size(); ++i) {
        if (ends[i].first == ends[i].second) return bad();
        at[ends[i].first].push_back(int(i)), at[ends[i].second].push_back(int(i));
    }
    std::vector<int> freeNodes;
    for (const auto& [n, ls] : at) {
        if (ls.size() > 2) return bad();
        if (ls.size() == 1) freeNodes.push_back(n);
    }
    if (freeNodes.size() != 2) return bad();
    // Start - the free end of the segment with the smallest ID (of the two end segments).
    const int la = at[freeNodes[0]].front(), lb = at[freeNodes[1]].front();
    int startNode = freeNodes[0];
    if (la == lb) startNode = ends[la].first;
    else if (compareIds(lines[lb].id, lines[la].id) < 0) startNode = freeNodes[1];
    std::vector<Point2> chain{nodes[startNode]};
    std::vector<bool> used(lines.size(), false);
    int cur = startNode;
    for (size_t step = 0; step < lines.size(); ++step) {
        int next = -1;
        for (int li : at[cur])
            if (!used[li]) next = li;
        if (next < 0) return bad();
        used[next] = true;
        cur = ends[next].first == cur ? ends[next].second : ends[next].first;
        chain.push_back(nodes[cur]);
    }
    if (std::count(used.begin(), used.end(), false) > 0) return bad();  // disconnected
    const Point2 u0 = (chain[1] - chain[0]).normalized(), uk = (chain.back() - chain[chain.size() - 2]).normalized();
    const double side = s.flip ? -1.0 : 1.0;
    auto left = [&](const Point2& u) -> Point2 { return Point2(-u.y(), u.x()) * side; };  // not an Eigen expression: temp object
    std::vector<Point2> poly;
    poly.push_back(chain.front() - L * u0);
    for (const auto& p : chain) poly.push_back(p);
    poly.push_back(chain.back() + L * uk);
    poly.push_back(chain.back() + L * uk + L * left(uk));
    poly.push_back(chain.front() - L * u0 + L * left(u0));
    if (!simplePolygon(poly)) return bad();
    if (signedArea(poly) < 0) std::reverse(poly.begin(), poly.end());
    return extrudeTool(k, polygonProfile(se->second.frame, poly), kernel::Extent{kernel::Extent::Kind::Symmetric, 2 * L, {}});
}

SectionFrame assemblySectionFrame(const kernel::Kernel& k, const regen::AssemblyState& st) {
    SectionFrame f;
    f.all.setEmpty();
    for (const auto& e : st.instances)
        if (e.state)
            for (const auto& b : e.state->bodies) {
                const BBox3 bb = k.bbox(b.shape);
                if (bb.isEmpty()) continue;
                for (int c = 0; c < 8; ++c) f.all.extend(Point3(e.world.apply(bb.corner(BBox3::CornerType(c)))));
            }
    f.L = f.all.isEmpty() ? 100.0 : 2 * f.all.diagonal().norm() + 10;
    return f;
}

Result<Plane> assemblySectionPlane(const kernel::Kernel& k, const regen::AssemblyState& st, const doc::Document& d, const doc::Section& s,
                                   const regen::PartState* v) {
    if (!s.plane) return sectionError("SECTION_BAD_REF").with("ref", std::string("sketch"));
    const Json& ref = *s.plane;
    Plane pl;
    if (ref.contains("datum")) {
        auto p = sketch::datumPlane(ref["datum"].get<std::string>());
        if (!p) return sectionError("SECTION_REF_NOT_FOUND").with("ref", ref["datum"].get<std::string>());
        pl = *p;
    } else if (ref.contains("face")) {
        // Assembly reference <path>/<face name> (assembly.md §7.1).
        const std::string r = ref["face"].get<std::string>();
        const naming::PathName pn = naming::splitPath(r);  // naming.md §5.3а
        const auto* e = pn.path.empty() ? nullptr : st.find(pn.path);
        if (!e || !e->state) return sectionError("SECTION_REF_NOT_FOUND").with("ref", r);
        const auto rr = naming::resolve(pn.leaf, naming::EntityKind::Face, e->state->names);
        if (rr.error || rr.entities.size() != 1 ||
            (rr.status != naming::ResolveStatus::Exact && rr.status != naming::ResolveStatus::Remapped))
            return sectionError("SECTION_REF_NOT_FOUND").with("ref", r);
        const auto& fe = e->state->names.faces.at(size_t(rr.entities.front().index));
        const kernel::Shape& body = e->state->bodies.at(size_t(fe.body)).shape;
        if (k.faceSurface(body, fe.face).kind != kernel::SurfaceInfo::Kind::Plane) return sectionError("SECTION_BAD_REF").with("ref", r);
        const Point3 c = k.faceCentroid(body, fe.face);
        pl.origin = e->world.apply(c);
        pl.normal = e->world.applyDir(k.faceNormalAt(body, fe.face, c)).normalized();
        pl.x = pl.normal.unitOrthogonal();
        pl.y = pl.normal.cross(pl.x);
    } else {  // assembly reference plane (S2-ASM-012): in the virtual state
        const std::string id = ref.value("feature", std::string());
        if (!v || !v->planes.count(id)) return sectionError("SECTION_REF_NOT_FOUND").with("ref", id);
        pl = v->planes.at(id);
    }
    const auto vars = regen::documentVariables(d);
    auto off = expr::evaluateField(s.offset, expr::LENGTH, vars.env(), regen::documentUnits(d));
    if (!off) return off.error();
    pl.origin += off->value * pl.normal;
    if (s.flip) {
        pl.normal = -pl.normal;
        pl.y = -pl.y;
    }
    return pl;
}

Result<kernel::Shape> assemblySectionTool(const kernel::Kernel& k, const regen::AssemblyState& st, const doc::Document& d,
                                          const doc::Section& s, const SectionFrame& fr, const regen::PartState* v) {
    if (!s.plane && v) return sectionTool(k, *v, d, s, fr);  // assembly sketch (S2-ASM-012): polyline section in assembly coords
    auto pl = assemblySectionPlane(k, st, d, s, v);
    if (!pl) return pl.error();
    const double L = fr.L;
    const Point2 c = fr.all.isEmpty() ? Point2::Zero() : pl->toLocal(Point3(fr.all.center()));
    const std::vector<Point2> sq{c + Point2(-L, -L), c + Point2(L, -L), c + Point2(L, L), c + Point2(-L, L)};
    return extrudeTool(k, polygonProfile(*pl, sq), kernel::Extent{kernel::Extent::Kind::Blind, L, {}});
}

Result<io::BodyMesh> sectionedBodyMesh(const kernel::Kernel& k, const regen::PartState& st, int bodyIndex,
                                       const std::vector<kernel::Shape>& tools, const kernel::TessParams& params,
                                       std::vector<int>* failed) {
    const regen::Body& b = st.bodies.at(bodyIndex);
    // Cutting (§4 item 1) tracing faces back to the originals: origin[i] - face of the original body or −1 (cut face).
    kernel::Shape cur = b.shape;
    std::vector<int> origin(size_t(k.counts(cur).faces));
    for (size_t i = 0; i < origin.size(); ++i) origin[i] = int(i);
    for (size_t ti = 0; ti < tools.size(); ++ti) {
        auto r = k.boolean(kernel::BoolKind::Subtract, cur, tools[ti]);
        if (!r) {
            if (failed) failed->push_back(int(ti));
            continue;
        }
        std::vector<int> next(r->history.faceOrigins.size(), -1);
        for (size_t i = 0; i < next.size(); ++i)
            for (const auto& o : r->history.faceOrigins[i])
                if (o.kind == kernel::Origin::Kind::FromFace && o.input.operand == 0) {
                    next[i] = o.input.face >= 0 && size_t(o.input.face) < origin.size() ? origin[size_t(o.input.face)] : -1;
                    break;
                }
        cur = r->shape;
        origin = std::move(next);
    }
    io::BodyMesh m;
    m.bodyId = b.id;
    m.chordTol = params.chordTol;
    if (k.counts(cur).faces == 0) return m;  // cut away entirely
    auto t = k.tessellate(cur, params);
    if (!t) return t.error();
    std::map<int, std::string> origFaceName, origEdgeName, origVertexName;
    for (const auto& f : st.names.faces)
        if (f.body == bodyIndex) origFaceName[f.face] = f.full;
    for (const auto& e : st.names.edges)
        if (e.body == bodyIndex) origEdgeName[e.logical] = e.full;
    for (const auto& v : st.names.vertices)
        if (v.body == bodyIndex) origVertexName[v.vertex] = v.full;
    auto faceName = [&](int f) -> std::string {
        if (f < 0 || size_t(f) >= origin.size() || origin[size_t(f)] < 0) return {};
        auto it = origFaceName.find(origin[size_t(f)]);
        return it != origFaceName.end() ? it->second : std::string();
    };
    // Edges (§4 item 3): by the pair of original faces and proximity to the original logical edge.
    const auto topoNew = kernel::canonicalTopology(k, cur);
    const auto topoOld = kernel::canonicalTopology(k, b.shape);
    auto edgeName = [&](int le) -> std::string {
        const auto& e = topoNew.logicalEdges.at(size_t(le));
        const int fa = e.faces.first < 0 || size_t(e.faces.first) >= origin.size() ? -1 : origin[size_t(e.faces.first)];
        const int fb = e.faces.second < 0 || size_t(e.faces.second) >= origin.size() ? -1 : origin[size_t(e.faces.second)];
        if (fa < 0 || fb < 0 || e.edges.empty()) return {};
        const Point3 c = k.edgeCentroid(cur, e.edges.front());
        double best = SECTION_MATCH_TOL;
        int hit = -1;
        for (size_t j = 0; j < topoOld.logicalEdges.size(); ++j) {
            const auto& o = topoOld.logicalEdges[j];
            if (!((o.faces.first == fa && o.faces.second == fb) || (o.faces.first == fb && o.faces.second == fa))) continue;
            for (int re : o.edges) {
                const double dd = k.distanceToEdge(b.shape, re, c);
                if (dd <= best) best = dd, hit = int(j);
            }
        }
        if (hit < 0) return {};
        auto it = origEdgeName.find(hit);
        return it != origEdgeName.end() ? it->second : std::string();
    };
    // Face order: by name bytewise, empty (cut faces) - first, in kernel index order (§4 item 5).
    std::vector<const kernel::FaceMesh*> faces;
    for (const auto& fm : t->faces) faces.push_back(&fm);
    std::stable_sort(faces.begin(), faces.end(), [&](auto* a, auto* c) { return faceName(a->face) < faceName(c->face); });
    for (const auto* fm : faces) {
        const uint32_t base = static_cast<uint32_t>(m.positions.size() / 3);
        io::FaceRange fr;
        fr.name = faceName(fm->face);
        fr.firstTriangle = static_cast<uint32_t>(m.triangles.size() / 3);
        fr.count = static_cast<uint32_t>(fm->triangles.size() / 3);
        m.positions.insert(m.positions.end(), fm->positions.begin(), fm->positions.end());
        m.normals.insert(m.normals.end(), fm->normals.begin(), fm->normals.end());
        for (uint32_t i : fm->triangles) m.triangles.push_back(base + i);
        m.faceRanges.push_back(std::move(fr));
    }
    for (const auto& e : t->edges) m.edges.push_back({edgeName(e.edge), e.polylines});
    std::stable_sort(m.edges.begin(), m.edges.end(), [](const auto& a, const auto& c) { return a.name < c.name; });
    // Vertices (§4 item 4): only those coinciding with the originals.
    for (const auto& v : t->vertices) {
        const Point3 p = k.vertexPoint(cur, v.vertex);
        for (const auto& [vi, name] : origVertexName)
            if ((k.vertexPoint(b.shape, vi) - p).norm() <= SECTION_MATCH_TOL) {
                m.vertices.push_back({name, v.position});
                break;
            }
    }
    std::sort(m.vertices.begin(), m.vertices.end(), [](const auto& a, const auto& c) { return a.name < c.name; });
    for (int fi : t->failedFaces) m.failedFaces.push_back(faceName(fi));
    std::sort(m.failedFaces.begin(), m.failedFaces.end());
    return m;
}

namespace {

// The bodies of a view model cut by model sections (drawing.md §6.26): the cut-away regions of the part (§3) or of the
// assembly (§7а, in assembly coordinates — a placed body is moved there first) are subtracted from each body; a body cut
// away entirely is dropped; a section that cannot be built or subtracted is skipped.
// Planar cut faces of a cut body for hatching on drawing views (drawing.md §6.26, FX10-DRW-002): the boundary of each
// face's triangulation (edges of one triangle), chained into loops, the outer (largest area in the face plane) first.
void appendCutFaces(const kernel::Kernel& k, const kernel::Shape& body, const std::vector<bool>& cut, int bodyIndex,
                    std::vector<regen::DrawingCutFace>& out) {
    if (std::find(cut.begin(), cut.end(), true) == cut.end()) return;
    auto tess = k.tessellate(body, {regen::DRAWING_CUT_CHORD, regen::DRAWING_CUT_ANGLE});
    if (!tess) return;
    for (const auto& fm : tess->faces) {
        if (fm.face < 0 || size_t(fm.face) >= cut.size() || !cut[size_t(fm.face)]) continue;
        const auto surf = k.faceSurface(body, fm.face);
        if (surf.kind != kernel::SurfaceInfo::Kind::Plane) continue;
        regen::DrawingCutFace cf;
        cf.body = bodyIndex;
        cf.normal = k.faceNormalAt(body, fm.face, k.faceCentroid(body, fm.face));
        // Vertices welded by position: a face triangulation may repeat nodes on its boundary.
        std::map<std::array<float, 3>, uint32_t> weld;
        std::vector<Point3> pts;
        std::vector<uint32_t> idx(fm.positions.size() / 3);
        for (size_t v = 0; v < idx.size(); ++v) {
            const std::array<float, 3> p{fm.positions[3 * v], fm.positions[3 * v + 1], fm.positions[3 * v + 2]};
            auto [it, fresh] = weld.emplace(p, uint32_t(pts.size()));
            if (fresh) pts.emplace_back(double(p[0]), double(p[1]), double(p[2]));
            idx[v] = it->second;
        }
        // Boundary edges: used by exactly one triangle; directed as in that triangle.
        std::map<std::pair<uint32_t, uint32_t>, int> uses;
        for (size_t t = 0; t + 2 < fm.triangles.size(); t += 3)
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = idx[fm.triangles[t + size_t(e)]], c = idx[fm.triangles[t + size_t((e + 1) % 3)]];
                if (a == c) continue;
                ++uses[{std::min(a, c), std::max(a, c)}];
            }
        std::multimap<uint32_t, uint32_t> next;
        for (size_t t = 0; t + 2 < fm.triangles.size(); t += 3)
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = idx[fm.triangles[t + size_t(e)]], c = idx[fm.triangles[t + size_t((e + 1) % 3)]];
                if (a != c && uses[{std::min(a, c), std::max(a, c)}] == 1) next.emplace(a, c);
            }
        while (!next.empty()) {
            const uint32_t start = next.begin()->first;
            std::vector<Point3> loop;
            uint32_t at = start;
            for (size_t guard = 0; guard <= pts.size(); ++guard) {
                const auto it = next.find(at);
                if (it == next.end()) break;
                loop.push_back(pts[at]);
                at = it->second;
                next.erase(it);
                if (at == start) break;
            }
            if (loop.size() >= 3) cf.loops.push_back(std::move(loop));
        }
        if (cf.loops.empty()) continue;
        // The outer loop first: the largest area (projected along the face normal).
        auto area = [&](const std::vector<Point3>& l) {
            Vec3 s = Vec3::Zero();
            for (size_t i = 0; i < l.size(); ++i) s += l[i].cross(l[(i + 1) % l.size()]);
            return std::abs(s.dot(cf.normal)) / 2;
        };
        std::sort(cf.loops.begin(), cf.loops.end(), [&](const auto& a, const auto& b) { return area(a) > area(b); });
        out.push_back(std::move(cf));
    }
}

void cutModelBySections(const kernel::Kernel& k, const doc::Document& pd, const regen::PartResult& pr, const std::vector<std::string>& ids,
                        regen::DrawingModel& m) {
    const doc::Element& el = pd.active();
    std::vector<kernel::Shape> tools;
    auto section = [&](const std::string& id) -> const doc::Section* {
        for (const auto& s : el.sections)
            if (s.id == id) return &s;
        return nullptr;
    };
    if (pr.assembly) {
        const SectionFrame fr = assemblySectionFrame(k, *pr.assembly);
        for (const auto& id : ids)
            if (const doc::Section* s = section(id))
                if (auto t = assemblySectionTool(k, *pr.assembly, pd, *s, fr)) tools.push_back(*t);
    } else if (pr.final) {
        const SectionFrame fr = sectionFrame(k, *pr.final);
        for (const auto& id : ids)
            if (const doc::Section* s = section(id))
                if (auto t = sectionTool(k, *pr.final, pd, *s, fr)) tools.push_back(*t);
    }
    if (tools.empty()) return;
    const kernel::Motion identity;
    std::vector<kernel::PlacedShape> bodies;
    std::vector<std::string> colors;
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        kernel::PlacedShape b = m.bodies[i];
        if (b.motion.r != identity.r || b.motion.t != identity.t) {
            auto moved = k.transform(b.shape, b.motion);
            if (moved) b = {moved->shape, identity};
        }
        const bool placed = b.motion.r != identity.r || b.motion.t != identity.t;  // could not be moved: not cut
        // Faces traced back through the subtractions (§4 item 2): true — a cut face (from a tool).
        std::vector<bool> cut(size_t(k.counts(b.shape).faces), false);
        if (!placed)
            for (const auto& t : tools)
                if (auto r = k.boolean(kernel::BoolKind::Subtract, b.shape, t)) {
                    std::vector<bool> next(r->history.faceOrigins.size(), true);
                    for (size_t f = 0; f < next.size(); ++f)
                        for (const auto& o : r->history.faceOrigins[f])
                            if (o.kind == kernel::Origin::Kind::FromFace && o.input.operand == 0) {
                                next[f] = o.input.face >= 0 && size_t(o.input.face) < cut.size() && cut[size_t(o.input.face)];
                                break;
                            }
                    b.shape = r->shape;
                    cut = std::move(next);
                }
        if (k.counts(b.shape).faces == 0) continue;  // cut away entirely
        appendCutFaces(k, b.shape, cut, int(bodies.size()), m.cutFaces);
        bodies.push_back(b);
        colors.push_back(i < m.colors.size() ? m.colors[i] : std::string());
    }
    m.bodies = std::move(bodies);
    m.colors = std::move(colors);
}

}  // namespace

void installDrawingSectionCutter() {
    static const bool once = [] {
        regen::setDrawingModelSections(cutModelBySections);
        return true;
    }();
    (void)once;
}

}  // namespace construe::api
