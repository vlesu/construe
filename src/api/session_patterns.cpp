// Session: exploded view and instance patterns of an assembly (spec/core-api.md §4.1в; spec/assembly.md §7д, §7е).
#include "api/session.h"

#include "api/errors.h"
#include "base/strings.h"

#include <set>

namespace construe::api {

namespace {

Error patternDefError(const std::string& detail) { return Error("API_INVALID_DEFINITION").with("detail", detail).at("/" + detail); }

}  // namespace

// ---------------------------------------------------------------- exploded view (assembly.md §7д.4)

Status Session::setExploded(bool active, bool reset) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    doc::Document nd = doc();
    auto& x = nd.active().exploded;
    if (active) {
        if (!x || reset) x = doc::Exploded{};  // placements absent — automatic explosion at the next regeneration
        x->active = true;
    } else if (x) {
        x->active = false;
        if (!x->placements && x->suppressedMates.empty()) x.reset();
    }
    if (nd.active().exploded == doc().active().exploded) return okStatus();
    const std::string asmId = nd.active().id;
    return commit(std::move(nd), "undo.exploded", {}, asmId);
}

// ---------------------------------------------------------------- instance patterns (assembly.md §7е)

// Document with pattern record p (with id and name) at the end of patterns or in place of the record with its id;
// checks of core-api §4.1в in order: schema, source, references, expressions.
Result<doc::Document> Session::withInstancePattern(const doc::Document& base, const Json& p) const {
    const int ei = base.activeIndex();
    Json dj = doc::documentToJsonValue(base);
    Json& el = dj["elements"][ei];
    if (!el.contains("patterns")) el["patterns"] = Json::array();
    Json& ps = el["patterns"];
    size_t idx = ps.size();
    for (size_t i = 0; i < ps.size(); ++i)
        if (ps[i]["id"] == p["id"]) idx = i;
    if (idx < ps.size()) ps[idx] = p;
    else ps.push_back(p);
    auto r = doc::documentFromJson(dj);
    if (!r) {
        const Error& e = r.error();
        const std::string prefix = "/elements/" + std::to_string(ei) + "/patterns/" + std::to_string(idx);
        std::string path = schemaPathIn(e, prefix);
        std::string field = path.size() > 1 ? path.substr(1, path.find('/', 1) == std::string::npos ? std::string::npos : path.find('/', 1) - 1) : "";
        if (field.empty())
            if (const std::string* name = e.stringParam("name")) field = *name;
        Error out("API_INVALID_DEFINITION");
        out.with("detail", field.empty() ? e.code : field);
        if (!path.empty()) out.at(path);
        return out;
    }
    r->activeElement = base.activeElement;
    const doc::Element& ne = r->active();
    const auto& ins = ne.instances;
    const std::string src = p.value("source", std::string());
    if (std::none_of(ins.begin(), ins.end(), [&](const doc::Instance& in) { return in.id == src; })) return patternDefError("source");
    // References — as mate references (core-api §4.1в addMate).
    auto refOk = [&](const std::string& ref) {
        return checkMateDefinition(Json{{"type", "coincident"}, {"refs", Json::array({ref, "origin"})}}).ok();
    };
    const std::string mode = p.value("mode", std::string());
    std::vector<std::pair<std::string, std::string>> exprs;  // (field, expression)
    if (mode == "linear") {
        for (const auto& d : p["directions"]) {
            if (!refOk(d["ref"].get<std::string>())) return patternDefError("ref");
            exprs.push_back({"count", d["count"]});
            exprs.push_back({"spacing", d["spacing"]});
        }
    } else if (mode == "circular") {
        if (!refOk(p["axis"].get<std::string>())) return patternDefError("axis");
        exprs.push_back({"count", p["count"]});
        exprs.push_back({"angle", p["angle"]});
    } else {
        const std::string path = p["feature"]["instance"].get<std::string>();
        const std::string top = path.substr(0, path.find('/'));
        if (std::none_of(ins.begin(), ins.end(), [&](const doc::Instance& in) { return in.id == top; })) return patternDefError("feature");
    }
    for (const auto& [field, e] : exprs)
        if (!expr::parse(e)) return patternDefError(field);
    return r;
}

Result<std::string> Session::addInstancePattern(const Json& def, const std::optional<std::string>& name) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    if (!def.is_object()) return Error("API_INVALID_DEFINITION").with("detail", std::string("definition must be an object"));
    if (def.contains("id") || def.contains("name")) return Error("API_INVALID_DEFINITION").with("detail", std::string("id and name not allowed"));
    std::string n;
    if (name) {
        n = trim(*name);
        if (n.empty()) return Error("API_INVALID_NAME").with("name", *name);
    }
    syncAssemblyPositions();
    doc::Document probe = doc();
    const std::string id = doc::allocate(probe, doc::IdKind::Pattern);
    if (n.empty()) {  // «Instance pattern» + N (document.md §2.12): N — among the patterns of this assembly
        std::set<std::string> taken;
        for (const auto& p : probe.active().patterns) taken.insert(p.name);
        n = doc::firstFreeName(doc::nameBase("instancePattern"), taken);
    }
    Json p = Json{{"id", id}, {"name", n}};
    for (auto it = def.begin(); it != def.end(); ++it) p[it.key()] = it.value();
    auto r = withInstancePattern(probe, p);
    if (!r) return r.error();
    if (auto s = commit(std::move(*r), "undo.addInstancePattern", {}, id); !s) return s.error();
    return id;
}

Status Session::updateInstancePattern(const std::string& id, const Json& def) {
    if (auto s = checkNoDrag(); !s) return s;
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    const int pi = doc().active().patternIndex(id);
    if (pi < 0) return notFound(id);
    if (!def.is_object()) return Error("API_INVALID_DEFINITION").with("detail", std::string("definition must be an object"));
    if (def.contains("id") || def.contains("name")) return Error("API_INVALID_DEFINITION").with("detail", std::string("id and name not allowed"));
    syncAssemblyPositions();
    Json p = doc::instancePatternToJsonValue(doc().active().patterns[size_t(pi)]);
    // A new mode drops the fields of the previous one that def does not give (core-api §4.1в).
    if (def.contains("mode") && def["mode"] != p["mode"])
        for (const char* k : {"directions", "axis", "count", "angle", "reverse", "feature"})
            if (!def.contains(k)) p.erase(k);
    for (auto it = def.begin(); it != def.end(); ++it) {
        if (it->is_null() || (it->is_boolean() && !it->get<bool>())) p.erase(it.key());
        else p[it.key()] = it.value();
    }
    if (p == doc::instancePatternToJsonValue(doc().active().patterns[size_t(pi)])) return okStatus();
    auto r = withInstancePattern(doc(), p);
    if (!r) return r.error();
    return commit(std::move(*r), "undo.editInstancePattern", {}, id);
}

Status Session::deleteInstancePattern(const std::string& id) {
    if (auto s = checkNoDrag(); !s) return s;
    if (doc().active().patternIndex(id) < 0) return notFound(id);
    syncAssemblyPositions();
    doc::Document nd = doc();
    auto& ps = nd.active().patterns;
    ps.erase(ps.begin() + nd.active().patternIndex(id));
    return commit(std::move(nd), "undo.deleteInstancePattern", {}, id);
}

Status Session::renameInstancePattern(const std::string& id, const std::string& name) {
    if (auto s = checkNoDrag(); !s) return s;
    const int pi = doc().active().patternIndex(id);
    if (pi < 0) return notFound(id);
    const std::string n = trim(name);
    if (n.empty()) return Error("API_INVALID_NAME").with("name", name);
    doc::Document nd = doc();
    nd.active().patterns[size_t(pi)].name = n;
    return commit(std::move(nd), "undo.renameInstancePattern", {}, id);
}

Status Session::setInstancePatternSuppressed(const std::string& id, bool suppressed) {
    if (auto s = checkNoDrag(); !s) return s;
    const int pi = doc().active().patternIndex(id);
    if (pi < 0) return notFound(id);
    syncAssemblyPositions();
    doc::Document nd = doc();
    auto& def = nd.active().patterns[size_t(pi)].def;
    if (suppressed) def["suppressed"] = true;
    else def.erase("suppressed");
    if (nd.active().patterns == doc().active().patterns) return okStatus();
    return commit(std::move(nd), "undo.instancePatternSuppressed", {}, id);
}

}  // namespace construe::api
