// Session: assembly weld seams - commands and status (spec/core-api.md §4.1в, §5 welds; spec/weld.md).
#include "api/session.h"

#include "api/errors.h"
#include "base/strings.h"
#include "drawing/weld_annotations.h"
#include "drawing/weld_geometry.h"
#include "pmi/weld.h"
#include "regen/assembly_ref.h"
#include "regen/naming.h"

#include <set>

namespace construe::api {

namespace {

Error weldDefError(const std::string& name, const std::string& detail) {
    return Error("API_INVALID_DEFINITION").with("name", name).with("detail", detail).at("/" + name);
}

// Instance path of an edge or face reference "<instance path>/<name>" of kind `kind` (core-api §4.1в, FX8-WLD-001;
// syntactic parsing — assembly.md §3.1): the path starts with a top-level instance of el; nullopt — not such a reference.
std::optional<std::string> instanceRefPath(const doc::Element& el, const std::string& ref, naming::EntityKind kind) {
    const naming::PathName pn = naming::splitPath(ref);
    if (pn.path.empty()) return std::nullopt;
    const std::string top = regen::refTopInstance(ref);
    if (std::none_of(el.instances.begin(), el.instances.end(), [&](const doc::Instance& i) { return i.id == top; })) return std::nullopt;
    const auto name = naming::parse(pn.leaf);
    if (!name || name->kind != kind) return std::nullopt;
    return pn.path;
}

bool edgeRefValid(const doc::Element& el, const std::string& ref) { return instanceRefPath(el, ref, naming::EntityKind::Edge).has_value(); }

std::optional<std::string> faceRefPath(const doc::Element& el, const std::string& ref) {
    return instanceRefPath(el, ref, naming::EntityKind::Face);
}

// Body mesh of a weld body (api layout, core-api §5.2): one face range, the edges as polylines.
io::BodyMesh weldMesh(const kernel::Tessellation& t, const std::string& id, double chordTol) {
    io::BodyMesh m;
    m.bodyId = "weld:" + id;
    m.chordTol = chordTol;
    io::FaceRange fr{m.bodyId, 0, 0};
    for (const auto& f : t.faces) {
        const uint32_t base = uint32_t(m.positions.size() / 3);
        m.positions.insert(m.positions.end(), f.positions.begin(), f.positions.end());
        m.normals.insert(m.normals.end(), f.normals.begin(), f.normals.end());
        for (uint32_t i : f.triangles) m.triangles.push_back(base + i);
    }
    fr.count = uint32_t(m.triangles.size() / 3);
    m.faceRanges.push_back(fr);
    for (const auto& e : t.edges) m.edges.push_back({m.bodyId + ":e" + std::to_string(e.edge), e.polylines});
    return m;
}

}  // namespace

struct Session::WeldGeom {
    drawing::WeldPath path;
    drawing::WeldBody body;
    mutable std::mutex mu;
    mutable std::map<double, io::BodyMesh> meshes;  // by chord tolerance
};

// Weld line and body in the virtual state v (weld.md §6а, §6б), cached by the record and the leg value per state.
std::shared_ptr<const Session::WeldGeom> Session::weldGeom(const regen::StatePtr& v, const Json& weld, const doc::Document* d) const {
    const auto values = drawing::weldValues(d ? *d : doc(), weld);
    const std::string key = weld.dump() + "|" + (values.leg ? std::to_string(*values.leg) : std::string());
    {
        std::lock_guard<std::mutex> lock(weldMu_);
        if (weldState_ != v) {
            weldState_ = v;
            weldCache_.clear();
        }
        if (auto it = weldCache_.find(key); it != weldCache_.end()) return it->second;
    }
    auto g = std::make_shared<WeldGeom>();
    g->path = drawing::weldPath(k_, *v, weld);
    const Json* type = pmi::weldTypeInfo(weld.value("standard", std::string()), weld.value("type", std::string()));
    if (!g->path.error && type) g->body = drawing::weldBody(k_, *v, g->path, (*type)["joint"].get<std::string>(), values.leg);
    std::lock_guard<std::mutex> lock(weldMu_);
    if (weldState_ == v) weldCache_[key] = g;
    return g;
}

const Json& Session::weldTypes() { return pmi::weldTypes(); }

// Document with weld record weld (with id and name) at the end of welds or in place of the record with its id; checks
// of core-api §4.1в in order: schema, weldCheck, edge references, values.
Result<doc::Document> Session::withWeld(const doc::Document& base, const Json& weld) const {
    const int ei = base.activeIndex();
    Json dj = doc::documentToJsonValue(base);
    Json& el = dj["elements"][ei];
    if (!el.contains("welds")) el["welds"] = Json::array();
    Json& ws = el["welds"];
    size_t idx = ws.size();
    for (size_t i = 0; i < ws.size(); ++i)
        if (ws[i]["id"] == weld["id"]) idx = i;
    if (idx < ws.size()) ws[idx] = weld;
    else ws.push_back(weld);
    auto r = doc::documentFromJson(dj);
    if (!r) {
        const Error& e = r.error();
        const std::string prefix = "/elements/" + std::to_string(ei) + "/welds/" + std::to_string(idx);
        std::string p = schemaPathIn(e, prefix);
        const std::string* name = e.stringParam("name");
        std::string field = name ? *name : std::string();
        if (field.empty() && p.size() > 1) field = p.substr(1, p.find('/', 1) == std::string::npos ? std::string::npos : p.find('/', 1) - 1);
        Error out("API_INVALID_DEFINITION");
        out.with("name", field).with("detail", e.code);
        out.at(p.empty() && !field.empty() ? "/" + field : p);
        return out;
    }
    r->activeElement = base.activeElement;
    const doc::Element& ne = r->active();
    if (weld.contains("edges"))
        for (size_t i = 0; i < weld["edges"].size(); ++i)
            if (!edgeRefValid(ne, weld["edges"][i].get<std::string>()))
                return Error("API_INVALID_DEFINITION").with("name", std::string("edges")).with("index", double(i))
                    .with("detail", std::string("edge reference")).at("/edges/" + std::to_string(i));
    if (weld.contains("faces")) {  // FX8-WLD-001: face references of two parts at least
        std::set<std::string> paths;
        for (size_t i = 0; i < weld["faces"].size(); ++i) {
            const auto p = faceRefPath(ne, weld["faces"][i].get<std::string>());
            if (!p)
                return Error("API_INVALID_DEFINITION").with("name", std::string("faces")).with("index", double(i))
                    .with("detail", std::string("face reference")).at("/faces/" + std::to_string(i));
            paths.insert(*p);
        }
        if (paths.size() < 2) return weldDefError("faces", "parts");
    }
    const auto v = drawing::weldValues(*r, weld);
    if (v.error) return weldDefError(*v.error->stringParam("name"), "value");
    return r;
}

Result<std::string> Session::addWeld(const Json& def, const std::optional<std::string>& name) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    if (!def.is_object()) return invalidDefinition("definition must be an object");
    if (def.contains("id") || def.contains("name")) return invalidDefinition("id and name not allowed");
    std::string n;
    if (name) {
        n = trim(*name);
        if (n.empty()) return Error("API_INVALID_NAME").with("name", *name);
    }
    doc::Document probe = doc();
    const std::string id = doc::allocate(probe, doc::IdKind::Weld);
    if (n.empty()) {  // "Шов N" ("Weld N"): the smallest free N among the welds of this assembly (weld.md §2.1)
        std::set<std::string> taken;
        for (const auto& w : doc().active().welds) taken.insert(w.value("name", std::string()));
        n = doc::firstFreeName(doc::nameBase("weld"), taken);
    }
    Json w = Json::object();
    w["id"] = id;
    w["name"] = n;
    for (auto it = def.begin(); it != def.end(); ++it) w[it.key()] = it.value();
    auto r = withWeld(probe, w);
    if (!r) return r.error();
    if (auto s = commit(std::move(*r), "undo.addWeld", {}, id); !s) return s.error();
    return id;
}

Status Session::updateWeld(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    const int wi = doc().active().weldIndex(id);
    if (wi < 0) return notFound(id);
    if (!def.is_object()) return invalidDefinition("definition must be an object");
    if (def.contains("id") || def.contains("name")) return invalidDefinition("id and name not allowed");
    // Merge (core-api §4.1в): fields replace, null removes, false of a "true only" flag removes.
    Json w = doc().active().welds[size_t(wi)];
    for (auto it = def.begin(); it != def.end(); ++it) {
        if (it->is_null() || (it->is_boolean() && !it->get<bool>())) w.erase(it.key());
        else w[it.key()] = it.value();
    }
    if (w == doc().active().welds[size_t(wi)]) return okStatus();
    auto r = withWeld(doc(), w);
    if (!r) return r.error();
    return commit(std::move(*r), "undo.editWeld", {}, id);
}

Status Session::deleteWeld(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (doc().active().weldIndex(id) < 0) return notFound(id);
    doc::Document nd = doc();
    auto& ws = nd.active().welds;
    ws.erase(ws.begin() + nd.active().weldIndex(id));
    return commit(std::move(nd), "undo.deleteWeld", {}, id);
}

Status Session::renameWeld(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    const int wi = doc().active().weldIndex(id);
    if (wi < 0) return notFound(id);
    const std::string n = trim(name);
    if (n.empty()) return Error("API_INVALID_NAME").with("name", name);
    doc::Document nd = doc();
    nd.active().welds[size_t(wi)]["name"] = n;
    return commit(std::move(nd), "undo.renameWeld", {}, id);
}

// Weld status (weld.md §6): values, then edges in the virtual assembly state.
Result<std::vector<Session::WeldInfo>> Session::welds() const {
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    const auto v = assemblyVirtual();
    if (!v) return apiError("API_STATE_UNAVAILABLE");
    std::vector<WeldInfo> out;
    for (const auto& w : doc().active().welds) {
        WeldInfo info;
        info.id = w["id"].get<std::string>();
        info.name = w["name"].get<std::string>();
        const auto values = drawing::weldValues(doc(), w);
        info.label = pmi::weldLabelText(pmi::weldLabel(w, drawing::weldTexts(values)));
        if (values.error) info.error = values.error;
        else if (auto lost = drawing::weldLostEdge(v->names, w)) info.error = Error("WELD_EDGE_LOST").with("ref", *lost);
        if (!info.error) {
            const auto g = weldGeom(v, w);
            if (g->path.error) info.error = g->path.error;
            info.length = g->path.length();
            info.volume = g->body.volume;
        }
        out.push_back(std::move(info));
    }
    return out;
}

namespace {

// Color of a weld body on the render view of a drawing (regen.md §3а) — that of the 3D view (app-ux §8а COLOR_WELD_BODY).
constexpr const char* WELD_BODY_COLOR = "#C62020";

// Shown welds (weld.md §7) of assembly element el of document D in its evaluated state a: own, then nested on any depth.
std::vector<ShownWeld> collectShownWelds(const doc::Document& D, const doc::Element& el, const regen::AssemblyState& a,
                                                  const regen::LinkMap* links) {
    std::vector<ShownWeld> out;
    for (const auto& w : el.welds) out.push_back({w, &D, {}});
    // Nested assemblies (FX9-ASM-008): their welds as when opened, references prefixed by the instance path.
    for (const auto& e : a.instances) {
        if (e.kind != "assembly" || e.suppressed || e.error || e.hidden) continue;
        const doc::Document* ED = &D;
        if (!e.file.empty()) {
            if (!links) continue;
            auto it = links->find(e.file);
            if (it == links->end() || !it->second.doc) continue;
            ED = static_cast<const doc::Document*>(it->second.doc);
        }
        const int ei = ED->elementIndex(e.element);
        if (ei < 0) continue;
        for (const auto& w : ED->elements[size_t(ei)].welds) {
            Json x = w;
            for (const char* key : {"edges", "faces"})
                if (x.contains(key))
                    for (auto& r : x[key]) r = e.path + "/" + r.get<std::string>();
            out.push_back({std::move(x), ED, e.path});
        }
    }
    return out;
}

// Weld bodies of an assembly drawing view model (drawing.md §6.3, weld.md §7а, FX9-WLD-002).
void appendWeldModelBodies(const kernel::Kernel& k, const doc::Document& pd, const regen::PartResult& pr, const regen::LinkMap* links,
                           regen::DrawingModel& m) {
    if (!pr.assembly || !pr.final) return;
    for (const auto& sw : collectShownWelds(pd, pd.active(), *pr.assembly, links)) {
        const auto values = drawing::weldValues(*sw.doc, sw.weld);
        if (values.error) continue;
        const Json* type = pmi::weldTypeInfo(sw.weld.value("standard", std::string()), sw.weld.value("type", std::string()));
        if (!type) continue;
        const auto path = drawing::weldPath(k, *pr.final, sw.weld);
        if (path.error) continue;
        const auto body = drawing::weldBody(k, *pr.final, path, (*type)["joint"].get<std::string>(), values.leg);
        if (body.shape.null()) continue;
        m.bodies.push_back({body.shape, {}});
        m.colors.push_back(WELD_BODY_COLOR);
    }
}

}  // namespace

void Session::installWeldModelBodies() {
    static const bool once = [] {
        regen::setDrawingModelExtras(appendWeldModelBodies);
        return true;
    }();
    (void)once;
}

std::vector<ShownWeld> Session::shownWelds(const regen::AssemblyState& a) const {
    return collectShownWelds(doc(), doc().active(), a, &links_);
}

std::vector<std::pair<const doc::Element*, kernel::MassProperties>> Session::weldMasses(const regen::AssemblyState& a) const {
    std::vector<std::pair<const doc::Element*, kernel::MassProperties>> out;
    const auto v = assemblyVirtual();
    if (!v) return out;
    for (const auto& sw : shownWelds(a)) {
        if (drawing::weldValues(*sw.doc, sw.weld).error) continue;
        const auto g = weldGeom(v, sw.weld, sw.doc);
        if (g->path.error || g->body.shape.null()) continue;
        const auto& refs = sw.weld.contains("faces") ? sw.weld["faces"] : sw.weld["edges"];
        const doc::Element* host = nullptr;
        if (!refs.empty())  // the part instance of the first reference (assembly.md §3.1)
            if (const auto r = regen::parseAssemblyRef(a, refs[0].get<std::string>()); r.kind == regen::AssemblyRef::Kind::Part)
                host = elementOfEntry(*r.entry);
        out.emplace_back(host, k_.massProperties(g->body.shape));
    }
    return out;
}

// Weld lines and bodies for the view (weld.md §7): faces welds — the weld line; every weld — its body mesh, cut by the
// effective sections (FX9-WLD-001).
Result<std::vector<Session::WeldDisplay>> Session::weldDisplay(const kernel::TessParams& params, const std::vector<Json>& sections) const {
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    const auto v = assemblyVirtual();
    const auto a = assembly();
    if (!v || !a) return apiError("API_STATE_UNAVAILABLE");
    std::vector<WeldDisplay> out;
    // The exploded view (assembly.md §7д.5, FX10-ASM-004; user's order 2026-10-31): welds are not shown.
    if (a->exploded) return out;
    for (const auto& sw : shownWelds(*a)) {
        const Json& w = sw.weld;
        if (drawing::weldValues(*sw.doc, w).error) continue;
        WeldDisplay d;
        d.id = (sw.path.empty() ? std::string() : sw.path + "/") + w["id"].get<std::string>();
        const auto& refs = w.contains("faces") ? w["faces"] : w["edges"];
        if (!refs.empty()) d.host = regen::refTopInstance(refs[0].get<std::string>());
        const auto g = weldGeom(v, w, sw.doc);
        if (w.contains("faces"))
            for (const auto& p : g->path.pieces) d.polylines.push_back(p.points);
        if (!g->body.shape.null()) {
            // Sections (sections.md §7а): those excluding the host instance do not cut the weld.
            if (auto cut = sections.empty() ? std::nullopt : assemblyCutAway(*a, sections, d.host)) {
                if (auto r = k_.boolean(kernel::BoolKind::Subtract, g->body.shape, *cut)) {
                    if (k_.counts(r->shape).faces > 0)  // no faces — the weld is entirely cut away
                        if (auto t = k_.tessellate(r->shape, params)) d.body = weldMesh(*t, d.id, params.chordTol);
                    out.push_back(std::move(d));
                    continue;
                }
            }
            std::lock_guard<std::mutex> lock(g->mu);
            auto it = g->meshes.find(params.chordTol);
            if (it == g->meshes.end()) {
                auto t = k_.tessellate(g->body.shape, params);
                if (t) it = g->meshes.emplace(params.chordTol, weldMesh(*t, d.id, params.chordTol)).first;
            }
            if (it != g->meshes.end()) d.body = it->second;
        }
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace construe::api
