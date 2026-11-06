// Session: assemblies - instances, mates, analysis (spec/core-api.md §4.1в, spec/assembly.md).
#include "api/session.h"
#include "regen/hole_tables.h"
#include <Eigen/Eigenvalues>

#include "sketch/trim.h"

#include "api/errors.h"
#include "api/feature_commands.h"
#include "api/sections.h"
#include "regen/instance_patterns.h"
#include "regen/mates.h"
#include "font/font.h"
#include "api/sketch_commands.h"

#include "base/ids.h"
#include "base/files.h"
#include "base/strings.h"
#include "base/number_format.h"
#include "doc/schema.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <tuple>
#include <regex>
#include <set>
#include <sstream>

namespace construe::api {

// ---------------------------------------------------------------- §4.1в assemblies (assembly.md)

namespace {

// Element asmId contains (via subassemblies) element target - inserting target into asmId would create a cycle.
bool containsElement(const doc::Document& d, const std::string& asmId, const std::string& target, int depth = 0) {
    if (asmId == target) return true;
    if (depth > 64) return true;
    const int i = d.elementIndex(asmId);
    if (i < 0) return false;
    for (const auto& in : d.elements[size_t(i)].instances)
        if (containsElement(d, in.refElement, target, depth + 1)) return true;
    return false;
}

doc::Instance* findInstance(doc::Element& el, const std::string& id) {
    for (auto& in : el.instances)
        if (in.id == id) return &in;
    return nullptr;
}

}  // namespace

Result<Session::ResolvedRef> Session::resolveRef(const Json& ref, bool checkCycle) const {
    if (!ref.is_object() || !ref.contains("element") || !ref["element"].is_string()) return badArg("ref");
    ResolvedRef out;
    out.element = ref["element"];
    const std::string& target = out.element;
    if (ref.contains("file")) {  // object of another file (assembly.md §5а)
        if (!ref["file"].is_string()) return badArg("ref.file");
        if (!path_) return apiError("API_NO_PATH");
        const std::string dir = regen::parentDir(selfPath());
        const std::string abs = regen::linkPath(dir, ref["file"].get<std::string>());
        std::ifstream f(std::filesystem::u8path(abs), std::ios::binary);
        if (!f) return Error("IO_READ").with("detail", std::string("cannot open")).with("file", abs);
        std::stringstream ss;
        ss << f.rdbuf();
        auto pd = doc::parse(ss.str());
        if (!pd) return pd.error();
        const int ti = pd->elementIndex(target);
        if (ti < 0) return notFound(target);
        out.name = pd->elements[size_t(ti)].name;
        out.kind = pd->elements[size_t(ti)].kind;
        out.file = relativeU8(abs, dir);
        if (!checkCycle) return out;
        // Cycle through files: the inserted object (recursively) references this assembly of this file.
        std::function<bool(const doc::Document&, const std::string&, const std::string&, int)> reaches =
            [&](const doc::Document& D, const std::string& file, const std::string& el, int depth) -> bool {
            if (depth > 64) return true;
            if (file == selfPath() && el == doc().active().id) return true;
            const int i = D.elementIndex(el);
            if (i < 0) return false;
            for (const auto& x : D.elements[size_t(i)].instances) {
                if (x.refFile.empty()) {
                    if (reaches(D, file, x.refElement, depth + 1)) return true;
                    continue;
                }
                const std::string a2 = regen::linkPath(regen::parentDir(file), x.refFile);
                if (a2 == selfPath()) {
                    if (reaches(doc(), a2, x.refElement, depth + 1)) return true;
                    continue;
                }
                std::ifstream f2(std::filesystem::u8path(a2), std::ios::binary);
                if (!f2) continue;
                std::stringstream s2;
                s2 << f2.rdbuf();
                if (auto d2 = doc::parse(s2.str()); d2 && reaches(*d2, a2, x.refElement, depth + 1)) return true;
            }
            return false;
        };
        if (reaches(*pd, abs, target, 0)) return Error("API_ASSEMBLY_CYCLE").with("id", target);
    } else {
        const int ti = doc().elementIndex(target);
        if (ti < 0) return notFound(target);
        if (checkCycle && containsElement(doc(), target, doc().active().id)) return Error("API_ASSEMBLY_CYCLE").with("id", target);
        out.name = doc().elements[size_t(ti)].name;
        out.kind = doc().elements[size_t(ti)].kind;
    }
    return out;
}

Result<std::string> Session::insertInstance(const Json& ref, const std::optional<std::array<double, 3>>& position) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    auto rr = resolveRef(ref, true);
    if (!rr) return rr.error();
    // Only parts (with imported bodies) and assemblies are components (core-api §4.1в, FX13-ASM-001): not a drawing or a
    // specification.
    if (rr->kind != "part" && rr->kind != "body" && rr->kind != "assembly") return Error("API_NOT_COMPONENT").with("id", rr->element);
    doc::Document nd = doc();
    doc::Instance in;
    in.id = doc::allocate(nd, doc::IdKind::Instance);
    in.refFile = rr->file;
    in.refElement = rr->element;
    placeNewInstance(nd.active(), in, rr->name, position);
    const std::string id = in.id;
    nd.active().instances.push_back(in);
    if (auto s = commit(std::move(nd), "undo.insertInstance", {}, id); !s) return s.error();
    return id;
}

void Session::placeNewInstance(const doc::Element& el, doc::Instance& in, const std::string& base,
                               const std::optional<std::array<double, 3>>& position) const {
    std::set<std::string> taken;
    for (const auto& x : el.instances) taken.insert(x.name);
    for (int n = 1;; ++n)
        if (!taken.count(base + " <" + std::to_string(n) + ">")) {
            in.name = base + " <" + std::to_string(n) + ">";
            break;
        }
    in.fixed = el.instances.empty();
    if (position) {
        in.position = *position;
    } else if (auto a = assembly(); a && !a->instances.empty()) {
        // To the right of the assembly bounding box by a fraction of the diagonal (assembly.md §8).
        BBox3 all;
        all.setEmpty();
        for (const auto& e : a->instances)
            if (e.state)
                for (const auto& b : e.state->bodies) {
                    const BBox3 bb = k_.bbox(b.shape);
                    if (bb.isEmpty()) continue;
                    for (int c = 0; c < 8; ++c) all.extend(Point3(e.world.apply(bb.corner(BBox3::CornerType(c)))));
                }
        if (!all.isEmpty()) in.position = {all.max().x() + INSERT_GAP_FRACTION * all.diagonal().norm(), all.min().y(), all.min().z()};
    }
}

namespace {
// Mates of the exploded view's mode for the previews (assembly.md §7д.4, FX10-ASM-003): the automatic mode's suppressed
// set S is written into the document only by the next command (syncAssemblyPositions), so until then the published
// state's S is taken; without it the first drag after switching the view on solved the face coincidences too and pulled
// the part back to its assembled place.
// Library fasteners (§7д.5, FX10-ASM-004) take no part either: a follower moves with its attachment, a dragged one moves
// freely.
doc::Element explodedPreviewElement(const doc::Element& el, const regen::AssemblyState& a) {
    if (!el.explodedActive()) return el;
    doc::Element x = el;
    if (a.exploded && (a.explodedAuto || a.explodedNew)) x.exploded->suppressedMates = a.explodedSuppressed;
    x = regen::explodedMateElement(x);
    for (auto& m : x.mates)
        for (const auto& r : m.refs)
            if (const auto* e = a.find(regen::refTopInstance(r)); e && regen::isLibraryFastener(*e)) m.suppressed = true;
    return x;
}

// Followers in the preview placements (§7д.5): left out — they are no manual moves; the view moves them with their
// attachments (app-ux §8а); the dragged instance itself stays.
void dropFollowers(std::map<std::string, Session::Placement>& out, const regen::AssemblyState& a, const std::string& dragged) {
    for (const auto& fa : a.explodedFollow)
        if (fa.first != dragged) out.erase(fa.first);
}

// Placement of top-level instance id in the exploded view (assembly.md §7д.4): replaced or added in instance order.
void setExplodedPlacement(doc::Element& a, const std::string& id, const std::array<double, 3>& position, const std::array<double, 4>& rotation) {
    if (!a.exploded) a.exploded.emplace();
    if (!a.exploded->placements) a.exploded->placements.emplace();
    auto& ps = *a.exploded->placements;
    for (auto& p : ps)
        if (p.instance == id) {
            p.position = position, p.rotation = rotation;
            return;
        }
    ps.push_back({id, position, rotation});
    std::map<std::string, size_t> order;
    for (size_t i = 0; i < a.instances.size(); ++i) order[a.instances[i].id] = i;
    std::stable_sort(ps.begin(), ps.end(), [&](const doc::ExplodedPlacement& x, const doc::ExplodedPlacement& y) {
        const auto ix = order.find(x.instance), iy = order.find(y.instance);
        return (ix == order.end() ? order.size() : ix->second) < (iy == order.end() ? order.size() : iy->second);
    });
}
}  // namespace

Status Session::moveInstance(const std::string& id, const std::array<double, 3>& position, const std::array<double, 4>& rotation) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    doc::Document nd = doc();
    if (nd.active().explodedActive())  // a derived connection fastener: its manual exploded placement (§7д.5)
        if (const auto a = assembly(); a && a->find(id) && !a->find(id)->connection.empty())
            return moveInstances({{id, Placement{position, rotation}}});
    doc::Instance* in = findInstance(nd.active(), id);
    if (!in) return notFound(id);
    if (in->fixed) return Error("API_INSTANCE_FIXED").with("id", id);
    const double n = std::sqrt(rotation[0] * rotation[0] + rotation[1] * rotation[1] + rotation[2] * rotation[2] + rotation[3] * rotation[3]);
    if (!(n > 0) || !std::isfinite(n)) return badArg("rotation");
    std::array<double, 4> q;
    for (int k = 0; k < 4; ++k) q[size_t(k)] = rotation[size_t(k)] / n;
    if (nd.active().explodedActive()) {  // the exploded view's own placement (assembly.md §7д.4)
        setExplodedPlacement(nd.active(), id, position, q);
    } else {
        in->position = position;
        in->rotation = q;
    }
    return commit(std::move(nd), "undo.moveInstance", {}, id);
}

Status Session::setInstanceFlags(const std::string& id, const Json& flags) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    if (!flags.is_object()) return badArg("flags");
    doc::Document nd = doc();
    doc::Instance* in = findInstance(nd.active(), id);
    if (!in) return notFound(id);
    for (auto it = flags.begin(); it != flags.end(); ++it) {
        if (!it.value().is_boolean()) return badArg(it.key());
        const bool v = it.value().get<bool>();
        if (it.key() == "fixed") in->fixed = v;
        else if (it.key() == "suppressed") in->suppressed = v;
        else if (it.key() == "hidden") in->hidden = v;
        else if (it.key() == "transparent") in->transparent = v;
        else return badArg(it.key());
    }
    return commit(std::move(nd), "undo.instanceProperties", {}, id);
}

Status Session::setInstanceColor(const std::string& id, const std::optional<std::string>& color) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    std::optional<std::string> c;
    if (color) {
        std::string up = *color;
        for (auto& ch : up) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        const bool ok = up.size() == 7 && up[0] == '#' &&
                        std::all_of(up.begin() + 1, up.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; });
        if (!ok) return badArg("color");
        c = up;
    }
    syncAssemblyPositions();
    doc::Document nd = doc();
    doc::Instance* in = findInstance(nd.active(), id);
    if (!in) return notFound(id);
    if (in->color == c) return okStatus();
    in->color = c;
    return commit(std::move(nd), "undo.instanceColor", {}, id);
}

Status Session::renameInstance(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    const std::string n = trim(name);
    doc::Document nd = doc();
    doc::Instance* in = findInstance(nd.active(), id);
    if (!in) return notFound(id);
    if (n.empty()) return Error("API_INVALID_NAME").with("name", name);
    in->name = n;
    return commit(std::move(nd), "undo.renameInstance", {}, id);
}

namespace {

// Cuts of library element `instance` of assembly `assembly` (library.md §6.6 item 8, FX10-LIB-009) removed from the part
// elements of document d; a rollback marker on a removed feature moves to the previous remaining one.
void eraseLibraryCuts(doc::Document& d, const std::string& assembly, const std::string& instance) {
    for (auto& el : d.elements) {
        if (el.isAssembly()) continue;
        std::vector<doc::FeaturePtr> kept;
        for (const auto& f : el.features) {
            const doc::Json* o = f->type == doc::FeatureType::ConnectionCut && f->pattern() ? &f->pattern()->def["owner"] : nullptr;
            if (!o || !o->contains("library") || (*o)["assembly"].get<std::string>() != assembly || (*o)["connection"].get<std::string>() != instance) {
                kept.push_back(f);
                continue;
            }
            if (el.rollback.kind == doc::Rollback::Kind::After && el.rollback.feature == f->id)
                el.rollback = kept.empty() ? doc::Rollback{doc::Rollback::Kind::Start, {}} : doc::Rollback{doc::Rollback::Kind::After, kept.back()->id};
        }
        el.features = std::move(kept);
    }
}

}  // namespace

Status Session::deleteInstance(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    const auto& insts = doc().active().instances;
    if (std::none_of(insts.begin(), insts.end(), [&](const doc::Instance& x) { return x.id == id; })) return notFound(id);
    // Paths that go with the instance: its own and those of its patterns' children (assembly.md §7е.1).
    std::vector<std::string> roots{id};
    for (const auto& p : doc().active().patterns)
        if (p.source() == id) roots.push_back(p.id);
    auto refersTo = [&](const std::string& ref) {
        for (const auto& r : roots)
            if (ref.size() > r.size() && ref.compare(0, r.size(), r) == 0 && ref[r.size()] == '/') return true;
        return false;
    };
    // Its connections (solutions.md §7.8) go with their fasteners and owned cuts — through deleteFeature, in the same
    // undo step; assembly cuts and holes stay with a lost reference (assembly.md §7г.5).
    std::vector<std::string> connections;
    for (const auto& f : doc().active().features) {
        const auto* g = f->type == doc::FeatureType::Connection ? f->pattern() : nullptr;  // a connection keeps its record as is
        if (!g || !g->def.contains("inputs") || !g->def["inputs"].is_object()) continue;
        bool refers = false;
        for (auto it = g->def["inputs"].begin(); it != g->def["inputs"].end() && !refers; ++it) {
            if (it.value().is_string()) refers = refersTo(it.value().get<std::string>());
            else if (it.value().is_array())  // a list of axes (solutions.md §7.11)
                for (const auto& a : it.value()) refers = refers || (a.is_string() && refersTo(a.get<std::string>()));
        }
        if (refers) connections.push_back(f->id);
    }
    beginGroup("undo.deleteInstance");
    for (const auto& c : connections)
        if (auto s = deleteFeature(c); !s) {
            endGroup();
            return s;
        }
    doc::Document nd = doc();
    auto& el = nd.active();
    std::erase_if(el.instances, [&](const doc::Instance& x) { return x.id == id; });
    eraseLibraryCuts(nd, nd.active().id, id);  // the cuts of a library element (library.md §6.6 item 8)
    // Its patterns and its placement in the exploded view go with it (assembly.md §7д.4, §7е.4).
    std::erase_if(el.patterns, [&](const doc::InstancePattern& p) { return p.source() == id; });
    if (auto& x = el.exploded; x && x->placements)
        std::erase_if(*x->placements, [&](const doc::ExplodedPlacement& p) { return p.instance == id; });
    // Its mates — any reference to its geometry, wherever the tree shows them — and their exploded-view records.
    std::set<std::string> mates;
    for (const auto& m : el.mates)
        if (refersTo(m.refs[0]) || refersTo(m.refs[1])) mates.insert(m.id);
    std::erase_if(el.mates, [&](const doc::Mate& m) { return mates.count(m.id) > 0; });
    if (auto& x = el.exploded) {
        std::erase_if(x->suppressedMates, [&](const std::string& m) { return mates.count(m) > 0; });
        if (x->knownMates) std::erase_if(*x->knownMates, [&](const std::string& m) { return mates.count(m) > 0; });
    }
    // Its weld seams (weld.md §2.1): a reference among the edges or faces.
    std::erase_if(el.welds, [&](const Json& w) {
        for (const char* key : {"edges", "faces"})
            if (w.contains(key) && w[key].is_array())
                for (const auto& r : w[key])
                    if (r.is_string() && refersTo(r.get<std::string>())) return true;
        return false;
    });
    auto s = commit(std::move(nd), "undo.deleteInstance", {}, id);
    endGroup();
    return s;
}

namespace {

// Top-level instance the tree shows a mate under (app-ux §8а, FX1-ASM-003): the last in `order` among the instances of
// its references; empty — none (source geometry only).
std::string mateOwner(const doc::Mate& m, const std::vector<std::string>& order) {
    int best = -1;
    for (const auto& r : m.refs) {
        const std::string top = regen::refTopInstance(r);  // assembly.md §3.1
        if (top.empty()) continue;
        const auto it = std::find(order.begin(), order.end(), top);
        if (it != order.end()) best = std::max(best, int(it - order.begin()));
    }
    return best >= 0 ? order[size_t(best)] : std::string();
}

}  // namespace

Result<std::vector<std::string>> Session::reorderedInstances(const std::string& id, const std::string& beforeId) const {
    const auto& el = doc().active();
    if (!el.isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", el.id);
    std::vector<std::string> order;
    for (const auto& in : el.instances) order.push_back(in.id);
    if (std::find(order.begin(), order.end(), id) == order.end()) return notFound(id);
    if (!beforeId.empty() && std::find(order.begin(), order.end(), beforeId) == order.end()) return notFound(beforeId);
    if (beforeId == id) return badArg("beforeId");
    std::vector<std::string> next;
    for (const auto& x : order) {
        if (x == beforeId) next.push_back(id);
        if (x != id) next.push_back(x);
    }
    if (beforeId.empty()) next.push_back(id);
    // Every mate stays under the same instance (FX9-ASM-011): a mated part does not go above its bases.
    for (const auto& m : el.mates)
        if (mateOwner(m, order) != mateOwner(m, next)) return Error("API_INSTANCE_ORDER").with("id", m.id).with("name", m.name);
    return next;
}

std::optional<std::string> Session::partConstructionName(const std::string& ref) {
    const auto pn = naming::splitPath(ref);
    if (pn.path.empty() || !regen::isConstructionName(pn.leaf)) return std::nullopt;
    return pn.leaf;
}

Status Session::reorderInstance(const std::string& id, const std::string& beforeId) {
    if (auto s = checkNoDrag(); !s) return s;
    auto next = reorderedInstances(id, beforeId);
    if (!next) return next.error();
    syncAssemblyPositions();
    doc::Document nd = doc();
    auto& ins = nd.active().instances;
    std::vector<doc::Instance> sorted;
    for (const auto& x : *next)
        for (const auto& in : ins)
            if (in.id == x) sorted.push_back(in);
    if (sorted == ins) return okStatus();  // the order is unchanged — no undo step
    ins = std::move(sorted);
    return commit(std::move(nd), "undo.reorderInstance", {}, id);
}

// ---------------------------------------------------------------- mates (assembly.md §7)

namespace {

Session::Placement placementOf(const regen::Motion3& m) {
    const Eigen::Quaterniond q(m.R);
    return {{m.t.x(), m.t.y(), m.t.z()}, {q.w(), q.x(), q.y(), q.z()}};
}

// The default name base of a mate type (assembly.md §7.1) in the interface language (document.md §2.12, FX14-UI-004).
std::string mateBaseName(const std::string& t) {
    const std::string base = doc::nameBase("mate." + t);
    return base.empty() ? doc::nameBase("mate.lock") : base;
}

}  // namespace

namespace {
bool samePlacement(const std::array<double, 3>& p1, const std::array<double, 4>& q1, const std::array<double, 3>& p2,
                   const std::array<double, 4>& q2) {
    double diff = 0, qd = 0;
    for (int k = 0; k < 3; ++k) diff = std::max(diff, std::abs(p1[size_t(k)] - p2[size_t(k)]));
    for (int k = 0; k < 4; ++k) qd += q1[size_t(k)] * q2[size_t(k)];  // q and −q are the same rotation
    return diff <= 1e-9 && std::abs(std::abs(qd) - 1) <= 1e-12;
}
regen::Motion3 inverseMotion(const regen::Motion3& m) { return {m.R.transpose(), -(m.R.transpose() * m.t)}; }
}  // namespace

std::string Session::mateUnitOf(const regen::AssemblyState& st, const std::string& path) { return regen::mateUnitOf(st, path); }

regen::AssemblyRef Session::parseAssemblyRef(const regen::AssemblyState& st, const std::string& ref) { return regen::parseAssemblyRef(st, ref); }

std::string Session::refTopInstance(const std::string& ref) { return regen::refTopInstance(ref); }

std::vector<doc::Instance::FormPlacement> Session::instanceForm(const regen::AssemblyState& st, const regen::InstanceEntry& node) {
    std::vector<doc::Instance::FormPlacement> out;
    if (!node.flex) return out;
    for (const auto& u : node.flex->flexUnits) {
        const std::string path = node.path + "/" + u;
        const auto* e = st.find(path);
        const auto* parent = st.find(path.substr(0, path.rfind('/')));
        if (!e || !parent) continue;
        const Placement p = placementOf(inverseMotion(parent->world) * e->world);  // in the frame of its own assembly
        out.push_back({u, p.position, p.rotation});
    }
    return out;
}

void Session::syncAssemblyPositions() {
    if (!doc().active().isAssembly()) return;
    PartResultPtr pr;
    {
        std::lock_guard lk(mu_);
        pr = published_;
    }
    if (!pr || !pr->assembly || pr->revision != revision_) return;
    doc::Document nd = doc();
    bool changed = false;
    if (nd.active().explodedActive()) {  // the exploded view: its placements (and the automatic mode, §7д.4)
        if (!pr->assembly->exploded) return;
        doc::Exploded x = *nd.active().exploded;
        std::vector<doc::ExplodedPlacement> ps;
        for (const auto& in : nd.active().instances) {
            const auto* e = pr->assembly->find(in.id);
            // Fasteners following their attachments have no record (§7д.5): a record is a manual move.
            if (!e || e->suppressed || e->error || pr->assembly->explodedFollow.count(in.id)) continue;
            const Placement p = placementOf(e->world);
            ps.push_back({in.id, p.position, p.rotation});
        }
        if (x.placements)  // manual moves of derived connection fasteners stay (§7д.5)
            for (const auto& p : *x.placements)
                if (p.instance.find('/') != std::string::npos && !pr->assembly->explodedFollow.count(p.instance)) ps.push_back(p);
        // Only a real change is written: placements equal within the solve's accuracy stay as they are.
        auto same = [](const doc::ExplodedPlacement& a, const doc::ExplodedPlacement& b) {
            double diff = 0, qd = 0;
            for (int k = 0; k < 3; ++k) diff = std::max(diff, std::abs(a.position[size_t(k)] - b.position[size_t(k)]));
            for (int k = 0; k < 4; ++k) qd += a.rotation[size_t(k)] * b.rotation[size_t(k)];
            return a.instance == b.instance && diff <= 1e-9 && std::abs(std::abs(qd) - 1) <= 1e-12;
        };
        const bool equal = x.placements && x.placements->size() == ps.size() &&
                           std::equal(ps.begin(), ps.end(), x.placements->begin(), same);
        if (!equal) x.placements = ps;
        if (pr->assembly->explodedAuto || pr->assembly->explodedNew) x.suppressedMates = pr->assembly->explodedSuppressed;
        // The mates the saved view has taken into account (§7д.1, FX12-ASM-002): those added later are new to it.
        x.knownMates.emplace();
        for (const auto& m : nd.active().mates) x.knownMates->push_back(m.id);
        if (!(x == *nd.active().exploded)) {
            nd.active().exploded = x;
            undo_->replaceCurrent(doc::Snapshot{nd, undo_->current().label, ctx_, undo_->current().labelName});
        }
        return;
    }
    for (auto& in : nd.active().instances) {
        const auto* e = pr->assembly->find(in.id);
        // Form of an underdetermined subassembly (§2б, FX9-ASM-010): its free units; a fully determined one keeps none.
        if (e && !e->error && !e->suppressed && e->kind == "assembly") {
            // Kept: entries the instance already has and units the solve moved inside the subassembly — a new
            // instance has no form until its shape changes.
            std::vector<doc::Instance::FormPlacement> form;
            for (auto& f : instanceForm(*pr->assembly, *e)) {
                const bool had = std::any_of(in.form.begin(), in.form.end(), [&](const auto& x) { return x.path == f.path; });
                const auto* ue = pr->assembly->find(in.id + "/" + f.path);
                if (had || (ue && ue->formChanged)) form.push_back(std::move(f));
            }
            const bool same = form.size() == in.form.size() &&
                              std::equal(form.begin(), form.end(), in.form.begin(), [](const auto& a, const auto& b) {
                                  return a.path == b.path && samePlacement(a.position, a.rotation, b.position, b.rotation);
                              });
            if (!same) {
                in.form = std::move(form);
                changed = true;
            }
        }
        if (!e || e->fixed || e->freeDof < 0) continue;  // not in the mate system - placement from the document
        const Placement p = placementOf(e->world);
        double diff = 0;
        for (int k = 0; k < 3; ++k) diff = std::max(diff, std::abs(p.position[size_t(k)] - in.position[size_t(k)]));
        // q and −q are the same rotation: compare by the absolute value of the dot product.
        double qd = 0;
        for (int k = 0; k < 4; ++k) qd += p.rotation[size_t(k)] * in.rotation[size_t(k)];
        if (diff > 1e-9 || std::abs(std::abs(qd) - 1) > 1e-12) {
            in.position = p.position;
            in.rotation = p.rotation;
            changed = true;
        }
    }
    // Gear mates (assembly.md §7.4, FX10-GEAR-004): the rim angles follow the solved placements, so the next move of
    // one rim (moveInstances, a new angle of its mate) turns the other one from here.
    for (auto& m : nd.active().mates) {
        if (m.type != "gear" || m.suppressed) continue;
        const auto ms = std::find_if(pr->assembly->mates.begin(), pr->assembly->mates.end(), [&](const auto& x) { return x.id == m.id; });
        if (ms == pr->assembly->mates.end() || ms->error) continue;
        const auto g = regen::mateGearAngles(k_, *pr->assembly, m.refs[0], m.refs[1]);
        if (!g) continue;
        if (!m.angles || std::abs((*m.angles)[0] - (*g)[0]) > 1e-9 || std::abs((*m.angles)[1] - (*g)[1]) > 1e-9) {
            m.angles = *g;
            changed = true;
        }
    }
    if (changed) undo_->replaceCurrent(doc::Snapshot{nd, undo_->current().label, ctx_, undo_->current().labelName});
}

// Mate definition check (core-api §4.1в): type, two references, a value for distance/angle and only for them.
Status Session::checkAssemblyFeature(const Json& def) const {
    if (!doc().active().isAssembly()) return okStatus();
    const std::string type = def.is_object() && def.contains("type") && def["type"].is_string() ? def["type"].get<std::string>() : std::string();
    if (type != "plane" && type != "axis" && type != "sketch" && type != "extrude" && type != "hole")
        return Error("API_NOT_PART").with("id", doc().active().id);
    if (type == "extrude" && (!def.contains("operation") || def["operation"] != "cut")) return invalidDefinition("operation", "/operation");
    if (def.contains("targets") && def["targets"].is_array())
        for (const auto& t : def["targets"]) {
            const std::string p = t.is_string() ? t.get<std::string>() : std::string();
            const std::string top = p.substr(0, p.find('/'));
            const auto& ins = doc().active().instances;
            if (std::none_of(ins.begin(), ins.end(), [&](const doc::Instance& in) { return in.id == top; }))
                return invalidDefinition("targets", "/targets");
        }
    return okStatus();
}

Status Session::checkMateDefinition(const Json& def) const {
    static const std::set<std::string> types{"coincident", "concentric", "distance", "angle", "parallel", "perpendicular", "tangent", "gear", "lock"};
    if (!def.is_object() || !def.contains("type") || !def["type"].is_string() || !types.count(def["type"].get<std::string>()))
        return Error("API_INVALID_DEFINITION").with("detail", std::string("type")).at("/type");
    if (!def.contains("refs") || !def["refs"].is_array() || def["refs"].size() != 2 || !def["refs"][0].is_string() || !def["refs"][1].is_string())
        return Error("API_INVALID_DEFINITION").with("detail", std::string("refs")).at("/refs");
    const std::string t = def["type"];
    const bool needsValue = t == "distance" || t == "angle";
    if (needsValue != (def.contains("value") && def["value"].is_string()))
        return Error("API_INVALID_DEFINITION").with("detail", std::string("value")).at("/value");
    for (const auto& r : def["refs"]) {
        const std::string ref = r.get<std::string>();
        if (ref == "origin" || ref.rfind("plane:", 0) == 0 || ref.rfind("axis:", 0) == 0) continue;
        if (ref.find('/') == std::string::npos) {  // assembly reference plane / axis / sketch (assembly.md §7б, FX7-ASM-006)
            const size_t colon = ref.find(':');
            const auto f = doc().active().find(ref.substr(0, colon));
            if (f && colon == std::string::npos &&
                (f->type == doc::FeatureType::Plane || f->type == doc::FeatureType::Axis || f->type == doc::FeatureType::Sketch))
                continue;
            if (f && colon != std::string::npos && f->sketchData())
                if (const auto* e = f->sketchData()->findEntity(ref.substr(colon + 1));
                    e && (e->type == sketch::EntityType::Point || e->type == sketch::EntityType::Line || e->type == sketch::EntityType::Circle ||
                          e->type == sketch::EntityType::Arc))
                    continue;
            return Error("API_INVALID_DEFINITION").with("detail", std::string("ref")).at("/refs");
        }
        const std::string top = regen::refTopInstance(ref);  // syntactic parsing (assembly.md §3.1)
        bool found = false;
        for (const auto& in : doc().active().instances) found = found || in.id == top;
        if (!found) return notFound(top);
        // A stable name of the body or a construction element of the part (assembly.md §7.1, FX9-ASM-001).
        const std::string leaf = naming::splitPath(ref).leaf;
        if (!regen::isConstructionName(leaf) && !naming::parse(leaf))
            return Error("API_INVALID_DEFINITION").with("detail", std::string("ref name")).at("/refs");
    }
    if (def.contains("flip") && !def["flip"].is_boolean()) return Error("API_INVALID_DEFINITION").with("detail", std::string("flip")).at("/flip");
    return okStatus();
}

// Mate fields from a validated definition; for a fixed mate - relative placement from the current placements.
Status Session::fillMate(doc::Mate& m, const Json& def) {
    const std::string t = def["type"];
    m.type = t;
    m.refs = {def["refs"][0].get<std::string>(), def["refs"][1].get<std::string>()};
    m.value.reset();
    if (t == "distance" || t == "angle") m.value = def["value"].get<std::string>();
    m.flip = false;  // "flip" in a definition is the one-off turn (turnForMate), never stored (FX9-ASM-005)
    m.relative.reset();
    m.angles.reset();
    if (t == "gear") {
        // Rim angles from the current placements (assembly.md §7.4, FX10-GEAR-004): the rims keep their mutual turn;
        // references that are not two pitch cylinders — no angles, the solve reports ASM_MATE_BAD_REF.
        runner_.waitIdle(solutions::SOL_SYNC_WAIT_MS);
        if (auto a = assembly()) m.angles = regen::mateGearAngles(k_, *a, m.refs[0], m.refs[1]);
    }
    if (t == "lock") {
        // The relative placement is taken from the current positions (core-api.md addMate): the regeneration of the
        // current revision is awaited first - otherwise a just inserted or moved instance is missing or stale in the
        // published assembly and the result depends on the worker thread's timing.
        runner_.waitIdle(solutions::SOL_SYNC_WAIT_MS);
        auto a = assembly();
        if (!a) return Error("API_STATE_UNAVAILABLE");
        auto rel = regen::mateRelative(k_, *a, m.refs[0], m.refs[1]);
        if (!rel) return Error("API_STATE_UNAVAILABLE");
        m.relative = *rel;
    }
    return okStatus();
}

// «Flip» (assembly.md §7.4а, FX9-ASM-005): "flip": true in the definition turns the free instance of the mate once
// in document nd (its top-level placement); a mate without a side — API_INVALID_DEFINITION (flip).
Status Session::turnForMate(doc::Document& nd, const doc::Mate& m) {
    runner_.waitIdle(solutions::SOL_SYNC_WAIT_MS);
    auto a = assembly();
    if (!a) return Error("API_STATE_UNAVAILABLE");
    const auto t = regen::mateTurn(k_, *a, m.type, m.refs);
    if (!t) return Error("API_INVALID_DEFINITION").with("detail", std::string("flip")).at("/flip");
    for (auto& in : nd.active().instances)
        if (in.id == t->first) {
            const Placement p = placementOf(t->second);
            in.position = p.position;
            in.rotation = p.rotation;
        }
    return okStatus();
}

bool Session::mateHasSide(const std::string& type, const std::array<std::string, 2>& refs) const {
    auto a = assembly();
    return a && regen::mateHasSide(k_, *a, type, refs);
}

std::optional<int> Session::mateRefTeeth(const std::string& ref) const {
    auto a = assembly();
    const auto p = a ? regen::matePrimitive(k_, *a, ref) : std::nullopt;
    if (!p || p->second.teeth <= 0) return std::nullopt;
    return p->second.teeth;
}

std::optional<double> Session::mateCurrentValue(const std::string& type, const std::array<std::string, 2>& refs) const {
    auto a = assembly();
    auto v = a ? regen::mateCurrentValue(k_, *a, type, refs) : std::nullopt;
    if (!v) return std::nullopt;
    const expr::DocumentUnits u = units();
    return *v / (type == "angle" ? expr::angleFactor(u.angle) : expr::lengthFactor(u.length));
}

Result<std::string> Session::addMate(const Json& def, const std::optional<std::string>& name) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    if (auto s = checkMateDefinition(def); !s) return s.error();
    const std::string t = def["type"];
    doc::Mate m;
    if (auto s = fillMate(m, def); !s) return s.error();
    doc::Document nd = doc();
    m.id = doc::allocate(nd, doc::IdKind::Mate);
    if (name && !trim(*name).empty()) {
        m.name = trim(*name);
    } else {
        std::set<std::string> taken;
        for (const auto& x : nd.active().mates) taken.insert(x.name);
        for (int n = 1;; ++n)
            if (!taken.count(mateBaseName(t) + std::to_string(n))) {
                m.name = mateBaseName(t) + std::to_string(n);
                break;
            }
    }
    const std::string id = m.id;
    if (def.value("flip", false))
        if (auto s = turnForMate(nd, m); !s) return s.error();
    nd.active().mates.push_back(m);
    if (auto s = commit(std::move(nd), "undo.addMate", {}, id); !s) return s.error();
    return id;
}

// ---------------------------------------------------------------- seating on a shaft (library.md §6.5, S6A-BRG-003)

namespace {

constexpr double SEAT_PERP_TOL = 1e-6;      // |sin|: shoulder normal parallel to the journal axis
constexpr double SEAT_FACE_TOL = 1e-6;      // mm: a point lies on a face; a plane passes through the origin
constexpr double SEAT_PROBE_MIN = 0.01;     // mm: step past the journal diameter when looking for the shoulder
constexpr double SEAT_PROBE_REL = 1e-3;     // × r
constexpr double SEAT_DIAMETER_TOL = 1e-6;  // relative: the bore diameter equals the journal diameter

// Unit vector perpendicular to the axis: the world unit vector least parallel to it, orthogonalized (solutions.md §7.3).
Vec3 perpendicularTo(const Vec3& a) {
    const Vec3 units[3] = {Vec3::UnitX(), Vec3::UnitY(), Vec3::UnitZ()};
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(units[i].dot(a)) < std::abs(units[best].dot(a))) best = i;
    return (units[best] - units[best].dot(a) * a).normalized();
}

struct SeatFace {
    const regen::InstanceEntry* entry = nullptr;
    int index = -1;  // in the part's name table
    kernel::SurfaceInfo surface;
};

// An attachment pair for the initial placement (library.md §6.6 item 6): element primitive (part frame) and place (world).
struct AttachPair {
    std::string kind;  // "axis", "plane", "point"
    Vec3 p, e;         // element point and direction
    Vec3 q, w;         // place point and direction
};

// Placement of the element where all attachments coincide (library.md §6.6 item 6): the rotation from the direction
// pairs, then the translation of least deviation from the first pair by least squares.
regen::Motion3 attachmentPlacement(size_t n, const std::function<AttachPair(size_t)>& pairAt) {
    std::vector<AttachPair> ps;
    for (size_t i = 0; i < n; ++i) ps.push_back(pairAt(i));
    Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
    // Directions to align: plane — the normal against the place normal; axis — the sign keeps the current direction.
    struct Dir {
        Vec3 e, w;
        bool axis;
    };
    std::vector<Dir> dirs;
    for (const auto& x : ps) {
        if (x.kind == "point") continue;
        const Vec3 e = x.e.normalized();
        Vec3 w = x.w.normalized();
        if (x.kind == "plane") w = -w;
        dirs.push_back({e, w, x.kind == "axis"});
    }
    // Axis signs agree with a plane whose element normal is parallel to the axis (the body goes on the place's side).
    for (auto& [e, w, axis] : dirs)
        for (size_t i = 0; i < ps.size() && axis; ++i)
            if (ps[i].kind == "plane" && std::abs(ps[i].e.normalized().dot(e)) > 1 - 1e-9) {
                const Vec3 en = ps[i].e.normalized(), wn = -ps[i].w.normalized();
                if (std::abs(wn.dot(w)) > 1 - 1e-6) w = (en.dot(e) > 0) == (wn.dot(w) > 0) ? w : Vec3(-w);
            }
    if (!dirs.empty()) {
        const Vec3 e1 = dirs[0].e, w1 = dirs[0].w;
        if (e1.dot(w1) < -1 + 1e-12) {
            Vec3 ax = Vec3::UnitX();
            if (std::abs(w1.x()) > 0.9) ax = Vec3::UnitY();
            ax = (ax - ax.dot(w1) * w1).normalized();
            R = Eigen::AngleAxisd(std::numbers::pi, ax).toRotationMatrix();
        } else {
            R = Eigen::Quaterniond::FromTwoVectors(e1, w1).toRotationMatrix();
        }
        for (size_t j = 1; j < dirs.size(); ++j) {
            const Vec3 wj = dirs[j].w;
            if (wj.cross(w1).norm() <= 1e-6) continue;
            Vec3 a = R * dirs[j].e, b = wj;
            a -= a.dot(w1) * w1;
            b -= b.dot(w1) * w1;
            if (a.norm() < 1e-9 || b.norm() < 1e-9) break;
            if (dirs[j].axis && a.dot(b) < 0) b = -b;  // an axis may go either way: the nearer of ±w
            const double ang = std::atan2(a.normalized().cross(b.normalized()).dot(w1), a.normalized().dot(b.normalized()));
            R = Eigen::AngleAxisd(ang, w1).toRotationMatrix() * R;
            break;
        }
    }
    // Translation: least squares with the least deviation from t0 = q1 − R·p1.
    const Vec3 t0 = ps.empty() ? Vec3::Zero() : Vec3(ps[0].q - R * ps[0].p);
    std::vector<Eigen::RowVector3d> rows;
    std::vector<double> rhs;
    for (const auto& x : ps) {
        const Vec3 d = x.q - (R * x.p + t0);  // residual at t0; the rows act on Δ = t − t0
        if (x.kind == "point") {
            for (int c = 0; c < 3; ++c) {
                Eigen::RowVector3d r = Eigen::RowVector3d::Zero();
                r[c] = 1;
                rows.push_back(r);
                rhs.push_back(d[c]);
            }
        } else if (x.kind == "plane") {
            const Vec3 nq = x.w.normalized();
            rows.push_back(nq.transpose());
            rhs.push_back(nq.dot(d));
        } else {
            const Vec3 wa = x.w.normalized();
            Vec3 u = Vec3::UnitX();
            if (std::abs(wa.x()) > 0.9) u = Vec3::UnitY();
            u = (u - u.dot(wa) * wa).normalized();
            const Vec3 v = wa.cross(u);
            rows.push_back(u.transpose());
            rhs.push_back(u.dot(d));
            rows.push_back(v.transpose());
            rhs.push_back(v.dot(d));
        }
    }
    Vec3 delta = Vec3::Zero();
    if (!rows.empty()) {
        Eigen::MatrixXd A(Eigen::Index(rows.size()), 3);
        Eigen::VectorXd b(Eigen::Index(rows.size()));
        for (size_t i = 0; i < rows.size(); ++i) {
            A.row(Eigen::Index(i)) = rows[i];
            b[Eigen::Index(i)] = rhs[i];
        }
        Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> cod(A);
        delta = cod.solve(b);
    }
    return {R, t0 + delta};
}

}  // namespace

Result<std::string> Session::seatLibraryPart(const std::string& packageId, const std::string& size, const std::string& journal,
                                             const std::optional<std::string>& shoulder) {
    // A special case of the attachments (library.md §6.5): the first axis attachment — the journal, the first plane one —
    // the shoulder.
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    const auto cat = libraryCatalog();
    const library::CatalogItem* item = cat->find(packageId);
    if (!item) return notFound(packageId);
    std::string axisName, planeName;
    for (const auto& at : item->package.description.attachments) {
        if (at.kind == "axis" && axisName.empty()) axisName = at.name;
        if (at.kind == "plane" && planeName.empty()) planeName = at.name;
    }
    const auto& rows = item->package.sizes.rows;
    if (std::none_of(rows.begin(), rows.end(), [&](const library::SizeRow& r) { return r.name == size; })) return notFound(size);
    if (axisName.empty() || planeName.empty()) return Error("API_SEAT_PART").with("name", libraryText(item->package.description.title));
    std::map<std::string, std::string> targets{{axisName, journal}};
    if (shoulder) targets[planeName] = *shoulder;
    return attachLibraryPartImpl(packageId, size, targets, true);
}

Result<std::string> Session::attachLibraryPart(const std::string& packageId, const std::string& size,
                                               const std::map<std::string, std::string>& targets) {
    return attachLibraryPartImpl(packageId, size, targets, false);
}

std::map<std::string, std::pair<std::string, std::string>> Session::attachmentMates(const std::string& instance) const {
    std::map<std::string, std::pair<std::string, std::string>> out;
    if (!doc().active().isAssembly()) return out;
    const doc::Instance* in = nullptr;
    for (const auto& x : doc().active().instances)
        if (x.id == instance) in = &x;
    if (!in || !doc::isLibraryRef(in->refFile)) return out;
    const doc::LibraryPart* lp = doc().libraryPart(in->refFile.substr(std::string(doc::LIBRARY_REF_PREFIX).size()));
    const auto cat = libraryCatalog();
    const library::CatalogItem* item = lp ? cat->find(lp->package) : nullptr;
    if (!item) return out;
    for (const auto& at : item->package.description.attachments)
        for (const auto& m : doc().active().mates) {
            const std::string own = instance + "/" + at.ref;
            if (m.refs[1] == own) out[at.name] = {m.id, m.refs[0]};
            else if (m.refs[0] == own) out[at.name] = {m.id, m.refs[1]};
            else continue;
            break;
        }
    return out;
}

std::map<std::string, std::string> Session::libraryAttachmentPlaces(const std::string& instance) const {
    std::map<std::string, std::string> out;
    for (const auto& [name, mp] : attachmentMates(instance)) out[name] = mp.second;
    return out;
}

Status Session::replaceLibraryPart(const std::string& instance, const std::string& packageId, const std::string& size,
                                   const std::map<std::string, std::string>& targets) {
    auto r = attachLibraryPartImpl(packageId, size, targets, false, instance);
    if (!r) return r.error();
    return okStatus();
}

Result<std::string> Session::attachLibraryPartImpl(const std::string& packageId, const std::string& size,
                                                   const std::map<std::string, std::string>& targetsIn, bool seat,
                                                   const std::string& replace) {
    using Prim = regen::MatePrimitive;
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    // Replacement (§6.7): the instance must be a library one; its attachment mates give the current places.
    std::map<std::string, std::pair<std::string, std::string>> oldMates;
    if (!replace.empty()) {
        const doc::Instance* old = nullptr;
        for (const auto& x : doc().active().instances)
            if (x.id == replace) old = &x;
        if (!old) return notFound(replace);
        if (!doc::isLibraryRef(old->refFile)) return Error("API_NOT_LIBRARY_INSTANCE").with("id", replace);
        oldMates = attachmentMates(replace);
    }
    const auto cat = libraryCatalog();
    const library::CatalogItem* item = cat->find(packageId);
    if (!item) return notFound(packageId);
    const auto& rows = item->package.sizes.rows;
    if (std::none_of(rows.begin(), rows.end(), [&](const library::SizeRow& r) { return r.name == size; })) return notFound(size);
    const auto& atts = item->package.description.attachments;
    const std::string title = libraryText(item->package.description.title);  // messages (library.md §17.2)
    for (const auto& [name, ref] : targetsIn)
        if (std::none_of(atts.begin(), atts.end(), [&](const library::Attachment& x) { return x.name == name; }))
            return Error("API_INVALID_DEFINITION").with("detail", name);
    std::map<std::string, std::string> targets = targetsIn;
    for (const auto& at : atts)
        if (!targets.count(at.name))
            if (auto it = oldMates.find(at.name); it != oldMates.end()) targets[at.name] = it->second.second;
    if (atts.empty() && replace.empty()) return seat ? Error("API_SEAT_PART").with("name", title) : Error("API_ATTACH_MISSING").with("name", std::string());
    // Error codes of seatLibraryPart (library.md §6.5).
    auto placeError = [&](const library::Attachment& at, const std::string& ref, const char* code) {
        if (seat) return Error(at.kind == "axis" ? "API_SEAT_JOURNAL" : "API_SEAT_SHOULDER").with("ref", ref);
        return Error(code).with("name", at.name).with("ref", ref);
    };
    // Faces and vertices of the current assembly state (assembly.md §3: "<instance path>/<name>").
    const auto a = assembly();
    auto resolveFace = [&](const std::string& ref) -> std::optional<SeatFace> {
        if (!a) return std::nullopt;
        const naming::PathName pn = naming::splitPath(ref);
        const auto* e = pn.path.empty() ? nullptr : a->find(pn.path);
        if (!e || !e->state || !e->connection.empty()) return std::nullopt;
        const auto idx = regen::mateFace(k_, *e->state, pn.leaf);  // one face or a split one (assembly.md §7.2)
        if (!idx) return std::nullopt;
        SeatFace f{e, *idx, {}};
        const auto& fe = e->state->names.faces.at(size_t(f.index));
        f.surface = k_.faceSurface(e->state->bodies.at(size_t(fe.body)).shape, fe.face);
        return f;
    };
    auto targetPrim = [&](const library::Attachment& at, const std::string& ref) -> std::optional<Prim> {
        if (!a) return std::nullopt;
        const naming::PathName pn = naming::splitPath(ref);
        const auto parsed = naming::parse(pn.leaf);
        if (!parsed) return std::nullopt;
        const bool vertex = parsed->kind == naming::EntityKind::Vertex;
        if (vertex != (at.kind == "point") || (!vertex && parsed->kind != naming::EntityKind::Face)) return std::nullopt;
        if (!vertex) {
            const auto f = resolveFace(ref);
            if (!f) return std::nullopt;
            const auto kd = f->surface.kind;
            if (at.kind == "axis" && kd != kernel::SurfaceInfo::Kind::Cylinder && kd != kernel::SurfaceInfo::Kind::Cone) return std::nullopt;
            if (at.kind == "plane" && kd != kernel::SurfaceInfo::Kind::Plane) return std::nullopt;
        }
        auto p = regen::worldMatePrimitive(k_, *a, ref);
        if (!p) return std::nullopt;
        const bool ok = at.kind == "axis" ? p->kind == Prim::Kind::Axis : at.kind == "plane" ? p->kind == Prim::Kind::Plane : p->kind == Prim::Kind::Point;
        return ok ? p : std::nullopt;
    };
    // The element part with the chosen size (as a derived instance, solutions.md §7.6).
    doc::LibraryPart lp = library::embed(item->package);
    doc::Document pd = *lp.document;
    for (auto& vt : pd.variantTables)
        if (vt.id == lp.sizesTable) vt.active = size;
    pd.activeElement = item->package.element;
    regen::Regenerator gen(k_);
    const auto pr = gen.regenerate(pd, 0, nullptr, false);
    // An assembly element (library.md §2.6): primitives of its base and child elements in the frame of the template assembly.
    const bool asmElement = pd.active().isAssembly();
    auto elementPrim = [&](const library::Attachment& at) -> std::optional<Prim> {
        std::optional<Prim> p;
        if (at.ref == "origin") {
            p = Prim{Prim::Kind::Point, Vec3::Zero(), Vec3::UnitZ(), 0};
        } else if (at.ref.rfind("plane:", 0) == 0) {
            if (const auto pl = sketch::datumPlane(at.ref)) p = Prim{Prim::Kind::Plane, pl->origin, pl->normal.normalized(), 0};
        } else if (at.ref == "axis:x" || at.ref == "axis:y" || at.ref == "axis:z") {
            p = Prim{Prim::Kind::Axis, Vec3::Zero(), sketch::datumAxis(at.ref).value_or(Vec3::UnitZ()), 0};
        } else if (asmElement) {
            if (pr && pr->assembly) p = regen::worldMatePrimitive(k_, *pr->assembly, at.ref);
        } else if (pr && pr->final) {
            p = regen::partPrimitive(k_, *pr->final, at.ref);
        }
        if (!p) return std::nullopt;
        if (at.flip) p->w = -p->w;
        const bool ok = at.kind == "axis" ? p->kind == Prim::Kind::Axis : at.kind == "plane" ? p->kind == Prim::Kind::Plane : p->kind == Prim::Kind::Point;
        return ok ? p : std::nullopt;
    };
    // 2. Places in attachment order.
    struct Use {
        const library::Attachment* at;
        std::string ref;
        Prim world, local;
    };
    std::vector<Use> uses;
    const library::Attachment* firstAxis = nullptr;
    for (const auto& at : atts)
        if (at.kind == "axis" && !firstAxis) firstAxis = &at;
    for (const auto& at : atts) {
        std::string ref;
        if (auto it = targets.find(at.name); it != targets.end() && !it->second.empty()) {
            ref = it->second;
            if (!targetPrim(at, ref)) return placeError(at, ref, "API_ATTACH_TARGET");
        } else if (at.autoRule == "shoulder" && firstAxis) {
            // The shoulder next to the journal (library.md §6.5 «Поиск уступа»).
            const auto jt = targets.find(firstAxis->name);
            if (jt == targets.end()) continue;  // the journal itself is missing — reported by its attachment
            const auto j = resolveFace(jt->second);
            if (!j || j->surface.kind != kernel::SurfaceInfo::Kind::Cylinder) continue;
            const double r = j->surface.radius;
            const Vec3 axis = j->surface.dir.normalized();
            const regen::PartState& shaft = *j->entry->state;
            const Vec3 u = perpendicularTo(axis);
            const double delta = std::max(SEAT_PROBE_MIN, SEAT_PROBE_REL * r);
            std::set<int> seen;
            std::vector<int> found;
            for (const auto& e : shaft.names.edges) {
                const int other = e.f1 == j->index ? e.f2 : e.f2 == j->index ? e.f1 : -1;
                if (other < 0 || other == j->index || !seen.insert(other).second) continue;
                const auto& fe = shaft.names.faces.at(size_t(other));
                const kernel::Shape& body = shaft.bodies.at(size_t(fe.body)).shape;
                const auto si = k_.faceSurface(body, fe.face);
                if (si.kind != kernel::SurfaceInfo::Kind::Plane || si.dir.normalized().cross(axis).norm() > SEAT_PERP_TOL) continue;
                const Vec3 n = si.dir.normalized();
                const Point3 o = j->surface.point + axis * ((si.point - j->surface.point).dot(n) / axis.dot(n));
                if (k_.distanceToFace(body, fe.face, o + (r + delta) * u) <= SEAT_FACE_TOL) found.push_back(other);
            }
            if (found.empty()) return Error("API_SEAT_NO_SHOULDER").with("ref", jt->second);
            if (found.size() > 1) return Error("API_SEAT_SHOULDER_AMBIGUOUS").with("ref", jt->second);
            ref = j->entry->path + "/" + shaft.names.faces.at(size_t(found.front())).full;
            if (!targetPrim(at, ref)) return placeError(at, ref, "API_ATTACH_TARGET");
        } else if (!at.optional) {
            return placeError(at, std::string(), "API_ATTACH_MISSING").with("name", at.name);
        } else {
            continue;
        }
        const auto lp0 = elementPrim(at);
        if (!lp0) return seat ? Error("API_SEAT_PART").with("name", title) : Error("API_ATTACH_PART").with("name", at.name).with("title", title);
        uses.push_back({&at, ref, *targetPrim(at, ref), *lp0});
    }
    if (uses.empty() && !atts.empty()) return placeError(atts.front(), std::string(), "API_ATTACH_MISSING").with("name", atts.front().name);
    // 4. Directions agree: |cos| between the element directions equals that between the places.
    for (size_t i = 0; i < uses.size(); ++i)
        for (size_t j = i + 1; j < uses.size(); ++j) {
            if (uses[i].at->kind == "point" || uses[j].at->kind == "point") continue;
            const double ce = std::abs(uses[i].local.w.normalized().dot(uses[j].local.w.normalized()));
            const double cw = std::abs(uses[i].world.w.normalized().dot(uses[j].world.w.normalized()));
            if (std::abs(ce - cw) > SEAT_PERP_TOL) return placeError(*uses[j].at, uses[j].ref, "API_ATTACH_GEOMETRY");
        }
    // 5. Diameters.
    const expr::VariableTable rowVars = library::rowVariables(item->package, size);
    for (const auto& u : uses) {
        if (!u.at->matchDiameter) continue;
        const expr::VariableEntry* e = rowVars.find(u.at->diameterKey);
        const double bore = e && e->value.ok() ? e->value->value : 0;
        if (std::abs(bore - 2 * u.world.r) > SEAT_DIAMETER_TOL * std::max(1.0, 2 * u.world.r))
            return Error("API_SEAT_DIAMETER").with("bore", bore).with("journal", 2 * u.world.r);
    }
    // 6. Initial placement where the attachments coincide.
    const regen::Motion3 M = uses.empty() ? regen::Motion3{} : attachmentPlacement(uses.size(), [&](size_t i) {
        return AttachPair{uses[i].at->kind, uses[i].local.p, uses[i].local.w, uses[i].world.p, uses[i].world.w};
    });
    // 7. One step: the instance (as insertLibraryPart, or the replaced one) and a mate per attachment.
    syncAssemblyPositions();
    doc::Document nd = doc();
    if (!nd.libraryPart(lp.id)) nd.libraryParts.push_back(lp);
    const doc::Document& copy = *nd.libraryPart(lp.id)->document;
    std::string id;
    if (!replace.empty()) {
        std::set<std::string> drop;
        for (const auto& [name, mp] : oldMates) drop.insert(mp.first);
        auto& ms = nd.active().mates;
        ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const doc::Mate& m) { return drop.count(m.id) > 0; }), ms.end());
        for (auto& in : nd.active().instances) {
            if (in.id != replace) continue;
            in.refFile = doc::libraryRef(lp.id);
            in.refElement = item->package.element;
            in.variantRows = {{nd.libraryPart(lp.id)->sizesTable, size}};
            in.overrides.clear();
            if (!uses.empty() && !in.fixed) {
                const Placement pl = placementOf(M);
                in.position = pl.position;
                in.rotation = pl.rotation;
            }
        }
        id = replace;
    } else {
        doc::Instance in;
        in.id = doc::allocate(nd, doc::IdKind::Instance);
        in.refFile = doc::libraryRef(lp.id);
        in.refElement = item->package.element;
        in.variantRows = {{nd.libraryPart(lp.id)->sizesTable, size}};
        placeNewInstance(nd.active(), in, libraryText(copy.elements[size_t(copy.elementIndex(in.refElement))].name), std::nullopt);  // library.md §17.4
        if (!nd.active().instances.empty()) {
            const Placement pl = placementOf(M);
            in.position = pl.position;
            in.rotation = pl.rotation;
        }
        id = in.id;
        nd.active().instances.push_back(in);
    }
    for (const auto& u : uses) {
        doc::Mate m;
        m.id = doc::allocate(nd, doc::IdKind::Mate);
        m.type = u.at->mateType();
        m.refs = {u.ref, id + "/" + u.at->ref};
        std::set<std::string> taken;
        for (const auto& x : nd.active().mates) taken.insert(x.name);
        for (int n = 1;; ++n)
            if (!taken.count(mateBaseName(m.type) + std::to_string(n))) {
                m.name = mateBaseName(m.type) + std::to_string(n);
                break;
            }
        nd.active().mates.push_back(m);
    }
    // 8. Cuts of the element (library.md §6.6 item 8, FX10-LIB-009): a connectionCut owned by the instance at the end of the
    // part of the place, the tool frame in the part's coordinates.
    const std::string asmId = nd.active().id;
    if (!replace.empty()) eraseLibraryCuts(nd, asmId, id);
    regen::Motion3 Mi = M;
    std::string instName;
    for (const auto& in : nd.active().instances)
        if (in.id == id) {
            Mi = regen::motionOf(in.position, in.rotation);
            instName = in.name;
        }
    for (const auto& c : item->package.description.cuts) {
        const auto u = std::find_if(uses.begin(), uses.end(), [&](const Use& x) { return x.at->name == c.attachment; });
        if (u == uses.end()) continue;
        const naming::PathName pn = naming::splitPath(u->ref);
        const auto* e = a ? a->find(pn.path) : nullptr;
        if (!e || !e->state || !e->file.empty() || !e->connection.empty() || !e->pattern.empty()) continue;  // §10: other files
        const int ei = nd.elementIndex(e->element);
        if (ei < 0 || nd.elements[size_t(ei)].isAssembly()) continue;
        auto tool = library::cutTool(item->package, c, size);
        if (!tool) return tool.error();
        const regen::Motion3 F = inverseMotion(e->world) * Mi;  // template → part
        auto vec = [](const Vec3& v) { return Json::array({v.x(), v.y(), v.z()}); };
        const bool rev = !tool->revolve.empty();
        Json def = Json::object();
        def["owner"] = Json{{"assembly", asmId}, {"connection", id}, {"name", instName}, {"library", true}};
        def["source"] = e->path;
        def["output"] = c.name;
        def["title"] = c.title;
        def["frame"] = Json{{"origin", vec(F.t)},
                            {"x", vec(F.R * Vec3::UnitX())},
                            {"y", vec(F.R * (rev ? Vec3(-Vec3::UnitZ()) : Vec3(Vec3::UnitY())))},
                            {"z", vec(F.R * (rev ? Vec3(Vec3::UnitY()) : Vec3(Vec3::UnitZ())))}};
        if (rev) {
            Json pts = Json::array();
            for (const auto& p : tool->revolve) pts.push_back(Json::array({p[0], p[1]}));
            def["revolve"] = pts;
        } else {
            const auto& b = *tool->box;
            def["box"] = Json{{"x", Json::array({b[0][0], b[0][1]})}, {"y", Json::array({b[1][0], b[1][1]})}, {"z", Json::array({b[2][0], b[2][1]})}};
        }
        auto nf = std::make_shared<doc::Feature>();
        nf->id = doc::allocate(nd, doc::IdKind::Feature);
        nf->type = doc::FeatureType::ConnectionCut;
        nf->name = libraryText(c.title) + " (" + instName + ")";  // library.md §17.4
        nf->suppressed = false;
        doc::PatternData pdata;
        pdata.def = std::move(def);
        nf->data = std::move(pdata);
        nd.elements[size_t(ei)].features.push_back(nf);
    }
    const char* undoKey = !replace.empty() ? "undo.replaceLibraryPart" : seat ? "undo.seatLibraryPart" : "undo.attachLibraryPart";
    if (auto s = commit(std::move(nd), undoKey, {}, id); !s) return s.error();
    return id;
}

namespace {

// Merge for updateMate (core-api §4.1в): def fields replace mate fields, null removes.
Result<Json> mergedMateDefinition(const doc::Mate& m, const Json& def) {
    if (!def.is_object()) return Error("API_INVALID_DEFINITION").with("detail", std::string("definition must be an object"));
    Json full = Json::object();
    full["type"] = m.type;
    full["refs"] = Json::array({m.refs[0], m.refs[1]});
    if (m.value) full["value"] = *m.value;
    if (m.flip) full["flip"] = true;
    for (auto it = def.begin(); it != def.end(); ++it)
        if (it.key() != "type" && it.key() != "refs" && it.key() != "value" && it.key() != "flip")
            return Error("API_INVALID_DEFINITION").with("detail", it.key() + " not allowed").at("/" + it.key());
    mergePatch(full, def);
    // Type without a value - the value of the previous type is discarded.
    if (full.contains("type") && full["type"].is_string() && full["type"] != "distance" && full["type"] != "angle" && !def.contains("value"))
        full.erase("value");
    return full;
}

}  // namespace

Status Session::updateMate(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    doc::Document nd = doc();
    doc::Mate* m = nullptr;
    for (auto& x : nd.active().mates)
        if (x.id == id) m = &x;
    if (!m) return notFound(id);
    auto merged = mergedMateDefinition(*m, def);
    if (!merged) return merged.error();
    const Json full = *merged;
    if (auto s = checkMateDefinition(full); !s) return s;
    const bool keepRelative = m->type == "lock" && full["type"] == "lock" && !def.contains("refs");
    const bool keepAngles = m->type == "gear" && full["type"] == "gear" && !def.contains("refs") && m->angles;
    const auto relative = m->relative;
    const auto angles = m->angles;
    if (auto s = fillMate(*m, full); !s) return s;
    if (keepRelative) m->relative = relative;  // fixed mate with the same objects - the previous relative placement
    if (keepAngles) m->angles = angles;        // gear with the same rims - the previous rim angles
    if (def.value("flip", false))
        if (auto s = turnForMate(nd, *m); !s) return s;
    return commit(std::move(nd), "undo.editMate", {}, id);
}

Status Session::deleteMate(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    syncAssemblyPositions();
    doc::Document nd = doc();
    auto& ms = nd.active().mates;
    const auto n = ms.size();
    std::erase_if(ms, [&](const doc::Mate& x) { return x.id == id; });
    if (ms.size() == n) return notFound(id);
    if (auto& x = nd.active().exploded) {  // §7д.4
        std::erase(x->suppressedMates, id);
        if (x->knownMates) std::erase(*x->knownMates, id);
    }
    return commit(std::move(nd), "undo.deleteMate", {}, id);
}

Status Session::renameMate(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    const std::string n = trim(name);
    doc::Document nd = doc();
    doc::Mate* m = nullptr;
    for (auto& x : nd.active().mates)
        if (x.id == id) m = &x;
    if (!m) return notFound(id);
    if (n.empty()) return Error("API_INVALID_NAME").with("name", name);
    m->name = n;
    return commit(std::move(nd), "undo.renameMate", {}, id);
}

Status Session::setMateSuppressed(const std::string& id, bool suppressed) {
    if (auto s = checkNoDrag(); !s) return s;
    syncAssemblyPositions();
    doc::Document nd = doc();
    doc::Mate* m = nullptr;
    for (auto& x : nd.active().mates)
        if (x.id == id) m = &x;
    if (!m) return notFound(id);
    if (nd.active().explodedActive()) {  // suppression of the exploded view's mode (assembly.md §7д.4)
        auto& sm = nd.active().exploded->suppressedMates;
        std::erase(sm, id);
        if (suppressed) {
            sm.push_back(id);
            std::map<std::string, size_t> order;
            for (size_t i = 0; i < nd.active().mates.size(); ++i) order[nd.active().mates[i].id] = i;
            std::stable_sort(sm.begin(), sm.end(), [&](const std::string& x, const std::string& y) { return order[x] < order[y]; });
        }
    } else {
        m->suppressed = suppressed;
    }
    return commit(std::move(nd), "undo.mateSuppressed", {}, id);
}

Status Session::moveInstances(const std::map<std::string, Placement>& positions) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    doc::Document nd = doc();
    const auto a = assembly();
    // New world placement of entry q: with the deepest moved path containing it (§2б).
    auto newWorld = [&](const std::string& q) {
        const regen::Motion3 old = a->find(q)->world;
        for (std::string p = q;;) {
            if (auto it = positions.find(p); it != positions.end())
                if (const auto* pe = a->find(p)) return motionOf(it->second) * (inverseMotion(pe->world) * old);
            const size_t sl = p.rfind('/');
            if (sl == std::string::npos) return old;
            p.resize(sl);
        }
    };
    for (const auto& [id, p] : positions) {
        // A derived connection fastener on the exploded view: its own record, a manual move (§7д.5, FX10-ASM-004).
        if (const auto* ce = a ? a->find(id) : nullptr; ce && !ce->connection.empty()) {
            if (!nd.active().explodedActive()) return notFound(id);
            setExplodedPlacement(nd.active(), id, p.position, p.rotation);
            continue;
        }
        if (const size_t sl = id.find('/'); sl != std::string::npos) {  // a free unit of a subassembly — its form (§2б)
            doc::Instance* in = findInstance(nd.active(), id.substr(0, sl));
            if (!in || !a || !a->find(id)) return notFound(id);
            if (nd.active().explodedActive()) continue;
            const std::string parent = id.substr(0, id.rfind('/'));
            const Placement rel = placementOf(inverseMotion(newWorld(parent)) * newWorld(id));
            const Placement cur = placementOf(inverseMotion(a->find(parent)->world) * a->find(id)->world);
            const doc::Instance::FormPlacement f{id.substr(sl + 1), rel.position, rel.rotation};
            const bool had = std::any_of(in->form.begin(), in->form.end(), [&](const auto& x) { return x.path == f.path; });
            if (!had && samePlacement(rel.position, rel.rotation, cur.position, cur.rotation)) continue;  // not moved
            auto it = std::find_if(in->form.begin(), in->form.end(), [&](const auto& x) { return x.path == f.path; });
            if (it != in->form.end()) *it = f;
            else in->form.push_back(f);
            continue;
        }
        doc::Instance* in = findInstance(nd.active(), id);
        if (!in) return notFound(id);
        if (in->fixed) continue;  // a fixed instance does not move (the solution did not move it either)
        if (nd.active().explodedActive()) {  // §7д.4
            setExplodedPlacement(nd.active(), id, p.position, p.rotation);
            continue;
        }
        in->position = p.position;
        in->rotation = p.rotation;
    }
    return commit(std::move(nd), "undo.moveInstance", {}, positions.empty() ? std::string() : positions.begin()->first);
}

Result<std::map<std::string, Session::Placement>> Session::previewInstanceDrag(const std::string& id, const Placement& to,
                                                                               const std::map<std::string, Placement>* from) {
    auto a = assembly();
    if (!a) return Error("API_STATE_UNAVAILABLE");
    if (!a->find(id)) return notFound(id);
    regen::AssemblyState st = *a;
    // The start of the solve — the previous preview (FX12-ASM-001, §7.6): each given entry moves with its descendants
    // rigidly; shallower entries first, so a free unit of a subassembly lands on its own previewed placement.
    if (from) {
        std::vector<std::pair<std::string, regen::Motion3>> given;
        for (const auto& [path, p] : *from) given.emplace_back(path, motionOf(p));
        std::sort(given.begin(), given.end(), [](const auto& x, const auto& y) {
            return std::count(x.first.begin(), x.first.end(), '/') < std::count(y.first.begin(), y.first.end(), '/');
        });
        for (const auto& [path, w] : given) {
            const auto* e = st.find(path);
            if (!e) continue;
            regen::Motion3 inv;  // e->world⁻¹
            inv.R = e->world.R.transpose();
            inv.t = -(inv.R * e->world.t);
            const regen::Motion3 delta = w * inv;
            for (auto& x : st.instances)
                if (x.path == path || x.path.rfind(path + "/", 0) == 0) x.world = delta * x.world;
        }
    }
    const auto vars = regen::documentVariables(doc(), &links_, selfPath());
    // In the exploded view — the mates of its mode (assembly.md §7д.4).
    const doc::Element el = explodedPreviewElement(doc().active(), *a);
    regen::MateSolveOptions o;
    o.translationOnly = doc().active().explodedActive();  // translations only (§7д.4, FX8-ASM-006)
    if (a->find(id)->flex) o.rigid.insert(id);  // an underdetermined subassembly moves as a whole (§2б)
    regen::Motion3 dm = regen::motionOf(to.position, to.rotation);
    if (o.translationOnly) dm.R = a->find(id)->world.R;
    regen::solveMates(k_, el, vars, regen::documentUnits(doc()), st, std::make_pair(id, dm), nullptr, o);
    std::map<std::string, Placement> out;
    bool dragInSystem = false;
    for (const auto& e : st.instances)
        if (e.depth == 0 || e.freeDof >= 0) {  // top-level and free units of subassemblies (§2б)
            out[e.path] = placementOf(e.world);
            dragInSystem = dragInSystem || (e.path == id && e.freeDof >= 0);
        }
    if (!dragInSystem) {  // instance without mates - free movement, its descendants with it
        out[id] = o.translationOnly ? placementOf(dm) : to;
        for (auto it = out.begin(); it != out.end();) it = it->first.rfind(id + "/", 0) == 0 ? out.erase(it) : std::next(it);
    }
    if (a->exploded) dropFollowers(out, *a, id);
    return out;
}

Result<std::map<std::string, Session::Placement>> Session::previewInstancePull(const std::string& id, const Vec3& grab, const Vec3& target) {
    auto a = assembly();
    if (!a) return Error("API_STATE_UNAVAILABLE");
    const auto* e0 = a->find(id);
    // A derived connection fastener on the exploded view moves freely, alone (§7д.5): the grab is carried onto the target.
    if (e0 && !e0->connection.empty() && a->exploded && doc().active().explodedActive()) {
        regen::Motion3 m = e0->world;
        m.t += target - m.apply(grab);
        return std::map<std::string, Placement>{{id, placementOf(m)}};
    }
    // A top-level instance or a body of the system inside an underdetermined subassembly (§2б).
    if (!e0 || (e0->depth != 0 && (regen::mateUnitOf(*a, id) != id || e0->freeDof < 0))) return notFound(id);
    regen::AssemblyState st = *a;
    const auto vars = regen::documentVariables(doc(), &links_, selfPath());
    const bool exploded = doc().active().explodedActive();
    const doc::Element el = explodedPreviewElement(doc().active(), *a);
    regen::MateSolveOptions o;
    o.translationOnly = exploded;  // §7д.4: translations only
    o.pull = regen::MatePull{id, grab, target};
    if (e0->flex) o.rigid.insert(id);  // the subassembly as a whole, in its form (§2б)
    regen::solveMates(k_, el, vars, regen::documentUnits(doc()), st, std::nullopt, nullptr, o);
    std::map<std::string, Placement> out;
    bool inSystem = false;
    for (const auto& e : st.instances)
        if (e.depth == 0 || e.freeDof >= 0) {
            out[e.path] = placementOf(e.world);
            inSystem = inSystem || (e.path == id && e.freeDof >= 0);
        }
    if (!inSystem && !e0->fixed) {  // instance without mates: grab is carried onto the target (§7.6б)
        regen::Motion3 m = e0->world;
        m.t += target - m.apply(grab);
        out[id] = placementOf(m);
        for (auto it = out.begin(); it != out.end();) it = it->first.rfind(id + "/", 0) == 0 ? out.erase(it) : std::next(it);
    }
    if (a->exploded) dropFollowers(out, *a, id);
    return out;
}

Result<Session::MatePreview> Session::previewMate(const Json& def, const std::optional<std::string>& editing) {
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    auto a = assembly();
    if (!a) return Error("API_STATE_UNAVAILABLE");
    doc::Element el = doc().active();
    doc::Mate* m = nullptr;
    if (editing) {
        for (auto& x : el.mates)
            if (x.id == *editing) m = &x;
        if (!m) return notFound(*editing);
    }
    Json full = def;
    if (m) {
        auto merged = mergedMateDefinition(*m, def);
        if (!merged) return merged.error();
        full = *merged;
    }
    if (auto s = checkMateDefinition(full); !s) return s.error();
    doc::Mate nm = m ? *m : doc::Mate{};
    const bool keepRelative = m && m->type == "lock" && full["type"] == "lock" && !def.contains("refs");
    const bool keepAngles = m && m->type == "gear" && full["type"] == "gear" && !def.contains("refs") && m->angles;
    if (auto s = fillMate(nm, full); !s) return s.error();
    if (keepRelative) nm.relative = m->relative;
    if (keepAngles) nm.angles = m->angles;
    nm.suppressed = false;
    if (m) {
        *m = nm;
    } else {
        nm.id = "preview";  // not a document ID: the mate exists only in this solve
        el.mates.push_back(nm);
    }
    const std::string mid = nm.id;
    regen::AssemblyState st = *a;
    if (def.value("flip", false)) {  // «Flip» in the preview: the flipped start placement (FX9-ASM-005)
        const auto t = regen::mateTurn(k_, st, nm.type, nm.refs);
        if (!t) return Error("API_INVALID_DEFINITION").with("detail", std::string("flip")).at("/flip");
        const regen::Motion3 d = t->second * regen::Motion3{st.find(t->first)->world.R.transpose(),
                                                            -(st.find(t->first)->world.R.transpose() * st.find(t->first)->world.t)};
        for (auto& e : st.instances)
            if (e.path == t->first || e.path.rfind(t->first + "/", 0) == 0) e.world = d * e.world;
    }
    const auto vars = regen::documentVariables(doc(), &links_, selfPath());
    const bool exploded = el.explodedActive();
    regen::MateSolveOptions o;
    o.translationOnly = exploded;
    regen::solveMates(k_, explodedPreviewElement(el, *a), vars, regen::documentUnits(doc()), st, std::nullopt, nullptr, o);
    MatePreview out;
    for (const auto& e : st.instances)
        if (e.depth == 0 || e.freeDof >= 0) {  // with the free units of subassemblies (§2б)
            out.placements[e.path] = placementOf(e.world);
            out.freeDof[e.path] = e.fixed ? 0 : e.freeDof;
        }
    for (const auto& ms : st.mates)
        if (ms.id == mid) out.error = ms.error, out.warning = ms.warning;
    return out;
}

std::optional<kernel::SurfaceInfo::Kind> Session::assemblyFaceKind(const std::string& ref) const {
    const auto si = assemblyFaceSurface(ref);
    if (!si) return std::nullopt;
    return si->kind;
}

std::optional<kernel::SurfaceInfo> Session::assemblyFaceSurface(const std::string& ref) const {
    auto a = assembly();
    if (!a) return std::nullopt;
    const regen::AssemblyRef r = regen::parseAssemblyRef(*a, ref);  // assembly.md §3.1
    if (r.kind != regen::AssemblyRef::Kind::Part || r.construction) return std::nullopt;
    const regen::InstanceEntry* e = r.entry;
    const auto rr = naming::resolve(r.leaf, naming::EntityKind::Face, e->state->names);
    if (rr.error || rr.entities.size() != 1) return std::nullopt;
    const auto& fe = e->state->names.faces.at(size_t(rr.entities.front().index));
    return k_.faceSurface(e->state->bodies.at(size_t(fe.body)).shape, fe.face);
}

// Hole axes for the connection wizard (solutions.md §7.11а, S7F-ASM-008).
Result<std::vector<std::string>> Session::holeAxisFaces(const std::string& ref, const std::string& preferFace, bool sameHoles) const {
    auto a = assembly();
    if (!a) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    auto prim = regen::worldMatePrimitive(k_, *a, ref);
    const auto noHole = Error("SOL_AXIS_NO_HOLE").with("name", ref);
    // A point or circle of an assembly sketch on an instance face (not a mate support, its plane follows the parts):
    // from the virtual state of the assembly.
    if (const size_t colon = ref.find(':'); !prim && colon != std::string::npos && ref.find('/') == std::string::npos)
        if (const auto v = assemblyVirtual())
            if (const auto sk = v->sketches.find(ref.substr(0, colon)); sk != v->sketches.end()) {
                const Plane& fr = sk->second.frame;
                const auto& P = sk->second.result.state.points;
                std::string pid = ref.substr(colon + 1);
                bool circle = false;
                if (const auto f = doc().active().find(sk->first); f && f->sketchData())
                    if (const auto* en = f->sketchData()->findEntity(pid);
                        en && (en->type == sketch::EntityType::Circle || en->type == sketch::EntityType::Arc))
                        pid = en->center, circle = true;
                if (const auto it = P.find(pid); it != P.end())
                    prim = regen::MatePrimitive{circle ? regen::MatePrimitive::Kind::Axis : regen::MatePrimitive::Kind::Point,
                                                fr.toWorld(it->second), fr.normal.normalized(), 0};
            }
    if (!prim || prim->kind == regen::MatePrimitive::Kind::Plane) return noHole;
    // A point (a sketch point, a vertex): the hole whose axis passes through it; otherwise the axis line itself.
    const bool byPoint = prim->kind == regen::MatePrimitive::Kind::Point;
    const Vec3 p = prim->p, w = prim->w.normalized();
    const std::string preferPath = preferFace.empty() ? std::string() : naming::splitPath(preferFace).path;
    struct Cyl {
        std::string ref, path;
        Vec3 p, w;
        double r = 0;
        bool convex = true;
    };
    std::vector<Cyl> all;
    for (const auto& e : a->instances) {
        if (!e.state || e.suppressed || e.hidden || !e.connection.empty()) continue;
        for (const auto& fe : e.state->names.faces) {
            const auto si = k_.faceSurface(e.state->bodies.at(size_t(fe.body)).shape, fe.face);
            if (si.kind != kernel::SurfaceInfo::Kind::Cylinder) continue;
            all.push_back({e.path + "/" + fe.full, e.path, e.world.apply(si.point), e.world.applyDir(si.dir).normalized(), si.radius, si.convex});
        }
    }
    auto parallel = [](const Vec3& u, const Vec3& v) { return u.cross(v).norm() <= HOLE_AXIS_ANGLE_TOL; };
    auto coaxial = [&](const Cyl& c, const Vec3& q, const Vec3& d) { return parallel(c.w, d) && (c.p - q).cross(d).norm() <= HOLE_AXIS_TOL; };
    // The hole of the axis: the picked cylinder itself; otherwise a coaxial cylinder — of the preferred instance first,
    // a hole (concave) before a shaft, the smallest radius, then by reference.
    const Cyl* main = nullptr;
    for (const auto& c : all)
        if (c.ref == ref) main = &c;
    if (!main) {
        std::vector<const Cyl*> co;
        for (const auto& c : all)
            if (byPoint ? (p - c.p).cross(c.w).norm() <= HOLE_AXIS_TOL : coaxial(c, p, w)) co.push_back(&c);
        if (co.empty()) return noHole;
        std::sort(co.begin(), co.end(), [&](const Cyl* x, const Cyl* y) {
            return std::make_tuple(x->path != preferPath, x->convex, x->r, x->ref) < std::make_tuple(y->path != preferPath, y->convex, y->r, y->ref);
        });
        main = co.front();
    }
    std::vector<std::string> out{main->ref};
    if (!sameHoles) return out;
    // All the same holes of its part (a hole pattern): the same instance, kind, radius and direction; one face per axis.
    std::vector<const Cyl*> same;
    for (const auto& c : all)
        if (&c != main && c.path == main->path && c.convex == main->convex && std::abs(c.r - main->r) <= HOLE_AXIS_TOL && parallel(c.w, main->w))
            same.push_back(&c);
    std::sort(same.begin(), same.end(), [](const Cyl* x, const Cyl* y) { return x->ref < y->ref; });
    std::vector<const Cyl*> taken{main};
    for (const Cyl* c : same) {
        bool dup = false;
        for (const Cyl* t : taken) dup = dup || coaxial(*c, t->p, t->w);
        if (dup) continue;
        taken.push_back(c);
        out.push_back(c->ref);
    }
    return out;
}

// ---------------------------------------------------------------- assembly analysis (assembly.md §7а)

namespace {
inline constexpr double INTERFERENCE_MIN_VOLUME = 1e-6;  // mm³, assembly.md §8
inline constexpr double INTERFERENCE_THREAD_TOL = 0.01;   // mm: one nominal diameter, one axis of a thread pair (§8)

kernel::Motion kmotion(const regen::Motion3& m) {
    kernel::Motion k;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) k.r[size_t(3 * i + j)] = m.R(i, j);
    k.t = m.t;
    return k;
}

// A thread of an instance in world coordinates (regen.md §4.13: from origin along dir for length).
struct WorldThread {
    Point3 o;
    Vec3 dir;
    double d = 0, length = 0;
    bool internal = false;
};

// Thread zones of a pair of instances (assembly.md §7а, FX10-ASM-005): for every external thread of one and internal
// thread of the other of one nominal diameter on one axis — the cylinder of radius d/2 over the overlap of the two along
// the axis, both widened by INTERFERENCE_THREAD_TOL.
std::vector<kernel::Shape> threadZones(const kernel::Kernel& k, const std::vector<WorldThread>& A, const std::vector<WorldThread>& B) {
    std::vector<kernel::Shape> out;
    for (const auto& a : A)
        for (const auto& b : B) {
            if (a.internal == b.internal || std::abs(a.d - b.d) > INTERFERENCE_THREAD_TOL || std::abs(a.dir.dot(b.dir)) < 1 - 1e-6) continue;
            const Vec3 ob = b.o - a.o;
            if ((ob - ob.dot(a.dir) * a.dir).norm() > INTERFERENCE_THREAD_TOL) continue;
            const double b0 = ob.dot(a.dir), b1 = b0 + b.length * b.dir.dot(a.dir);
            const double lo = std::max(0.0, std::min(b0, b1)) - INTERFERENCE_THREAD_TOL;
            const double hi = std::min(a.length, std::max(b0, b1)) + INTERFERENCE_THREAD_TOL;
            if (hi - lo <= 2 * INTERFERENCE_THREAD_TOL) continue;
            kernel::Profile p;
            p.plane.origin = a.o + lo * a.dir;
            p.plane.normal = a.dir;
            p.plane.x = a.dir.unitOrthogonal();
            p.plane.y = a.dir.cross(p.plane.x);
            kernel::Segment3 c;
            c.id = "s1";
            c.kind = kernel::Segment3::Kind::Circle;
            c.center = p.plane.origin;
            c.radius = a.d / 2 + INTERFERENCE_THREAD_TOL;
            p.outer.segments.push_back(c);
            if (auto z = k.extrude(p, kernel::Extent{kernel::Extent::Kind::Blind, hi - lo, {}}, false)) out.push_back(z->shape);
        }
    return out;
}
}  // namespace

const doc::Element* Session::elementOfEntry(const regen::InstanceEntry& e) const {
    const doc::Document* D = &doc();
    if (!e.file.empty()) {
        auto it = links_.find(e.file);
        if (it == links_.end() || !it->second.doc) return nullptr;
        D = static_cast<const doc::Document*>(it->second.doc);
    }
    const int i = D->elementIndex(e.element);
    return i < 0 ? nullptr : &D->elements[size_t(i)];
}

Result<std::vector<Session::Interference>> Session::interferences() const {
    auto a = assembly();
    if (!a) return Error("API_STATE_UNAVAILABLE");
    struct Leaf {
        const regen::InstanceEntry* e;
        std::vector<kernel::Shape> shapes;
        BBox3 box;
        std::vector<WorldThread> threads;
    };
    std::vector<Leaf> leaves;
    for (const auto& e : a->instances) {
        if (!e.state || e.suppressed || e.error) continue;
        Leaf l{&e, {}, {}, {}};
        l.box.setEmpty();
        for (const auto& t : e.state->threads)
            l.threads.push_back({e.world.apply(t.origin), e.world.applyDir(t.dir).normalized(), t.d, t.length, t.internal});
        for (const auto& b : e.state->bodies) {
            auto t = k_.transform(b.shape, kmotion(e.world));
            if (!t) continue;
            l.shapes.push_back(t->shape);
            l.box.extend(k_.bbox(t->shape));
        }
        leaves.push_back(std::move(l));
    }
    std::vector<Interference> out;
    for (size_t i = 0; i < leaves.size(); ++i)
        for (size_t j = i + 1; j < leaves.size(); ++j) {
            if (leaves[i].box.isEmpty() || leaves[j].box.isEmpty() || !leaves[i].box.intersects(leaves[j].box)) continue;
            // A mating thread pair is no interference in its thread zone (§7а, FX10-ASM-005): the common volume there
            // is the thread modelled at the nominal and minor diameters.
            const auto zones = threadZones(k_, leaves[i].threads, leaves[j].threads);
            double v = 0;
            for (const auto& s1 : leaves[i].shapes)
                for (const auto& s2 : leaves[j].shapes)
                    if (auto r = k_.boolean(kernel::BoolKind::Intersect, s1, s2)) {
                        v += k_.massProperties(r->shape).volume;
                        for (const auto& z : zones)
                            if (auto in = k_.boolean(kernel::BoolKind::Intersect, r->shape, z)) v -= k_.massProperties(in->shape).volume;
                    }
            if (v > INTERFERENCE_MIN_VOLUME) out.push_back({leaves[i].e->path, leaves[j].e->path, v});
        }
    return out;
}

Result<Session::InstanceInfo> Session::instanceInfo(const std::string& path) const {
    auto a = assembly();
    if (!a) return Error("API_STATE_UNAVAILABLE");
    const auto* e = a->find(path);
    if (!e) return notFound(path);
    InstanceInfo out;
    out.name = e->name;
    const doc::Element* el = elementOfEntry(*e);
    if (el) out.designation = el->designation;
    // Designation with the instance's variable values substituted (expr.md §5а.1, assembly.md §2а; S6-PARAM-001).
    if (out.designation && out.designation->find('{') != std::string::npos && e->depth == 0) {
        for (const auto& in : doc().active().instances) {
            if (in.id != e->instance) continue;
            std::string file;
            if (const doc::Document* T = regen::instanceTarget(doc(), in, &links_, selfPath(), &file)) {
                const auto table = regen::instanceVariableTable(regen::documentVariables(doc(), &links_, selfPath()),
                                                                regen::instanceQualifiers(doc()).at(in.id), *T, T == &doc());
                out.designation = expr::substitute(*out.designation, table, regen::documentUnits(*T));
            }
        }
    }
    // Library part copy: designation by the package rule with the instance's values (library.md §7).
    if (e->depth == 0)
        if (const doc::LibraryPart* lp = instanceLibraryPart(e->instance))
            for (const auto& in : doc().active().instances)
                if (in.id == e->instance) {
                    const auto table = regen::instanceVariableTable(regen::documentVariables(doc(), &links_, selfPath()),
                                                                    regen::instanceQualifiers(doc()).at(in.id), *lp->document, false);
                    out.designation = expr::substitute(lp->designation, table, regen::documentUnits(*lp->document));
                    out.library = libraryText(lp->title);  // library.md §17.2
                }
    if (el && el->material) out.material = (*el->material)["name"].get<std::string>();
    // Part - itself; subassembly - the sum over the parts inside.
    bool allMass = true;
    double mass = 0;
    for (const auto& x : a->instances) {
        if (!x.state || x.suppressed || x.error) continue;
        if (!(x.path == path || x.path.rfind(path + "/", 0) == 0)) continue;
        double v = 0;
        for (const auto& b : x.state->bodies) v += k_.massProperties(b.shape).volume;
        out.volume += v;
        const doc::Element* xe = elementOfEntry(x);
        if (xe && xe->material) mass += (*xe->material)["density"].get<double>() * v * 1e-9;
        else allMass = false;
    }
    if (allMass && out.volume > 0) out.mass = mass;
    return out;
}

Result<Session::AssemblyMass> Session::assemblyMassProperties() const {
    auto a = assembly();
    if (!a) return Error("API_STATE_UNAVAILABLE");
    struct Part {
        double rho;  // kg/m³ (without material - 1)
        kernel::MassProperties mp;  // in world coordinates (center and tensor rotated)
    };
    std::vector<Part> parts;
    AssemblyMass out;
    for (const auto& e : a->instances) {
        if (!e.state || e.suppressed || e.error) continue;
        const doc::Element* el = elementOfEntry(e);
        const bool hasMat = el && el->material;
        if (!hasMat) ++out.missingMaterial;
        for (const auto& b : e.state->bodies) {
            kernel::MassProperties mp = k_.massProperties(b.shape);
            mp.centroid = e.world.apply(mp.centroid);
            mp.inertia = e.world.R * mp.inertia * e.world.R.transpose();
            parts.push_back({hasMat ? (*el->material)["density"].get<double>() : 1.0, mp});
        }
    }
    // Weld bodies (weld.md §7а, FX9-WLD-002): the material of the host part (the instance of the first reference); the
    // host without material is already counted in missingMaterial. Bodies are in world coordinates.
    for (const auto& [host, mp] : weldMasses(*a))
        parts.push_back({host && host->material ? (*host->material)["density"].get<double>() : 1.0, mp});
    MassInfo& r = out.props;
    double m = 0;
    Vec3 c = Vec3::Zero();
    for (const auto& p : parts) {
        r.volume += p.mp.volume;
        r.area += p.mp.area;
        const double w = out.missingMaterial == 0 ? p.rho : 1.0;  // without mass - volume center
        m += w * p.mp.volume;
        c += w * p.mp.volume * Vec3(p.mp.centroid);
    }
    if (m > 0) {
        r.centroid = Point3(c / m);
        Eigen::Matrix3d I = Eigen::Matrix3d::Zero();
        for (const auto& p : parts) {
            const double w = out.missingMaterial == 0 ? p.rho : 1.0;
            const Vec3 d = Vec3(p.mp.centroid) - Vec3(r.centroid);
            I += w * (p.mp.inertia + p.mp.volume * (d.squaredNorm() * Eigen::Matrix3d::Identity() - d * d.transpose()));
        }
        I *= 1e-9;
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(I);
        for (int i = 0; i < 3; ++i) {
            r.principalMoments[size_t(i)] = es.eigenvalues()[i];
            r.principalAxes[size_t(i)] = es.eigenvectors().col(i);
        }
    }
    if (out.missingMaterial == 0 && !parts.empty()) r.mass = m * 1e-9;
    return out;
}

regen::StatePtr Session::assemblyVirtual() const {
    std::lock_guard lk(mu_);
    if (!published_ || !published_->assembly || !doc().active().isAssembly()) return nullptr;
    return published_->final;
}

regen::AssemblyPtr Session::assembly() const {
    std::lock_guard lk(mu_);
    if (!published_ || !doc().active().isAssembly()) return nullptr;
    return published_->assembly;
}

Result<io::BodyMesh> Session::instanceSectionTessellation(const std::string& path, const std::string& bodyId,
                                                          const kernel::TessParams& params, const std::vector<Json>& defs,
                                                          double acceptChordTol) {
    auto a = assembly();
    if (!a) return Error("API_STATE_UNAVAILABLE");
    const auto* e = a->find(path);
    if (!e || !e->state) return notFound(path);
    const std::string top = path.substr(0, path.find('/'));
    // Sections effective for the instance: not hidden and not excluding it (sections.md §7а).
    std::vector<doc::Section> active;
    for (const auto& def : defs) {
        if (!def.is_object()) continue;
        doc::Section s = doc::sectionFromJsonValue(def);
        if (s.hidden || std::find(s.exclude.begin(), s.exclude.end(), top) != s.exclude.end()) continue;
        if (!s.plane && s.sketch.empty()) continue;
        active.push_back(s);
    }
    if (active.empty()) return instanceTessellation(path, bodyId, params, acceptChordTol);
    const auto& bodies = e->state->bodies;
    auto it = std::find_if(bodies.begin(), bodies.end(), [&](const regen::Body& b) { return b.id == bodyId; });
    if (it == bodies.end()) return notFound(bodyId);
    Json key = Json::array();
    key.push_back("asm");
    key.push_back(std::to_string(reinterpret_cast<uintptr_t>(e->state.get())));
    key.push_back(path + "/" + bodyId);  // chord - in the nested key (sectionCache_)
    for (int i = 0; i < 9; ++i) key.push_back(e->world.R(i / 3, i % 3));
    for (int i = 0; i < 3; ++i) key.push_back(e->world.t[i]);
    for (const auto& s : active) key.push_back(doc::sectionToJsonValue(s));
    for (const auto& x : a->instances) key.push_back(x.path + std::to_string(x.world.t.x()) + "," + std::to_string(x.world.t.y()) + "," + std::to_string(x.world.t.z()));
    const std::string ks = key.dump();
    if (auto c = sectionCached(ks, params.chordTol, acceptChordTol)) return *c;
    const auto cutAway = assemblyCutAway(*a, defs, top);
    if (!cutAway) return instanceTessellation(path, bodyId, params, acceptChordTol);
    // Into part coordinates: the inverse world motion of the instance.
    const regen::Motion3 inv{e->world.R.transpose(), -(e->world.R.transpose() * e->world.t)};
    kernel::Motion km;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) km.r[size_t(3 * i + j)] = inv.R(i, j);
    km.t = inv.t;
    auto local = k_.transform(*cutAway, km);
    if (!local) return instanceTessellation(path, bodyId, params, acceptChordTol);
    auto m = sectionedBodyMesh(k_, *e->state, int(it - bodies.begin()), {local->shape}, params);
    if (!m) return m.error();
    ++tessellations_;
    sectionPut(ks, params.chordTol, *m);
    return m;
}

std::optional<kernel::Shape> Session::assemblyCutAway(const regen::AssemblyState& a, const std::vector<Json>& defs,
                                                      const std::string& top) const {
    std::vector<doc::Section> active;
    for (const auto& def : defs) {
        if (!def.is_object()) continue;
        doc::Section s = doc::sectionFromJsonValue(def);
        if (s.hidden || std::find(s.exclude.begin(), s.exclude.end(), top) != s.exclude.end()) continue;
        if (!s.plane && s.sketch.empty()) continue;
        active.push_back(s);
    }
    if (active.empty()) return std::nullopt;
    const SectionFrame fr = assemblySectionFrame(k_, a);
    std::vector<kernel::Shape> tools;
    for (const auto& s : active)
        if (auto t = assemblySectionTool(k_, a, doc(), s, fr, assemblyVirtual().get())) tools.push_back(*t);
    if (tools.empty()) return std::nullopt;
    kernel::Shape cutAway = tools.front();
    for (size_t i = 1; i < tools.size(); ++i)
        if (auto r = k_.boolean(kernel::BoolKind::Intersect, cutAway, tools[i])) cutAway = r->shape;
    if (k_.counts(cutAway).faces == 0) return std::nullopt;
    return cutAway;
}

Result<io::BodyMesh> Session::instanceTessellation(const std::string& path, const std::string& bodyId, const kernel::TessParams& params,
                                                   double acceptChordTol) {
    auto a = assembly();
    if (!a) return Error("API_STATE_UNAVAILABLE");
    const auto* e = a->find(path);
    if (!e || !e->state) return notFound(path);
    const auto& bodies = e->state->bodies;
    auto it = std::find_if(bodies.begin(), bodies.end(), [&](const regen::Body& b) { return b.id == bodyId; });
    if (it == bodies.end()) return notFound(bodyId);
    if (!(params.chordTol >= kernel::KERNEL_MIN_CHORD && params.chordTol <= kernel::KERNEL_MAX_CHORD))
        return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("chordTol")).with("actual", params.chordTol);
    const TessCache::Key key{e->state, bodyId, params.chordTol, params.angleTol};
    if (auto cached = tessCache_.get(key, acceptChordTol)) return *cached;
    auto m = bodyMesh(k_, *e->state, static_cast<int>(it - bodies.begin()), params);
    if (!m) return m.error();
    ++tessellations_;
    tessCache_.put(key, *m);
    return m;
}

}  // namespace construe::api
