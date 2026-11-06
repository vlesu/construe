// Session: sections (spec/core-api.md §4.1б).
#include "api/session.h"
#include "regen/hole_tables.h"
#include <Eigen/Eigenvalues>

#include "sketch/trim.h"

#include "api/errors.h"
#include "api/feature_commands.h"
#include "api/sections.h"
#include "regen/mates.h"
#include "font/font.h"
#include "api/sketch_commands.h"

#include "base/ids.h"
#include "base/strings.h"
#include "base/number_format.h"
#include "doc/schema.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <regex>
#include <set>
#include <sstream>

namespace construe::api {

// ---------------------------------------------------------------- §4.1б sections

namespace {

// Section from a definition (without id/name) - via the document, to validate against the schema (core-api §4.1б).
Result<doc::Document> applySection(const doc::Document& base, const std::string& id, const std::string& name, const Json& def,
                                   bool replace) {
    if (!def.is_object()) return Error("API_INVALID_DEFINITION").with("detail", std::string("definition must be an object"));
    for (const char* k : {"id", "name"})
        if (def.contains(k)) return Error("API_INVALID_DEFINITION").with("detail", std::string(k) + " not allowed").at(std::string("/") + k);
    Json sj = Json::object();
    sj["id"] = id;
    sj["name"] = name;
    for (auto it = def.begin(); it != def.end(); ++it) sj[it.key()] = it.value();
    Json dj = doc::documentToJsonValue(base);
    const int ei = base.activeIndex();
    Json& el = dj["elements"][ei];
    if (!el.contains("sections")) el["sections"] = Json::array();
    Json& ss = el["sections"];
    size_t idx = ss.size();
    if (replace)
        for (size_t i = 0; i < ss.size(); ++i)
            if (ss[i]["id"] == id) idx = i;
    if (idx < ss.size()) ss[idx] = sj;
    else ss.push_back(sj);
    auto r = doc::documentFromJson(dj);
    if (!r) {
        Error e = r.error();
        if (e.code == "DOC_SCHEMA") return schemaErrorAt(e, "/elements/" + std::to_string(ei) + "/sections/" + std::to_string(idx));
        e.path.reset();
        return e;
    }
    r->activeElement = base.activeElement;
    return r;
}

Status checkSectionRefs(const doc::Element& el, const Json& def) {
    if (el.isAssembly()) {
        auto need = [&](const std::string& id, doc::FeatureType t) -> Status {
            const auto f = el.find(id);
            if (!f) return notFound(id);
            if (f->type != t) return Error("DOC_BAD_REF_KIND").with("id", id);
            return okStatus();
        };
        if (def.contains("sketch") && def["sketch"].is_string())
            if (auto s = need(def["sketch"].get<std::string>(), doc::FeatureType::Sketch); !s) return s;
        if (def.contains("plane") && def["plane"].is_object() && def["plane"].contains("feature") && def["plane"]["feature"].is_string())
            if (auto s = need(def["plane"]["feature"].get<std::string>(), doc::FeatureType::Plane); !s) return s;
        if (def.contains("exclude") && def["exclude"].is_array())
            for (const auto& x : def["exclude"]) {
                bool found = false;
                for (const auto& in : el.instances) found = found || (x.is_string() && in.id == x.get<std::string>());
                if (!found) return notFound(x.is_string() ? x.get<std::string>() : std::string());
            }
        if (def.contains("offset") && def["offset"].is_string())
            if (auto a = expr::parse(def["offset"].get<std::string>()); !a) return a.error();
        return okStatus();
    }
    auto need = [&](const std::string& id, doc::FeatureType t) -> Status {
        const auto f = el.find(id);
        if (!f) return notFound(id);
        if (f->type != t) return Error("DOC_BAD_REF_KIND").with("id", id);
        return okStatus();
    };
    if (def.contains("sketch") && def["sketch"].is_string())
        if (auto s = need(def["sketch"].get<std::string>(), doc::FeatureType::Sketch); !s) return s;
    if (def.contains("plane") && def["plane"].is_object() && def["plane"].contains("feature") && def["plane"]["feature"].is_string())
        if (auto s = need(def["plane"]["feature"].get<std::string>(), doc::FeatureType::Plane); !s) return s;
    if (def.contains("offset") && def["offset"].is_string())
        if (auto a = expr::parse(def["offset"].get<std::string>()); !a) return a.error();
    return okStatus();
}

const doc::Section* findSection(const doc::Element& el, const std::string& id) {
    for (const auto& s : el.sections)
        if (s.id == id) return &s;
    return nullptr;
}

// Definition (without id/name) → Section; the rough shape is checked here, the rest - by the section status.
std::optional<doc::Section> sectionFromDef(const Json& def) {
    if (!def.is_object()) return std::nullopt;
    if (def.contains("plane")) {
        const Json& p = def["plane"];
        if (!p.is_object() || p.size() != 1) return std::nullopt;
        const auto it = p.begin();
        if ((it.key() != "datum" && it.key() != "feature" && it.key() != "face") || !it.value().is_string()) return std::nullopt;
        if (!def.contains("offset") || !def["offset"].is_string()) return std::nullopt;
    } else if (!def.contains("sketch") || !def["sketch"].is_string()) {
        return std::nullopt;
    }
    return doc::sectionFromJsonValue(def);
}

}  // namespace

Result<std::string> Session::addSection(const Json& def, const std::optional<std::string>& name) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (doc().active().isDrawing() || doc().active().isSpecification())  // drawing, specification (§4.1г, §4.1е)
        return Error("API_NOT_PART").with("id", doc().active().id);
    std::string n;
    if (name) {
        n = trim(*name);
        if (n.empty()) return Error("API_INVALID_NAME").with("name", *name);
    } else {
        std::set<std::string> taken;
        for (const auto& s : doc().active().sections) taken.insert(s.name);
        n = doc::firstFreeName(doc::nameBase("section"), taken);
    }
    doc::Document nd = doc();
    const std::string id = doc::allocate(nd, doc::IdKind::Feature);
    auto r = applySection(nd, id, n, def, false);
    if (!r) return r.error();
    if (auto s = checkSectionRefs(doc().active(), def); !s) return s.error();
    if (auto s = commit(std::move(*r), "undo.addSection", {}, id); !s) return s.error();
    return id;
}

Status Session::updateSection(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    const doc::Section* sec = findSection(doc().active(), id);
    if (!sec) return notFound(id);
    auto r = applySection(doc(), id, sec->name, def, true);
    if (!r) return r.error();
    if (auto s = checkSectionRefs(doc().active(), def); !s) return s;
    return commit(std::move(*r), "undo.editSection", {}, id);
}

Status Session::deleteSection(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!findSection(doc().active(), id)) return notFound(id);
    doc::Document nd = doc();
    std::erase_if(nd.active().sections, [&](const doc::Section& s) { return s.id == id; });
    return commit(std::move(nd), "undo.deleteSection", {}, id);
}

Status Session::renameSection(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!findSection(doc().active(), id)) return notFound(id);
    const std::string n = trim(name);
    if (n.empty()) return Error("API_INVALID_NAME").with("name", name);
    doc::Document nd = doc();
    for (auto& s : nd.active().sections)
        if (s.id == id) s.name = n;
    return commit(std::move(nd), "undo.renameSection", {}, id);
}

Status Session::setSectionHidden(const std::string& id, bool hidden) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!findSection(doc().active(), id)) return notFound(id);
    doc::Document nd = doc();
    for (auto& s : nd.active().sections)
        if (s.id == id) s.hidden = hidden;
    return commit(std::move(nd), "undo.sectionVisibility", {}, id);
}

std::vector<Session::SectionInfo> Session::sections() const {
    std::vector<SectionInfo> out;
    if (doc().active().isAssembly()) {  // assembly sections (sections.md §7а)
        auto a = assembly();
        std::optional<SectionFrame> afr;
        for (const auto& s : doc().active().sections) {
            SectionInfo si{s.id, s.name, doc::sectionToJsonValue(s), s.hidden, std::nullopt};
            si.def.erase("id"), si.def.erase("name");
            if (!s.hidden && a) {
                if (!afr) afr = assemblySectionFrame(k_, *a);
                if (auto t = assemblySectionTool(k_, *a, doc(), s, *afr, assemblyVirtual().get()); !t) si.error = t.error();
            }
            out.push_back(std::move(si));
        }
        return out;
    }
    auto st = stateOf(StateRef::final());
    std::optional<SectionFrame> fr;
    for (const auto& s : doc().active().sections) {
        SectionInfo si{s.id, s.name, doc::sectionToJsonValue(s), s.hidden, std::nullopt};
        si.def.erase("id"), si.def.erase("name");
        if (!s.hidden) {
            if (!st) {
                si.error = st.error();
            } else {
                if (!fr) fr = sectionFrame(k_, **st);
                auto t = sectionTool(k_, **st, doc(), s, *fr);
                if (!t) si.error = t.error();
            }
        }
        out.push_back(std::move(si));
    }
    return out;
}

Result<std::optional<Error>> Session::sectionStatus(const Json& def, const StateRef& ref) const {
    if (doc().active().isAssembly()) {
        auto a = assembly();
        if (!a) return Error("API_STATE_UNAVAILABLE");
        const auto s = sectionFromDef(def);
        if (!s) return std::optional<Error>(Error("API_INVALID_DEFINITION").with("detail", std::string("section definition")));
        auto t = assemblySectionTool(k_, *a, doc(), *s, assemblySectionFrame(k_, *a), assemblyVirtual().get());
        if (!t) return std::optional<Error>(t.error());
        return std::optional<Error>();
    }
    auto st = stateOf(ref);
    if (!st) return st.error();
    const auto s = sectionFromDef(def);
    if (!s) return std::optional<Error>(Error("API_INVALID_DEFINITION").with("detail", std::string("section definition")));
    auto t = sectionTool(k_, **st, doc(), *s, sectionFrame(k_, **st));
    if (!t) return std::optional<Error>(t.error());
    return std::optional<Error>();
}

Result<Plane> Session::sectionPlane(const Json& def, const StateRef& ref) const {
    auto st = stateOf(ref);
    if (!st) return st.error();
    const auto s = sectionFromDef(def);
    if (!s || !s->plane) return Error("SECTION_BAD_REF").with("ref", std::string("plane"));
    return api::sectionPlane(k_, **st, doc(), *s);
}

std::optional<io::BodyMesh> Session::sectionCached(const std::string& key, double chordTol, double acceptChordTol) const {
    const auto slot = sectionCache_.find(key);
    if (slot == sectionCache_.end()) return std::nullopt;
    if (acceptChordTol <= 0) {
        if (auto c = slot->second.find(chordTol); c != slot->second.end()) return c->second;
        return std::nullopt;
    }
    const io::BodyMesh* best = nullptr;  // largest chord ≤ max(acceptChordTol, chordTol) (§5.2, FX3-PERF-002)
    for (const auto& [ch, m] : slot->second)
        if (ch <= std::max(acceptChordTol, chordTol)) best = &m;
    if (best) return *best;
    return std::nullopt;
}

void Session::sectionPut(const std::string& key, double chordTol, const io::BodyMesh& m) {
    size_t n = 0;
    for (const auto& [k, v] : sectionCache_) n += v.size();
    if (n >= TESS_CACHE_MAX_ENTRIES) sectionCache_.clear();
    sectionCache_[key][chordTol] = m;
}

Result<io::BodyMesh> Session::sectionTessellation(const std::string& bodyId, const StateRef& ref, const kernel::TessParams& params,
                                                  const std::vector<Json>& defs, double acceptChordTol) {
    auto st = stateOf(ref);
    if (!st) return st.error();
    const auto& bodies = (*st)->bodies;
    auto it = std::find_if(bodies.begin(), bodies.end(), [&](const regen::Body& b) { return b.id == bodyId; });
    if (it == bodies.end()) return notFound(bodyId);
    std::vector<doc::Section> active;
    for (const auto& def : defs)
        if (auto s = sectionFromDef(def); s && !s->hidden) active.push_back(*s);
    if (active.empty()) return tessellation(bodyId, ref, params, acceptChordTol);
    if (!(params.chordTol >= kernel::KERNEL_MIN_CHORD && params.chordTol <= kernel::KERNEL_MAX_CHORD))
        return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("chordTol")).with("actual", params.chordTol);
    // Cache key (sections.md §7): state, body, parameters, sections, units and document variables.
    Json key = Json::array();
    key.push_back(std::to_string(reinterpret_cast<uintptr_t>(st->get())));
    key.push_back(bodyId);
    key.push_back(params.angleTol);  // chord - in the nested key (sectionCache_)
    for (const auto& s : active) key.push_back(doc::sectionToJsonValue(s));
    key.push_back(std::string(doc().lengthUnit) + "/" + doc().angleUnit);
    for (const auto& v : doc().variables) key.push_back(v.name + "=" + v.expr);
    const std::string ks = key.dump();
    if (auto c = sectionCached(ks, params.chordTol, acceptChordTol)) return *c;
    const SectionFrame fr = sectionFrame(k_, **st);
    std::vector<kernel::Shape> tools;
    for (const auto& s : active)
        if (auto t = sectionTool(k_, **st, doc(), s, fr)) tools.push_back(*t);
    if (tools.empty()) return tessellation(bodyId, ref, params, acceptChordTol);
    // Common part of the cut-away regions (sections.md §4 item 1, Р-031): two planes - a quarter cut.
    kernel::Shape cutAway = tools.front();
    for (size_t i = 1; i < tools.size(); ++i)
        if (auto r = k_.boolean(kernel::BoolKind::Intersect, cutAway, tools[i])) cutAway = r->shape;
    if (k_.counts(cutAway).faces == 0) return tessellation(bodyId, ref, params, acceptChordTol);
    auto m = sectionedBodyMesh(k_, **st, int(it - bodies.begin()), {cutAway}, params);
    if (!m) return m.error();
    ++tessellations_;
    sectionPut(ks, params.chordTol, *m);
    return m;
}

}  // namespace construe::api
