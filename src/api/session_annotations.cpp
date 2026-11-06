// Session: technical requirements - part annotations (spec/core-api.md §4.1д, §5 annotations; spec/pmi.md).
#include "api/session.h"

#include "api/errors.h"
#include "regen/assembly_ref.h"
#include "regen/naming.h"

#include <algorithm>
#include <set>

namespace construe::api {

namespace {

// Elements carrying annotations: parts, bodies and assemblies (faces of instances, FX12-PMI-003).
bool partLike(const doc::Element& el) { return !el.isDrawing() && !el.isSpecification(); }

// An assembly face reference "<instance path>/<face name>" whose path starts with a top-level instance of el
// (core-api §4.1д, assembly.md §3.1).
bool assemblyFaceRef(const doc::Element& el, const std::string& ref) {
    const naming::PathName pn = naming::splitPath(ref);
    if (pn.path.empty()) return false;
    const std::string top = regen::refTopInstance(ref);
    if (std::none_of(el.instances.begin(), el.instances.end(), [&](const doc::Instance& i) { return i.id == top; })) return false;
    const auto name = naming::parse(pn.leaf);
    return name && name->kind == naming::EntityKind::Face;
}

// Document with annotation def (without id) under ID id: appended at the end or replacing record id; checked by schema
// and field consistency (document.md §5) - errors are converted to API_INVALID_DEFINITION with a path inside def.
Result<doc::Document> applyAnnotation(const doc::Document& base, const std::string& id, const Json& def, bool replace) {
    if (!def.is_object()) return invalidDefinition("definition must be an object");
    if (def.contains("id")) return invalidDefinition("id not allowed", "/id");
    Json aj = Json::object();
    aj["id"] = id;
    for (auto it = def.begin(); it != def.end(); ++it) aj[it.key()] = it.value();
    Json dj = doc::documentToJsonValue(base);
    const int ei = base.activeIndex();
    Json& el = dj["elements"][ei];
    if (!el.contains("annotations")) el["annotations"] = Json::array();
    Json& as = el["annotations"];
    size_t idx = as.size();
    if (replace)
        for (size_t i = 0; i < as.size(); ++i)
            if (as[i]["id"] == id) idx = i;
    if (idx < as.size()) as[idx] = aj;
    else as.push_back(aj);
    auto r = doc::documentFromJson(dj);
    if (!r) {
        const Error& e = r.error();
        const std::string prefix = "/elements/" + std::to_string(ei) + "/annotations/" + std::to_string(idx);
        std::string p = schemaPathIn(e, prefix);
        Error out("API_INVALID_DEFINITION");
        const std::string* detail = e.stringParam("detail");
        const std::string* name = e.stringParam("name");
        out.with("detail", detail ? *detail : name ? "field " + *name : e.code);
        if (name && p.empty()) p = "/" + *name;
        if (!p.empty()) out.at(p);
        return out;
    }
    r->activeElement = base.activeElement;
    return r;
}

// Checks 2–4 of core-api.md §4.1д (references, size admissibility, repeats); self - ID of the edited annotation.
Status checkAnnotationRefs(const doc::Element& el, const Json& def, const std::string& self) {
    const std::string type = def["type"].get<std::string>();
    if (def.contains("face")) {
        if (el.isAssembly()) {
            if (!assemblyFaceRef(el, def["face"].get<std::string>())) return invalidDefinition("instance face reference", "/face");
        } else {
            auto pn = naming::parse(def["face"].get<std::string>());
            if (!pn || pn->kind != naming::EntityKind::Face) return invalidDefinition("face name", "/face");
        }
    }
    if (def.contains("dimension") && el.isAssembly()) return invalidDefinition("no sketch dimensions in an assembly", "/dimension");
    if (def.contains("dimension")) {
        const std::string sk = def["dimension"]["sketch"], cid = def["dimension"]["constraint"];
        const auto f = el.find(sk);
        if (!f) return notFound(sk);
        const auto* sd = f->sketchData();
        if (!sd) return Error("API_NOT_SKETCH").with("id", sk);
        const sketch::Constraint* c = nullptr;
        for (const auto& x : sd->constraints)
            if (x.id == cid) c = &x;
        if (!c) return notFound(cid);
        if (!pmi::toleranceDimension(c->type)) return invalidDefinition("dimension type", "/dimension/constraint");
        if (def.contains("field") && !pmi::sizeDimension(c->type)) return invalidDefinition("field requires a size dimension", "/field");
    }
    for (const auto& a : el.annotations) {
        if (a["id"].get<std::string>() == self) continue;
        const std::string at = a["type"].get<std::string>();
        if (type == "fit" && at == "fit") {
            if (def.contains("dimension") && a.contains("dimension") && a["dimension"] == def["dimension"])
                return invalidDefinition("dimension already has a tolerance", "/dimension");
            if (def.contains("face") && a.contains("face") && a["face"] == def["face"])
                return invalidDefinition("face already has a tolerance", "/face");
        }
        if (type == "datum" && at == "datum" && a["letter"] == def["letter"]) return invalidDefinition("datum letter in use", "/letter");
    }
    return okStatus();
}

}  // namespace

Result<std::string> Session::addAnnotation(const Json& def) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!partLike(doc().active())) return Error("API_NOT_PART").with("id", doc().active().id);
    doc::Document nd = doc();
    const std::string id = doc::allocate(nd, doc::IdKind::Annotation);
    auto r = applyAnnotation(nd, id, def, false);
    if (!r) return r.error();
    if (auto s = checkAnnotationRefs(doc().active(), def, id); !s) return s.error();
    if (auto s = commit(std::move(*r), "undo.addAnnotation"); !s) return s.error();
    return id;
}

Status Session::updateAnnotation(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    const auto& el = doc().active();
    if (!partLike(el)) return Error("API_NOT_PART").with("id", el.id);
    const int i = el.annotationIndex(id);
    if (i < 0) return notFound(id);
    if (!def.is_object() || !def.contains("type") || def["type"] != el.annotations[size_t(i)]["type"])
        return invalidDefinition("type cannot change", "/type");
    auto r = applyAnnotation(doc(), id, def, true);
    if (!r) return r.error();
    if (auto s = checkAnnotationRefs(el, def, id); !s) return s;
    return commit(std::move(*r), "undo.editAnnotation");
}

Status Session::deleteAnnotation(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!partLike(doc().active())) return Error("API_NOT_PART").with("id", doc().active().id);
    if (doc().active().annotationIndex(id) < 0) return notFound(id);
    doc::Document nd = doc();
    std::erase_if(nd.active().annotations, [&](const Json& a) { return a["id"].get<std::string>() == id; });
    return commit(std::move(nd), "undo.deleteAnnotation");
}

Result<std::vector<Session::AnnotationInfo>> Session::annotations(const StateRef& ref) const {
    const auto& el = doc().active();
    std::vector<AnnotationInfo> out;
    if (!partLike(el)) return out;
    // Assembly (FX12-PMI-003): faces of instances in the virtual assembly state (assembly coordinates).
    regen::StatePtr hold;
    if (el.isAssembly()) {
        hold = assemblyVirtual();
        if (!hold) return apiError("API_STATE_UNAVAILABLE");
    } else {
        auto st = stateOf(ref);
        if (!st) return st.error();
        hold = *st;
    }
    const regen::PartState& s = *hold;
    std::set<std::string> letters;  // part datum letters (pmi.md §4.4)
    for (const auto& a : el.annotations)
        if (a["type"] == "datum") letters.insert(a["letter"].get<std::string>());
    std::set<std::string> seenLetters;
    for (const auto& a : el.annotations) {
        AnnotationInfo info;
        info.id = a["id"].get<std::string>();
        info.def = a;
        const std::string type = a["type"].get<std::string>();
        auto fail = [&](Error e) {
            if (!info.error) info.error = std::move(e);
        };
        // Reference: a face (core-api.md §5 AnnotationInfo).
        std::optional<kernel::SurfaceInfo> surface;
        if (a.contains("face")) {
            const std::string face = a["face"].get<std::string>();
            const auto rr = naming::resolve(face, naming::EntityKind::Face, s.names);
            const auto fate = naming::refFate(rr, true);
            if (fate == naming::RefFate::Lost) {
                fail(Error("PMI_REF_LOST").with("name", face));
            } else if (fate == naming::RefFate::Ambiguous) {
                fail(Error("PMI_REF_AMBIGUOUS").with("name", face).with("actual", double(rr.entities.size())));
            } else {
                const auto& fe = s.names.faces.at(rr.entities.front().index);
                const auto& body = s.bodies.at(fe.body);
                info.bodyId = body.id;
                info.face = fe.full;
                // Anchor point - at the click point at (FX1-FEAT-005), otherwise at the face center.
                Point3 c = k_.faceCentroid(body.shape, fe.face);
                if (a.contains("at")) c = Point3(a["at"][0].get<double>(), a["at"][1].get<double>(), a["at"][2].get<double>());
                info.anchor = k_.closestPointOnFace(body.shape, fe.face, c);
                info.normal = k_.faceNormalAt(body.shape, fe.face, *info.anchor);
                surface = k_.faceSurface(body.shape, fe.face);
            }
        }
        if (type == "fit") {
            if (a.contains("face") && !info.error) {
                if (!surface || surface->kind != kernel::SurfaceInfo::Kind::Cylinder) fail(Error("PMI_NOT_CYLINDER").with("name", a["face"].get<std::string>()));
                else info.nominal = 2 * surface->radius;
            }
            if (a.contains("dimension")) {
                const std::string sk = a["dimension"]["sketch"], cid = a["dimension"]["constraint"];
                const auto f = el.find(sk);
                const sketch::Constraint* c = nullptr;
                if (f && f->sketchData())
                    for (const auto& x : f->sketchData()->constraints)
                        if (x.id == cid) c = &x;
                if (!c) {
                    fail(Error("PMI_REF_LOST").with("name", cid));
                } else if (!pmi::toleranceDimension(c->type) || (a.contains("field") && !pmi::sizeDimension(c->type))) {
                    fail(Error("PMI_DIMENSION_KIND").with("id", cid));
                } else {
                    const auto sd = withProjections(doc(), *f);
                    auto m = sketch::measure(sd ? *sd : *f->sketchData(), cid);
                    if (!m) fail(Error("PMI_REF_LOST").with("name", cid));
                    else info.nominal = std::abs(*m);
                }
            }
            if (!info.error) {
                if (a.contains("field")) {
                    auto d = pmi::fieldDeviations(a["field"].get<std::string>(), info.nominal.value_or(0));
                    if (!d) fail(d.error());
                    else info.upper = d->upper, info.lower = d->lower;
                } else {
                    info.upper = a["upper"].get<double>();
                    info.lower = a["lower"].get<double>();
                }
            }
        }
        if (type == "geometric" && a.contains("datums"))
            for (const auto& l : a["datums"])
                if (!letters.count(l.get<std::string>())) fail(Error("PMI_DATUM_UNDEFINED").with("name", l.get<std::string>()));
        if (type == "datum" && !seenLetters.insert(a["letter"].get<std::string>()).second)
            fail(Error("PMI_DATUM_DUPLICATE").with("name", a["letter"].get<std::string>()));
        out.push_back(std::move(info));
    }
    // The exploded view (FX14-PMI-001, assembly.md §7д.2 item 4): an annotation moves with the instance of its face —
    // anchor and normal by the displacement of that instance; the definition stays in assembled coordinates.
    if (el.isAssembly())
        if (const auto a = assembly(); a && a->exploded)
            for (auto& info : out) {
                if (info.bodyId.empty() || !info.anchor) continue;
                info.move = a->explodedMoveOf(info.bodyId);
                info.anchor = info.move.apply(*info.anchor);
                if (info.normal) info.normal = info.move.applyDir(*info.normal);
            }
    return out;
}

}  // namespace construe::api
