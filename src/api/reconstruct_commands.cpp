// Reconstruction of an imported body's history: building a part from the recognition plan in a temporary session,
// match verification, level fallback (spec/reconstruction.md §5, §6; core-api.md §4.7г; S4-REC-002).
#include "api/session.h"
#include "api/sketch_relations.h"

#include "api/errors.h"
#include "rec/recognize.h"
#include "regen/assembly.h"
#include "regen/assembly_ref.h"
#include "regen/naming.h"
#include "sketch/sketch.h"

#include <Eigen/Geometry>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace construe::api {

namespace {

constexpr int REC_WAIT_MS = 600000;      // waiting for the temporary session's regeneration (§5.1)
constexpr double REC_NAME_TOL = 1e-4;    // mm: key and edge length when looking up a name (§5.3)
constexpr double REC_PLANE_TOL = 1e-6;   // mm, rad: sketch plane coinciding with a built-in plane or a face (§5.3)
constexpr double M_PI_VAL = 3.14159265358979323846;
constexpr double REC_ATTEMPTS_SECONDS = 120;  // wall time of the attempts of one body (§5.5)
constexpr const char* REC_NAME_SUFFIX = " (история)";  // name of the new part and assembly (§5.1, §5.6)
constexpr double REC_REMAP_REL = 1e-2;  // remapping mate references: relative area / length spread (core-api §4.7г, FX10-IO-008)

// Number for an expression (§5.3): noise below REC_LIN_TOL around a value that is a multiple of 1e-6 is removed.
std::string num(double v) {
    const double r = std::round(v * 1e6) / 1e6;
    if (std::abs(v - r) <= rec::REC_LIN_TOL) v = r;
    if (std::abs(v) < 1e-12) v = 0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}
Json arr3(const Vec3& v) { return Json::array({v.x(), v.y(), v.z()}); }


// Built-in axis coinciding with the line (o, d); empty - none.
std::string datumAxis(const Point3& o, const Vec3& d) {
    for (const auto& a : sketch::datumAxisDirections())
        if (d.normalized().cross(a.dir).norm() <= REC_PLANE_TOL && o.cross(a.dir).norm() <= REC_PLANE_TOL * 10) return a.id;
    return {};
}

// A point of a plan curve (plane coordinates) for choosing the sketch face (§5.3): a line's middle, an arc's start, a
// spline's start, a circle's point at angle 0.
Point2 curvePoint(const kernel::Curve2& c) {
    using C2 = kernel::Curve2::Kind;
    if (c.kind == C2::Line) return (c.p1 + c.p2) / 2;
    if (c.kind == C2::Arc) return c.start;
    if (c.kind == C2::Spline) return c.p1;
    return c.center + Point2(c.radius, 0);
}


// Geometry of a stable-named entity for remapping mate references (core-api §4.7г, FX10-IO-008): the entity kind, the
// naming key (center of mass) and measure (area, length), the surface / curve kind and the outward normal of a plane.
struct RemapGeo {
    naming::EntityKind kind = naming::EntityKind::Face;
    Point3 key = Point3::Zero();
    double measure = 0;
    int type = -1;  // SurfaceInfo::Kind of a face, CurveInfo::Kind of an edge; −1 — vertex
    std::optional<Vec3> normal;
};
RemapGeo remapGeo(const kernel::Kernel& k, const regen::PartState& st, naming::EntityKind kind, int idx, const regen::Motion3& m) {
    RemapGeo g;
    g.kind = kind;
    if (kind == naming::EntityKind::Face) {
        const auto& fe = st.names.faces.at(size_t(idx));
        const kernel::Shape& body = st.bodies.at(size_t(fe.body)).shape;
        const auto si = k.faceSurface(body, fe.face);
        g.key = fe.key, g.measure = fe.measure, g.type = int(si.kind);
        if (si.kind == kernel::SurfaceInfo::Kind::Plane) g.normal = m.applyDir(k.faceNormalAt(body, fe.face, fe.key).normalized());
    } else if (kind == naming::EntityKind::Edge) {
        const auto& ee = st.names.edges.at(size_t(idx));
        g.key = ee.key, g.measure = ee.measure, g.type = int(k.edgeCurve(st.bodies.at(size_t(ee.body)).shape, ee.logical).kind);
    } else {
        g.key = st.names.vertices.at(size_t(idx)).key;
    }
    g.key = m.apply(g.key);
    return g;
}

// Part a remapped reference may land in: instance path prefix ("" — the part itself, "i3/" — an instance of the result
// assembly), its state and the motion of its coordinates into the replaced body's ones.
struct RemapTarget {
    std::string prefix;
    regen::StatePtr state;
    regen::Motion3 motion;
};

// The stable name `name` of the body state `from` → the name of the entity of the same kind and type in `to` that
// coincides with it by position: key within `tol`, measure within REC_REMAP_REL, the same plane normal; the nearest one,
// another one as near (within tol / 10) — ambiguous. nullopt — no such entity (core-api §4.7г, FX10-IO-008).
std::optional<std::string> remapName(const kernel::Kernel& k, const regen::PartState& from, const std::string& name,
                                     const std::vector<RemapTarget>& to, double tol) {
    auto pn = naming::parse(name);
    if (!pn) return std::nullopt;
    const auto rr = naming::resolve(name, pn->kind, from.names);
    if (naming::refFate(rr, false) != naming::RefFate::Resolved) return std::nullopt;
    const RemapGeo g = remapGeo(k, from, pn->kind, rr.entities.front().index, regen::Motion3{});
    struct Cand {
        double d;
        std::string name;
    };
    std::vector<Cand> cands;
    for (const auto& t : to) {
        if (!t.state) continue;
        const auto& names = t.state->names;
        const size_t n = pn->kind == naming::EntityKind::Face ? names.faces.size()
                         : pn->kind == naming::EntityKind::Edge ? names.edges.size() : names.vertices.size();
        for (size_t i = 0; i < n; ++i) {
            const RemapGeo c = remapGeo(k, *t.state, pn->kind, int(i), t.motion);
            if (c.type != g.type) continue;
            const double d = (c.key - g.key).norm();
            if (d > tol) continue;
            if (std::abs(c.measure - g.measure) > REC_REMAP_REL * std::max(c.measure, g.measure) + tol) continue;
            if (g.normal && (!c.normal || c.normal->dot(*g.normal) < 1 - rec::REC_ANGLE_TOL)) continue;
            const std::string& full = pn->kind == naming::EntityKind::Face ? names.faces[i].full
                                      : pn->kind == naming::EntityKind::Edge ? names.edges[i].full : names.vertices[i].full;
            // The new name must itself resolve to this one entity.
            const auto nr = naming::resolve(full, pn->kind, names);
            if (naming::refFate(nr, false) != naming::RefFate::Resolved || nr.entities.front().index != int(i)) continue;
            cands.push_back({d, t.prefix + full});
        }
    }
    if (cands.empty()) return std::nullopt;
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    if (cands.size() > 1 && cands[1].d - cands[0].d <= tol / 10) return std::nullopt;
    return cands[0].name;
}

}  // namespace

std::optional<doc::Document> Session::buildReconstruction(const doc::Document& from, const std::string& bodyId, int position, const std::string& name,
                                                          const rec::Plan& P, const std::optional<kernel::Shape>& baseShape, size_t first,
                                                          const std::set<size_t>& skipOps, bool verify, const kernel::Shape& source,
                                                          double roundTol, ReconstructReport& report, size_t& failedOp,
                                                          const Cancel* cancel) const {
    failedOp = SIZE_MAX;
    // Dimension values (§5.3, §5.4): rounding within roundTol (S4-REC-013), otherwise exact.
    auto mm = [roundTol](double v) { return num(rec::roundValue(v, roundTol)) + " mm"; };
    auto dimText = [roundTol](double v, bool angle) {
        return angle ? num(rec::roundValue(v * 180 / M_PI_VAL, roundTol)) + " deg" : num(rec::roundValue(v, roundTol)) + " mm";
    };
    using OK = rec::Op::Kind;
    doc::Document base = from;
    base.activeElement = bodyId;
    std::string sourceName;
    if (const int bi = base.elementIndex(bodyId); bi >= 0)
        if (const auto* pd = base.elements[size_t(bi)].features[0]->pattern()) sourceName = pd->def.value("source", std::string());
    Session T(k_);
    T.reset(base);
    auto partId = T.addElement("part");
    if (!partId) return std::nullopt;
    if (!T.renameElement(*partId, name) || !T.moveElement(*partId, position)) return std::nullopt;
    report.element = *partId;
    report.features.clear();
    report.base = false;
    report.rounded = false;
    auto fail = [&](const std::string& why) -> std::optional<doc::Document> {
        std::string kept;
        for (size_t j : skipOps) kept += (kept.empty() ? " without op " : ",") + std::to_string(j);
        report.notes.push_back({"verify", "from op " + std::to_string(first) + (baseShape ? " with base" : "") + kept +
                                              (roundTol > 0 ? " rounded" : "") + ": " + why});
        return std::nullopt;
    };
    // Failure of operation i (§5.5): feature error, sketch, edge names, pattern axis.
    auto failAt = [&](size_t i, const std::string& why) {
        failedOp = i;
        return fail(why);
    };
    auto state = [&]() -> regen::StatePtr {
        if (!T.waitIdle(REC_WAIT_MS)) return nullptr;
        auto pr = T.regenResult();
        return pr ? pr->final : nullptr;
    };
    // Feature and status check (§5.1).
    auto addFeat = [&](const Json& def) -> std::optional<std::string> {
        auto r = T.addFeature(def);
        if (!r) return std::nullopt;
        if (!T.waitIdle(REC_WAIT_MS)) return std::nullopt;
        auto st = T.featureStatus(*r);
        if (!st || (st->evaluated && st->status == regen::FeatureStatus::Error)) {
            if (std::getenv("CONSTRUE_REC_TRACE") && st)
                for (const auto& m : st->messages) std::fprintf(stderr, "feature error: %s %s\n", m.e.code.c_str(), def.dump().substr(0, 200).c_str());
            return std::nullopt;
        }
        return *r;
    };
    // PlaneRef of the plan's plane (§5.3).
    auto planeRef = [&](const Plane& p, const Point3& interior) -> Json {
        for (const auto& d : sketch::datumPlaneNormals())
            if (p.normal.cross(d.dir).norm() <= REC_PLANE_TOL && std::abs(p.origin.dot(d.dir)) <= REC_PLANE_TOL * 10) return Json{{"datum", d.id}};
        if (auto st = state()) {
            std::string best;
            for (const auto& fe : st->names.faces) {
                const auto& shape = st->bodies[size_t(fe.body)].shape;
                const auto si = k_.faceSurface(shape, fe.face);
                if (si.kind != kernel::SurfaceInfo::Kind::Plane || si.dir.normalized().dot(p.normal) < 1 - REC_PLANE_TOL) continue;
                if (std::abs((p.origin - si.point).dot(p.normal)) > REC_PLANE_TOL * 10) continue;
                if (k_.distanceToFace(shape, fe.face, interior) > REC_PLANE_TOL * 10) continue;
                if (best.empty() || fe.full < best) best = fe.full;
            }
            if (!best.empty()) return Json{{"face", best}};
        }
        return Json{{"frame", {{"origin", arr3(p.origin)}, {"x", arr3(p.x)}, {"y", arr3(p.y)}}}};
    };
    // Sketch on the plan's plane: sketch ID and coordinate system.
    auto openSketch = [&](const Plane& p, const Point3& interior) -> std::optional<std::pair<std::string, Plane>> {
        auto sid = T.createSketch(planeRef(p, interior));
        if (!sid) return std::nullopt;
        if (!T.waitIdle(REC_WAIT_MS)) return std::nullopt;
        auto ss = T.sketchState(*sid);
        if (!ss) return std::nullopt;
        return std::make_pair(*sid, ss->frame);
    };
    auto closeSketch = [&]() {
        (void)sketchops::addDesignRelations(T, rec::REC_LIN_TOL * 100);  // §5.4: symmetry and equalities (FX10-IO-004)
        (void)T.fullyDefineWith({}, dimText);  // §5.4: where it works out
        (void)T.exitSketch();
    };
    // Plan curves into the sketch (§5.3): own points for each curve, coincident at matching ends. Returns the sketchAdd
    // response and the temporary IDs of segments on the axis (for the revolution axis).
    auto addCurves = [&](const rec::Op& op, const Plane& frame, std::vector<std::string>* axisLines,
                         bool constructionAxis) -> std::optional<std::map<std::string, std::string>> {
        const bool flip = frame.normal.dot(op.plane.normal) < 0;
        auto L = [&](const Point2& q) { return frame.toLocal(op.plane.toWorld(q)); };
        Json items = Json::array();
        int n = 0;
        std::vector<std::pair<std::string, Point2>> ends;
        auto pt = [&](const Point2& q, bool end) {
            const std::string id = "t" + std::to_string(++n);
            items.push_back({{"id", id}, {"type", "point"}, {"x", q.x()}, {"y", q.y()}});
            if (end) ends.push_back({id, q});
            return id;
        };
        using C2 = kernel::Curve2::Kind;
        for (const auto& c : op.curves) {
            if (c.kind == C2::Line) {
                const std::string a = pt(L(c.p1), true), b = pt(L(c.p2), true);
                const std::string id = "t" + std::to_string(++n);
                items.push_back({{"id", id}, {"type", "line"}, {"p1", a}, {"p2", b}});
                if (axisLines && op.kind == OK::Revolve && std::abs(c.p1.x()) <= 1e-9 && std::abs(c.p2.x()) <= 1e-9) axisLines->push_back(id);
            } else if (c.kind == C2::Circle) {
                const std::string ce = pt(L(c.center), false);
                items.push_back({{"id", "t" + std::to_string(++n)}, {"type", "circle"}, {"center", ce}, {"r", c.radius}});
            } else if (c.kind == C2::Arc) {
                const std::string ce = pt(L(c.center), false);
                const std::string s = pt(L(flip ? c.end : c.start), true), e = pt(L(flip ? c.start : c.end), true);
                items.push_back({{"id", "t" + std::to_string(++n)}, {"type", "arc"}, {"center", ce}, {"start", s}, {"end", e}});
            } else if (c.kind == C2::Spline) {
                // B-spline by its poles (S4A-SPL-004): the end poles join neighbours, the inner ones are free points.
                Json poles = Json::array();
                const auto& sp = c.spline;
                for (size_t i = 0; i < sp.poles.size(); ++i) poles.push_back(pt(L(sp.poles[i]), i == 0 || i + 1 == sp.poles.size()));
                Json e{{"id", "t" + std::to_string(++n)}, {"type", "bspline"}, {"poles", poles}, {"degree", sp.degree},
                       {"knots", sp.knots}, {"mults", sp.mults}};
                if (!sp.weights.empty()) e["weights"] = sp.weights;
                items.push_back(e);
            }
        }
        if (constructionAxis && axisLines) {
            // Revolution axis without a profile segment on it - an auxiliary segment along the axis (§5.3).
            double y0 = 0, y1 = 0;
            for (const auto& c : op.curves)
                for (const Point2& q : {c.p1, c.p2, c.start, c.end, c.center}) y0 = std::min(y0, q.y()), y1 = std::max(y1, q.y());
            if (y1 - y0 < 1) y1 = y0 + 1;
            const std::string a = pt(L(Point2(0, y0)), false), b = pt(L(Point2(0, y1)), false);
            const std::string id = "t" + std::to_string(++n);
            items.push_back({{"id", id}, {"type", "line"}, {"p1", a}, {"p2", b}, {"construction", true}});
            axisLines->push_back(id);
        }
        // Matching ends - a chain of coincident inside each group.
        std::vector<bool> used(ends.size(), false);
        for (size_t i = 0; i < ends.size(); ++i) {
            if (used[i]) continue;
            std::string prev = ends[i].first;
            for (size_t j = i + 1; j < ends.size(); ++j)
                if (!used[j] && (ends[j].second - ends[i].second).norm() <= rec::REC_LIN_TOL * 100) {
                    used[j] = true;
                    items.push_back({{"type", "coincident"}, {"refs", {prev, ends[j].first}}});
                    prev = ends[j].first;
                }
        }
        auto r = T.sketchAdd(items);
        if (!r) return std::nullopt;
        return *r;
    };
    auto addPoints = [&](const std::vector<Point3>& pts, const Plane& frame) -> bool {
        Json items = Json::array();
        int n = 0;
        for (const auto& p : pts) {
            const Point2 q = frame.toLocal(p);
            items.push_back({{"id", "t" + std::to_string(++n)}, {"type", "point"}, {"x", q.x()}, {"y", q.y()}});
        }
        return T.sketchAdd(items).ok();
    };
    // Circular pattern axis (§5.3): a built-in axis or a cylindrical face; reverse - by direction.
    auto circularAxis = [&](const rec::Op& p, bool& reverse) -> std::optional<Json> {
        if (const std::string d = datumAxis(p.axisOrigin, p.axisDir); !d.empty()) {
            for (const auto& a : sketch::datumAxisDirections())
                if (d == a.id) reverse = p.axisDir.dot(a.dir) < 0;
            return Json{{"datum", d}};
        }
        auto st = state();
        if (!st) return std::nullopt;
        std::string best;
        Vec3 bestDir = Vec3::UnitZ();
        for (const auto& fe : st->names.faces) {
            const auto si = k_.faceSurface(st->bodies[size_t(fe.body)].shape, fe.face);
            if (si.kind != kernel::SurfaceInfo::Kind::Cylinder) continue;
            if (si.dir.normalized().cross(p.axisDir.normalized()).norm() > REC_PLANE_TOL) continue;
            if ((p.axisOrigin - si.point).cross(si.dir.normalized()).norm() > REC_PLANE_TOL * 10) continue;
            if (best.empty() || fe.full < best) best = fe.full, bestDir = si.dir;
        }
        if (best.empty()) return std::nullopt;
        reverse = p.axisDir.dot(bestDir) < 0;
        return Json{{"face", best}};
    };
    auto edgeNames = [&](const std::vector<rec::EdgeKey>& keys) -> std::optional<std::vector<std::string>> {
        auto st = state();
        if (!st) return std::nullopt;
        std::vector<std::string> out;
        auto lookup = [&](const rec::EdgeKey& ek) {
            for (const auto& e : st->names.edges)
                if ((e.key - ek.key).norm() <= REC_NAME_TOL && std::abs(e.measure - ek.length) <= REC_NAME_TOL) return e.full;
            return std::string();
        };
        // The edge as connected pieces of one curve in the result (§5.3): a seam or a vertex of the built part splits it
        // where the recognition body has none — edges on the same circle or line whose union has the key and the length.
        auto unionOf = [&](const rec::EdgeKey& ek) {
            using CK = kernel::CurveInfo::Kind;
            std::vector<std::string> none;
            if (ek.kind == CK::Other) return none;
            struct Cand {
                const naming::EdgeEntry* e;
                kernel::CurveInfo c;
            };
            std::vector<Cand> cands;
            for (const auto& e : st->names.edges) {
                if (e.measure > ek.length + REC_NAME_TOL || (e.key - ek.key).norm() > ek.length + REC_NAME_TOL) continue;
                auto c = k_.edgeCurve(st->bodies[size_t(e.body)].shape, e.logical);
                if (c.kind == ek.kind) cands.push_back({&e, c});
            }
            auto sameCurve = [](const kernel::CurveInfo& a, const kernel::CurveInfo& b) {
                if (a.kind == CK::Circle)
                    return (a.center - b.center).norm() <= REC_NAME_TOL && std::abs(a.radius - b.radius) <= REC_NAME_TOL &&
                           a.axis.normalized().cross(b.axis.normalized()).norm() <= REC_PLANE_TOL;
                const Vec3 d = (a.p2 - a.p1).normalized();
                auto off = [&](const Point3& q) { return (q - a.p1 - (q - a.p1).dot(d) * d).norm(); };
                return d.cross((b.p2 - b.p1).normalized()).norm() <= REC_PLANE_TOL && off(b.p1) <= REC_NAME_TOL && off(b.p2) <= REC_NAME_TOL;
            };
            std::vector<bool> used(cands.size(), false);
            for (size_t i = 0; i < cands.size(); ++i) {
                if (used[i]) continue;
                std::vector<size_t> group;
                for (size_t j = i; j < cands.size(); ++j)
                    if (!used[j] && sameCurve(cands[i].c, cands[j].c)) group.push_back(j), used[j] = true;
                if (group.size() < 2 || group.size() > 12) continue;
                // Subsets of two and more pieces in a fixed order; the first whose union matches.
                for (unsigned mask = 1; mask < (1u << group.size()); ++mask) {
                    std::vector<size_t> pick;
                    for (size_t q = 0; q < group.size(); ++q)
                        if (mask & (1u << q)) pick.push_back(group[q]);
                    if (pick.size() < 2) continue;
                    double len = 0;
                    Vec3 key = Vec3::Zero();
                    for (size_t q : pick) len += cands[q].e->measure, key += cands[q].e->measure * Vec3(cands[q].e->key);
                    const double tol = REC_NAME_TOL * double(pick.size());
                    if (std::abs(len - ek.length) > tol || len <= 0 || (Point3(key / len) - ek.key).norm() > tol) continue;
                    std::vector<std::string> names;
                    for (size_t q : pick) names.push_back(cands[q].e->full);
                    return names;
                }
            }
            return none;
        };
        for (const auto& ek : keys) {
            // The whole edge; otherwise the same pieces of one curve (a base body keeps them, §5.3); otherwise pieces of
            // the result whose union is the edge.
            std::vector<std::string> found{lookup(ek)};
            if (found[0].empty() && !ek.pieces.empty()) {
                found.clear();
                for (const auto& pc : ek.pieces) found.push_back(lookup(pc));
            }
            if (std::find(found.begin(), found.end(), std::string()) != found.end())
                if (auto u = unionOf(ek); !u.empty()) found = u;
            for (const auto& f : found) {
                if (f.empty()) return std::nullopt;
                if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
            }
        }
        return out;
    };

    // Base body (§5.2).
    if (baseShape) {
        auto brep = k_.shapeToBrep(*baseShape);
        if (!brep) return fail("brep");
        doc::Document nd = T.doc();
        auto f = std::make_shared<doc::Feature>();
        f->id = doc::allocate(nd, doc::IdKind::Feature);
        f->type = doc::FeatureType::Import;
        f->name = doc::defaultFeatureName(nd.active(), doc::FeatureType::Import);
        doc::PatternData pd;
        pd.def = Json{{"brep", *brep}};
        if (!sourceName.empty()) pd.def["source"] = sourceName;
        f->data = std::move(pd);
        const std::string fid = f->id;
        nd.active().features.push_back(f);
        if (!T.commit(std::move(nd), "undo.addFeature", {}, fid)) return fail("import");
        report.base = true;
        report.features.push_back({fid, "import"});
    }
    {
        std::map<size_t, std::string> featOf;
        // Sketches shared by extrusions of one level with different depths (§4.11 item 5): by the owner's index.
        struct SharedSketch {
            std::string sketch;
            Plane frame;
            std::vector<sketch::Region> regions;
        };
        std::map<size_t, SharedSketch> sharedSketch;
        // Regions of the sketch whose interior point is inside the curves of op (plan coordinates of op's plane).
        auto regionsInside = [](const SharedSketch& sh, const rec::Op& op) {
            Json ids = Json::array();
            for (const auto& r : sh.regions)
                if (rec::insideCurves(op.curves, op.plane.toLocal(sh.frame.toWorld(r.interiorPoint)))) ids.push_back(r.regionId);
            return ids;
        };
        std::set<size_t> skip = skipOps;  // blocks kept in the base body (§5.5 item 2) and patterns built with their target
        for (size_t i = first; i < P.ops.size(); ++i) {
            if (cancelled(cancel)) return std::nullopt;  // §4.7з: an interrupted attempt leaves no trace
            if (skip.count(i)) continue;
            const rec::Op& op = P.ops[i];
            if (std::getenv("CONSTRUE_REC_TRACE")) std::fprintf(stderr, "start op %zu kind %d\n", i, int(op.kind)), std::fflush(stderr);
            std::optional<std::string> fid;
            const std::string idx = "op " + std::to_string(i);
            switch (op.kind) {
                case OK::Extrude: {
                    // Another depth of one level (§4.11 item 5): the regions of its curves in the sketch of the earlier
                    // extrusion; that one not built in this attempt (kept in the base) — its own sketch, as usual.
                    if (op.sketchOf >= 0 && sharedSketch.count(size_t(op.sketchOf))) {
                        const auto& sh = sharedSketch.at(size_t(op.sketchOf));
                        const Json prof = regionsInside(sh, op);
                        if (prof.empty()) return failAt(i, idx + " regions");
                        const Vec3 want = op.reverse ? Vec3(-op.plane.normal) : op.plane.normal;
                        const bool rev = sh.frame.normal.dot(want) < 0;
                        fid = addFeat({{"type", "extrude"}, {"sketch", sh.sketch}, {"profiles", prof}, {"direction", rev ? "reverse" : "normal"},
                                       {"end", {{"type", "blind"}, {"depth", mm(op.depth)}}}, {"operation", op.cut ? "cut" : "add"}, {"targets", "all"}});
                        break;
                    }
                    // Dependent circular pattern without an axis (§5.3) - contours of all instances in this sketch.
                    rec::Op eop = op;
                    for (size_t j = i + 1; j < P.ops.size(); ++j)
                        if (P.ops[j].target == int(i) && P.ops[j].kind == OK::Pattern && P.ops[j].circular && !P.ops[j].curves.empty()) {
                            bool rv = false;
                            if (!circularAxis(P.ops[j], rv)) {
                                eop.curves = P.ops[j].curves;
                                skip.insert(j);
                            }
                        }
                    Point3 interior = op.plane.origin;
                    if (!eop.curves.empty()) {
                        const auto& c = eop.curves[0];
                        interior = op.plane.toWorld(curvePoint(c));
                    }
                    auto sk = openSketch(op.plane, interior);
                    if (!sk) return failAt(i, idx + " sketch");
                    // Lettering (§4.7а): an editable sketch text — anchor point and text entity — when the sketch
                    // normal is the plan's (a text cannot be mirrored); otherwise the letter outlines.
                    if (op.text && sk->second.normal.dot(op.plane.normal) > 0) {
                        const Point2 a = sk->second.toLocal(op.plane.toWorld(op.text->anchor));
                        const Point2 b = sk->second.toLocal(op.plane.toWorld(op.text->anchor + Point2(std::cos(op.text->angle), std::sin(op.text->angle))));
                        const double ang = std::atan2(b.y() - a.y(), b.x() - a.x());
                        Json items = Json::array({{{"id", "t1"}, {"type", "point"}, {"x", a.x()}, {"y", a.y()}},
                                                  {{"id", "t2"}, {"type", "text"}, {"anchor", "t1"}, {"text", op.text->text}, {"font", op.text->font},
                                                   {"height", rec::roundValue(op.text->height, roundTol)}, {"angle", ang}}});
                        if (!T.sketchAdd(items)) return failAt(i, idx + " text");
                    } else {
                        rec::Op all = eop;  // with the curves of the later extrusions sharing this sketch (§4.11 item 5)
                        all.curves.insert(all.curves.end(), op.shared.begin(), op.shared.end());
                        if (!addCurves(all, sk->second, nullptr, false)) return failAt(i, idx + " curves");
                    }
                    closeSketch();
                    Json prof = "all";
                    if (!op.shared.empty() && !op.text) {
                        SharedSketch sh{sk->first, sk->second, {}};
                        if (auto ss = T.sketchState(sk->first)) sh.regions = ss->result.profiles.regions;
                        prof = regionsInside(sh, eop);
                        if (!prof.empty()) {
                            sharedSketch[i] = std::move(sh);
                        } else {
                            if (std::getenv("CONSTRUE_REC_TRACE")) {
                                std::fprintf(stderr, "shared sketch op %zu: %zu regions, own curves %zu, shared %zu\n", i, sh.regions.size(), eop.curves.size(), op.shared.size());
                                for (const auto& r : sh.regions) {
                                    const Point2 q = op.plane.toLocal(sh.frame.toWorld(r.interiorPoint));
                                    std::fprintf(stderr, "  region %s interior (%.6g %.6g)\n", r.regionId.c_str(), q.x(), q.y());
                                }
                                for (const auto& c : eop.curves)
                                    std::fprintf(stderr, "  curve %d p1(%.6g %.6g) p2(%.6g %.6g) c(%.6g %.6g) r %.6g\n", int(c.kind), c.p1.x(), c.p1.y(), c.p2.x(), c.p2.y(), c.center.x(), c.center.y(), c.radius);
                            }
                            // §4.11 item 5 (FX10-REC-003): no region of the shared sketch lies inside the own curves —
                            // the operation gets its own sketch without the shared curves; the later ones then build
                            // their own sketches on the same plane (as when the owner is not built).
                            if (!T.deleteFeature(sk->first)) return failAt(i, idx + " regions");
                            sk = openSketch(op.plane, interior);
                            if (!sk) return failAt(i, idx + " sketch");
                            if (!addCurves(eop, sk->second, nullptr, false)) return failAt(i, idx + " curves");
                            closeSketch();
                            prof = "all";
                        }
                    }
                    const Vec3 want = op.reverse ? Vec3(-op.plane.normal) : op.plane.normal;
                    const bool rev = sk->second.normal.dot(want) < 0;
                    // Through cut (§4.6) - all the way through in both directions from the sketch plane.
                    const Json end = op.through ? Json{{"type", op.both ? "throughAllBoth" : "throughAll"}}
                                                : Json{{"type", "blind"}, {"depth", mm(op.depth)}};
                    fid = addFeat({{"type", "extrude"}, {"sketch", sk->first}, {"profiles", prof}, {"direction", rev ? "reverse" : "normal"},
                                   {"end", end}, {"operation", op.cut ? "cut" : "add"}, {"targets", "all"}});
                    break;
                }
                case OK::Revolve: {
                    auto sk = openSketch(op.plane, op.plane.origin);
                    if (!sk) return failAt(i, idx + " sketch");
                    const std::string datum = datumAxis(op.axisOrigin, op.axisDir);
                    bool onAxis = false;
                    for (const auto& c : op.curves)
                        onAxis = onAxis || (c.kind == kernel::Curve2::Kind::Line && std::abs(c.p1.x()) <= 1e-9 && std::abs(c.p2.x()) <= 1e-9);
                    std::vector<std::string> axisLines;
                    auto added = addCurves(op, sk->second, &axisLines, datum.empty() && !onAxis);
                    if (!added) return failAt(i, idx + " curves");
                    closeSketch();
                    Json axis;
                    if (!datum.empty()) axis = {{"datum", datum}};
                    else if (!axisLines.empty()) axis = {{"sketchLine", added->at(axisLines[0])}};
                    else return failAt(i, idx + " axis");
                    const Json angle = op.sector > 0 ? Json{{"type", "symmetric"}, {"value", num(op.sector * 180 / M_PI_VAL) + " deg"}} : Json{{"type", "full"}};
                    fid = addFeat({{"type", "revolve"}, {"sketch", sk->first}, {"profiles", "all"}, {"axis", axis}, {"angle", angle},
                                   {"operation", op.cut ? "cut" : "add"}, {"targets", "all"}});
                    break;
                }
                case OK::Hole:
                case OK::CircleCut: {
                    std::vector<Point3> pts = op.points;
                    // Dependent circular pattern without an axis - all points in this feature (§5.3).
                    for (size_t j = i + 1; j < P.ops.size(); ++j)
                        if (P.ops[j].target == int(i) && P.ops[j].kind == OK::Pattern && P.ops[j].circular) {
                            bool rv = false;
                            if (!circularAxis(P.ops[j], rv)) {
                                pts = P.ops[j].points;
                                skip.insert(j);
                            }
                        }
                    auto sk = openSketch(op.plane, pts[0]);
                    if (!sk) return failAt(i, idx + " sketch");
                    if (op.kind == OK::Hole) {
                        if (!addPoints(pts, sk->second)) return failAt(i, idx + " points");
                    } else {
                        rec::Op circles = op;
                        circles.curves.clear();
                        for (const auto& p : pts) {
                            kernel::Curve2 c;
                            c.kind = kernel::Curve2::Kind::Circle;
                            c.center = op.plane.toLocal(p);
                            c.radius = op.radius;
                            circles.curves.push_back(c);
                        }
                        if (!addCurves(circles, sk->second, nullptr, false)) return failAt(i, idx + " circles");
                    }
                    closeSketch();
                    // Material - against the normal of the plan's plane (§2.2).
                    const bool rev = sk->second.normal.dot(op.plane.normal) < 0;
                    if (op.kind == OK::Hole) {
                        Json def = {{"type", "hole"}, {"sketch", sk->first}, {"kind", op.holeKind}, {"size", op.size}};
                        if (op.holeKind != "tapped") def["fit"] = op.fit;
                        else if (op.pitch > 0) def["pitch"] = op.pitch;
                        def["end"] = op.through ? Json{{"type", "throughAll"}} : Json{{"type", "blind"}, {"depth", mm(op.depth)}};
                        if (rev) def["reverse"] = true;
                        fid = addFeat(def);
                    } else {
                        const Json end = op.both ? Json{{"type", "throughAllBoth"}}
                                         : op.through ? Json{{"type", "throughAll"}}
                                                      : Json{{"type", "blind"}, {"depth", mm(op.depth)}};
                        fid = addFeat({{"type", "extrude"}, {"sketch", sk->first}, {"profiles", "all"}, {"direction", rev ? "normal" : "reverse"},
                                       {"end", end}, {"operation", "cut"}, {"targets", "all"}});
                    }
                    break;
                }
                case OK::Pattern: {
                    auto t = featOf.find(size_t(op.target));
                    if (t == featOf.end()) return failAt(i, idx + " pattern target");
                    Json def = {{"type", "pattern"}, {"features", {t->second}}};
                    if (op.circular) {
                        bool rv = false;
                        auto axis = circularAxis(op, rv);
                        if (!axis) return failAt(i, idx + " pattern axis");
                        def["mode"] = "circular";
                        def["axis"] = *axis;
                        def["count"] = std::to_string(op.count);
                        def["angle"] = "360 deg";
                        if (rv) def["reverse"] = true;
                    } else {
                        def["mode"] = "linear";
                        Json dirs = Json::array();
                        for (const auto& d : op.linear) {
                            const std::string ax = datumAxis(Point3::Zero(), d.dir);
                            if (ax.empty()) return failAt(i, idx + " pattern direction");
                            Json jd = {{"ref", {{"datum", ax}}}, {"count", std::to_string(d.count)}, {"spacing", mm(std::abs(d.spacing))}};
                            bool neg = d.spacing < 0;
                            for (const auto& a : sketch::datumAxisDirections())
                                if (ax == a.id && d.dir.dot(a.dir) < 0) neg = !neg;
                            if (neg) jd["reverse"] = true;
                            dirs.push_back(jd);
                        }
                        def["directions"] = dirs;
                    }
                    fid = addFeat(def);
                    break;
                }
                case OK::Mirror: {
                    auto t = featOf.find(size_t(op.target));
                    if (t == featOf.end()) return failAt(i, idx + " mirror target");
                    fid = addFeat({{"type", "mirror"}, {"features", {t->second}}, {"plane", {{"datum", op.mirrorPlane}}}});
                    break;
                }
                case OK::Draft: {
                    // Faces before the draft (§4.5 item 1а): a plane of the current result through the point with the normal.
                    auto st = state();
                    if (!st) return failAt(i, idx + " state");
                    auto planarFace = [&](const Point3& at, const Vec3& n, bool eitherSide) -> std::pair<std::string, Vec3> {
                        std::string best;
                        Vec3 bestN = n;
                        for (const auto& fe : st->names.faces) {
                            const auto& shape = st->bodies[size_t(fe.body)].shape;
                            const auto si = k_.faceSurface(shape, fe.face);
                            if (si.kind != kernel::SurfaceInfo::Kind::Plane) continue;
                            const double c = si.dir.normalized().dot(n.normalized());
                            if (c < 1 - REC_PLANE_TOL && !(eitherSide && c < -1 + REC_PLANE_TOL)) continue;
                            if (k_.distanceToFace(shape, fe.face, at) > REC_PLANE_TOL * 10) continue;
                            if (best.empty() || fe.full < best) best = fe.full, bestN = si.dir.normalized();
                        }
                        return {best, bestN};
                    };
                    // A face before the draft — a plane or a cylinder (a drafted cylinder becomes a cone, S4-REC-023)
                    // through the point with the normal there.
                    auto draftFace = [&](const Point3& at, const Vec3& n) {
                        std::string best;
                        for (const auto& fe : st->names.faces) {
                            const auto& shape = st->bodies[size_t(fe.body)].shape;
                            const auto kind = k_.faceSurface(shape, fe.face).kind;
                            if (kind != kernel::SurfaceInfo::Kind::Plane && kind != kernel::SurfaceInfo::Kind::Cylinder) continue;
                            if (k_.distanceToFace(shape, fe.face, at) > REC_PLANE_TOL * 10) continue;
                            if (k_.faceNormalAt(shape, fe.face, at).normalized().dot(n.normalized()) < 1 - REC_PLANE_TOL) continue;
                            if (best.empty() || fe.full < best) best = fe.full;
                        }
                        return best;
                    };
                    Json faces = Json::array();
                    for (size_t j = 0; j < op.points.size(); ++j) {
                        const std::string name = draftFace(op.points[j], op.normals[j]);
                        if (name.empty()) return failAt(i, idx + " draft face");
                        if (std::find(faces.begin(), faces.end(), Json(name)) == faces.end()) faces.push_back(name);
                    }
                    // Neutral plane: a built-in plane, else an end face of the result in it; pull — against its normal
                    // when the plan's pull is opposite.
                    Json neutral;
                    Vec3 nref = op.plane.normal;
                    for (const auto& d : sketch::datumPlaneNormals())
                        if (neutral.is_null() && op.plane.normal.cross(d.dir).norm() <= REC_PLANE_TOL && std::abs(op.plane.origin.dot(d.dir)) <= REC_PLANE_TOL * 10)
                            neutral = Json{{"datum", d.id}}, nref = d.dir;
                    if (neutral.is_null()) {
                        const auto [name, n] = planarFace(op.points[0], op.plane.normal, true);
                        if (name.empty()) return failAt(i, idx + " neutral");
                        neutral = Json{{"face", name}}, nref = n;
                    }
                    const Vec3 pull = op.reverse ? Vec3(-op.plane.normal) : op.plane.normal;
                    Json def = {{"type", "draft"}, {"faces", faces}, {"neutral", neutral}, {"angle", dimText(op.angle, true)}};
                    if (nref.dot(pull) < 0) def["reverse"] = true;
                    fid = addFeat(def);
                    break;
                }
                case OK::Loft: {
                    // Two sections (§4.9): the outline on the body and the top cap.
                    std::vector<std::string> sketches;
                    for (int sec = 0; sec < 2; ++sec) {
                        rec::Op so = op;
                        if (sec == 1) so.plane = op.plane2, so.curves = op.curves2;
                        if (so.curves.empty()) return failAt(i, idx + " section");
                        const auto& c = so.curves[0];
                        const Point3 interior = so.plane.toWorld(curvePoint(c));
                        auto sk = openSketch(so.plane, interior);
                        if (!sk) return failAt(i, idx + " sketch");
                        if (!addCurves(so, sk->second, nullptr, false)) return failAt(i, idx + " curves");
                        closeSketch();
                        sketches.push_back(sk->first);
                    }
                    fid = addFeat({{"type", "loft"}, {"sections", {{{"sketch", sketches[0]}}, {{"sketch", sketches[1]}}}}, {"operation", "add"}});
                    break;
                }
                case OK::Fillet:
                case OK::Chamfer: {
                    auto names = edgeNames(op.edges);
                    // No edge has the keys (a seam or another order of operations cut them differently, §5.3): the edges of
                    // the current result that the body after the operation no longer has.
                    if (!names) {
                        auto cur = state();
                        if (cur) {
                            std::vector<std::string> gone;
                            for (const auto& e : cur->names.edges) {
                                if (e.kernelEdges.empty()) continue;
                                const auto& shape = cur->bodies[size_t(e.body)].shape;
                                const Point3 mid = k_.edgeMidpoint(shape, e.kernelEdges[e.kernelEdges.size() / 2]);
                                if (k_.distanceToShape(P.after[i], mid) > rec::REC_MATCH_DIST * 10 && k_.distanceToShape(P.after[i], mid) < op.size2) gone.push_back(e.full);
                            }
                            if (!gone.empty()) names = gone;
                        }
                    }
                    if (!names) return failAt(i, idx + " edges");
                    if (std::getenv("CONSTRUE_REC_TRACE")) std::fprintf(stderr, "fillet feature: %zu edges, keys %zu, size %g\n", names->size(), op.edges.size(), op.size2), std::fflush(stderr);
                    Json def = {{"type", op.kind == OK::Fillet ? "fillet" : "chamfer"}, {"edges", *names}};
                    def[op.kind == OK::Fillet ? "radius" : "distance"] = mm(op.size2);
                    fid = addFeat(def);
                    break;
                }
            }
            if (!fid) return failAt(i, idx + " feature");
            featOf[i] = *fid;
            if (std::getenv("CONSTRUE_REC_TRACE"))
                if (auto ts = state(); ts && !ts->bodies.empty()) std::fprintf(stderr, "built op %zu: V %.6f\n", i, k_.massProperties(ts->bodies[0].shape).volume);
            report.features.push_back({*fid, doc::featureTypeName(T.doc().active().find(*fid)->type)});
        }
    }
    // Verification (§6).
    auto st = state();
    if (!st) return fail("regeneration");
    auto pr = T.regenResult();
    for (const auto& f : T.doc().active().features) {
        auto it = pr->featureStatus.find(f->id);
        if (it != pr->featureStatus.end() && it->second.status == regen::FeatureStatus::Error) return fail("feature " + f->id + " error");
    }
    if (!verify) return T.doc();  // import of the same shape (§5.5 item 4)
    std::vector<kernel::Shape> shapes;
    for (const auto& b : st->bodies) shapes.push_back(b.shape);
    if (shapes.empty() || !rec::bodiesMatch(k_, source, shapes.size() == 1 ? shapes[0] : k_.combine(shapes).shape, roundTol))
        return fail("shape mismatch");
    report.rounded = roundTol > 0;
    return T.doc();
}

std::optional<doc::Document> Session::reconstructSolid(const doc::Document& from, const std::string& bodyElement, int position,
                                                       const std::string& name, const kernel::Shape& solid, double roundTol,
                                                       ReconstructReport& rep, const Cancel* cancel, bool importOnly) const {
    const rec::Plan plan = importOnly ? rec::Plan{} : rec::recognize(k_, solid, cancel);
    if (importOnly) rep.notes.push_back({"limit", "product time spent: the body stays imported"});
    if (cancelled(cancel)) return std::nullopt;  // §4.7з: an interrupted recognition leaves no trace
    for (const auto& n : plan.notes) rep.notes.push_back({n.kind, n.detail});
    // Attempt (§5.5) with rounded dimension values; if it fails - the same attempt with exact values (§5.4).
    auto attempt = [&](const std::optional<kernel::Shape>& base, size_t first, const std::set<size_t>& skip,
                       size_t& failed) -> std::optional<doc::Document> {
        if (roundTol > 0) {
            auto nd = buildReconstruction(from, bodyElement, position, name, plan, base, first, skip, true, solid, roundTol, rep, failed, cancel);
            if (nd || cancelled(cancel)) return nd;
        }
        return buildReconstruction(from, bodyElement, position, name, plan, base, first, skip, true, solid, 0, rep, failed, cancel);
    };
    // Block of operation i (§5.5): its first operation; the body right after the removal of the block is the body after
    // the previous block (the core for the first one).
    auto blockStart = [&](size_t i) {
        while (i > 0 && plan.resume[i - 1] == plan.resume[i]) --i;
        return i;
    };
    // Base body with block b kept (§5.5 item 2): the block cuts material away (its removal added volume) — body −
    // (removed − after[b]); otherwise it adds — body ∪ (after[b] − removed). The result must be one solid.
    auto keepBlock = [&](const kernel::Shape& body, size_t b) -> std::optional<kernel::Shape> {
        const kernel::Shape& withBlock = plan.after[b];
        const kernel::Shape& removed = b > 0 ? plan.after[b - 1] : plan.core;
        const bool cuts = k_.massProperties(removed).volume > k_.massProperties(withBlock).volume;
        auto delta = cuts ? k_.boolean(kernel::BoolKind::Subtract, removed, withBlock) : k_.boolean(kernel::BoolKind::Subtract, withBlock, removed);
        if (!delta || delta->shape.null() || k_.counts(delta->shape).solids < 1) return std::nullopt;
        auto r = k_.boolean(cuts ? kernel::BoolKind::Subtract : kernel::BoolKind::Union, body, delta->shape);
        if (!r || r->shape.null() || k_.counts(r->shape).solids != 1 || !(k_.massProperties(r->shape).volume > 0)) return std::nullopt;
        return r->shape;
    };
    if (std::getenv("CONSTRUE_REC_TRACE")) std::fprintf(stderr, "attempts\n"), std::fflush(stderr);
    // Attempts (§5.5): full; on a finishing-operation failure - first without its block (the block stays in the base
    // body), then base body = body after its block and the next blocks; on a base failure or mismatch - base body = core
    // and all finishing ones; the last - import of the original shape.
    std::optional<doc::Document> nd;
    std::optional<kernel::Shape> base;
    size_t first = 0;
    bool coreTried = false;
    std::set<size_t> skip;  // operations of the blocks kept in the base body
    struct Saved {
        std::optional<kernel::Shape> base;
        size_t first = 0;
        bool coreTried = false;
        size_t failed = SIZE_MAX;
    };
    std::optional<Saved> beforeSkip;  // state at the first kept block: the rollback continues from it
    bool keeping = true;              // keeping blocks is still allowed (once given up — the previous steps only)
    int widen = 0;                    // widenings of the kept blocks done (§5.5 item 2b): 1 — the run of the kind, 2 — to the end
    bool reorderTried = false;        // the finishing in another order tried (§5.5 item 1a)
    if (plan.baseKind == "none") base = plan.core, first = plan.baseOps, coreTried = true;
    const auto attemptsStart = std::chrono::steady_clock::now();
    for (size_t guard = 0; guard < 2 * plan.ops.size() + 5 && !nd && !plan.ops.empty(); ++guard) {
        if (cancelled(cancel)) return std::nullopt;
        // §5.5: a body whose attempts keep failing ends as an import, the user does not wait for all of them.
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - attemptsStart).count() > REC_ATTEMPTS_SECONDS * rec::REC_TIME_SCALE) {
            rep.notes.push_back({"limit", "attempts time spent"});
            break;
        }
        size_t failed = SIZE_MAX;
        nd = attempt(base, first, skip, failed);
        if (nd) {
            rep.level = base ? "partial" : "full";
            break;
        }
        if (std::getenv("CONSTRUE_REC_TRACE")) std::fprintf(stderr, "attempt failed at %zu\n", failed), std::fflush(stderr);
        const bool finishing = failed != SIZE_MAX && failed >= plan.baseOps && failed < plan.after.size();
        // §5.5 item 1a (FX10-REC-003): the first full attempt failed on a fillet or chamfer — the finishing in another order
        // (fillets meeting at a corner are built in one order and not in the other).
        if (!reorderTried && !base && first == 0 && finishing &&
            (plan.ops[failed].kind == rec::Op::Kind::Fillet || plan.ops[failed].kind == rec::Op::Kind::Chamfer)) {
            reorderTried = true;
            for (auto& moved : finishingReordered(plan, failed)) {
                if (cancelled(cancel)) return std::nullopt;
                if (std::chrono::duration<double>(std::chrono::steady_clock::now() - attemptsStart).count() > REC_ATTEMPTS_SECONDS * rec::REC_TIME_SCALE) break;
                size_t f2 = SIZE_MAX;
                if (roundTol > 0) nd = buildReconstruction(from, bodyElement, position, name, moved, std::nullopt, 0, {}, true, solid, roundTol, rep, f2, cancel);
                if (!nd && !cancelled(cancel)) nd = buildReconstruction(from, bodyElement, position, name, moved, std::nullopt, 0, {}, true, solid, 0, rep, f2, cancel);
                if (nd) break;
            }
            if (nd) {
                rep.level = "full";
                break;
            }
        }
        if (finishing && keeping && !skip.count(failed)) {
            const size_t b = blockStart(failed);
            if (auto kept = keepBlock(base ? *base : plan.core, b)) {
                if (!beforeSkip) beforeSkip = Saved{base, first, coreTried, failed};
                if (!base) first = plan.baseOps, coreTried = true;
                base = *kept;
                for (size_t j = b; j < plan.resume[failed]; ++j) skip.insert(j);
                continue;
            }
        }
        if (beforeSkip) {
            // Keeping the failed block alone did not give a part (§5.5 item 2b): keep the whole run of finishing blocks of
            // its kind, then every block from it to the end; what precedes them (holes, patterns) is still built.
            bool widened = false;
            while (!widened && widen < 2) {
                ++widen;
                const size_t f = beforeSkip->failed;
                size_t from = blockStart(f), to = plan.resume[f];
                if (widen == 1) {
                    const auto kind = plan.ops[f].kind;
                    while (from > plan.baseOps && plan.ops[from - 1].kind == kind) from = blockStart(from - 1);
                    while (to < plan.after.size() && plan.ops[to].kind == kind) to = plan.resume[to];
                } else {
                    to = plan.after.size();
                }
                std::set<size_t> wide;
                for (size_t j = from; j < to; ++j) wide.insert(j);
                if (wide == skip) continue;  // the same attempt was made
                kernel::Shape body = beforeSkip->base ? *beforeSkip->base : plan.core;
                bool ok = true;
                for (size_t j = from; j < to && ok; j = plan.resume[j])
                    if (auto kept = keepBlock(body, j)) body = *kept;
                    else ok = false;
                if (!ok) continue;
                first = beforeSkip->base ? beforeSkip->first : plan.baseOps;
                coreTried = beforeSkip->base ? beforeSkip->coreTried : true;
                base = body;
                skip = std::move(wide);
                widened = true;
            }
            if (widened) continue;
            // Keeping blocks did not give a part: the previous rollback steps from the first failed block.
            keeping = false;
            skip.clear();
            base = beforeSkip->base, first = beforeSkip->first, coreTried = beforeSkip->coreTried, failed = beforeSkip->failed;
            beforeSkip.reset();
        }
        if (failed != SIZE_MAX && failed >= plan.baseOps && failed < plan.after.size()) {
            base = plan.after[failed];
            first = plan.resume[failed];
            continue;
        }
        if (failed == SIZE_MAX && coreTried && first >= plan.baseOps && first < plan.after.size()) {
            // Mismatch - the first block of finishing operations goes into the base body (§5.5 item 3).
            base = plan.after[first];
            first = plan.resume[first];
            continue;
        }
        if (coreTried) break;
        base = plan.core, first = plan.baseOps, coreTried = true;
    }
    if (!nd) {
        if (cancelled(cancel)) return std::nullopt;
        size_t failed = SIZE_MAX;
        nd = buildReconstruction(from, bodyElement, position, name, plan, solid, plan.ops.size(), {}, false, solid, 0, rep, failed, cancel);
        rep.level = "importOnly";
    }
    return nd;
}

Result<ReconstructReport> Session::reconstructBody(const std::string& bodyElement, const ReconstructOptions& options) {
    auto p = prepareReconstruct(bodyElement, options);
    if (!p) return p.error();
    return applyReconstruct(std::move(*p));
}

// Preparation (§4.7з): recognition and the trial sessions of every solid — the long part; the session is not changed.
Result<ReconstructPrepared> Session::prepareReconstruct(const std::string& bodyElement, const ReconstructOptions& options,
                                                        const Cancel* cancel) const {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    const int bi = doc().elementIndex(bodyElement);
    if (bi < 0) return notFound(bodyElement);
    const doc::Element& el = doc().elements[size_t(bi)];
    if (!el.isBody()) return Error("API_NOT_BODY").with("id", bodyElement);
    const auto* pd = el.features[0]->pattern();
    auto shape = k_.shapeFromBrep(pd ? pd->def.value("brep", std::string()) : std::string());
    if (!shape) return shape.error();
    // Healing of a shape imported without healing (reconstruction.md §1; S4-REC-017): as on STEP import.
    std::optional<ReconstructReport::Heal> heal;
    if (options.healTolerance > 0)
        if (auto h = k_.healShape(*shape, options.healTolerance); h && h->toleranceBefore > kernel::KERNEL_MAX_ENTITY_TOL) {
            heal = ReconstructReport::Heal{h->healed, h->toleranceBefore, h->healed ? h->toleranceAfter : h->toleranceBefore};
            if (h->healed) shape = h->shape;
        }
    // Bodies (solids) of the source body; identical ones form groups, each group is reconstructed once into its own
    // part from its first solid (§5.6, §5.7; S4-REC-012, FX7-IO-001).
    std::vector<kernel::Shape> solids;
    if (k_.counts(*shape).solids > 1)
        for (auto& [sol, faces] : k_.components(*shape)) solids.push_back(sol);
    if (solids.empty()) solids.push_back(*shape);
    const auto groups = solids.size() > 1 ? rec::groupCongruent(k_, solids) : std::vector<rec::CongruenceEntry>(1);
    std::vector<size_t> reps;          // representative solid of each part, in part order
    std::map<size_t, size_t> partOf;   // representative solid → part index
    for (size_t i = 0; i < solids.size(); ++i)
        if (groups[i].group == int(i)) partOf[i] = reps.size(), reps.push_back(i);
    const std::string bodyName = el.name;
    doc::Document cur = doc();
    // The first free element name: stem, "stem 2", …; a replaced body's name counts as free (FX10-IO-001).
    auto freeName = [&](const doc::Document& d, const std::string& stem, bool replacing = true) {
        auto taken = [&](const std::string& n) {
            for (const auto& e : d.elements)
                if (e.name == n && !(replacing && options.replace && e.id == bodyElement)) return true;
            return false;
        };
        std::string name = stem;
        for (int n = 2; taken(name); ++n) name = stem + " " + std::to_string(n);
        return name;
    };
    ReconstructReport total;
    const auto productStart = std::chrono::steady_clock::now();
    for (size_t pi = 0; pi < reps.size(); ++pi) {
        const size_t i = reps[pi];
        reportProgress(cancel, int(pi), int(reps.size()));
        if (cancelled(cancel)) return Error("API_CANCELLED");
        // Name (§5.1): "<body> (history)", for several bodies "<body> (history, body N)"; if taken - " 2", " 3", …
        // Replacing the body (core-api §4.7г, FX10-IO-001): the body's own name, "<body> - тело N"; the body is not counted.
        const std::string stem = options.replace ? (solids.size() == 1 ? bodyName : bodyName + " - тело " + std::to_string(i + 1))
                                                 : bodyName + (solids.size() == 1 ? std::string(REC_NAME_SUFFIX) : " (история, тело " + std::to_string(i + 1) + ")");
        const std::string name = freeName(cur, stem);
        ReconstructReport rep;
        const bool late = pi > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - productStart).count() > options.productSeconds;
        auto nd = reconstructSolid(cur, bodyElement, bi + 1 + int(pi), name, solids[i], std::max(0.0, options.roundTolerance), rep, cancel, late);
        if (!nd && cancelled(cancel)) return Error("API_CANCELLED");
        if (!nd) return Error("API_STATE_UNAVAILABLE").with("detail", std::string("reconstruction failed"));
        cur = std::move(*nd);
        rep.instances = 0;
        for (const auto& g : groups) rep.instances += g.group == int(i) ? 1 : 0;
        total.parts.push_back(rep);
    }
    total.instances = int(solids.size());
    // Summary report: the first part; level - full if all full, importOnly if all import-only, otherwise partial.
    const auto& p0 = total.parts.front();
    total.element = p0.element;
    bool allFull = true, allImport = true;
    for (size_t i = 0; i < total.parts.size(); ++i) {
        const auto& p = total.parts[i];
        allFull = allFull && p.level == "full";
        allImport = allImport && p.level == "importOnly";
        total.features.insert(total.features.end(), p.features.begin(), p.features.end());
        total.base = total.base || p.base;
        const std::string prefix = total.parts.size() > 1 ? "solid " + std::to_string(reps[i] + 1) + ": " : "";
        for (const auto& [kind, detail] : p.notes) total.notes.push_back({kind, prefix + detail});
    }
    total.level = allFull ? "full" : allImport ? "importOnly" : "partial";
    // Replacing (core-api §4.7г, FX10-IO-001): an unsuccessful result leaves the document as it is; with keepPartial
    // (the «Recognize design» command, FX10-IO-008) it is added beside the body under the names without replacing.
    const bool replacing = options.replace && allFull;
    if (options.replace && !allFull) {
        if (!options.keepPartial) {
            total.element = bodyElement;
            total.heal = heal;
            return ReconstructPrepared{doc(), std::move(total), revision_, true};
        }
        for (size_t pi = 0; pi < reps.size(); ++pi) {
            const int ei = cur.elementIndex(total.parts[pi].element);
            if (ei < 0) continue;
            cur.elements[size_t(ei)].name.clear();  // not taken by itself
            cur.elements[size_t(ei)].name =
                freeName(cur, bodyName + (solids.size() == 1 ? std::string(REC_NAME_SUFFIX) : " (история, тело " + std::to_string(reps[pi] + 1) + ")"), false);
        }
    }
    total.heal = heal;
    total.rounded = false;
    for (const auto& p : total.parts) total.rounded = total.rounded || p.rounded;
    cur.activeElement = total.element;
    if (solids.size() > 1) {
        // Assembly of the parts (§5.6, S4-REC-014): "<body name> (history)" right after the parts; an instance per solid
        // in solid order placed by the motion of its group representative onto it (parts are in the representative's
        // world coordinates; FX7-IO-001), the first one fixed; the assembly is active.
        doc::Element a;
        a.id = doc::allocate(cur, doc::IdKind::Element);
        a.kind = "assembly";
        a.name = freeName(cur, replacing ? bodyName : bodyName + REC_NAME_SUFFIX, replacing);
        a.rollback = {doc::Rollback::Kind::End, {}};
        std::vector<int> numbered(reps.size(), 0);
        for (size_t i = 0; i < solids.size(); ++i) {
            const size_t part = partOf.at(size_t(groups[i].group));
            const auto& p = total.parts[part];
            const int pi = cur.elementIndex(p.element);
            if (pi < 0) continue;
            doc::Instance in;
            in.id = doc::allocate(cur, doc::IdKind::Instance);
            in.refElement = p.element;
            in.name = cur.elements[size_t(pi)].name + " <" + std::to_string(++numbered[part]) + ">";
            if (groups[i].group != int(i)) {
                const auto& m = groups[i].motion;
                Eigen::Matrix3d R;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) R(r, c) = m.r[size_t(3 * r + c)];
                Eigen::Quaterniond q(R);
                q.normalize();
                if (q.w() < 0) q.coeffs() = -q.coeffs();
                in.position = {m.t.x(), m.t.y(), m.t.z()};
                in.rotation = {q.w(), q.x(), q.y(), q.z()};
            }
            in.fixed = a.instances.empty();
            a.instances.push_back(in);
        }
        total.assembly = a.id;
        cur.elements.insert(cur.elements.begin() + (bi + 1 + int(reps.size())), std::move(a));
        cur.activeElement = total.assembly;
    }
    if (replacing) {
        // The result takes the body's place: mate references on the body's faces, edges and vertices move to the
        // coinciding ones of the result (FX10-IO-008), assembly instances and drawing views of the body refer to it, the
        // body is removed.
        const std::string result = total.assembly.empty() ? total.element : total.assembly;
        const double tol = rec::REC_MATCH_DIST + rec::REC_ROUND_SPREAD * std::max(0.0, options.roundTolerance) + std::max(0.0, options.healTolerance);
        if (auto s = remapBodyMateRefs(cur, bodyElement, result, tol, total.remapped, total.lostRefs, cancel); !s) return s.error();
        for (auto& e : cur.elements)
            for (auto& in : e.instances)
                if (in.refFile.empty() && in.refElement == bodyElement) in.refElement = result;
        for (auto& e : cur.elements)
            for (auto& sh : e.sheets)
                for (auto& v : sh.views)
                    if (v.refFile.empty() && v.refElement == bodyElement) v.refElement = result;
        const bool wasActive = doc().activeElement == bodyElement;
        cur.elements.erase(cur.elements.begin() + bi);
        if (!wasActive && cur.elementIndex(doc().activeElement) >= 0) cur.activeElement = doc().activeElement;
        total.replaced = true;
    }
    reportProgress(cancel, int(solids.size()), int(solids.size()));
    if (cancelled(cancel)) return Error("API_CANCELLED");
    return ReconstructPrepared{std::move(cur), std::move(total), revision_};
}

Status Session::remapBodyMateRefs(doc::Document& d, const std::string& bodyId, const std::string& result, double tol, int& remapped,
                                  std::vector<std::pair<std::string, std::string>>& lost, const Cancel* cancel) const {
    // References "<path>/<name>" whose instance path ends at an instance of the body in this file (core-api §4.7г).
    struct Hit {
        size_t element, mate, ref;
        std::string path, leaf;
    };
    std::vector<Hit> hits;
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t mi = 0; mi < d.elements[ei].mates.size(); ++mi)
            for (size_t ri = 0; ri < 2; ++ri) {
                const auto pn = naming::splitPath(d.elements[ei].mates[mi].refs[ri]);
                if (pn.path.empty() || pn.leaf.empty()) continue;
                const doc::Element* e = &d.elements[ei];
                bool body = false;
                for (size_t from = 0; e;) {
                    const size_t slash = pn.path.find('/', from);
                    const std::string seg = pn.path.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
                    const doc::Instance* in = nullptr;
                    for (const auto& x : e->instances) in = !in && x.id == seg ? &x : in;
                    if (!in || !in->refFile.empty()) break;
                    if (slash == std::string::npos) {
                        body = in->refElement == bodyId;
                        break;
                    }
                    const int next = d.elementIndex(in->refElement);
                    e = next < 0 ? nullptr : &d.elements[size_t(next)];
                    from = slash + 1;
                }
                if (body) hits.push_back({ei, mi, ri, pn.path, pn.leaf});
            }
    if (hits.empty()) return okStatus();
    // States: the body's, the result part's or the parts of the result assembly (instances in its coordinates).
    std::map<std::string, regen::StatePtr> states;
    auto stateOf = [&](const std::string& id) -> Result<regen::StatePtr> {
        if (auto it = states.find(id); it != states.end()) return it->second;
        doc::Document x = d;
        const int i = x.elementIndex(id);
        if (i < 0) return regen::StatePtr();
        x.activeElement = id;
        x.elements[size_t(i)].rollback = {doc::Rollback::Kind::End, {}};
        regen::Regenerator rg(k_);  // not regen_: that one belongs to the worker thread
        auto r = rg.regenerate(x, revision_, cancel, false, &links_, selfPath());
        if (!r) return Error("API_CANCELLED");
        return states[id] = r->final;
    };
    auto from = stateOf(bodyId);
    if (!from) return from.error();
    std::vector<RemapTarget> to;
    const int ri = d.elementIndex(result);
    if (ri >= 0 && d.elements[size_t(ri)].isAssembly()) {
        for (const auto& in : d.elements[size_t(ri)].instances) {
            auto st = stateOf(in.refElement);
            if (!st) return st.error();
            to.push_back({in.id + "/", *st, regen::motionOf(in.position, in.rotation)});
        }
    } else {
        auto st = stateOf(result);
        if (!st) return st.error();
        to.push_back({"", *st, regen::Motion3{}});
    }
    const bool assembly = ri >= 0 && d.elements[size_t(ri)].isAssembly();
    for (const auto& h : hits) {
        auto& m = d.elements[h.element].mates[h.mate];
        // A construction element of the part (origin, planes, sketches) stays with a part result (FX9-ASM-001).
        std::optional<std::string> name;
        if (regen::isConstructionName(h.leaf)) {
            if (!assembly) continue;
        } else if (*from) {
            name = remapName(k_, **from, h.leaf, to, tol);
        }
        if (name) {
            m.refs[h.ref] = h.path + "/" + *name;
            ++remapped;
        } else if (lost.empty() || lost.back() != std::make_pair(d.elements[h.element].id, m.id)) {
            lost.push_back({d.elements[h.element].id, m.id});
        }
    }
    return okStatus();
}

Result<ReconstructReport> Session::applyReconstruct(ReconstructPrepared prepared) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (ctx_) return Error("API_SKETCH_OPEN").with("id", *ctx_);
    if (prepared.revision != revision_) return Error("API_STALE");
    if (prepared.unchanged) return std::move(prepared.report);
    if (auto s = commit(std::move(prepared.document), "undo.reconstruct", {}, prepared.report.element); !s) return s.error();
    return std::move(prepared.report);
}

}  // namespace construe::api
