// Session: lost references and their repair (spec/core-api.md §4.4 rebindReferences, §5 lostReferences; regen.md §8а;
// S7F-FEAT-003).
#include "api/session.h"

#include "api/errors.h"
#include "api/feature_commands.h"
#include "regen/naming.h"

#include <algorithm>
#include <set>

namespace construe::api {

namespace {

// JSON Pointer token escaping (RFC 6901).
std::string pointerToken(const std::string& key) {
    std::string out;
    for (char c : key) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out += c;
    }
    return out;
}

// Pointers of all string values equal to value: object keys in ascending byte order, array elements by index.
void collectPaths(const Json& j, const std::string& at, const std::string& value, std::vector<std::string>& out) {
    if (j.is_string()) {
        if (j.get_ref<const std::string&>() == value) out.push_back(at);
    } else if (j.is_object()) {
        std::vector<std::string> keys;
        for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) collectPaths(j.at(k), at + "/" + pointerToken(k), value, out);
    } else if (j.is_array()) {
        for (size_t i = 0; i < j.size(); ++i) collectPaths(j[i], at + "/" + std::to_string(i), value, out);
    }
}

// Kind of a reference by its name (naming.md §2.3); a context name — by its leaf.
std::optional<naming::EntityKind> referenceKind(const std::string& ref) {
    const auto p = naming::parse(naming::splitPath(ref).leaf);
    if (!p) return std::nullopt;
    return p->kind;
}

const char* kindName(std::optional<naming::EntityKind> k) {
    if (!k) return "unknown";
    switch (*k) {
        case naming::EntityKind::Face: return "face";
        case naming::EntityKind::Edge: return "edge";
        case naming::EntityKind::Vertex: return "vertex";
    }
    return "unknown";
}

// Ghost of ref in state st (regen.md §8а): the meshes of the resolved entities, merged; nullopt — not resolved.
std::optional<io::BodyMesh> ghostMesh(const kernel::Kernel& k, const regen::PartState& st, const std::string& ref,
                                      naming::EntityKind kind, const kernel::TessParams& params,
                                      std::map<int, io::BodyMesh>& meshes) {
    // Assembly names "<path>/<name>" resolve in the virtual assembly state (naming.md §5.3а); in-context names of a part
    // (assembly.md §7в) do not resolve in its own names — no ghost.
    const auto rr = naming::resolve(ref, kind, st.names);
    if (rr.status == naming::ResolveStatus::Lost || rr.entities.empty()) return std::nullopt;
    io::BodyMesh g;
    for (const auto& e : rr.entities) {
        int body = 0;
        std::string name;
        std::set<std::string> edgeNames;
        if (e.kind == naming::EntityKind::Face) {
            const auto& fe = st.names.faces.at(size_t(e.index));
            body = fe.body;
            name = fe.full;
            for (const auto& ee : st.names.edges)
                if (ee.f1 == e.index || ee.f2 == e.index) edgeNames.insert(ee.full);
        } else if (e.kind == naming::EntityKind::Edge) {
            const auto& ee = st.names.edges.at(size_t(e.index));
            body = ee.body;
            edgeNames.insert(ee.full);
        } else {
            const auto& ve = st.names.vertices.at(size_t(e.index));
            body = ve.body;
            name = ve.full;
        }
        auto it = meshes.find(body);
        if (it == meshes.end()) {
            auto m = bodyMesh(k, st, body, params);
            if (!m) return std::nullopt;
            it = meshes.emplace(body, std::move(*m)).first;
        }
        const io::BodyMesh& m = it->second;
        if (g.bodyId.empty()) g.bodyId = m.bodyId;
        g.chordTol = m.chordTol;
        if (e.kind == naming::EntityKind::Face)
            for (const auto& fr : m.faceRanges) {
                if (fr.name != name) continue;
                io::FaceRange out{fr.name, uint32_t(g.triangles.size() / 3), fr.count};
                std::map<uint32_t, uint32_t> remap;
                for (uint32_t t = fr.firstTriangle * 3; t < (fr.firstTriangle + fr.count) * 3; ++t) {
                    const uint32_t v = m.triangles[t];
                    auto [r, inserted] = remap.emplace(v, uint32_t(g.positions.size() / 3));
                    if (inserted)
                        for (int c = 0; c < 3; ++c) {
                            g.positions.push_back(m.positions[size_t(v) * 3 + size_t(c)]);
                            g.normals.push_back(m.normals[size_t(v) * 3 + size_t(c)]);
                        }
                    g.triangles.push_back(r->second);
                }
                g.faceRanges.push_back(out);
            }
        for (const auto& ep : m.edges)
            if (edgeNames.count(ep.name)) g.edges.push_back(ep);
        if (e.kind == naming::EntityKind::Vertex)
            for (const auto& vp : m.vertices)
                if (vp.name == name) g.vertices.push_back(vp);
    }
    return g;
}

// Weld references that do not resolve in the virtual assembly names (regen.md §8а, weld.md §6): edges, then faces.
std::vector<std::string> weldLostRefs(const naming::NameTable& names, const Json& weld) {
    std::vector<std::string> out;
    for (const auto& [key, kind] : {std::pair{"edges", naming::EntityKind::Edge}, std::pair{"faces", naming::EntityKind::Face}}) {
        if (!weld.contains(key) || !weld[key].is_array()) continue;
        for (const auto& r : weld[key]) {
            if (!r.is_string()) continue;
            const auto rr = naming::resolve(r.get<std::string>(), kind, names);
            if (rr.status == naming::ResolveStatus::Lost || rr.entities.empty())
                if (std::find(out.begin(), out.end(), r.get<std::string>()) == out.end()) out.push_back(r.get<std::string>());
        }
    }
    return out;
}

// The reference names an entity of a suppressed instance (or of one nested in it): not a broken reference — the
// instance is off (regen.md §8а «Assembly objects»).
bool inSuppressedInstance(const regen::AssemblyState& a, const std::string& ref) {
    const std::string path = naming::splitPath(ref).path;
    for (size_t end = path.find('/'); ; end = path.find('/', end + 1)) {
        const auto* e = a.find(path.substr(0, end));
        if (e && e->suppressed) return true;
        if (end == std::string::npos) return false;
    }
}

const std::string* mateLostRef(const regen::MateStatus& m) {
    return m.error && m.error->code == "ASM_MATE_REF_LOST" ? m.error->stringParam("ref") : nullptr;
}

}  // namespace

std::vector<std::string> nameBaseKeys() {
    std::vector<std::string> keys;
    for (const auto& kv : doc::builtinNameBases()) keys.push_back(kv.first);
    return keys;
}

void setNameBases(std::map<std::string, std::string> table) { doc::setNameBases(std::move(table)); }

std::vector<std::string> lostReferenceIds(const regen::FeatureResult& fr) {
    std::vector<std::string> ids;
    for (const auto& m : fr.messages) {
        if (!m.error || m.e.code != "REGEN_REF_LOST") continue;
        auto it = m.e.params.find("ids");
        if (it == m.e.params.end()) continue;
        if (const auto* v = std::get_if<std::vector<std::string>>(&it->second)) {
            for (const auto& id : *v)
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        } else if (const auto* one = std::get_if<std::string>(&it->second)) {
            if (std::find(ids.begin(), ids.end(), *one) == ids.end()) ids.push_back(*one);
        }
    }
    return ids;
}

std::map<std::string, regen::StatePtr> referenceStates(const doc::Document& d, const regen::PartResult& r) {
    std::map<std::string, regen::StatePtr> out;
    regen::StatePtr context = r.initial;
    for (const auto& f : d.active().features) {
        const auto st = r.featureStatus.find(f->id);
        const bool evaluated = st != r.featureStatus.end() && st->second.status != regen::FeatureStatus::Suppressed &&
                               st->second.status != regen::FeatureStatus::RolledBack;
        if (evaluated && context && lostReferenceIds(st->second).empty()) out[f->id] = context;
        if (const auto a = r.after.find(f->id); a != r.after.end() && a->second) context = a->second;
    }
    // Assembly objects (regen.md §8а): mates and welds — the virtual assembly state.
    if (r.assembly && r.final && d.active().isAssembly()) {
        for (const auto& m : r.assembly->mates)
            if (!mateLostRef(m)) out[m.id] = r.final;
        for (const auto& w : d.active().welds)
            if (weldLostRefs(r.final->names, w).empty()) out[w.value("id", std::string())] = r.final;
    }
    return out;
}

Result<std::vector<LostReference>> Session::lostReferences(const kernel::TessParams& params) const {
    if (!(params.chordTol >= kernel::KERNEL_MIN_CHORD && params.chordTol <= kernel::KERNEL_MAX_CHORD))
        return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("chordTol")).with("actual", params.chordTol);
    PartResultPtr pr;
    std::map<std::string, regen::StatePtr> ghosts;
    {
        std::lock_guard lk(mu_);
        pr = published_;
        ghosts = ghostStates_;
    }
    std::vector<LostReference> out;
    if (!pr) return out;
    for (const auto& f : doc().active().features) {
        const auto st = pr->featureStatus.find(f->id);
        if (st == pr->featureStatus.end()) continue;
        const auto ids = lostReferenceIds(st->second);
        if (ids.empty()) continue;
        Json def = doc::featureToJsonValue(*f);
        for (const char* key : {"id", "name", "suppressed"}) def.erase(key);
        const auto g = ghosts.find(f->id);
        std::map<int, io::BodyMesh> meshes;  // bodies of this feature's reference state
        for (const auto& id : ids) {
            LostReference lr;
            lr.feature = f->id;
            lr.ref = id;
            collectPaths(def, "", id, lr.paths);
            const auto kind = referenceKind(id);
            lr.kind = kindName(kind);
            if (kind && g != ghosts.end() && g->second) lr.ghost = ghostMesh(k_, *g->second, id, *kind, params, meshes);
            out.push_back(std::move(lr));
        }
    }
    // Mates and welds of the active assembly (regen.md §8а «Assembly objects»), document order.
    const doc::Element& el = doc().active();
    if (!el.isAssembly() || !pr->final) return out;
    auto add = [&](const std::string& owner, const std::string& id, const std::string& ref, const Json& record) {
        LostReference lr;
        lr.owner = owner;
        lr.feature = id;
        lr.ref = ref;
        collectPaths(record, "", ref, lr.paths);
        const auto kind = referenceKind(ref);
        lr.kind = kindName(kind);
        const auto g = ghosts.find(id);
        std::map<int, io::BodyMesh> meshes;
        if (kind && g != ghosts.end() && g->second) lr.ghost = ghostMesh(k_, *g->second, ref, *kind, params, meshes);
        out.push_back(std::move(lr));
    };
    if (pr->assembly)
        for (const auto& m : el.mates) {
            const auto* st = pr->assembly->mate(m.id);
            const std::string* ref = st ? mateLostRef(*st) : nullptr;
            if (!ref || inSuppressedInstance(*pr->assembly, *ref)) continue;
            Json rec = doc::mateToJsonValue(m);
            for (const char* key : {"id", "name", "suppressed"}) rec.erase(key);
            add("mate", m.id, *ref, rec);
        }
    for (const auto& w : el.welds) {
        Json rec = w;
        for (const char* key : {"id", "name"}) rec.erase(key);
        for (const auto& ref : weldLostRefs(pr->final->names, w))
            if (!pr->assembly || !inSuppressedInstance(*pr->assembly, ref)) add("weld", w.value("id", std::string()), ref, rec);
    }
    return out;
}

// Replacements in a JSON record (core-api §4.4 rebindReferences): the pointers must name string values; not /type.
static Result<Json> replacedRecord(Json rec, const std::vector<Rebind>& replacements) {
    std::set<std::string> seen;
    for (const auto& r : replacements) {
        if (r.path.empty() || r.path == "/type" || !seen.insert(r.path).second) return badArg("path");
        if (r.ref.empty()) return badArg("ref");
        Json::json_pointer ptr;
        try {
            ptr = Json::json_pointer(r.path);
        } catch (const std::exception&) {
            return badArg("path");
        }
        if (!rec.contains(ptr) || !rec[ptr].is_string()) return badArg("path");
        rec[ptr] = r.ref;
    }
    return rec;
}

Status Session::rebindAssemblyObject(const std::string& id, const std::vector<Rebind>& replacements) {
    const doc::Element& el = doc().active();
    const doc::Mate* mate = nullptr;
    for (const auto& m : el.mates)
        if (m.id == id) mate = &m;
    const int wi = el.isAssembly() ? el.weldIndex(id) : -1;
    if (!mate && wi < 0) return notFound(id);
    if (replacements.empty()) return okStatus();
    Json rec = mate ? doc::mateToJsonValue(*mate) : el.welds[size_t(wi)];
    for (const char* key : {"id", "name", "suppressed"}) rec.erase(key);
    auto next = replacedRecord(rec, replacements);
    if (!next) return next.error();
    Json changed = Json::object();  // only the changed top-level fields, as updateMate / updateWeld merge them
    for (auto it = next->begin(); it != next->end(); ++it)
        if (!rec.contains(it.key()) || rec[it.key()] != it.value()) changed[it.key()] = it.value();
    beginGroup("undo.rebindReferences");
    const Status s = mate ? updateMate(id, changed) : updateWeld(id, changed);
    if (auto e = endGroup(); s && !e) return e;
    return s;
}

size_t Session::undoDepth() const { return undo_->index(); }

Status Session::rebindReferences(const std::string& id, const std::vector<Rebind>& replacements) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document d = doc();
    const int pos = d.active().indexOf(id);
    if (pos < 0) return rebindAssemblyObject(id, replacements);
    if (auto s = checkOwnedFeature(id, false); !s) return s;  // solutions.md §7.10 item 2
    if (replacements.empty()) return okStatus();
    const doc::Feature& old = *d.active().features[size_t(pos)];
    Json def = doc::featureToJsonValue(old);
    for (const char* key : {"id", "name", "suppressed"}) def.erase(key);
    auto replaced = replacedRecord(std::move(def), replacements);
    if (!replaced) return replaced.error();
    def = std::move(*replaced);
    auto fj = featureJsonFromDefinition(def, id, old.name, old.suppressed);
    if (!fj) return fj.error();
    if (auto s = checkDefinitionRefs(d, def, id, pos); !s) return s;
    auto r = applyFeatureJson(d, id, *fj, pos, true);
    if (!r) return r.error();
    std::vector<std::string> touched;
    if (old.type == doc::FeatureType::Sketch) touched.push_back(id);
    return commit(std::move(*r), "undo.rebindReferences", touched, id);
}

}  // namespace construe::api

namespace construe::api {

Result<std::vector<std::string>> Session::deleteLostDimensions(const std::string& sheetId) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isDrawing()) return Error("API_NOT_DRAWING").with("id", doc().active().id);
    const auto& sheets = doc().active().sheets;
    const auto sh = std::find_if(sheets.begin(), sheets.end(), [&](const doc::Sheet& x) { return x.id == sheetId; });
    if (sh == sheets.end()) return notFound(sheetId);
    auto g = sheetGraphics(sheetId);
    if (!g) return g.error();
    std::set<std::string> lost;
    for (const auto& e : g->errors)
        if (e.error.code == "DRAWING_DIM_REF_LOST") lost.insert(e.view);
    std::vector<std::string> out;
    for (const auto& dim : sh->dimensions)
        if (lost.count(dim.id)) out.push_back(dim.id);
    if (out.empty()) return out;
    doc::Document nd = doc();
    for (auto& s : nd.active().sheets)
        if (s.id == sheetId)
            s.dimensions.erase(std::remove_if(s.dimensions.begin(), s.dimensions.end(),
                                              [&](const doc::SheetDimension& x) { return lost.count(x.id) > 0; }),
                               s.dimensions.end());
    if (auto s = commit(std::move(nd), "undo.deleteLostDimensions", {}, sheetId); !s) return s.error();
    return out;
}

}  // namespace construe::api
