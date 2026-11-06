// Session: selection parsing — classes of the picked elements, the input tables, candidates and their preview
// (spec/selection.md §2.2, §3, §4, §7.1; core-api.md §5 selectionCandidates; FX7-SEL-001, FX7-PT-007).
#include "api/session.h"

#include "api/errors.h"
#include "regen/regen.h"
#include "sketch/sketch.h"

#include <algorithm>
#include <cmath>

namespace construe::api {

namespace {

// Picked element classified by selection.md §2.2.
struct Picked {
    char cls = '?';  // T L C P Z S R; '?' — other
    enum class Src { Datum, Feature, Face, Edge, Vertex, Sketch } src = Src::Datum;
    std::string ref;
    // Reference for the document: T — PointRef; L — LineRef ({datum}/{feature}/{edge}) or {sketchLine}; P — PlaneRef;
    // Z, S — {face}; C — TargetRef of its plane ({edge} or the center {sketchPoint}); R — {revolveAxis}.
    Json json;
    Point3 p = Point3::Zero();   // T point; L start; C center; P origin; Z axis point; S center
    Vec3 d = Vec3::UnitZ();      // L direction; C, P normal; Z axis
    double r = 0;                // C, Z, S radius
    Point3 p2 = Point3::Zero();  // L end (edge, sketch line)
    bool finite = false;         // L has two ends
    Json v1, v2;                 // L body edge: PointRefs of its end vertices (null — not found)
};

class Parser {
public:
    Parser(const kernel::Kernel& k, const doc::Document& d, const regen::PartState& st, const expr::VariableTable& vars)
        : k_(k), d_(d), st_(st), vars_(vars) {}

    Picked classify(const std::string& ref) const {
        Picked x;
        x.ref = ref;
        if (ref == "origin") {
            x.cls = 'T';
            x.json = {{"datum", "origin"}};
            return x;
        }
        if (ref.rfind("plane:", 0) == 0) {
            if (auto pl = sketch::datumPlane(ref)) {
                x.cls = 'P';
                x.json = {{"datum", ref}};
                x.p = pl->origin, x.d = pl->normal;
            }
            return x;
        }
        if (ref.rfind("axis:", 0) == 0) {
            x.cls = 'L';
            x.json = {{"datum", ref}};
            x.d = sketch::datumAxis(ref).value_or(Vec3::UnitZ());
            return x;
        }
        // Sketch entity "<sketch>:<entity>".
        const size_t colon = ref.find(':');
        if (const doc::FeaturePtr f = colon == std::string::npos ? nullptr : d_.active().find(ref.substr(0, colon)); f && f->sketchData()) {
            const std::string sk = ref.substr(0, colon), ent = ref.substr(colon + 1);
            auto it = st_.sketches.find(sk);
            const sketch::Entity* e = f && f->sketchData() ? f->sketchData()->findEntity(ent) : nullptr;
            if (!e || it == st_.sketches.end()) return x;
            const auto& P = it->second.result.state.points;
            const Plane& fr = it->second.frame;
            x.src = Picked::Src::Sketch;
            if (e->type == sketch::EntityType::Point && P.count(ent)) {
                x.cls = 'T';
                x.json = {{"sketchPoint", {{"sketch", sk}, {"entity", ent}}}};
                x.p = fr.toWorld(P.at(ent));
            } else if (e->type == sketch::EntityType::Line && P.count(e->p1) && P.count(e->p2)) {
                x.p = fr.toWorld(P.at(e->p1)), x.p2 = fr.toWorld(P.at(e->p2));
                if ((x.p2 - x.p).norm() < SEL_ON_TOL) return x;
                x.cls = 'L';
                x.json = {{"sketchLine", {{"sketch", sk}, {"entity", ent}}}};
                x.d = (x.p2 - x.p).normalized();
                x.finite = true;
            } else if ((e->type == sketch::EntityType::Circle || e->type == sketch::EntityType::Arc) && P.count(e->center)) {
                x.cls = 'C';
                x.json = {{"sketchPoint", {{"sketch", sk}, {"entity", e->center}}}};
                x.p = fr.toWorld(P.at(e->center));
                x.d = fr.normal.normalized();
                const auto rit = it->second.result.state.radii.find(e->id);
                x.r = e->type == sketch::EntityType::Arc && P.count(e->start) ? (P.at(e->start) - P.at(e->center)).norm()
                      : rit != it->second.result.state.radii.end() ? rit->second
                                                                   : e->r;
            }
            return x;
        }
        // Feature: reference plane, reference axis, revolve.
        if (const doc::FeaturePtr f = d_.active().find(ref)) {
            x.src = Picked::Src::Feature;
            if (f->type == doc::FeatureType::Plane) {
                if (auto it = st_.planes.find(ref); it != st_.planes.end()) {
                    x.cls = 'P';
                    x.json = {{"feature", ref}};
                    x.p = it->second.origin, x.d = it->second.normal.normalized();
                }
            } else if (f->type == doc::FeatureType::Axis) {
                if (auto it = st_.axes.find(ref); it != st_.axes.end()) {
                    x.cls = 'L';
                    x.json = {{"feature", ref}};
                    x.p = it->second.point, x.d = it->second.dir.normalized();
                }
            } else if (f->revolve()) {
                const auto pr = regen::previewReference(k_, d_, st_, Json{{"type", "direction"}, {"ref", {{"revolveAxis", ref}}}}, vars_);
                if (pr.axis) {
                    x.cls = 'R';
                    x.json = {{"revolveAxis", ref}};
                    x.p = pr.axis->point, x.d = pr.axis->dir.normalized();
                }
            }
            return x;
        }
        // Stable or context name of a face, edge or vertex.
        const regen::PartState* sp = &st_;
        if (naming::hasPath(ref) && sp->context) sp = sp->context.get();
        const regen::PartState& s = *sp;
        auto it = s.names.byName.find(ref);
        if (it == s.names.byName.end()) return x;
        switch (it->second.kind) {
            case naming::EntityKind::Face: {
                x.src = Picked::Src::Face;
                const auto& fe = s.names.faces.at(size_t(it->second.index));
                const auto si = k_.faceSurface(s.bodies.at(size_t(fe.body)).shape, fe.face);
                if (si.kind == kernel::SurfaceInfo::Kind::Plane) {
                    std::vector<regen::Message> w;
                    auto pl = regen::sketchPlaneOnFace(ref, s, k_, w);  // outward normal, as the evaluators resolve {face}
                    if (!pl) return x;
                    x.cls = 'P';
                    x.json = {{"face", ref}};
                    x.p = pl->origin, x.d = pl->normal.normalized();
                } else if (si.kind == kernel::SurfaceInfo::Kind::Cylinder || si.kind == kernel::SurfaceInfo::Kind::Cone) {
                    x.cls = 'Z';
                    x.json = {{"face", ref}};
                    x.p = si.point, x.d = si.dir.normalized(), x.r = si.radius;
                } else if (si.kind == kernel::SurfaceInfo::Kind::Sphere) {
                    x.cls = 'S';
                    x.json = {{"face", ref}};
                    x.p = si.point, x.r = si.radius;
                }
                return x;
            }
            case naming::EntityKind::Edge: {
                x.src = Picked::Src::Edge;
                const auto& ee = s.names.edges.at(size_t(it->second.index));
                const auto& shape = s.bodies.at(size_t(ee.body)).shape;
                const auto cv = k_.edgeCurve(shape, ee.logical);
                if (cv.kind == kernel::CurveInfo::Kind::Line && (cv.p2 - cv.p1).norm() > SEL_ON_TOL) {
                    x.cls = 'L';
                    x.json = {{"edge", ref}};
                    x.p = cv.p1, x.p2 = cv.p2, x.d = (cv.p2 - cv.p1).normalized();
                    x.finite = true;
                    // End vertices by position (П6, П7: three-point planes through the edge).
                    for (const auto& ve : s.names.vertices) {
                        if (ve.body != ee.body || ve.full.empty()) continue;
                        const Point3 q = k_.vertexPoint(shape, ve.vertex);
                        if ((q - cv.p1).norm() <= SEL_ON_TOL && x.v1.is_null()) x.v1 = {{"vertex", ve.full}};
                        else if ((q - cv.p2).norm() <= SEL_ON_TOL && x.v2.is_null()) x.v2 = {{"vertex", ve.full}};
                    }
                } else if (cv.kind == kernel::CurveInfo::Kind::Circle) {
                    x.cls = 'C';
                    x.json = {{"edge", ref}};
                    x.p = cv.center, x.d = cv.axis.normalized(), x.r = cv.radius;
                }
                return x;
            }
            case naming::EntityKind::Vertex: {
                x.src = Picked::Src::Vertex;
                const auto& ve = s.names.vertices.at(size_t(it->second.index));
                x.cls = 'T';
                x.json = {{"vertex", ref}};
                x.p = k_.vertexPoint(s.bodies.at(size_t(ve.body)).shape, ve.vertex);
                return x;
            }
        }
        return x;
    }

    // Preview by the regeneration evaluator (selection.md §7.1): false — the candidate would be an error after OK.
    bool preview(const std::string& input, SelectionCandidate& c, const Picked* plane) const {
        if (input == "plane") {
            if (c.aux.empty()) {
                if (!plane) return false;
                c.preview.plane = sketch::faceFrame(plane->p, plane->d);
                return true;
            }
            const auto pr = regen::previewReference(k_, d_, st_, c.aux[0], vars_);
            if (!pr.plane) return false;
            c.preview.plane = pr.plane;
            return true;
        }
        if (input == "point") {
            return true;
        }
        const Json ref = c.aux.empty() ? c.value : Json(nullptr);
        std::optional<regen::RefAxis> ax;
        if (c.aux.empty()) ax = regen::previewReference(k_, d_, st_, Json{{"type", "direction"}, {"ref", ref}}, vars_).axis;
        else ax = regen::previewReference(k_, d_, st_, c.aux[0], vars_).axis;
        if (!ax) return false;
        const std::pair<Point3, Vec3> pd{ax->point, ax->dir.normalized()};
        (input == "axis" ? c.preview.axis : c.preview.dir) = pd;
        return true;
    }

private:
    const kernel::Kernel& k_;
    const doc::Document& d_;
    const regen::PartState& st_;
    const expr::VariableTable& vars_;
};

SelectionCandidate candidate(const std::string& rule, int variant, const std::string& label, const std::vector<const Picked*>& params, bool direct,
                             bool confirm, Json value, std::vector<Json> aux = {}) {
    SelectionCandidate c;
    c.rule = rule;
    c.variant = variant;
    c.labelKey = label;
    const char* keys[] = {"a", "b", "c"};
    for (size_t i = 0; i < params.size() && i < 3; ++i) c.labelParams[keys[i]] = params[i]->ref;
    c.direct = direct;
    c.confirm = confirm;
    c.value = std::move(value);
    c.aux = std::move(aux);
    return c;
}

double along(const Picked& plane, const Point3& q) { return (q - plane.p).dot(plane.d); }
bool parallelDirs(const Vec3& a, const Vec3& b) { return a.normalized().cross(b.normalized()).norm() <= regen::REF_PARALLEL_TOL; }
bool perpendicularDirs(const Vec3& a, const Vec3& b) { return std::abs(a.normalized().dot(b.normalized())) <= regen::REF_PARALLEL_TOL; }
double distToLine(const Picked& l, const Point3& q) { return (q - l.p).cross(l.d).norm(); }

// Plane input (selection.md §3.1, Ф1 rows).
void planeRows(const std::vector<Picked>& ps, SelectionResult& r) {
    auto count = [&](char c) { return std::count_if(ps.begin(), ps.end(), [&](const Picked& p) { return p.cls == c; }); };
    auto first = [&](char c, int skip = 0) -> const Picked* {
        for (const auto& p : ps)
            if (p.cls == c && skip-- == 0) return &p;
        return nullptr;
    };
    const size_t n = ps.size();
    auto prefix = [&](const char* need) {
        r.status = "prefix";
        r.need = {need};
        r.extensible = true;
    };
    if (n == 1) {
        const Picked& a = ps[0];
        switch (a.cls) {
            case 'P':
                r.candidates.push_back(candidate("П1", 1, "ui.sel.plane", {&a}, true, false, a.json));
                r.extensible = true;
                return;
            case 'T': return prefix("ui.sel.need.planeAfterPoint");
            case 'L': return prefix("ui.sel.need.planeAfterLine");
            case 'C':
            case 'Z':
            case 'S': return prefix("ui.sel.need.planeAfterCurve");
            default: return;
        }
    }
    if (n == 2) {
        const Picked* P = first('P');
        if (count('T') == 2) return prefix("ui.sel.need.thirdPoint");
        if (P && count('T') == 1) {  // П2, П2а
            const Picked* T = first('T');
            if (std::abs(along(*P, T->p)) <= SEL_ON_TOL)
                r.candidates.push_back(candidate("П2а", 1, "ui.sel.planePointOn", {P, T}, true, true, P->json));
            else
                r.candidates.push_back(candidate("П2", 1, "ui.sel.planeThroughPoint", {P, T}, false, true, {{"aux", 0}},
                                                 {{{"type", "plane"}, {"mode", "offset"}, {"ref", P->json}, {"distance", "0"}, {"to", T->json}}}));
            return;
        }
        if (count('P') == 2) {  // П3
            const Picked *P1 = first('P'), *P2 = first('P', 1);
            if (parallelDirs(P1->d, P2->d))
                r.candidates.push_back(candidate("П3", 1, "ui.sel.planeMid", {P1, P2}, false, true, {{"aux", 0}},
                                                 {{{"type", "plane"}, {"mode", "mid"}, {"refs", {P1->json, P2->json}}}}));
            return;
        }
        if (P && count('L') == 1) {  // П5, П5а
            const Picked* L = first('L');
            if (!perpendicularDirs(L->d, P->d)) return;  // П5б — Ф2
            const bool edge = L->src == Picked::Src::Edge;
            const bool axis = L->src == Picked::Src::Datum || L->src == Picked::Src::Feature;
            if (edge)
                r.candidates.push_back(candidate("П5", 1, "ui.sel.planeThroughObject", {P, L}, false, true, {{"aux", 0}},
                                                 {{{"type", "plane"}, {"mode", "offset"}, {"ref", P->json}, {"distance", "0"}, {"to", L->json}}}));
            if (edge || axis)
                r.candidates.push_back(candidate(edge ? "П5" : "П5а", 2, "ui.sel.planePerpThroughLine", {P, L}, false, true, {{"aux", 0}},
                                                 {{{"type", "plane"}, {"mode", "angle"}, {"ref", P->json}, {"axis", L->json}, {"angle", "90 deg"}}}));
            return;
        }
        if (count('L') == 1 && count('T') == 1) {  // П6 (variant 1)
            const Picked *L = first('L'), *T = first('T');
            if (L->src != Picked::Src::Edge || L->v1.is_null() || L->v2.is_null() || distToLine(*L, T->p) <= SEL_ON_TOL) return;
            r.candidates.push_back(candidate("П6", 1, "ui.sel.planeLinePoint", {L, T}, false, true, {{"aux", 0}},
                                             {{{"type", "plane"}, {"mode", "threePoints"}, {"points", {L->v1, L->v2, T->json}}}}));
            return;
        }
        if (count('L') == 2) {  // П7
            const Picked *L1 = first('L'), *L2 = first('L', 1);
            if (L1->src != Picked::Src::Edge || L2->src != Picked::Src::Edge || L1->v1.is_null() || L1->v2.is_null() || L2->v1.is_null() ||
                L2->v2.is_null())
                return;
            const bool far2 = distToLine(*L1, L2->p2) > distToLine(*L1, L2->p);
            const Point3 q = far2 ? L2->p2 : L2->p;
            if (distToLine(*L1, q) <= SEL_ON_TOL) return;  // on one line
            const Vec3 nrm = L1->d.cross(q - L1->p).normalized();
            if (std::abs((L2->p - L1->p).dot(nrm)) > SEL_ON_TOL || std::abs((L2->p2 - L1->p).dot(nrm)) > SEL_ON_TOL) return;  // not coplanar
            r.candidates.push_back(candidate("П7", 1, "ui.sel.planeTwoLines", {L1, L2}, false, true, {{"aux", 0}},
                                             {{{"type", "plane"}, {"mode", "threePoints"}, {"points", {L1->v1, L1->v2, far2 ? L2->v2 : L2->v1}}}}));
            return;
        }
        if (P && (count('Z') == 1 || count('S') == 1)) {  // П8, П9 (variants 1–2)
            const Picked* Q = count('Z') ? first('Z') : first('S');
            if (Q->cls == 'Z' && !perpendicularDirs(Q->d, P->d)) return;
            const std::string rule = Q->cls == 'Z' ? "П8" : "П9";
            for (int v = 1; v <= 2; ++v) {
                Json def = {{"type", "plane"}, {"mode", "offset"}, {"ref", P->json}, {"distance", "0"}, {"to", Q->json}};
                if (v == 2) def["far"] = true;
                r.candidates.push_back(candidate(rule, v, v == 1 ? "ui.sel.planeTangentNear" : "ui.sel.planeTangentFar", {P, Q}, false, true,
                                                 {{"aux", 0}}, {def}));
            }
            return;
        }
        if (P && count('C') == 1) {  // П10
            const Picked* C = first('C');
            if (!parallelDirs(C->d, P->d)) return;
            r.candidates.push_back(candidate("П10", 1, "ui.sel.planeThroughObject", {P, C}, false, true, {{"aux", 0}},
                                             {{{"type", "plane"}, {"mode", "offset"}, {"ref", P->json}, {"distance", "0"}, {"to", C->json}}}));
            return;
        }
        return;
    }
    if (n == 3 && count('T') == 3) {  // П4: normal by the click order
        const Vec3 nrm = (ps[1].p - ps[0].p).cross(ps[2].p - ps[0].p);
        if (nrm.norm() < regen::REF_DEGENERATE_TOL) return;
        r.candidates.push_back(candidate("П4", 1, "ui.sel.planeThreePoints", {&ps[0], &ps[1], &ps[2]}, false, true, {{"aux", 0}},
                                         {{{"type", "plane"}, {"mode", "threePoints"}, {"points", {ps[0].json, ps[1].json, ps[2].json}}}}));
    }
}

// Direction (selection.md §3.2) and axis (§3.3) inputs, Ф1 rows.
void lineRows(bool axisInput, const std::vector<Picked>& ps, SelectionResult& r) {
    const size_t n = ps.size();
    auto prefix = [&](const char* need) {
        r.status = "prefix";
        r.need = {need};
        r.extensible = true;
    };
    if (n == 1) {
        const Picked& a = ps[0];
        const char* row = nullptr;
        const char* label = nullptr;
        Json value = a.json;
        switch (a.cls) {
            case 'L':
                if (a.src == Picked::Src::Edge) row = axisInput ? "О2" : "Н2", label = "ui.sel.alongEdge";
                else if (a.src == Picked::Src::Sketch) row = axisInput ? "О3" : "Н3", label = "ui.sel.alongEdge";
                else row = axisInput ? "О1" : "Н1", label = "ui.sel.axis";
                break;
            case 'P':
                if (axisInput) return prefix("ui.sel.need.axisAfterPlane");
                row = "Н4", label = "ui.sel.normal", value = {{"normal", a.json}};
                break;
            case 'Z': row = axisInput ? "О4" : "Н5", label = "ui.sel.faceAxis"; break;
            case 'R': row = axisInput ? "О5" : "Н6", label = "ui.sel.revolveAxis"; break;
            case 'T': return prefix("ui.sel.need.secondPoint");
            default: return;
        }
        r.candidates.push_back(candidate(row, 1, label, {&a}, true, false, value));
        return;
    }
    if (n == 2 && ps[0].cls == 'T' && ps[1].cls == 'T') {  // Н8, О7: from the first to the second
        if ((ps[1].p - ps[0].p).norm() < SEL_ON_TOL) return;
        r.candidates.push_back(candidate(axisInput ? "О7" : "Н8", 1, axisInput ? "ui.sel.axisTwoPoints" : "ui.sel.dirTwoPoints", {&ps[0], &ps[1]},
                                         false, true, {{"aux", 0}},
                                         {{{"type", "axis"}, {"mode", "twoPoints"}, {"points", {ps[0].json, ps[1].json}}}}));
        return;
    }
    if (axisInput && n == 2 && ps[0].cls == 'P' && ps[1].cls == 'P') {  // О8
        if (parallelDirs(ps[0].d, ps[1].d)) return;
        r.candidates.push_back(candidate("О8", 1, "ui.sel.axisPlanes", {&ps[0], &ps[1]}, false, true, {{"aux", 0}},
                                         {{{"type", "axis"}, {"mode", "planes"}, {"refs", {ps[0].json, ps[1].json}}}}));
    }
}

}  // namespace

Result<SelectionResult> Session::selectionCandidates(const std::string& input, const std::vector<std::string>& picks, const StateRef& ref,
                                                     const SelectionFilter& filter) const {
    if (input != "plane" && input != "direction" && input != "axis" && input != "point")
        return Error("API_INVALID_ARGUMENT").with("name", std::string("input"));
    if (picks.size() > SEL_MAX_PICKS) return Error("API_INVALID_ARGUMENT").with("name", std::string("picks"));
    auto st = stateOf(ref);
    if (!st) return st.error();
    SelectionResult r;
    if (picks.empty()) return r;
    const doc::Document d = doc();
    const auto vars = regen::documentVariables(d, &links_, selfPath());
    const Parser parser(k_, d, **st, vars);
    std::vector<Picked> ps;
    for (const auto& p : picks) {
        ps.push_back(parser.classify(p));
        if (ps.back().cls == '?') {
            r.status = "invalid";
            return r;  // unresolvable or "other" (§7.1): need is empty
        }
    }
    if (input == "plane") planeRows(ps, r);
    else if (input == "point") {
        if (ps.size() == 1 && ps[0].cls == 'T') {
            r.candidates.push_back(candidate("Т1", 1, "ui.sel.point", {&ps[0]}, true, false, ps[0].json));
            r.candidates.back().preview.point = ps[0].p;
        }
    } else {
        lineRows(input == "axis", ps, r);
    }
    // Preview by the evaluator; a candidate that would fail regeneration is not offered (§4 item 7).
    const Picked* plane = nullptr;
    for (const auto& p : ps)
        if (p.cls == 'P' && !plane) plane = &p;
    std::vector<SelectionCandidate> ok;
    for (auto& c : r.candidates) {
        if (filter.directOnly && !c.direct) continue;
        if (input == "point" || parser.preview(input, c, plane)) ok.push_back(std::move(c));
    }
    r.candidates = std::move(ok);
    if (!r.candidates.empty()) r.status = "candidates";
    else if (r.status != "prefix") {
        r.status = "invalid";
        r.extensible = false;
        r.need = {"ui.sel.need." + input};
    }
    return r;
}

}  // namespace construe::api
