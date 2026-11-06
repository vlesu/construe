// Linked sketch groups in CoreApi (spec/sketch.md §2.6, spec/core-api.md §4.6.2).
#include "api/sketch_groups.h"

#include "api/errors.h"
#include "regen/regen.h"
#include "sketch/offset.h"
#include "sketch/primitives.h"

#include <cctype>
#include <cmath>
#include <numbers>
#include <set>

namespace construe::api {

std::map<std::string, sketch::GroupParam> groupParamsFromJson(const Json& params) {
    std::map<std::string, sketch::GroupParam> out;
    if (!params.is_object()) return out;
    for (auto it = params.begin(); it != params.end(); ++it) {
        const Json& v = it.value();
        if (v.is_string()) out[it.key()] = sketch::GroupParam::str(v.get<std::string>());
        else if (v.is_boolean()) out[it.key()] = sketch::GroupParam::boolean(v.get<bool>());
        else if (v.is_number()) out[it.key()] = sketch::GroupParam::num(v.get<double>());
    }
    return out;
}

Json groupParamsToJson(const std::map<std::string, sketch::GroupParam>& params) {
    Json p = Json::object();
    for (const auto& [k, v] : params) {
        switch (v.kind) {
            case sketch::GroupParam::Kind::Number: p[k] = v.number; break;
            case sketch::GroupParam::Kind::Text: p[k] = v.text; break;
            case sketch::GroupParam::Kind::Flag: p[k] = v.flag; break;
        }
    }
    return p;
}

namespace {

// Parameter fields: only the listed ones, each is required (schema, sketch.md §2.6.3).
Status checkKeys(const Json& params, const std::vector<std::string>& keys) {
    if (!params.is_object()) return badArg("params");
    for (const auto& k : keys)
        if (!params.contains(k)) return badArg(k);
    for (auto it = params.begin(); it != params.end(); ++it)
        if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) return badArg(it.key());
    return okStatus();
}

// Expression with the document variables (sketch.md §2.6.2 item 1, S4-SKT-006).
Result<double> constant(const Json& params, const std::string& key, expr::Dim dim, const expr::DocumentUnits& units,
                        const expr::Env& env) {
    const Json& v = params[key];
    if (!v.is_string() || v.get<std::string>().empty()) return badArg(key);
    const expr::Env none = [](const std::string&) -> expr::EnvValue { return std::nullopt; };
    auto q = expr::evaluateField(v.get<std::string>(), dim, env ? env : none, units);
    if (!q) return badArg(key);
    return q->value;
}

// Instance count: an integer or a dimensionless expression with an integer value (sketch.md §2.6.3, S4-SKT-006).
Result<int> count(const Json& params, const std::string& key, int lo, const expr::DocumentUnits& units, const expr::Env& env) {
    const Json& v = params[key];
    double d = 0;
    if (v.is_number()) {
        d = v.get<double>();
        if (d != std::floor(d)) return badArg(key);
    } else {
        auto q = constant(params, key, expr::DIMLESS, units, env);
        if (!q) return q.error();
        d = std::round(*q);
        if (std::abs(*q - d) > sketch::PATTERN_COUNT_TOL) return badArg(key);
    }
    if (d < lo || d > sketch::PATTERN_MAX_INSTANCES) return badArg(key);
    return static_cast<int>(d);
}

// Array sources (sketch.md §2.6.3).
Status checkPatternSources(const sketch::SketchData& s, const std::vector<std::string>& sources, const std::string& selfId) {
    if (sources.empty()) return invalidItems("", "sources");
    std::set<std::string> owned, seen;
    for (const auto& e : s.entities)
        for (const auto& p : sketch::ownedPoints(e)) owned.insert(p);
    const sketch::Group* self = s.findGroup(selfId);
    for (const auto& id : sources) {
        const sketch::Entity* e = s.findEntity(id);
        if (!e) return invalidItems(id, "sources");
        const bool kindOk = e->type == sketch::EntityType::Line || e->type == sketch::EntityType::Circle ||
                            e->type == sketch::EntityType::Arc || e->type == sketch::EntityType::Ellipse ||  // ellipse — FX7-SK-012
                            (e->type == sketch::EntityType::Point && !owned.count(id));
        if (!kindOk || !seen.insert(id).second || (self && self->hasMember(id))) return invalidItems(id, "sources");
    }
    return okStatus();
}

}  // namespace

Result<std::vector<sketch::GroupItem>> generateGroup(const sketch::SketchData& s, const std::string& type,
                                                     const std::vector<std::string>& sources, const Json& params,
                                                     const expr::DocumentUnits& units, const expr::Env& env,
                                                     const std::string& selfId, const sketch::Group* old) {
    if (type == "linearPattern") {
        const bool withLine = params.is_object() && params.contains("line");
        const bool withLine2 = params.is_object() && params.contains("line2");  // FX12-FEAT-001
        std::vector<std::string> keys = {"count1", "step1", "angle1", "count2", "step2", "angle2"};
        if (withLine) keys.push_back("line");
        if (withLine2) keys.push_back("line2");
        if (auto st = checkKeys(params, keys); !st) return st.error();
        sketch::LinearPattern p;
        auto c1 = count(params, "count1", 1, units, env);
        if (!c1) return c1.error();
        auto c2 = count(params, "count2", 1, units, env);
        if (!c2) return c2.error();
        p.count1 = *c1, p.count2 = *c2;
        if (p.count1 * p.count2 < 2 || p.count1 * p.count2 > sketch::PATTERN_MAX_INSTANCES) return badArg("count1");
        for (auto [key, out, dim] : {std::tuple{"step1", &p.step1, expr::LENGTH}, std::tuple{"angle1", &p.angle1, expr::ANGLE},
                                     std::tuple{"step2", &p.step2, expr::LENGTH}, std::tuple{"angle2", &p.angle2, expr::ANGLE}}) {
            auto v = constant(params, key, dim, units, env);
            if (!v) return v.error();
            if (dim == expr::LENGTH && *v <= 0) return badArg(key);
            *out = *v;
        }
        // Axis segments (sketch.md §2.6.3, S4-SKT-006, FX12-FEAT-001): non-degenerate segments, not members.
        for (const char* key : {"line", "line2"}) {
            if (!params.contains(key)) continue;
            if (!params[key].is_string()) return badArg(key);
            const std::string id = params[key].get<std::string>();
            const sketch::Entity* l = s.findEntity(id);
            const sketch::Group* self = s.findGroup(selfId);
            if (!l || l->type != sketch::EntityType::Line || (self && self->hasMember(id))) return badArg(key);
            const sketch::Entity* a = s.findEntity(l->p1);
            const sketch::Entity* b = s.findEntity(l->p2);
            if (!a || !b || std::hypot(b->x - a->x, b->y - a->y) <= 1e-9) return badArg(key);
            (std::string(key) == "line" ? p.line : p.line2) = id;
        }
        if (auto st = checkPatternSources(s, sources, selfId); !st) return st.error();
        return sketch::linearPatternItems(s, sources, p);
    }
    if (type == "circularPattern") {
        if (auto st = checkKeys(params, {"center", "count", "angle"}); !st) return st.error();
        sketch::CircularPattern p;
        if (!params["center"].is_string()) return badArg("center");
        p.center = params["center"].get<std::string>();
        if (p.center != "origin") {
            const sketch::Entity* c = s.findEntity(p.center);
            const sketch::Group* self = s.findGroup(selfId);
            if (!c || c->type != sketch::EntityType::Point || std::find(sources.begin(), sources.end(), p.center) != sources.end() ||
                (self && self->hasMember(p.center)))
                return badArg("center");
        }
        auto n = count(params, "count", 2, units, env);
        if (!n) return n.error();
        p.count = *n;
        auto a = constant(params, "angle", expr::ANGLE, units, env);
        if (!a) return a.error();
        if (!(std::abs(*a) > 0 && std::abs(*a) <= 2 * std::numbers::pi + sketch::PATTERN_FULL_TOL)) return badArg("angle");
        p.angle = *a;
        if (auto st = checkPatternSources(s, sources, selfId); !st) return st.error();
        return sketch::circularPatternItems(s, sources, p);
    }
    if (type == "polygon" || type == "star") {  // primitives (sketch.md §2.6.4, S4-SKT-003)
        if (!sources.empty()) return invalidItems("", "sources");
        const bool poly = type == "polygon";
        if (auto st = checkKeys(params, poly ? std::vector<std::string>{"sides", "inscribed", "x", "y", "size", "angle"}
                                             : std::vector<std::string>{"rays", "d1", "d2", "x", "y", "angle"});
            !st)
            return st.error();
        auto n = count(params, poly ? "sides" : "rays", 3, units, env);
        if (!n || *n > sketch::PRIMITIVE_MAX_SIDES) return badArg(poly ? "sides" : "rays");
        sketch::PrimitivePlacement start;
        for (auto [key, out] : {std::pair{"x", &start.x}, std::pair{"y", &start.y}, std::pair{"angle", &start.angle}}) {
            if (!params[key].is_number()) return badArg(key);
            *out = params[key].get<double>();
        }
        if (poly) {
            if (!params["inscribed"].is_boolean()) return badArg("inscribed");
            if (!params["size"].is_number() || !(params["size"].get<double>() > 0)) return badArg("size");
            start.size = params["size"].get<double>();
            sketch::PolygonParams p;
            p.sides = *n;
            p.inscribed = params["inscribed"].get<bool>();
            p.start = start;
            return sketch::polygonItems(s, old, p);
        }
        sketch::StarParams p;
        p.rays = *n;
        p.start = start;
        auto d1 = constant(params, "d1", expr::LENGTH, units, env);
        if (!d1 || !(*d1 > 0)) return badArg("d1");
        auto d2 = constant(params, "d2", expr::LENGTH, units, env);
        if (!d2 || !(*d2 > 0) || std::abs(*d2 - *d1) <= 1e-9 * std::max(*d1, *d2)) return badArg("d2");
        p.d1 = *d1, p.d2 = *d2;
        p.d1Text = params["d1"].get<std::string>(), p.d2Text = params["d2"].get<std::string>();
        return sketch::starItems(s, old, p);
    }
    if (type == "offset") {  // offset (sketch.md §2.6.5, S7F-SK-005)
        if (auto st = checkKeys(params, {"distance"}); !st) return st.error();
        auto v = constant(params, "distance", expr::LENGTH, units, env);
        if (!v || !(std::abs(*v) > sketch::OFFSET_MIN)) return badArg("distance");
        auto r = sketch::offsetItems(s, sources, *v, offsetValueText(params["distance"].get<std::string>(), *v, units, env), selfId);
        if (r.error == "sources") return invalidItems(r.errorId, "sources");
        if (!r.error.empty()) return badArg("distance");
        return std::move(r.items);
    }
    return badArg("type");
}

std::string offsetValueText(const std::string& text, double value, const expr::DocumentUnits& units, const expr::Env& env) {
    if (value > 0) return text;
    // Without the leading minus, when the rest has the opposite value; otherwise "-(text)" (sketch.md §2.6.5).
    std::string t = text;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
    if (!t.empty() && t.front() == '-') {
        const std::string rest = t.substr(1);
        const expr::Env none = [](const std::string&) -> expr::EnvValue { return std::nullopt; };
        if (auto q = expr::evaluateField(rest, expr::LENGTH, env ? env : none, units); q && std::abs(q->value + value) <= 1e-9 * std::max(1.0, -value))
            return rest;
    }
    return "-(" + text + ")";
}

Result<sketch::SketchData> rebuildGroup(const sketch::SketchData& s, const std::string& groupId, const std::string& type,
                                        const std::vector<std::string>& sources, const Json& params,
                                        const expr::DocumentUnits& units, const expr::Env& env, const Allocate& allocate) {
    const sketch::Group* old = s.findGroup(groupId);
    auto items = generateGroup(s, type, sources, params, units, env, groupId, old);
    if (!items) return items.error();
    sketch::Group g = old ? *old : sketch::Group{};
    g.id = groupId;
    g.type = type;
    g.sources = sources;
    g.params = groupParamsFromJson(params);
    return sketch::applyGroupItems(s, std::move(g), *items, allocate);
}

sketch::SketchData reconcileGroups(sketch::SketchData s, const expr::DocumentUnits& units, const expr::Env& env,
                                   const Allocate& allocate) {
    // Each iteration either changes nothing or deletes/rebuilds a group; a rebuild restores the members, so
    // the number of iterations is bounded by the number of groups (cascade: a group on members of another group).
    for (size_t pass = 0; pass <= s.groups.size() + 1; ++pass) {
        bool changed = false;
        for (size_t k = 0; k < s.groups.size() && !changed; ++k) {
            const sketch::Group g = s.groups[k];
            std::vector<std::string> sources;
            for (const auto& id : g.sources)
                if (s.findEntity(id)) sources.push_back(id);
            bool memberLost = false;
            for (const auto& m : g.members) memberLost = memberLost || (!s.findEntity(m.id) && !s.findConstraint(m.id));
            bool centerLost = false;
            if (auto it = g.params.find("center"); it != g.params.end() && it->second.text != "origin")
                centerLost = !s.findEntity(it->second.text);
            for (const char* key : {"line", "line2"})  // axis segments (S4-SKT-006, FX12-FEAT-001)
                if (auto it = g.params.find(key); it != g.params.end()) centerLost = centerLost || !s.findEntity(it->second.text);
            const bool needsSources = g.type == "linearPattern" || g.type == "circularPattern";
            if (g.type == "offset" && (sources != g.sources || memberLost)) {
                // An offset whose source or member disappeared is unlinked: the offset contour stays as ordinary
                // geometry (widen a hole by an offset, then trim the original away; sketch.md §2.6.2, S7F-SK-005).
                s = sketch::removeGroup(std::move(s), g.id, true);
                changed = true;
            } else if (centerLost || (needsSources && sources.empty())) {
                s = sketch::removeGroup(std::move(s), g.id, false);
                changed = true;
            } else if (sources != g.sources || memberLost) {
                auto r = rebuildGroup(s, g.id, g.type, sources, groupParamsToJson(g.params), units, env, allocate);
                s = r ? std::move(*r) : sketch::removeGroup(std::move(s), g.id, false);
                changed = true;
            }
        }
        if (!changed) break;
    }
    return s;
}

doc::Document rebuildVariableGroups(doc::Document d) {
    const auto units = regen::documentUnits(d);
    const expr::Env env = regen::documentVariables(d).env();
    // A group references variables if one of its expression parameters references a name.
    auto usesNames = [](const sketch::Group& g) {
        for (const auto& [k, v] : g.params) {
            if (v.kind != sketch::GroupParam::Kind::Text || k == "center" || k == "line" || k == "line2") continue;
            auto ast = expr::parse(v.text);
            if (ast && !expr::references(*ast).empty()) return true;
        }
        return false;
    };
    for (size_t ei = 0; ei < d.elements.size(); ++ei) {
        for (size_t fi = 0; fi < d.elements[ei].features.size(); ++fi) {
            const doc::FeaturePtr& f = d.elements[ei].features[fi];
            const sketch::SketchData* sd0 = f ? f->sketchData() : nullptr;
            if (!sd0 || sd0->groups.empty()) continue;
            sketch::SketchData sd = *sd0;
            bool changed = false;
            const Allocate alloc = [&](bool entity) { return doc::allocate(d, entity ? doc::IdKind::Entity : doc::IdKind::Constraint); };
            for (size_t k = 0; k < sd.groups.size(); ++k) {
                const sketch::Group g = sd.groups[k];
                if (!usesNames(g)) continue;
                if (auto r = rebuildGroup(sd, g.id, g.type, g.sources, groupParamsToJson(g.params), units, env, alloc)) {
                    sd = reconcileGroups(std::move(*r), units, env, alloc);
                    changed = true;
                }
            }
            if (!changed) continue;
            auto nf = std::make_shared<doc::Feature>(*f);
            nf->data = std::move(sd);
            d.elements[ei].features[fi] = nf;
        }
    }
    return d;
}

doc::Document migrateOffsetGroups(doc::Document d) {
    const auto units = regen::documentUnits(d);
    const expr::Env env = regen::documentVariables(d).env();
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t fi = 0; fi < d.elements[ei].features.size(); ++fi) {
            const doc::FeaturePtr& f = d.elements[ei].features[fi];
            const sketch::SketchData* sd0 = f ? f->sketchData() : nullptr;
            if (!sd0 || sd0->groups.empty()) continue;
            sketch::SketchData sd = *sd0;
            bool changed = false;
            const Allocate alloc = [&](bool entity) { return doc::allocate(d, entity ? doc::IdKind::Entity : doc::IdKind::Constraint); };
            for (size_t k = 0; k < sd.groups.size(); ++k) {
                const sketch::Group g = sd.groups[k];
                if (g.type != "offset") continue;
                const Json params = groupParamsToJson(g.params);
                auto items = generateGroup(sd, g.type, g.sources, params, units, env, g.id, &g);
                if (!items) continue;  // e.g. projections not computed yet — the group stays as it is
                std::set<std::string> now, was;
                for (const auto& it : *items) now.insert(it.role);
                for (const auto& m : g.members) was.insert(m.role);
                if (now == was) continue;
                if (auto r = rebuildGroup(sd, g.id, g.type, g.sources, params, units, env, alloc)) {
                    sd = std::move(*r);
                    changed = true;
                }
            }
            if (!changed) continue;
            auto nf = std::make_shared<doc::Feature>(*f);
            nf->data = std::move(sd);
            d.elements[ei].features[fi] = nf;
        }
    return d;
}

std::pair<sketch::SketchData, std::vector<std::string>> deleteGroupsOf(sketch::SketchData s, const std::vector<std::string>& ids) {
    std::vector<std::string> rest;
    std::set<std::string> groups;
    for (const auto& id : ids) {
        if (s.findGroup(id)) groups.insert(id);
        else if (const sketch::Group* g = s.groupOfMember(id)) groups.insert(g->id);
        else rest.push_back(id);
    }
    for (const auto& g : groups) s = sketch::removeGroup(std::move(s), g, false);
    return {std::move(s), std::move(rest)};
}

}  // namespace construe::api
