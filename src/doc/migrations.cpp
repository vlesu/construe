// Registry of document format migrations (spec/document.md §4.3, §4.3а; S8-COMPAT-001).
#include "doc/migrations.h"

#include "base/math.h"
#include "doc/document.h"

#include <map>
#include <optional>
#include <string>

namespace construe::doc {

namespace {

// Migration v0 → v1 (§4.3, test-only version 0): the root key "defaultUnits" is renamed to "units".
void migrateDefaultUnits(Json& j) {
    Json migrated = Json::object();
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "defaultUnits") migrated["units"] = it.value();
        else migrated[it.key()] = it.value();
    }
    j = std::move(migrated);
}

// Migrations that only change schemaVersion: the older file is a valid document of the newer version (§4.4).
void unchanged(Json&) {}

// Migration v4 → v5 (§4.4, FX8-SK-004): in every sketch (any object with arrays "entities" and "constraints"), a
// dimension's absolute "label" [x, y] becomes "labelOffset" = label − anchor, the anchor computed from the stored point
// coordinates by the rule of sketch.md §2.2 «labelOffset»; a dimension whose anchor is unknown here (a reference to a
// projection) loses its label (display only: the label goes to the default position).
void migrateLabelsToOffsets(Json& j) {
    if (j.is_array()) {
        for (auto& x : j) migrateLabelsToOffsets(x);
        return;
    }
    if (!j.is_object()) return;
    if (j.contains("entities") && j["entities"].is_array() && j.contains("constraints") && j["constraints"].is_array()) {
        std::map<std::string, const Json*> ents;
        for (const auto& e : j["entities"])
            if (e.is_object() && e.contains("id") && e["id"].is_string()) ents[e["id"].get<std::string>()] = &e;
        auto pointOf = [&](const Json& id) -> std::optional<Point2> {
            if (!id.is_string()) return std::nullopt;
            if (id == "origin") return Point2(0, 0);
            const auto it = ents.find(id.get<std::string>());
            if (it == ents.end() || it->second->value("type", std::string()) != "point") return std::nullopt;
            const Json& e = *it->second;
            if (!e.contains("x") || !e.contains("y") || !e["x"].is_number() || !e["y"].is_number()) return std::nullopt;
            return Point2(e["x"].get<double>(), e["y"].get<double>());
        };
        auto anchorOf = [&](const Json& id) -> std::optional<Point2> {
            if (auto p = pointOf(id)) return p;
            if (!id.is_string()) return std::nullopt;
            const auto it = ents.find(id.get<std::string>());
            if (it == ents.end()) return std::nullopt;
            const Json& e = *it->second;
            const std::string t = e.value("type", std::string());
            if (t == "line" && e.contains("p1") && e.contains("p2")) {
                const auto a = pointOf(e["p1"]), b = pointOf(e["p2"]);
                if (a && b) return Point2((*a + *b) / 2);
            } else if ((t == "circle" || t == "arc" || t == "ellipse" || t == "ellipseArc") && e.contains("center")) {
                return pointOf(e["center"]);
            } else if (t == "text" && e.contains("anchor")) {
                return pointOf(e["anchor"]);
            } else if ((t == "bezier" || t == "bspline") && e.contains("poles") && e["poles"].is_array() && !e["poles"].empty()) {
                Point2 sum(0, 0);
                for (const auto& q : e["poles"]) {
                    const auto p = pointOf(q);
                    if (!p) return std::nullopt;
                    sum += *p;
                }
                return Point2(sum / double(e["poles"].size()));
            }
            return std::nullopt;
        };
        for (auto& c : j["constraints"]) {
            if (!c.is_object() || !c.contains("label")) continue;
            const Json lab = c["label"];
            c.erase("label");
            if (!lab.is_array() || lab.size() != 2 || !lab[0].is_number() || !lab[1].is_number() || !c.contains("refs") ||
                !c["refs"].is_array() || c["refs"].empty())
                continue;
            std::optional<Point2> anchor;
            if (c.value("type", std::string()) == "weight") {
                anchor = anchorOf(c["refs"][0]);
            } else {
                Point2 sum(0, 0);
                bool ok = true;
                for (const auto& r : c["refs"]) {
                    const auto a = anchorOf(r);
                    ok = ok && a.has_value();
                    if (a) sum += *a;
                }
                if (ok) anchor = Point2(sum / double(c["refs"].size()));
            }
            if (anchor) c["labelOffset"] = Json::array({lab[0].get<double>() - anchor->x(), lab[1].get<double>() - anchor->y()});
        }
    }
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it->is_array() || it->is_object()) migrateLabelsToOffsets(*it);
}

}  // namespace

const std::vector<Migration>& migrations() {
    // Append only (§4.3а): a new schema version adds one entry at the end; existing entries never change.
    static const std::vector<Migration> list = {
        {0, "migrate_0_to_1", "test-only version 0: root key defaultUnits renamed to units", migrateDefaultUnits},
        {1, "migrate_1_to_2", "content unchanged: version 2 added part properties and reference geometry", unchanged},
        {2, "migrate_2_to_3", "content unchanged: variables without a type are float", unchanged},
        {3, "migrate_3_to_4", "content unchanged: version 4 only added fields and features", unchanged},
        {4, "migrate_4_to_5", "sketch dimension label positions become offsets from the dimension anchor (FX8-SK-004)",
         migrateLabelsToOffsets},
    };
    return list;
}

void migrateToCurrent(Json& j) {
    const auto& list = migrations();
    for (int v = j["schemaVersion"].get<int>(); v < CURRENT_SCHEMA_VERSION; ++v) {
        list[static_cast<size_t>(v)].apply(j);
        j["schemaVersion"] = v + 1;
    }
}

}  // namespace construe::doc
