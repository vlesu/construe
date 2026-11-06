// CoreApi feature commands (api/feature_commands.h).
#include "api/feature_commands.h"

#include "api/errors.h"

namespace construe::api {

namespace {

// Feature definition references to features and entities.
struct DefRefs {
    std::string sketch;
    std::string sketchLine;
};
DefRefs defRefs(const Json& def) {
    DefRefs r;
    if (def.contains("sketch") && def["sketch"].is_string()) r.sketch = def["sketch"];
    if (def.contains("axis") && def["axis"].is_object() && def["axis"].contains("sketchLine") &&
        def["axis"]["sketchLine"].is_string())
        r.sketchLine = def["axis"]["sketchLine"];
    return r;
}

}  // namespace

doc::Document replaceFeature(doc::Document d, const doc::Feature& f) {
    auto& feats = d.active().features;
    for (auto& p : feats)
        if (p->id == f.id) p = std::make_shared<doc::Feature>(f);
    return d;
}

Result<doc::Document> applyFeatureJson(const doc::Document& base, const std::string& id, Json fj, int insertIndex, bool replace) {
    Json dj = doc::documentToJsonValue(base);
    const int ei = base.activeIndex();
    Json& feats = dj["elements"][ei]["features"];
    if (feats.is_null()) feats = Json::array();  // assembly without features: array is not written (assembly.md §7б)
    if (replace) feats[insertIndex] = fj;
    else feats.insert(feats.begin() + insertIndex, fj);
    auto r = doc::documentFromJson(dj);
    if (r) {
        r->activeElement = base.activeElement;  // session state (document.md §6)
        return r;
    }
    Error e = r.error();
    const std::string prefix = "/elements/" + std::to_string(ei) + "/features/" + std::to_string(insertIndex);
    if (e.code == "DOC_SCHEMA") return schemaErrorAt(e, prefix);
    if (e.code == "DOC_DANGLING_REF") {
        const std::string* ref = e.stringParam("id");
        return Error("API_ORDER_VIOLATION").with("id", id).with("ids", std::vector<std::string>{ref ? *ref : ""});
    }
    e.path.reset();
    return e;
}

Result<Json> featureJsonFromDefinition(const Json& def, const std::string& id, const std::string& name, bool suppressed) {
    if (!def.is_object()) return invalidDefinition("definition must be an object");
    if (!def.contains("type") || !def["type"].is_string()) return invalidDefinition("type required", "/type");
    for (const char* k : {"id", "name", "suppressed"})
        if (def.contains(k)) return invalidDefinition(std::string(k) + " not allowed", std::string("/") + k);
    Json fj = Json::object();
    fj["id"] = id;
    fj["type"] = def["type"];
    fj["name"] = name;
    fj["suppressed"] = suppressed;
    for (auto it = def.begin(); it != def.end(); ++it)
        if (it.key() != "type") fj[it.key()] = it.value();
    return fj;
}

doc::FeatureType featureTypeFromName(const std::string& t) {
    if (t == "extrude") return doc::FeatureType::Extrude;
    if (t == "revolve") return doc::FeatureType::Revolve;
    if (t == "fillet") return doc::FeatureType::Fillet;
    if (t == "chamfer") return doc::FeatureType::Chamfer;
    if (t == "plane") return doc::FeatureType::Plane;
    if (t == "axis") return doc::FeatureType::Axis;
    if (t == "pattern") return doc::FeatureType::Pattern;
    if (t == "mirror") return doc::FeatureType::Mirror;
    if (t == "shell") return doc::FeatureType::Shell;
    if (t == "draft") return doc::FeatureType::Draft;
    if (t == "sweep") return doc::FeatureType::Sweep;
    if (t == "loft") return doc::FeatureType::Loft;
    if (t == "hole") return doc::FeatureType::Hole;
    if (t == "thread") return doc::FeatureType::Thread;
    if (t == "sheetMetal") return doc::FeatureType::SheetMetal;
    if (t == "sheetWall") return doc::FeatureType::SheetWall;
    if (t == "sheetFlange") return doc::FeatureType::SheetFlange;
    if (t == "sheetBend") return doc::FeatureType::SheetBend;
    if (t == "sheetCut") return doc::FeatureType::SheetCut;
    if (t == "gear") return doc::FeatureType::Gear;
    if (t == "coil") return doc::FeatureType::Coil;
    return doc::FeatureType::Sketch;
}

Status checkDefinitionRefs(const doc::Document& d, const Json& def, const std::string& featureId, int position) {
    // References to reference geometry and point sketches (document.md §2.10а) - exist and are above in the history.
    std::vector<std::string> featureRefs;
    if (def.contains("type") && (def["type"] == "plane" || def["type"] == "axis"))
        for (const auto& fr : doc::refGeomFeatureRefs(def)) featureRefs.push_back(fr.id);
    if (def.contains("type") && (def["type"] == "pattern" || def["type"] == "mirror" || def["type"] == "draft" ||
                                 def["type"] == "sweep" || def["type"] == "loft" ||
                                 def["type"] == "hole" || def["type"] == "sheetWall" || def["type"] == "sheetBend" ||
                                 def["type"] == "sheetCut"))
        for (const auto& fr : doc::patternFeatureRefs(def)) featureRefs.push_back(fr.id);
    for (const char* key : {"plane", "axis"})
        if (def.contains(key) && def[key].is_object() && def[key].contains("feature") && def[key]["feature"].is_string())
            featureRefs.push_back(def[key]["feature"]);
    // Reference planes of the "up to" end of an extrude (document.md §2.8, S7F-FEAT-001).
    if (def.value("type", std::string()) == "extrude" && def.contains("end") && def["end"].is_object() && def["end"].contains("to") &&
        def["end"]["to"].is_array())
        for (const auto& r : def["end"]["to"])
            if (r.is_object() && r.contains("feature") && r["feature"].is_string()) featureRefs.push_back(r["feature"]);
    for (const auto& id : featureRefs) {
        const int i = d.active().indexOf(id);
        if (i < 0) return notFound(id);
        if (i >= position) return Error("API_ORDER_VIOLATION").with("id", featureId).with("ids", std::vector<std::string>{id});
    }
    const DefRefs r = defRefs(def);
    if (!r.sketch.empty()) {
        const int si = d.active().indexOf(r.sketch);
        if (si < 0) return notFound(r.sketch);
        if (si >= position)
            return Error("API_ORDER_VIOLATION").with("id", featureId).with("ids", std::vector<std::string>{r.sketch});
        if (!r.sketchLine.empty()) {
            const auto* s = d.active().features[si]->sketchData();
            if (s && !s->findEntity(r.sketchLine)) {
                // The entity may be in another sketch - then DOC_BAD_REF_KIND during document validation.
                bool elsewhere = false;
                for (const auto& f : d.active().features)
                    if (f->sketchData() && f->sketchData()->findEntity(r.sketchLine)) elsewhere = true;
                if (!elsewhere) return notFound(r.sketchLine);
            }
        }
    }
    return okStatus();
}

int insertionIndex(const doc::Element& el) {
    switch (el.rollback.kind) {
        case doc::Rollback::Kind::End: return static_cast<int>(el.features.size());
        case doc::Rollback::Kind::Start: return 0;
        case doc::Rollback::Kind::After: return el.indexOf(el.rollback.feature) + 1;
    }
    return static_cast<int>(el.features.size());
}

}  // namespace construe::api
