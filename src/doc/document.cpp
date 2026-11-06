// Document model: JSON ↔ model, parsing, canonical serialization, validation (spec/document.md).
#include "doc/document.h"

#include "base/ids.h"
#include "doc/migrations.h"
#include "doc/schema.h"
#include "sketch/sketch.h"
#include "pmi/pmi.h"
#include "pmi/weld.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace construe::doc {

const char* featureTypeName(FeatureType t) {
    switch (t) {
        case FeatureType::Sketch: return "sketch";
        case FeatureType::Extrude: return "extrude";
        case FeatureType::Revolve: return "revolve";
        case FeatureType::Fillet: return "fillet";
        case FeatureType::Chamfer: return "chamfer";
        case FeatureType::Plane: return "plane";
        case FeatureType::Axis: return "axis";
        case FeatureType::Pattern: return "pattern";
        case FeatureType::Mirror: return "mirror";
        case FeatureType::Shell: return "shell";
        case FeatureType::Draft: return "draft";
        case FeatureType::Sweep: return "sweep";
        case FeatureType::Loft: return "loft";
        case FeatureType::Hole: return "hole";
        case FeatureType::Thread: return "thread";
        case FeatureType::Import: return "import";
        case FeatureType::SheetMetal: return "sheetMetal";
        case FeatureType::SheetWall: return "sheetWall";
        case FeatureType::SheetFlange: return "sheetFlange";
        case FeatureType::SheetBend: return "sheetBend";
        case FeatureType::SheetCut: return "sheetCut";
        case FeatureType::Gear: return "gear";
        case FeatureType::Coil: return "coil";
        case FeatureType::Connection: return "connection";
        case FeatureType::ConnectionCut: return "connectionCut";
    }
    return "?";
}

std::vector<FeatureRefInDef> refGeomFeatureRefs(const Json& def) {
    std::vector<FeatureRefInDef> out;
    auto planeRef = [&](const Json& r, const std::string& path) {
        if (r.is_object() && r.contains("feature") && r["feature"].is_string()) out.push_back({r["feature"], "plane", path + "/feature"});
    };
    auto lineRef = [&](const Json& r, const std::string& path) {
        if (r.is_object() && r.contains("feature") && r["feature"].is_string()) out.push_back({r["feature"], "axis", path + "/feature"});
    };
    auto pointRef = [&](const Json& r, const std::string& path) {
        if (r.is_object() && r.contains("sketchPoint") && r["sketchPoint"].is_object() && r["sketchPoint"].contains("sketch") &&
            r["sketchPoint"]["sketch"].is_string())
            out.push_back({r["sketchPoint"]["sketch"], "sketch", path + "/sketchPoint/sketch"});
    };
    if (def.contains("ref")) planeRef(def["ref"], "/ref");
    if (def.contains("to")) pointRef(def["to"], "/to");  // offset plane up to an object (FX4-FEAT-001)
    if (def.contains("axis")) lineRef(def["axis"], "/axis");
    if (def.contains("refs") && def["refs"].is_array())
        for (size_t i = 0; i < def["refs"].size(); ++i) planeRef(def["refs"][i], "/refs/" + std::to_string(i));
    if (def.contains("points") && def["points"].is_array())
        for (size_t i = 0; i < def["points"].size(); ++i) pointRef(def["points"][i], "/points/" + std::to_string(i));
    return out;
}

std::vector<FeatureRefInDef> patternFeatureRefs(const Json& def) {
    std::vector<FeatureRefInDef> out;
    if (def.contains("features") && def["features"].is_array())
        for (size_t i = 0; i < def["features"].size(); ++i)
            if (def["features"][i].is_string()) out.push_back({def["features"][i], "tool", "/features/" + std::to_string(i)});
    // DirRef / AxisRef (§2.10а, FX7-PT-007): axis feature, plane of a normal, sketch of a sketch line, revolve feature.
    auto lineRef = [&](const Json& r, const std::string& path) {
        if (!r.is_object()) return;
        if (r.contains("feature") && r["feature"].is_string()) out.push_back({r["feature"], "axis", path + "/feature"});
        if (r.contains("normal") && r["normal"].is_object() && r["normal"].contains("feature") && r["normal"]["feature"].is_string())
            out.push_back({r["normal"]["feature"], "plane", path + "/normal/feature"});
        if (r.contains("sketchLine") && r["sketchLine"].is_object() && r["sketchLine"].contains("sketch") && r["sketchLine"]["sketch"].is_string())
            out.push_back({r["sketchLine"]["sketch"], "sketch", path + "/sketchLine/sketch"});
        if (r.contains("revolveAxis") && r["revolveAxis"].is_string()) out.push_back({r["revolveAxis"], "revolve", path + "/revolveAxis"});
    };
    if (def.contains("directions") && def["directions"].is_array())
        for (size_t i = 0; i < def["directions"].size(); ++i)
            if (def["directions"][i].is_object() && def["directions"][i].contains("ref"))
                lineRef(def["directions"][i]["ref"], "/directions/" + std::to_string(i) + "/ref");
    if (def.contains("axis")) lineRef(def["axis"], "/axis");
    if (def.contains("sketch") && def["sketch"].is_string()) out.push_back({def["sketch"], "sketch", "/sketch"});
    if (def.contains("sections") && def["sections"].is_array())
        for (size_t i = 0; i < def["sections"].size(); ++i)
            if (def["sections"][i].is_object() && def["sections"][i].contains("sketch") && def["sections"][i]["sketch"].is_string())
                out.push_back({def["sections"][i]["sketch"], "sketch", "/sections/" + std::to_string(i) + "/sketch"});
    if (def.contains("path") && def["path"].is_object() && def["path"].contains("sketch") && def["path"]["sketch"].is_string())
        out.push_back({def["path"]["sketch"], "sketch", "/path/sketch"});
    for (const char* key : {"plane", "neutral"})
        if (def.contains(key) && def[key].is_object() && def[key].contains("feature") && def[key]["feature"].is_string())
            out.push_back({def[key]["feature"], "plane", std::string("/") + key + "/feature"});
    // A connection input given by the points of a sketch (document.md §2.10н, FX8-BLT-002).
    if (def.contains("inputs") && def["inputs"].is_object())
        for (auto it = def["inputs"].begin(); it != def["inputs"].end(); ++it)
            if (it.value().is_object() && it.value().contains("sketch") && it.value()["sketch"].is_string())
                out.push_back({it.value()["sketch"], "sketch", "/inputs/" + it.key() + "/sketch"});
    return out;
}

void forEachExpression(Feature& f, const std::function<void(std::string& text, ExprDim dim, bool driving)>& fn) {
    if (auto* s = std::get_if<sketch::SketchData>(&f.data)) {
        for (auto& c : s->constraints)
            if (c.value && sketch::isDimensionType(c.type))
                fn(*c.value, sketch::isAngleDimension(c.type) ? ExprDim::Angle : sketch::isNumberDimension(c.type) ? ExprDim::Number : ExprDim::Length,
                   c.driving.value_or(true));
    } else if (auto* x = std::get_if<ExtrudeData>(&f.data)) {
        if (x->endType == "blind" || x->endType == "symmetric") fn(x->depth, ExprDim::Length, true);
        if (x->endType == "upTo" && !x->upToOffset.empty()) fn(x->upToOffset, ExprDim::Length, true);
        if (!x->tabLeft.empty()) fn(x->tabLeft, ExprDim::Length, true);
        if (!x->tabRight.empty()) fn(x->tabRight, ExprDim::Length, true);
    } else if (auto* r = std::get_if<RevolveData>(&f.data)) {
        if (r->angleType != "full") fn(r->angleValue, ExprDim::Angle, true);
    } else if (auto* e = std::get_if<EdgeFeatureData>(&f.data)) {
        fn(e->size, ExprDim::Length, true);
        if (!e->mode.empty()) fn(e->size2, e->mode == "distanceAngle" ? ExprDim::Angle : ExprDim::Length, true);
    } else if (auto* g = std::get_if<RefGeomData>(&f.data)) {
        auto field = [&](const char* key, ExprDim dim) {
            if (g->def.contains(key) && g->def[key].is_string()) {
                std::string t = g->def[key];
                fn(t, dim, true);
                g->def[key] = t;
            }
        };
        if (f.type == FeatureType::Plane) field("distance", ExprDim::Length), field("angle", ExprDim::Angle);
    } else if (auto* p = std::get_if<PatternData>(&f.data)) {
        auto field = [&](Json& obj, const char* key, ExprDim dim) {
            if (obj.is_object() && obj.contains(key) && obj[key].is_string()) {
                std::string t = obj[key];
                fn(t, dim, true);
                obj[key] = t;
            }
        };
        if (p->def.contains("directions") && p->def["directions"].is_array())
            for (auto& d : p->def["directions"]) field(d, "count", ExprDim::Number), field(d, "spacing", ExprDim::Length);
        field(p->def, "count", ExprDim::Number);
        field(p->def, "angle", ExprDim::Angle);
        field(p->def, "thickness", ExprDim::Length);
        if (p->def.contains("end")) field(p->def["end"], "depth", ExprDim::Length);
        // Thread by expressions (§2.10и, S6-LIB-008): diameter and pitch before length.
        if (f.type == FeatureType::Thread)
            field(p->def, "diameter", ExprDim::Length), field(p->def, "pitch", ExprDim::Length), field(p->def, "height", ExprDim::Length);
        field(p->def, "length", ExprDim::Length);
        // Sheet metal (§2.10л): fields in the order of the §2.11 table.
        if (f.type == FeatureType::SheetMetal) {
            field(p->def, "radius", ExprDim::Length);
            field(p->def, "kFactor", ExprDim::Number);
            if (p->def.contains("relief")) field(p->def["relief"], "width", ExprDim::Length), field(p->def["relief"], "depth", ExprDim::Length);
        }
        if (f.type == FeatureType::SheetWall) field(p->def, "width", ExprDim::Length);
        // Gear rim (§2.10м): fields in the order of the §2.11 table.
        if (f.type == FeatureType::Gear) {
            field(p->def, "module", ExprDim::Length);
            field(p->def, "teeth", ExprDim::Number);
            field(p->def, "shift", ExprDim::Number);
            field(p->def, "width", ExprDim::Length);
            field(p->def, "phase", ExprDim::Angle);
            if (p->def.contains("sector")) field(p->def["sector"], "start", ExprDim::Angle), field(p->def["sector"], "angle", ExprDim::Angle);
        }
        // Coil (§2.10п): fields in the order of the §2.11 table.
        if (f.type == FeatureType::Coil) {
            field(p->def, "pitch", ExprDim::Length);
            field(p->def, "turns", ExprDim::Number);
            field(p->def, "height", ExprDim::Length);
            field(p->def, "startAngle", ExprDim::Angle);
        }
        if (f.type == FeatureType::SheetFlange || f.type == FeatureType::SheetBend) field(p->def, "radius", ExprDim::Length);
        if (f.type == FeatureType::SheetFlange) {
            if (p->def.contains("offsets")) field(p->def["offsets"], "start", ExprDim::Length), field(p->def["offsets"], "end", ExprDim::Length);
            field(p->def, "gap", ExprDim::Length);
        }
    }
}

bool Element::isSheetPart() const {
    if (kind != "part") return false;
    if (sheetMetal) return *sheetMetal;
    for (const auto& f : features)
        if (f->type == FeatureType::SheetMetal) return true;
    return false;
}

bool isToolFeature(FeatureType t) {
    using T = FeatureType;
    return t == T::Extrude || t == T::Revolve || t == T::Sweep || t == T::Loft || t == T::Hole || t == T::Gear || t == T::Coil ||
           t == T::Pattern || t == T::Mirror || t == T::SheetCut || t == T::SheetFlange;  // sheet sources: FX8-SH-005
}

std::vector<ExprField> expressionFields(const Feature& feature) {
    Feature copy = feature;
    std::vector<ExprField> out;
    forEachExpression(copy, [&](std::string& text, ExprDim dim, bool driving) { out.push_back({text, dim, driving}); });
    return out;
}

void featureIdsInName(const std::string& name, std::set<std::string, IdLess>& out) {
    auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] != 'f' || (i > 0 && word(name[i - 1]))) continue;
        size_t j = i + 1;
        while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j]))) ++j;
        if (j > i + 1 && j < name.size() && name[j] == '.') out.insert(name.substr(i, j - i));
    }
}

std::set<std::string, IdLess> featureDependencies(const Feature& f) {
    std::set<std::string, IdLess> out;
    auto add = [&](const std::string& id) {
        if (!id.empty()) out.insert(id);
    };
    if (const auto* x = f.extrude()) {
        add(x->sketch);
        for (const auto& r : x->upTo)  // "up to" objects (§2.8, S7F-FEAT-001): names and reference planes
            if (r.is_object())
                for (auto it = r.begin(); it != r.end(); ++it)
                    if (!it.value().is_string()) continue;
                    else if (it.key() == "feature") add(it.value().get<std::string>());
                    else if (it.key() != "datum") featureIdsInName(it.value().get<std::string>(), out);
    }
    if (const auto* r = f.revolve()) add(r->sketch), add(r->axisFeature);
    if (const auto* e = f.edgeFeature())
        for (const auto& n : e->edges) featureIdsInName(n, out);
    const Json* jsonDef = nullptr;
    if (const auto* p = f.pattern()) {
        for (const auto& r : patternFeatureRefs(p->def)) add(r.id);
        jsonDef = &p->def;
    }
    if (const auto* g = f.refGeom()) {
        for (const auto& r : refGeomFeatureRefs(g->def)) add(r.id);
        jsonDef = &g->def;
    }
    if (jsonDef) {
        // Stable names — values of face/edge/vertex keys at any depth of the definition (§2.10а).
        auto scan = [&](auto&& self, const Json& j) -> void {
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) {
                    if ((it.key() == "face" || it.key() == "edge" || it.key() == "vertex") && it.value().is_string())
                        featureIdsInName(it.value().get<std::string>(), out);
                    else if ((it.key() == "faces" || it.key() == "edges") && it.value().is_array()) {
                        for (const auto& n : it.value())
                            if (n.is_string()) featureIdsInName(n.get<std::string>(), out);
                    } else {
                        self(self, it.value());
                    }
                }
            } else if (j.is_array()) {
                for (const auto& x : j) self(self, x);
            }
        };
        scan(scan, *jsonDef);
    }
    if (const auto* s = f.sketchData()) {
        add(s->plane.feature);
        featureIdsInName(s->plane.face, out);
        for (const auto& e : s->entities) {
            featureIdsInName(e.source, out);
            add(sketch::refAxisFeature(e.source));  // "refaxis:<axis feature>" (sketch.md §2.5, FX9-SK-001)
            // Sketch piercing: "pierce:f<N>" (sketch.md §2.5).
            if (e.source.rfind("pierce:f", 0) == 0 && e.source.size() > 8 &&
                std::all_of(e.source.begin() + 8, e.source.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); }))
                out.insert(e.source.substr(7));
        }
    }
    out.erase(f.id);
    return out;
}

Json canonicalMaterial(const Json& m) {
    Json out = Json::object();
    for (const char* k : {"id", "name", "density", "youngModulus", "poisson", "yieldStrength", "tensileStrength", "color"})
        if (m.contains(k)) out[k] = m[k];
    return out;
}

Json canonicalRestRoughness(const Json& r) {
    Json out = Json::object();
    for (const char* k : {"sign", "parameter", "value"})
        if (r.contains(k)) out[k] = r[k];
    return out;
}

int Element::indexOf(const std::string& featureId) const {
    for (size_t i = 0; i < features.size(); ++i)
        if (features[i]->id == featureId) return static_cast<int>(i);
    return -1;
}

FeaturePtr Element::find(const std::string& featureId) const {
    const int i = indexOf(featureId);
    return i < 0 ? nullptr : features[static_cast<size_t>(i)];
}

// ---------------------------------------------------------------- model → JSON

namespace {

Json branchToJson(const sketch::Branch& b) {
    Json j = Json::object();
    switch (b.kind) {
        case sketch::Branch::Kind::Side: j["side"] = b.side; break;
        case sketch::Branch::Kind::Contact: j["contact"] = b.contact; break;
        case sketch::Branch::Kind::Angle:
            j["sign"] = b.sign;
            j["supplementary"] = b.supplementary;
            break;
    }
    return j;
}

Json entityToJson(const sketch::Entity& e) {
    Json j = Json::object();
    j["id"] = e.id;
    j["type"] = sketch::entityTypeName(e.type);
    switch (e.type) {
        case sketch::EntityType::Point:
            j["x"] = e.x;
            j["y"] = e.y;
            break;
        case sketch::EntityType::Line:
            j["p1"] = e.p1;
            j["p2"] = e.p2;
            break;
        case sketch::EntityType::Circle:
            j["center"] = e.center;
            j["r"] = e.r;
            break;
        case sketch::EntityType::Arc:
            j["center"] = e.center;
            j["start"] = e.start;
            j["end"] = e.end;
            break;
        case sketch::EntityType::EllipseArc:  // S4-SKT-007
            j["center"] = e.center;
            j["start"] = e.start;
            j["end"] = e.end;
            j["rx"] = e.rx;
            j["ry"] = e.ry;
            j["angle"] = e.angle;
            break;
        case sketch::EntityType::Ellipse:  // S4-SKT-004
            j["center"] = e.center;
            j["rx"] = e.rx;
            j["ry"] = e.ry;
            j["angle"] = e.angle;
            break;
        case sketch::EntityType::Projected: j["source"] = e.source; break;
        case sketch::EntityType::Bezier:  // S4A-SPL-001
        case sketch::EntityType::BSpline:
            j["poles"] = e.poles;
            if (e.type == sketch::EntityType::BSpline) {
                j["degree"] = e.degree;
                j["knots"] = e.knots;
                j["mults"] = e.mults;
            }
            if (!e.weights.empty()) j["weights"] = e.weights;
            if (e.periodic) j["periodic"] = true;
            break;
        case sketch::EntityType::Text:
            j["anchor"] = e.center;
            j["text"] = e.text;
            j["font"] = e.font;
            j["height"] = e.height;
            if (e.angle != 0) j["angle"] = e.angle;
            if (!e.styles.empty()) {  // FX8-SK-007
                Json st = Json::array();
                for (const auto& sp : e.styles) {
                    Json o{{"from", sp.from}, {"to", sp.to}};
                    if (sp.bold) o["bold"] = true;
                    if (sp.italic) o["italic"] = true;
                    if (sp.underline) o["underline"] = true;
                    st.push_back(o);
                }
                j["styles"] = st;
            }
            break;
    }
    if (e.construction) j["construction"] = true;
    return j;
}

Json constraintToJson(const sketch::Constraint& c) {
    Json j = Json::object();
    j["id"] = c.id;
    j["type"] = c.type;
    j["refs"] = c.refs;
    if (!c.target.empty()) j["target"] = c.target;
    if (c.vector) j["vector"] = Json::array({(*c.vector)[0], (*c.vector)[1]});
    if (c.rotation) j["angle"] = *c.rotation;
    if (c.value) j["value"] = *c.value;
    if (c.driving) j["driving"] = *c.driving;
    if (c.arc) j["arc"] = *c.arc;
    if (c.diametral) j["diametral"] = true;
    if (c.branch) j["branch"] = branchToJson(*c.branch);
    if (c.labelOffset) j["labelOffset"] = Json::array({(*c.labelOffset)[0], (*c.labelOffset)[1]});
    if (c.automatic) j["auto"] = true;
    if (!c.yielded.empty()) {
        Json y = Json::array();
        for (const auto& k : c.yielded) y.push_back(constraintToJson(k));
        j["yielded"] = y;
    }
    return j;
}

Json groupToJson(const sketch::Group& g) {
    Json j = Json::object();
    j["id"] = g.id;
    j["type"] = g.type;
    j["sources"] = g.sources;
    Json p = Json::object();
    for (const auto& [k, v] : g.params) {
        switch (v.kind) {
            case sketch::GroupParam::Kind::Number: p[k] = v.number; break;
            case sketch::GroupParam::Kind::Text: p[k] = v.text; break;
            case sketch::GroupParam::Kind::Flag: p[k] = v.flag; break;
        }
    }
    j["params"] = p;
    Json ms = Json::array();
    for (const auto& m : g.members) ms.push_back(Json{{"role", m.role}, {"id", m.id}});
    j["members"] = ms;
    return j;
}

Json profilesToJson(const Profiles& p) {
    if (p.all) return "all";
    return Json(p.regions);
}

// Mate ↔ JSON (assembly.md §7.1).
Json mateToJson(const Mate& m) {
    Json j = Json::object();
    j["id"] = m.id;
    j["name"] = m.name;
    j["type"] = m.type;
    j["refs"] = Json::array({m.refs[0], m.refs[1]});
    if (m.value) j["value"] = *m.value;
    if (m.flip) j["flip"] = true;
    if (m.relative) {
        Json r = Json::array();
        for (double v : *m.relative) r.push_back(v);
        j["relative"] = r;
    }
    if (m.angles) j["angles"] = Json::array({(*m.angles)[0], (*m.angles)[1]});
    if (m.suppressed) j["suppressed"] = true;
    return j;
}

Json instancePatternToJson(const InstancePattern& p) {
    Json j = Json{{"id", p.id}, {"name", p.name}};
    for (const char* k : {"source", "mode", "directions", "axis", "count", "angle", "reverse", "feature", "suppressed"})
        if (p.def.contains(k)) j[k] = p.def[k];
    return j;
}

InstancePattern instancePatternFromJson(const Json& j) {
    InstancePattern p;
    p.id = j.value("id", std::string());
    p.name = j.value("name", std::string());
    p.def = j;
    p.def.erase("id");
    p.def.erase("name");
    return p;
}

Mate mateFromJson(const Json& j) {
    Mate m;
    m.id = j.value("id", std::string());
    m.name = j.value("name", std::string());
    m.type = j.value("type", std::string());
    if (j.contains("refs") && j["refs"].is_array() && j["refs"].size() == 2) m.refs = {j["refs"][0].get<std::string>(), j["refs"][1].get<std::string>()};
    if (j.contains("value")) m.value = j["value"].get<std::string>();
    m.flip = j.value("flip", false);
    if (j.contains("relative")) {
        std::array<double, 7> r{};
        for (size_t k = 0; k < 7; ++k) r[k] = j["relative"][k].get<double>();
        m.relative = r;
    }
    if (j.contains("angles") && j["angles"].is_array() && j["angles"].size() == 2)
        m.angles = std::array<double, 2>{j["angles"][0].get<double>(), j["angles"][1].get<double>()};
    m.suppressed = j.value("suppressed", false);
    return m;
}

// Section ↔ JSON (§2.4а): field order follows the schema (canonical serialization orders by it too).
Json sectionToJson(const Section& s) {
    Json j = Json::object();
    j["id"] = s.id;
    j["name"] = s.name;
    if (s.plane) {
        j["plane"] = *s.plane;
        j["offset"] = s.offset;
    } else {
        j["sketch"] = s.sketch;
    }
    if (s.flip) j["flip"] = true;
    if (s.hidden) j["hidden"] = true;
    if (!s.exclude.empty()) j["exclude"] = s.exclude;
    return j;
}

Section sectionFromJson(const Json& j) {
    Section s;
    if (j.contains("id")) s.id = j["id"].get<std::string>();
    if (j.contains("name")) s.name = j["name"].get<std::string>();
    if (j.contains("plane")) {
        s.plane = j["plane"];
        s.offset = j.value("offset", std::string("0"));
    }
    if (j.contains("sketch")) s.sketch = j["sketch"].get<std::string>();
    s.flip = j.value("flip", false);
    s.hidden = j.value("hidden", false);
    if (j.contains("exclude"))
        for (const auto& x : j["exclude"]) s.exclude.push_back(x.get<std::string>());
    return s;
}

// Drawing view and sheet ↔ JSON (§2.4г).
Json viewToJson(const DrawingView& v) {
    Json j = Json::object();
    j["id"] = v.id;
    j["name"] = v.name;
    j["kind"] = v.kind;
    if (v.kind == "base") {
        j["ref"] = v.refFile.empty() ? Json{{"element", v.refElement}} : Json{{"file", v.refFile}, {"element", v.refElement}};
        j["orientation"] = v.orientation;
        if (v.plane) j["plane"] = *v.plane;
        if (v.normal) j["normal"] = Json::array({(*v.normal)[0], (*v.normal)[1], (*v.normal)[2]});
        if (v.up) j["up"] = Json::array({(*v.up)[0], (*v.up)[1], (*v.up)[2]});
    } else if (v.kind == "section") {
        j["parent"] = v.parent;
        j["line"] = Json::array({Json::array({v.line[0][0], v.line[0][1]}), Json::array({v.line[1][0], v.line[1][1]})});
        if (v.lineBind) j["lineBind"] = *v.lineBind;
        if (v.flip) j["flip"] = true;
        if (v.sectionOnly) j["sectionOnly"] = true;
    } else if (v.kind == "detail") {
        j["parent"] = v.parent;
        j["center"] = Json::array({v.center[0], v.center[1]});
        j["radius"] = v.radius;
    } else {
        j["parent"] = v.parent;
        j["direction"] = v.direction;
    }
    j["position"] = Json::array({v.position[0], v.position[1]});
    if (v.scale) j["scale"] = *v.scale;
    if (v.hiddenLines) j["hiddenLines"] = true;
    if (v.tangentEdges) j["tangentEdges"] = true;
    if (!v.centerLines) j["centerLines"] = false;
    if (!v.pmi) j["pmi"] = false;
    if (!v.display.empty()) j["display"] = v.display;
    if (v.flat) j["flat"] = true;
    if (v.exploded) j["exploded"] = true;
    // S7F-DRW-005, FX10-DRW-003 (main and projected views): an empty list is written (none of the model's).
    if (v.sections && (v.kind == "base" || v.kind == "projected")) j["sections"] = *v.sections;
    return j;
}

DrawingView viewFromJson(const Json& j) {
    DrawingView v;
    if (j.contains("id")) v.id = j["id"].get<std::string>();
    if (j.contains("name")) v.name = j["name"].get<std::string>();
    v.kind = j.value("kind", std::string("base"));
    if (j.contains("ref")) {
        v.refElement = j["ref"]["element"].get<std::string>();
        if (j["ref"].contains("file")) v.refFile = j["ref"]["file"].get<std::string>();
    }
    v.orientation = j.value("orientation", std::string());
    v.parent = j.value("parent", std::string());
    v.direction = j.value("direction", std::string());
    if (j.contains("line"))
        for (size_t i = 0; i < 2; ++i) v.line[i] = {j["line"][i][0].get<double>(), j["line"][i][1].get<double>()};
    if (j.contains("lineBind")) v.lineBind = j["lineBind"];
    v.flip = j.value("flip", false);
    v.sectionOnly = j.value("sectionOnly", false);
    if (j.contains("center")) v.center = {j["center"][0].get<double>(), j["center"][1].get<double>()};
    v.radius = j.value("radius", 0.0);
    if (j.contains("position")) v.position = {j["position"][0].get<double>(), j["position"][1].get<double>()};
    if (j.contains("scale")) v.scale = j["scale"].get<std::string>();
    v.hiddenLines = j.value("hiddenLines", false);
    v.tangentEdges = j.value("tangentEdges", false);
    v.centerLines = j.value("centerLines", true);
    v.pmi = j.value("pmi", true);
    if (j.contains("plane")) v.plane = j["plane"];
    auto vec3 = [&](const char* key) -> std::optional<std::array<double, 3>> {
        if (!j.contains(key)) return std::nullopt;
        return std::array<double, 3>{j[key][0].get<double>(), j[key][1].get<double>(), j[key][2].get<double>()};
    };
    v.normal = vec3("normal");
    v.up = vec3("up");
    v.display = j.value("display", std::string());
    v.flat = j.value("flat", false);
    v.exploded = j.value("exploded", false);
    if (j.contains("sections") && j["sections"].is_array()) v.sections = j["sections"].get<std::vector<std::string>>();
    return v;
}

Json sheetTextToJson(const SheetText& t) {
    Json j = Json::object();
    j["id"] = t.id;
    j["position"] = Json::array({t.position[0], t.position[1]});
    j["text"] = t.text;
    j["height"] = t.height;
    if (t.angle != 0) j["angle"] = t.angle;
    if (!t.font.empty()) j["font"] = t.font;
    if (!t.styles.empty()) {  // FX9-DRW-006: the format of sketch text styles
        Json st = Json::array();
        for (const auto& sp : t.styles) {
            Json o{{"from", sp.from}, {"to", sp.to}};
            if (sp.bold) o["bold"] = true;
            if (sp.italic) o["italic"] = true;
            if (sp.underline) o["underline"] = true;
            st.push_back(o);
        }
        j["styles"] = st;
    }
    if (t.boxWidth > 0) j["boxWidth"] = t.boxWidth;  // FX9-DRW-007
    if (t.boxHeight > 0) j["boxHeight"] = t.boxHeight;
    // S7F-DRW-002: paragraph alignments up to the last not "left"; all left — no field.
    size_t n = t.aligns.size();
    while (n > 0 && (t.aligns[n - 1].empty() || t.aligns[n - 1] == "left")) --n;
    if (n > 0) {
        Json a = Json::array();
        for (size_t i = 0; i < n; ++i) a.push_back(t.aligns[i].empty() ? std::string("left") : t.aligns[i]);
        j["aligns"] = a;
    }
    return j;
}

SheetText sheetTextFromJson(const Json& j) {
    SheetText t;
    if (j.contains("id")) t.id = j["id"].get<std::string>();
    if (j.contains("position")) t.position = {j["position"][0].get<double>(), j["position"][1].get<double>()};
    t.text = j.value("text", std::string());
    t.height = j.value("height", 5.0);
    t.angle = j.value("angle", 0.0);
    t.font = j.value("font", std::string());
    if (j.contains("styles"))
        for (const auto& o : j["styles"]) {
            font::StyleSpan sp;
            sp.from = static_cast<int>(o["from"].get<double>()), sp.to = static_cast<int>(o["to"].get<double>());
            sp.bold = o.value("bold", false), sp.italic = o.value("italic", false), sp.underline = o.value("underline", false);
            t.styles.push_back(sp);
        }
    t.boxWidth = j.value("boxWidth", 0.0);
    t.boxHeight = j.value("boxHeight", 0.0);
    if (j.contains("aligns") && j["aligns"].is_array())
        for (const auto& a : j["aligns"]) t.aligns.push_back(a.get<std::string>());
    return t;
}

Json sheetMarkToJson(const SheetMark& m) {
    Json j = Json::object();
    j["id"] = m.id;
    j["kind"] = m.kind;
    if (!m.view.empty()) j["view"] = m.view;  // none only at a parts list (FX14-DRW-003)
    if (!m.source.empty()) j["source"] = m.source;
    if (m.position) j["position"] = Json::array({(*m.position)[0], (*m.position)[1]});
    if (m.local) j["local"] = true;
    if (m.def) j["def"] = *m.def;
    return j;
}

SheetMark sheetMarkFromJson(const Json& j) {
    SheetMark m;
    if (j.contains("id")) m.id = j["id"].get<std::string>();
    m.kind = j.value("kind", std::string("annotation"));
    m.view = j.value("view", std::string());
    m.source = j.value("source", std::string());
    if (j.contains("position")) m.position = std::array<double, 2>{j["position"][0].get<double>(), j["position"][1].get<double>()};
    m.local = j.value("local", false);
    if (j.contains("def")) m.def = j["def"];
    return m;
}

// Sheet construction (§2.4г, drawing.md §6.29, FX14-DRW-006).
Json constructionToJson(const SheetConstruction& c) {
    Json j = Json::object();
    j["id"] = c.id;
    j["kind"] = c.kind;
    j["view"] = c.view;
    Json ps = Json::array();
    for (const auto& p : c.points) {
        Json pj{{"xy", Json::array({p.xy[0], p.xy[1]})}};
        if (!p.bind.empty()) {
            Json bj{{p.bind, p.name}};
            if (p.at) bj["at"] = *p.at;
            pj["bind"] = bj;
        }
        ps.push_back(pj);
    }
    j["points"] = ps;
    if (c.radius) j["radius"] = *c.radius;
    if (!c.line.empty()) j["line"] = c.line;
    return j;
}

SheetConstruction constructionFromJson(const Json& j) {
    SheetConstruction c;
    if (j.contains("id")) c.id = j["id"].get<std::string>();
    c.kind = j.value("kind", std::string("line"));
    c.view = j.value("view", std::string());
    if (j.contains("points"))
        for (const auto& pj : j["points"]) {
            ConstructionPoint p;
            if (pj.contains("xy")) p.xy = {pj["xy"][0].get<double>(), pj["xy"][1].get<double>()};
            if (pj.contains("bind")) {
                const Json& bj = pj["bind"];
                for (const char* b : {"vertex", "center", "edge"})
                    if (bj.contains(b)) p.bind = b, p.name = bj[b].get<std::string>();
                if (bj.contains("at")) p.at = bj["at"].get<double>();
            }
            c.points.push_back(std::move(p));
        }
    if (j.contains("radius")) c.radius = j["radius"].get<double>();
    c.line = j.value("line", std::string());
    return c;
}

Json dimensionToJson(const SheetDimension& a) {
    Json j = Json::object();
    j["id"] = a.id;
    j["kind"] = a.kind;
    if (a.kind == "linear") j["orientation"] = a.orientation;
    j["view"] = a.view;
    Json refs = Json::array();
    for (const auto& r : a.refs) {
        Json rj{{r.kind, r.name}};
        if (r.side != 0) rj["side"] = r.side;
        if (!r.arc.empty()) rj["arc"] = r.arc;
        refs.push_back(rj);
    }
    j["refs"] = refs;
    j["offset"] = a.offset;
    if (a.kind != "linear" && a.kind != "arcLength") j["angle"] = a.angle;
    if (a.prefix) j["prefix"] = *a.prefix;
    if (a.text) j["text"] = *a.text;
    if (a.suffix) j["suffix"] = *a.suffix;
    if (a.tolerance) j["tolerance"] = *a.tolerance;
    if (a.brk) j["break"] = true;
    if (a.fit) j["fit"] = *a.fit;
    if (a.source) j["source"] = *a.source;
    return j;
}

SheetDimension dimensionFromJson(const Json& j) {
    SheetDimension a;
    if (j.contains("id")) a.id = j["id"].get<std::string>();
    a.kind = j.value("kind", std::string("linear"));
    a.orientation = j.value("orientation", std::string());
    a.view = j.value("view", std::string());
    if (j.contains("refs"))
        for (const auto& r : j["refs"]) {
            DimensionRef dr;
            dr.kind = r.contains("vertex") ? "vertex" : r.contains("silhouette") ? "silhouette" : r.contains("cut") ? "cut"
                    : r.contains("axis") ? "axis" : r.contains("center") ? "center" : r.contains("construction") ? "construction" : "edge";
            dr.name = r[dr.kind].get<std::string>();
            dr.side = r.value("side", 0);
            dr.arc = r.value("arc", std::string());
            a.refs.push_back(dr);
        }
    a.offset = j.value("offset", 0.0);
    a.angle = j.value("angle", 0.0);
    if (j.contains("prefix")) a.prefix = j["prefix"].get<std::string>();
    if (j.contains("text")) a.text = j["text"].get<std::string>();
    if (j.contains("suffix")) a.suffix = j["suffix"].get<std::string>();
    if (j.contains("tolerance")) a.tolerance = j["tolerance"].get<std::string>();
    a.brk = j.value("break", false);
    if (j.contains("fit")) a.fit = j["fit"];
    if (j.contains("source")) a.source = j["source"];
    return a;
}

Json sheetToJson(const Sheet& s) {
    Json j = Json::object();
    j["id"] = s.id;
    j["name"] = s.name;
    j["format"] = s.format;
    j["orientation"] = s.orientation;
    j["scale"] = s.scale;
    if (s.scaleManual) j["scaleManual"] = true;  // FX10-DRW-001
    if (!s.titleBlock.empty()) {
        Json tb = Json::object();
        for (const auto& [k, v] : s.titleBlock) tb[k] = v;
        j["titleBlock"] = tb;
    }
    if (!s.views.empty()) {
        Json vs = Json::array();
        for (const auto& v : s.views) vs.push_back(viewToJson(v));
        j["views"] = vs;
    }
    if (!s.dimensions.empty()) {
        Json as = Json::array();
        for (const auto& a : s.dimensions) as.push_back(dimensionToJson(a));
        j["dimensions"] = as;
    }
    if (!s.texts.empty()) {
        Json ts = Json::array();
        for (const auto& t : s.texts) ts.push_back(sheetTextToJson(t));
        j["texts"] = ts;
    }
    if (!s.marks.empty()) {
        Json ms = Json::array();
        for (const auto& m : s.marks) ms.push_back(sheetMarkToJson(m));
        j["marks"] = ms;
    }
    if (!s.constructions.empty()) {  // FX14-DRW-006
        Json cs = Json::array();
        for (const auto& c : s.constructions) cs.push_back(constructionToJson(c));
        j["constructions"] = cs;
    }
    return j;
}

Sheet sheetFromJson(const Json& j) {
    Sheet s;
    if (j.contains("id")) s.id = j["id"].get<std::string>();
    if (j.contains("name")) s.name = j["name"].get<std::string>();
    s.format = j.value("format", std::string("A3"));
    s.orientation = j.value("orientation", std::string("landscape"));
    s.scale = j.value("scale", std::string("1:1"));
    s.scaleManual = j.value("scaleManual", false);
    if (j.contains("titleBlock"))
        for (const auto& [k, v] : j["titleBlock"].items()) s.titleBlock[k] = v.get<std::string>();
    if (j.contains("views"))
        for (const auto& jv : j["views"]) s.views.push_back(viewFromJson(jv));
    if (j.contains("dimensions"))
        for (const auto& ja : j["dimensions"]) s.dimensions.push_back(dimensionFromJson(ja));
    if (j.contains("texts"))
        for (const auto& jt : j["texts"]) s.texts.push_back(sheetTextFromJson(jt));
    if (j.contains("marks"))
        for (const auto& jm : j["marks"]) s.marks.push_back(sheetMarkFromJson(jm));
    if (j.contains("constructions"))
        for (const auto& jc : j["constructions"]) s.constructions.push_back(constructionFromJson(jc));
    return s;
}

Json featureToJson(const Feature& f) {
    Json j = Json::object();
    j["id"] = f.id;
    j["type"] = featureTypeName(f.type);
    j["name"] = f.name;
    j["suppressed"] = f.suppressed;
    if (const auto* s = f.sketchData()) {
        if (s->plane.frame) {  // given frame (§2.7, S2-ASM-011)
            const auto& f = *s->plane.frame;
            j["plane"] = Json{{"frame", {{"origin", {f[0], f[1], f[2]}}, {"x", {f[3], f[4], f[5]}}, {"y", {f[6], f[7], f[8]}}}}};
        } else {
            j["plane"] = s->plane.onFeature() ? Json{{"feature", s->plane.feature}}
                         : s->plane.onFace()  ? Json{{"face", s->plane.face}}
                                              : Json{{"datum", s->plane.datum}};
        }
        Json ents = Json::array(), cons = Json::array();
        for (const auto& e : s->entities) ents.push_back(entityToJson(e));
        for (const auto& c : s->constraints) cons.push_back(constraintToJson(c));
        j["entities"] = ents;
        j["constraints"] = cons;
        if (!s->groups.empty()) {
            Json gs = Json::array();
            for (const auto& g : s->groups) gs.push_back(groupToJson(g));
            j["groups"] = gs;
        }
        if (!s->weakAnchors.empty()) j["weakAnchors"] = s->weakAnchors;  // S4-WEAK-006
    } else if (const auto* x = f.extrude()) {
        j["sketch"] = x->sketch;
        j["profiles"] = profilesToJson(x->profiles);
        j["direction"] = x->reverse ? "reverse" : "normal";
        Json end = Json::object();
        end["type"] = x->endType;
        if (x->endType == "blind" || x->endType == "symmetric") end["depth"] = x->depth;
        if (x->endType == "upTo") {  // S7F-FEAT-001
            end["to"] = x->upTo;
            if (!x->upToOffset.empty()) end["offset"] = x->upToOffset;
        }
        j["end"] = end;
        j["operation"] = x->operation;
        if (x->targets.empty()) j["targets"] = "all";
        else j["targets"] = x->targets;  // assembly cut (§2.8, assembly.md §7г.1)
        if (!x->tabLeft.empty() || !x->tabRight.empty()) {  // tab extensions (§2.8, FX7-SH-003)
            Json tab = Json::object();
            if (!x->tabLeft.empty()) tab["left"] = x->tabLeft;
            if (!x->tabRight.empty()) tab["right"] = x->tabRight;
            j["tab"] = tab;
        }
    } else if (const auto* r = f.revolve()) {
        j["sketch"] = r->sketch;
        j["profiles"] = profilesToJson(r->profiles);
        j["axis"] = !r->axisFeature.empty() ? Json{{"feature", r->axisFeature}}
                    : r->axisDatum.empty()   ? Json{{"sketchLine", r->axisSketchLine}}
                                             : Json{{"datum", r->axisDatum}};
        Json angle = Json::object();
        angle["type"] = r->angleType;
        if (r->angleType != "full") angle["value"] = r->angleValue;
        j["angle"] = angle;
        j["operation"] = r->operation;
        j["targets"] = "all";
    } else if (const auto* e = f.edgeFeature()) {
        j["edges"] = e->edges;
        j[f.type == FeatureType::Fillet ? "radius" : "distance"] = e->size;
        if (!e->mode.empty()) {
            j["mode"] = e->mode;
            j[e->mode == "distanceAngle" ? "angle" : "distance2"] = e->size2;
            if (e->flip) j["flip"] = true;
        }
    } else if (const auto* g = f.refGeom()) {
        for (auto it = g->def.begin(); it != g->def.end(); ++it) j[it.key()] = it.value();
    } else if (const auto* p = f.pattern()) {
        for (auto it = p->def.begin(); it != p->def.end(); ++it) j[it.key()] = it.value();
    }
    // Dimension tolerances of the feature (§2.6, FX12-PMI-007): optional, written only when set.
    if (f.tolerances && !f.tolerances->empty()) j["tolerances"] = *f.tolerances;
    return j;
}

Json elementToJson(const Element& e);

Json documentToJson(const Document& d) {
    Json j = Json::object();
    j["format"] = "construe-document";
    j["schemaVersion"] = CURRENT_SCHEMA_VERSION;
    if (d.savedBy) j["savedBy"] = Json{{"app", d.savedBy->app}, {"version", d.savedBy->version}};
    j["units"] = Json{{"length", d.lengthUnit}, {"angle", d.angleUnit}};
    j["counters"] = Json{{"element", d.counters.element},
                         {"feature", d.counters.feature},
                         {"entity", d.counters.entity},
                         {"constraint", d.counters.constraint},
                         {"variable", d.counters.variable}};
    if (d.counters.instance != 1) j["counters"]["instance"] = d.counters.instance;  // only after the first instance
    if (d.counters.mate != 1) j["counters"]["mate"] = d.counters.mate;
    if (d.counters.sheet != 1) j["counters"]["sheet"] = d.counters.sheet;  // drawing sheets and views (§2.4г)
    if (d.counters.view != 1) j["counters"]["view"] = d.counters.view;
    if (d.counters.annotation != 1) j["counters"]["annotation"] = d.counters.annotation;  // annotations (§2.4д)
    if (d.counters.dimension != 1) j["counters"]["dimension"] = d.counters.dimension;  // sheet dimensions (§2.4г)
    if (d.counters.group != 1) j["counters"]["group"] = d.counters.group;  // linked sketch groups (§2.7)
    if (d.counters.table != 1) j["counters"]["table"] = d.counters.table;  // variant tables (§2.3а)
    if (d.counters.weld != 1) j["counters"]["weld"] = d.counters.weld;  // assembly weld seams (§2.4б)
    if (d.counters.pattern != 1) j["counters"]["pattern"] = d.counters.pattern;  // assembly instance patterns (§2.4б)
    if (d.counters.text != 1) j["counters"]["text"] = d.counters.text;  // sheet texts (§2.4г, FX7-DRW-006)
    if (d.counters.mark != 1) j["counters"]["mark"] = d.counters.mark;  // sheet marks (§2.4г, FX9-DRW-008)
    if (d.counters.construction != 1) j["counters"]["construction"] = d.counters.construction;  // FX14-DRW-006
    if (d.counters.elementGroup != 1) j["counters"]["elementGroup"] = d.counters.elementGroup;  // §2.1а, FX10-TREE-001
    Json vars = Json::array();
    for (const auto& v : d.variables) {
        Json jv = Json::object();
        jv["id"] = v.id;
        jv["name"] = v.name;
        if (v.type != "float") jv["type"] = v.type;  // §2.3: float is not written
        jv["expr"] = v.expr;
        if (!v.values.empty()) jv["values"] = Json(v.values);
        if (v.comment) jv["comment"] = *v.comment;
        vars.push_back(jv);
    }
    j["variables"] = vars;
    if (!d.variantTables.empty()) {  // §2.3а
        Json ts = Json::array();
        for (const auto& t : d.variantTables) {
            Json rows = Json::array();
            for (const auto& r : t.variants) rows.push_back(Json{{"name", r.name}, {"values", Json(r.values)}});
            ts.push_back(Json{{"id", t.id}, {"name", t.name}, {"columns", Json(t.columns)}, {"variants", rows}, {"active", t.active}});
        }
        j["variantTables"] = ts;
    }
    if (!d.elementGroups.empty()) {  // §2.1а, FX10-TREE-001
        Json gs = Json::array();
        for (const auto& g : d.elementGroups) gs.push_back(Json{{"id", g.id}, {"name", g.name}});
        j["elementGroups"] = gs;
    }
    Json els = Json::array();
    for (const auto& e : d.elements) els.push_back(elementToJson(e));
    j["elements"] = els;
    if (!d.libraryParts.empty()) {  // §2.13
        Json ls = Json::array();
        for (const auto& lp : d.libraryParts)
            ls.push_back(Json{{"id", lp.id}, {"package", lp.package}, {"fingerprint", lp.fingerprint}, {"title", lp.title},
                              {"standard", lp.standard}, {"designation", lp.designation}, {"sizesTable", lp.sizesTable},
                              {"document", lp.document ? documentToJson(*lp.document) : Json::object()}});
        j["libraryParts"] = ls;
    }
    if (!d.solutions.empty()) {  // §2.14
        Json ss = Json::array();
        for (const auto& sc : d.solutions) {
            Json ts = Json::array();
            for (const auto& t : sc.tables) {
                Json rows = Json::array();
                for (const auto& r : t.rows) rows.push_back(Json{{"name", r.name}, {"values", Json(r.values)}});
                ts.push_back(Json{{"id", t.id}, {"columns", Json(t.columns)}, {"rows", rows}});
            }
            Json js{{"id", sc.id}, {"package", sc.package}, {"fingerprint", sc.fingerprint}, {"title", sc.title}, {"definition", sc.definition}};
            if (!ts.empty()) js["tables"] = ts;
            ss.push_back(js);
        }
        j["solutions"] = ss;
    }
    return j;
}

Json elementToJson(const Element& e) {
    {
        Json je = Json::object();
        je["id"] = e.id;
        je["kind"] = e.kind;
        je["name"] = e.name;
        if (!e.group.empty()) je["group"] = e.group;  // §2.1а, FX10-TREE-001
        if (e.isDrawing() && e.nameManual) je["nameManual"] = true;  // §2.4г, FX9-DRW-003
        if (e.designation) je["designation"] = *e.designation;
        if (e.isDrawing()) {  // §2.4г
            if (e.standard) je["standard"] = *e.standard;
            if (e.projection) je["projection"] = *e.projection;
            if (!e.dismissedMarks.empty()) {  // FX9-DRW-008
                Json ds = Json::array();
                for (const auto& dm : e.dismissedMarks) {
                    Json dj{{"kind", dm.kind}, {"model", dm.model}};
                    if (!dm.source.empty()) dj["source"] = dm.source;
                    ds.push_back(dj);
                }
                je["dismissedMarks"] = ds;
            }
            Json ss = Json::array();
            for (const auto& sh : e.sheets) ss.push_back(sheetToJson(sh));
            je["sheets"] = ss;
            return je;
        }
        if (e.isSpecification()) {  // §2.4е
            je["assembly"] = Json{{"element", e.specAssembly}};
            if (!e.specTitleBlock.empty()) {
                Json tb = Json::object();
                for (const auto& [k, v] : e.specTitleBlock) tb[k] = v;
                je["titleBlock"] = tb;
            }
            if (!e.specRows.empty()) je["rows"] = Json(e.specRows);
            if (!e.specExtraRows.empty()) je["extraRows"] = Json(e.specExtraRows);
            return je;
        }
        if (e.isAssembly()) {  // §2.4б
            Json is = Json::array();
            for (const auto& in : e.instances) {
                Json ji = Json::object();
                ji["id"] = in.id;
                ji["name"] = in.name;
                if (isLibraryRef(in.refFile))  // copy of a library package (§2.13)
                    ji["ref"] = Json{{"library", in.refFile.substr(std::char_traits<char>::length(LIBRARY_REF_PREFIX))}, {"element", in.refElement}};
                else
                    ji["ref"] = in.refFile.empty() ? Json{{"element", in.refElement}} : Json{{"file", in.refFile}, {"element", in.refElement}};
                ji["position"] = Json::array({in.position[0], in.position[1], in.position[2]});
                ji["rotation"] = Json::array({in.rotation[0], in.rotation[1], in.rotation[2], in.rotation[3]});
                if (in.fixed) ji["fixed"] = true;
                if (in.suppressed) ji["suppressed"] = true;
                if (in.hidden) ji["hidden"] = true;
                if (in.transparent) ji["transparent"] = true;
                if (in.color) ji["color"] = *in.color;  // FX12-ASM-003
                if (!in.variantRows.empty()) {  // §2.4б, S6-PARAM-004
                    Json vr = Json::array();
                    for (const auto& r : in.variantRows) vr.push_back(Json{{"table", r.table}, {"row", r.row}});
                    ji["variantRows"] = vr;
                }
                if (!in.overrides.empty()) {
                    Json ov = Json::array();
                    for (const auto& o : in.overrides) ov.push_back(Json{{"variable", o.variable}, {"expr", o.expr}});
                    ji["overrides"] = ov;
                }
                if (!in.form.empty()) {  // FX9-ASM-010
                    Json fm = Json::array();
                    for (const auto& f : in.form)
                        fm.push_back(Json{{"path", f.path},
                                          {"position", Json::array({f.position[0], f.position[1], f.position[2]})},
                                          {"rotation", Json::array({f.rotation[0], f.rotation[1], f.rotation[2], f.rotation[3]})}});
                    ji["form"] = fm;
                }
                is.push_back(ji);
            }
            je["instances"] = is;
            if (!e.features.empty()) {  // assembly features (assembly.md §7б)
                Json fs = Json::array();
                for (const auto& f : e.features) fs.push_back(featureToJson(*f));
                je["features"] = fs;
            }
            if (!e.sections.empty()) {
                Json ss = Json::array();
                for (const auto& sec : e.sections) ss.push_back(sectionToJson(sec));
                je["sections"] = ss;
            }
            if (!e.mates.empty()) {
                Json ms = Json::array();
                for (const auto& m : e.mates) ms.push_back(mateToJson(m));
                je["mates"] = ms;
            }
            if (!e.welds.empty()) je["welds"] = Json(e.welds);  // §2.4б, weld.md §2.1
            if (!e.annotations.empty()) je["annotations"] = Json(e.annotations);  // §2.4б: assembly annotations (FX12-PMI-003)
            if (!e.patterns.empty()) {  // §2.4б, assembly.md §7е
                Json ps = Json::array();
                for (const auto& p : e.patterns) ps.push_back(instancePatternToJson(p));
                je["patterns"] = ps;
            }
            if (e.exploded) {  // §2.4б, assembly.md §7д; an empty object is not written
                Json x = Json::object();
                if (e.exploded->active) x["active"] = true;
                if (e.exploded->placements) {
                    Json pl = Json::array();
                    for (const auto& p : *e.exploded->placements)
                        pl.push_back(Json{{"instance", p.instance},
                                          {"position", Json::array({p.position[0], p.position[1], p.position[2]})},
                                          {"rotation", Json::array({p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]})}});
                    x["placements"] = pl;
                }
                if (!e.exploded->suppressedMates.empty()) x["suppressedMates"] = e.exploded->suppressedMates;
                if (e.exploded->knownMates) x["knownMates"] = *e.exploded->knownMates;
                if (!x.empty()) je["exploded"] = x;
            }
            // Rollback of the assembly features (§2.4б, FX9-TREE-002): optional, the end is not written.
            if (e.rollback.kind == Rollback::Kind::Start) je["rollback"] = "start";
            else if (e.rollback.kind == Rollback::Kind::After) je["rollback"] = e.rollback.feature;
            return je;
        }
        if (e.transparent) je["transparent"] = true;
        if (e.sheetMetal) je["sheetMetal"] = *e.sheetMetal;
        if (e.material) je["material"] = canonicalMaterial(*e.material);
        if (e.color) je["color"] = *e.color;
        if (!e.faceColors.empty()) {
            Json fc = Json::array();
            for (const auto& [face, color] : e.faceColors) fc.push_back(Json{{"face", face}, {"color", color}});
            je["faceColors"] = fc;
        }
        switch (e.rollback.kind) {
            case Rollback::Kind::End: je["rollback"] = nullptr; break;
            case Rollback::Kind::Start: je["rollback"] = "start"; break;
            case Rollback::Kind::After: je["rollback"] = e.rollback.feature; break;
        }
        Json fs = Json::array();
        for (const auto& f : e.features) fs.push_back(featureToJson(*f));
        je["features"] = fs;
        if (!e.sections.empty()) {
            Json ss = Json::array();
            for (const auto& s : e.sections) ss.push_back(sectionToJson(s));
            je["sections"] = ss;
        }
        if (!e.annotations.empty()) {  // §2.4д
            Json as = Json::array();
            for (const auto& a : e.annotations) as.push_back(a);
            je["annotations"] = as;
        }
        if (e.restRoughness) je["restRoughness"] = canonicalRestRoughness(*e.restRoughness);  // §2.4, FX5-PMI-001
        if (e.context) {  // in-context design (assembly.md §7в)
            Json jc = Json{{"assembly", e.context->assembly}, {"instance", e.context->instance}};
            if (e.context->frozen) {
                Json fz = Json::array();
                for (const auto& fi : *e.context->frozen)
                    fz.push_back(Json{{"instance", fi.instance},
                                      {"position", Json::array({fi.position[0], fi.position[1], fi.position[2]})},
                                      {"rotation", Json::array({fi.rotation[0], fi.rotation[1], fi.rotation[2], fi.rotation[3]})},
                                      {"element", fi.element}});
                jc["frozen"] = fz;
            }
            je["context"] = jc;
        }
        return je;
    }
}

// ---------------------------------------------------------------- JSON (schema-validated) → model

sketch::EntityType entityTypeFrom(const std::string& s) {
    if (s == "line") return sketch::EntityType::Line;
    if (s == "circle") return sketch::EntityType::Circle;
    if (s == "arc") return sketch::EntityType::Arc;
    if (s == "projected") return sketch::EntityType::Projected;
    if (s == "text") return sketch::EntityType::Text;
    if (s == "ellipse") return sketch::EntityType::Ellipse;
    if (s == "ellipseArc") return sketch::EntityType::EllipseArc;
    if (s == "bezier") return sketch::EntityType::Bezier;
    if (s == "bspline") return sketch::EntityType::BSpline;
    return sketch::EntityType::Point;
}

Profiles profilesFrom(const Json& j) {
    Profiles p;
    if (j.is_string()) return p;
    p.all = false;
    for (const auto& r : j) p.regions.push_back(r);
    return p;
}

}  // namespace

sketch::Entity entityFromJson(const Json& je) {
    sketch::Entity e;
    e.id = je["id"];
    e.type = entityTypeFrom(je["type"]);
    if (je.contains("x")) e.x = je["x"];
    if (je.contains("y")) e.y = je["y"];
    if (je.contains("p1")) e.p1 = je["p1"];
    if (je.contains("p2")) e.p2 = je["p2"];
    if (je.contains("center")) e.center = je["center"];
    if (je.contains("r")) e.r = je["r"];
    if (je.contains("rx")) e.rx = je["rx"];
    if (je.contains("ry")) e.ry = je["ry"];
    if (je.contains("start")) e.start = je["start"];
    if (je.contains("end")) e.end = je["end"];
    if (je.contains("source")) e.source = je["source"];
    if (je.contains("anchor")) e.center = je["anchor"];
    if (je.contains("text")) e.text = je["text"];
    if (je.contains("styles"))  // text fragment styles (sketch.md §2.1, FX8-SK-007)
        for (const auto& o : je["styles"]) {
            font::StyleSpan sp;
            sp.from = static_cast<int>(o["from"].get<double>()), sp.to = static_cast<int>(o["to"].get<double>());
            sp.bold = o.value("bold", false), sp.italic = o.value("italic", false), sp.underline = o.value("underline", false);
            e.styles.push_back(sp);
        }
    if (je.contains("font")) e.font = je["font"];
    if (je.contains("height")) e.height = je["height"];
    if (je.contains("angle")) e.angle = je["angle"];
    if (je.contains("poles"))
        for (const auto& q : je["poles"]) e.poles.push_back(q);
    if (je.contains("weights"))
        for (const auto& w : je["weights"]) e.weights.push_back(w);
    if (je.contains("degree")) e.degree = je["degree"];
    if (je.contains("knots"))
        for (const auto& k : je["knots"]) e.knots.push_back(k);
    if (je.contains("mults"))
        for (const auto& m : je["mults"]) e.mults.push_back(m);
    if (je.contains("periodic")) e.periodic = je["periodic"];
    if (je.contains("construction")) e.construction = je["construction"];
    return e;
}

sketch::Constraint constraintFromJson(const Json& jc) {
    sketch::Constraint c;
    c.id = jc["id"];
    c.type = jc["type"];
    for (const auto& r : jc["refs"]) c.refs.push_back(r);
    if (jc.contains("target"))
        for (const auto& t : jc["target"]) c.target.push_back(t.get<double>());
    if (jc.contains("vector")) c.vector = std::array<double, 2>{jc["vector"][0].get<double>(), jc["vector"][1].get<double>()};
    if (jc.contains("angle")) c.rotation = jc["angle"].get<double>();
    if (jc.contains("value")) c.value = jc["value"].get<std::string>();
    if (jc.contains("driving")) c.driving = jc["driving"].get<bool>();
    if (jc.contains("arc")) c.arc = jc["arc"].get<std::string>();
    if (jc.contains("diametral")) c.diametral = jc["diametral"].get<bool>();
    if (jc.contains("branch")) {
        const Json& b = jc["branch"];
        sketch::Branch br;
        if (b.contains("side")) {
            br.kind = sketch::Branch::Kind::Side;
            br.side = static_cast<int>(b["side"].get<double>());
        } else if (b.contains("contact")) {
            br.kind = sketch::Branch::Kind::Contact;
            br.contact = b["contact"];
        } else {
            br.kind = sketch::Branch::Kind::Angle;
            br.sign = static_cast<int>(b["sign"].get<double>());
            br.supplementary = b["supplementary"];
        }
        c.branch = br;
    }
    if (jc.contains("labelOffset"))
        c.labelOffset = std::array<double, 2>{jc["labelOffset"][0].get<double>(), jc["labelOffset"][1].get<double>()};
    if (jc.contains("auto")) c.automatic = jc["auto"].get<bool>();
    if (jc.contains("yielded"))
        for (const auto& y : jc["yielded"]) c.yielded.push_back(constraintFromJson(y));
    return c;
}

sketch::Group groupFromJson(const Json& jg) {
    sketch::Group g;
    g.id = jg["id"];
    g.type = jg["type"];
    for (const auto& s : jg["sources"]) g.sources.push_back(s);
    for (auto it = jg["params"].begin(); it != jg["params"].end(); ++it) {
        const Json& v = it.value();
        if (v.is_string()) g.params[it.key()] = sketch::GroupParam::str(v.get<std::string>());
        else if (v.is_boolean()) g.params[it.key()] = sketch::GroupParam::boolean(v.get<bool>());
        else g.params[it.key()] = sketch::GroupParam::num(v.get<double>());
    }
    for (const auto& m : jg["members"]) g.members.push_back({m["role"].get<std::string>(), m["id"].get<std::string>()});
    return g;
}

namespace {

FeaturePtr featureFrom(const Json& j) {
    auto f = std::make_shared<Feature>();
    f->id = j["id"];
    f->name = j["name"];
    f->suppressed = j["suppressed"];
    if (j.contains("tolerances") && !j["tolerances"].empty()) f->tolerances = j["tolerances"];  // FX12-PMI-007
    const std::string type = j["type"];
    if (type == "sketch") {
        f->type = FeatureType::Sketch;
        sketch::SketchData s;
        if (j["plane"].contains("face")) s.plane.face = j["plane"]["face"];
        else if (j["plane"].contains("frame")) {
            std::array<double, 9> f{};
            const Json& jf = j["plane"]["frame"];
            for (int k = 0; k < 3; ++k) {
                f[size_t(k)] = jf["origin"][size_t(k)].get<double>();
                f[size_t(3 + k)] = jf["x"][size_t(k)].get<double>();
                f[size_t(6 + k)] = jf["y"][size_t(k)].get<double>();
            }
            s.plane.frame = f;
        } else if (j["plane"].contains("feature")) s.plane.feature = j["plane"]["feature"];
        else s.plane.datum = j["plane"]["datum"];
        for (const auto& je : j["entities"]) s.entities.push_back(entityFromJson(je));
        for (const auto& jc : j["constraints"]) s.constraints.push_back(constraintFromJson(jc));
        if (j.contains("groups"))
            for (const auto& jg : j["groups"]) s.groups.push_back(groupFromJson(jg));
        if (j.contains("weakAnchors"))
            for (const auto& a : j["weakAnchors"]) s.weakAnchors.push_back(a);
        f->data = std::move(s);
    } else if (type == "extrude") {
        f->type = FeatureType::Extrude;
        ExtrudeData x;
        x.sketch = j["sketch"];
        x.profiles = profilesFrom(j["profiles"]);
        x.reverse = j["direction"] == "reverse";
        x.endType = j["end"]["type"];
        if (j["end"].contains("depth")) x.depth = j["end"]["depth"];
        if (j["end"].contains("to")) x.upTo = j["end"]["to"];
        if (j["end"].contains("offset")) x.upToOffset = j["end"]["offset"];
        x.operation = j["operation"];
        if (j["targets"].is_array())
            for (const auto& t : j["targets"]) x.targets.push_back(t);
        if (j.contains("tab")) x.tabLeft = j["tab"].value("left", std::string()), x.tabRight = j["tab"].value("right", std::string());
        f->data = std::move(x);
    } else if (type == "revolve") {
        f->type = FeatureType::Revolve;
        RevolveData r;
        r.sketch = j["sketch"];
        r.profiles = profilesFrom(j["profiles"]);
        if (j["axis"].contains("datum")) r.axisDatum = j["axis"]["datum"];
        else if (j["axis"].contains("feature")) r.axisFeature = j["axis"]["feature"];
        else r.axisSketchLine = j["axis"]["sketchLine"];
        r.angleType = j["angle"]["type"];
        if (j["angle"].contains("value")) r.angleValue = j["angle"]["value"];
        r.operation = j["operation"];
        f->data = std::move(r);
    } else if (type == "pattern" || type == "mirror" || type == "shell" || type == "draft" || type == "sweep" || type == "loft" ||
               type == "hole" || type == "thread" || type == "import" || type.rfind("sheet", 0) == 0 || type == "gear" ||
               type == "connection" || type == "connectionCut" || type == "coil") {
        f->type = type == "pattern"  ? FeatureType::Pattern
                  : type == "mirror" ? FeatureType::Mirror
                  : type == "shell"  ? FeatureType::Shell
                  : type == "draft"  ? FeatureType::Draft
                  : type == "sweep"  ? FeatureType::Sweep
                  : type == "loft"   ? FeatureType::Loft
                  : type == "hole"   ? FeatureType::Hole
                  : type == "import" ? FeatureType::Import
                  : type == "sheetMetal"  ? FeatureType::SheetMetal
                  : type == "sheetWall"   ? FeatureType::SheetWall
                  : type == "sheetFlange" ? FeatureType::SheetFlange
                  : type == "sheetBend"   ? FeatureType::SheetBend
                  : type == "sheetCut"    ? FeatureType::SheetCut
                  : type == "gear"        ? FeatureType::Gear
                  : type == "connection"  ? FeatureType::Connection
                  : type == "connectionCut" ? FeatureType::ConnectionCut
                  : type == "coil"        ? FeatureType::Coil
                                     : FeatureType::Thread;
        PatternData p;
        p.def = Json::object();
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.key() != "id" && it.key() != "type" && it.key() != "name" && it.key() != "suppressed" && it.key() != "tolerances") p.def[it.key()] = it.value();
        f->data = std::move(p);
    } else if (type == "plane" || type == "axis") {
        f->type = type == "plane" ? FeatureType::Plane : FeatureType::Axis;
        RefGeomData g;
        g.def = Json::object();
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.key() != "id" && it.key() != "type" && it.key() != "name" && it.key() != "suppressed" && it.key() != "tolerances") g.def[it.key()] = it.value();
        f->data = std::move(g);
    } else {
        f->type = type == "fillet" ? FeatureType::Fillet : FeatureType::Chamfer;
        EdgeFeatureData e;
        for (const auto& x : j["edges"]) e.edges.push_back(x);
        e.size = j[type == "fillet" ? "radius" : "distance"];
        if (j.contains("mode") && j["mode"] != "distance") {
            e.mode = j["mode"];
            const char* key = e.mode == "distanceAngle" ? "angle" : "distance2";
            if (j.contains(key)) e.size2 = j[key];
            e.flip = j.value("flip", false);
        }
        f->data = std::move(e);
    }
    return f;
}

Element elementFrom(const Json& je);

Document documentFrom(const Json& j) {
    Document d;
    if (j.contains("savedBy")) d.savedBy = SavedBy{j["savedBy"]["app"], j["savedBy"]["version"]};
    d.lengthUnit = j["units"]["length"];
    d.angleUnit = j["units"]["angle"];
    const Json& c = j["counters"];
    d.counters = {static_cast<int64_t>(c["element"].get<double>()), static_cast<int64_t>(c["feature"].get<double>()),
                  static_cast<int64_t>(c["entity"].get<double>()), static_cast<int64_t>(c["constraint"].get<double>()),
                  static_cast<int64_t>(c["variable"].get<double>())};
    if (c.contains("instance")) d.counters.instance = static_cast<int64_t>(c["instance"].get<double>());
    if (c.contains("mate")) d.counters.mate = static_cast<int64_t>(c["mate"].get<double>());
    if (c.contains("sheet")) d.counters.sheet = static_cast<int64_t>(c["sheet"].get<double>());
    if (c.contains("view")) d.counters.view = static_cast<int64_t>(c["view"].get<double>());
    if (c.contains("annotation")) d.counters.annotation = static_cast<int64_t>(c["annotation"].get<double>());
    if (c.contains("dimension")) d.counters.dimension = static_cast<int64_t>(c["dimension"].get<double>());
    if (c.contains("group")) d.counters.group = static_cast<int64_t>(c["group"].get<double>());
    if (c.contains("table")) d.counters.table = static_cast<int64_t>(c["table"].get<double>());
    if (c.contains("weld")) d.counters.weld = static_cast<int64_t>(c["weld"].get<double>());
    if (c.contains("pattern")) d.counters.pattern = static_cast<int64_t>(c["pattern"].get<double>());
    if (c.contains("text")) d.counters.text = static_cast<int64_t>(c["text"].get<double>());
    if (c.contains("mark")) d.counters.mark = static_cast<int64_t>(c["mark"].get<double>());
    if (c.contains("construction")) d.counters.construction = static_cast<int64_t>(c["construction"].get<double>());
    if (c.contains("elementGroup")) d.counters.elementGroup = static_cast<int64_t>(c["elementGroup"].get<double>());
    for (const auto& jv : j["variables"]) {
        Variable v;
        v.id = jv["id"];
        v.name = jv["name"];
        if (jv.contains("type")) v.type = jv["type"].get<std::string>();
        v.expr = jv["expr"];
        if (jv.contains("values")) v.values = jv["values"].get<std::vector<std::string>>();
        if (jv.contains("comment")) v.comment = jv["comment"].get<std::string>();
        d.variables.push_back(v);
    }
    if (j.contains("variantTables"))  // §2.3а
        for (const auto& jt : j["variantTables"]) {
            VariantTable t;
            t.id = jt["id"];
            t.name = jt["name"];
            t.columns = jt["columns"].get<std::vector<std::string>>();
            for (const auto& jr : jt["variants"]) t.variants.push_back({jr["name"], jr["values"].get<std::vector<std::string>>()});
            t.active = jt["active"];
            d.variantTables.push_back(std::move(t));
        }
    if (j.contains("elementGroups"))  // §2.1а, FX10-TREE-001
        for (const auto& jg : j["elementGroups"]) d.elementGroups.push_back({jg["id"], jg["name"]});
    for (const auto& je : j["elements"]) d.elements.push_back(elementFrom(je));
    if (j.contains("libraryParts"))  // §2.13
        for (const auto& jl : j["libraryParts"]) {
            LibraryPart lp;
            lp.id = jl["id"];
            lp.package = jl["package"];
            lp.fingerprint = jl["fingerprint"];
            lp.title = jl["title"];
            lp.standard = jl["standard"];
            lp.designation = jl["designation"];
            lp.sizesTable = jl["sizesTable"];
            lp.document = std::make_shared<const Document>(documentFrom(jl["document"]));
            d.libraryParts.push_back(std::move(lp));
        }
    if (j.contains("solutions"))  // §2.14
        for (const auto& js : j["solutions"]) {
            SolutionCopy sc;
            sc.id = js["id"];
            sc.package = js["package"];
            sc.fingerprint = js["fingerprint"];
            sc.title = js["title"];
            sc.definition = js["definition"];
            if (js.contains("tables"))
                for (const auto& jt : js["tables"]) {
                    SolutionTable t;
                    t.id = jt["id"];
                    t.columns = jt["columns"].get<std::vector<std::string>>();
                    for (const auto& jr : jt["rows"]) t.rows.push_back({jr["name"], jr["values"].get<std::vector<std::string>>()});
                    sc.tables.push_back(std::move(t));
                }
            d.solutions.push_back(std::move(sc));
        }
    return d;
}

Error docError(const char* code, const std::string& path);

// Instance references with both file and library (§2.4б): the schema allows each key, the combination is an error.
Status checkInstanceRefs(const Json& j) {
    for (size_t ei = 0; ei < j["elements"].size(); ++ei) {
        const Json& je = j["elements"][ei];
        if (!je.contains("instances")) continue;
        for (size_t ii = 0; ii < je["instances"].size(); ++ii) {
            const Json& r = je["instances"][ii]["ref"];
            if (r.contains("file") && r.contains("library"))
                return docError("DOC_SCHEMA", "/elements/" + std::to_string(ei) + "/instances/" + std::to_string(ii) + "/ref/library")
                    .with("name", std::string("library"));
        }
    }
    if (j.contains("libraryParts"))
        for (size_t li = 0; li < j["libraryParts"].size(); ++li)
            if (auto s = checkInstanceRefs(j["libraryParts"][li]["document"]); !s) {
                Error e = s.error();
                e.at("/libraryParts/" + std::to_string(li) + "/document" + e.path.value_or(""));
                return e;
            }
    return okStatus();
}

Element elementFrom(const Json& je) {
    {
        Element e;
        e.id = je["id"];
        e.kind = je["kind"];
        e.name = je["name"];
        if (je.contains("group")) e.group = je["group"].get<std::string>();  // §2.1а
        if (je.contains("designation")) e.designation = je["designation"].get<std::string>();
        if (je.contains("material")) e.material = canonicalMaterial(je["material"]);
        if (je.contains("color")) e.color = je["color"].get<std::string>();
        if (je.contains("faceColors"))
            for (const auto& fc : je["faceColors"]) e.faceColors.push_back({fc["face"], fc["color"]});
        e.transparent = je.value("transparent", false);
        if (je.contains("sheetMetal")) e.sheetMetal = je["sheetMetal"].get<bool>();
        if (e.isDrawing()) {  // §2.4г
            if (je.contains("standard")) e.standard = je["standard"].get<std::string>();
            if (je.contains("projection")) e.projection = je["projection"].get<std::string>();
            e.nameManual = je.value("nameManual", false);
            if (je.contains("dismissedMarks"))
                for (const auto& dj : je["dismissedMarks"])
                    e.dismissedMarks.push_back({dj["kind"].get<std::string>(), dj["model"].get<std::string>(), dj.value("source", std::string())});
            for (const auto& js : je["sheets"]) e.sheets.push_back(sheetFromJson(js));
            e.rollback = {Rollback::Kind::End, {}};
            return e;
        }
        if (e.isSpecification()) {  // §2.4е
            e.specAssembly = je["assembly"]["element"];
            if (je.contains("titleBlock"))
                for (auto it = je["titleBlock"].begin(); it != je["titleBlock"].end(); ++it) e.specTitleBlock[it.key()] = it.value();
            if (je.contains("rows"))
                for (const auto& r : je["rows"]) e.specRows.push_back(r);
            if (je.contains("extraRows"))
                for (const auto& r : je["extraRows"]) e.specExtraRows.push_back(r);
            e.rollback = {Rollback::Kind::End, {}};
            return e;
        }
        if (e.isAssembly()) {
            for (const auto& ji : je["instances"]) {
                Instance in;
                in.id = ji["id"];
                in.name = ji["name"];
                in.refElement = ji["ref"]["element"];
                if (ji["ref"].contains("file")) in.refFile = ji["ref"]["file"];
                if (ji["ref"].contains("library")) in.refFile = libraryRef(ji["ref"]["library"].get<std::string>());
                for (int k = 0; k < 3; ++k) in.position[size_t(k)] = ji["position"][size_t(k)].get<double>();
                for (int k = 0; k < 4; ++k) in.rotation[size_t(k)] = ji["rotation"][size_t(k)].get<double>();
                in.fixed = ji.value("fixed", false);
                in.suppressed = ji.value("suppressed", false);
                in.hidden = ji.value("hidden", false);
                in.transparent = ji.value("transparent", false);
                if (ji.contains("color")) in.color = ji["color"].get<std::string>();  // FX12-ASM-003
                if (ji.contains("variantRows"))
                    for (const auto& r : ji["variantRows"]) in.variantRows.push_back({r["table"], r["row"]});
                if (ji.contains("overrides"))
                    for (const auto& o : ji["overrides"]) in.overrides.push_back({o["variable"], o["expr"]});
                if (ji.contains("form"))  // FX9-ASM-010
                    for (const auto& f : ji["form"]) {
                        Instance::FormPlacement fp;
                        fp.path = f["path"];
                        for (int k = 0; k < 3; ++k) fp.position[size_t(k)] = f["position"][size_t(k)].get<double>();
                        for (int k = 0; k < 4; ++k) fp.rotation[size_t(k)] = f["rotation"][size_t(k)].get<double>();
                        in.form.push_back(fp);
                    }
                e.instances.push_back(in);
            }
            if (je.contains("mates"))
                for (const auto& jm : je["mates"]) e.mates.push_back(mateFromJson(jm));
            if (je.contains("welds"))
                for (const auto& jw : je["welds"]) e.welds.push_back(jw);
            if (je.contains("annotations"))  // FX12-PMI-003
                for (const auto& ja : je["annotations"]) e.annotations.push_back(ja);
            if (je.contains("patterns"))
                for (const auto& jp : je["patterns"]) e.patterns.push_back(instancePatternFromJson(jp));
            if (je.contains("exploded")) {
                const Json& x = je["exploded"];
                Exploded ex;
                ex.active = x.value("active", false);
                if (x.contains("placements")) {
                    ex.placements.emplace();
                    for (const auto& p : x["placements"]) {
                        ExplodedPlacement ep;
                        ep.instance = p["instance"];
                        for (int k = 0; k < 3; ++k) ep.position[size_t(k)] = p["position"][size_t(k)].get<double>();
                        for (int k = 0; k < 4; ++k) ep.rotation[size_t(k)] = p["rotation"][size_t(k)].get<double>();
                        ex.placements->push_back(ep);
                    }
                }
                if (x.contains("suppressedMates"))
                    for (const auto& m : x["suppressedMates"]) ex.suppressedMates.push_back(m);
                if (x.contains("knownMates")) {
                    ex.knownMates.emplace();
                    for (const auto& m : x["knownMates"]) ex.knownMates->push_back(m);
                }
                e.exploded = ex;
            }
            if (je.contains("features"))
                for (const auto& jf : je["features"]) e.features.push_back(featureFrom(jf));
            if (je.contains("sections"))
                for (const auto& js : je["sections"]) e.sections.push_back(sectionFromJson(js));
            e.rollback = {Rollback::Kind::End, {}};  // absent — the end (§2.4б, FX9-TREE-002)
            if (je.contains("rollback") && je["rollback"].is_string())
                e.rollback = je["rollback"] == "start" ? Rollback{Rollback::Kind::Start, {}} : Rollback{Rollback::Kind::After, je["rollback"].get<std::string>()};
            return e;
        }
        std::sort(e.faceColors.begin(), e.faceColors.end());
        if (je["rollback"].is_null()) e.rollback = {Rollback::Kind::End, {}};
        else if (je["rollback"] == "start") e.rollback = {Rollback::Kind::Start, {}};
        else e.rollback = {Rollback::Kind::After, je["rollback"].get<std::string>()};
        for (const auto& jf : je["features"]) e.features.push_back(featureFrom(jf));
        if (je.contains("sections"))
            for (const auto& js : je["sections"]) e.sections.push_back(sectionFromJson(js));
        if (je.contains("annotations"))
            for (const auto& ja : je["annotations"]) e.annotations.push_back(ja);
        if (je.contains("restRoughness")) e.restRoughness = canonicalRestRoughness(je["restRoughness"]);
        if (je.contains("context")) {  // assembly.md §7в
            PartContext c;
            c.assembly = je["context"]["assembly"];
            c.instance = je["context"]["instance"];
            if (je["context"].contains("frozen")) {
                c.frozen.emplace();
                for (const auto& jf : je["context"]["frozen"]) {
                    FrozenInstance fi;
                    fi.instance = jf["instance"];
                    for (int k = 0; k < 3; ++k) fi.position[size_t(k)] = jf["position"][size_t(k)].get<double>();
                    for (int k = 0; k < 4; ++k) fi.rotation[size_t(k)] = jf["rotation"][size_t(k)].get<double>();
                    fi.element = jf["element"];
                    c.frozen->push_back(std::move(fi));
                }
            }
            e.context = std::move(c);
        }
        return e;
    }
}

// ---------------------------------------------------------------- semantic validation (stage 6)

int64_t idNumber(const std::string& id) {
    size_t i = 0;
    while (i < id.size() && id[i] >= 'a' && id[i] <= 'z') ++i;
    return std::stoll(id.substr(i));
}

Error docError(const char* code, const std::string& path) {
    Error e(code);
    e.at(path);
    return e;
}

// Consistency of annotation fields (document.md §5 "DOC_SCHEMA (annotations)"); input is schema-validated.
Status checkAnnotation(const Json& a, const std::string& path) {
    auto bad = [&](const std::string& name) { return docError("DOC_SCHEMA", path).with("name", name); };
    const std::string type = a["type"].get<std::string>();
    if (type == "roughness") {
        if (a.contains("parameter") != a.contains("value")) return bad(a.contains("parameter") ? "value" : "parameter");
    } else if (type == "fit") {
        if (a.contains("dimension") == a.contains("face")) return bad("dimension");
        if (a.contains("at") && a.contains("dimension")) return bad("at");  // click point — only for a face (§2.4д)
        if (a.contains("labelOffset") && a.contains("dimension")) return bad("labelOffset");  // sign position — only for a face (FX12-PMI-002)
        const bool field = a.contains("field"), up = a.contains("upper"), lo = a.contains("lower");
        if (field && (up || lo)) return bad("field");
        if (!field && !up && !lo) return bad("field");
        if (!field && up != lo) return bad(up ? "lower" : "upper");
        if (up && lo && a["upper"].get<double>() <= a["lower"].get<double>()) return bad("upper");
        if (a.contains("showDeviations") && !field) return bad("showDeviations");
    } else if (type == "geometric") {
        const std::string c = a["characteristic"].get<std::string>();
        if (a.contains("datums") != pmi::needsDatums(c)) return bad("datums");
        if (a.contains("datums")) {
            std::set<std::string> seen;
            for (const auto& l : a["datums"])
                if (!pmi::validDatumLetter(l.get<std::string>()) || !seen.insert(l.get<std::string>()).second) return bad("datums");
        }
        if (a.contains("diameter") && !pmi::allowsDiameter(c)) return bad("diameter");
        if (a.contains("material") && !pmi::allowsMaterial(c)) return bad("material");
    } else if (type == "datum") {
        if (!pmi::validDatumLetter(a["letter"].get<std::string>())) return bad("letter");
    }
    return okStatus();
}

std::string elPath(size_t ei) { return "/elements/" + std::to_string(ei); }
std::string fPath(size_t ei, size_t fi) { return elPath(ei) + "/features/" + std::to_string(fi); }

struct EntityLoc {
    size_t element, feature;  // owning sketch
    const sketch::Entity* entity;
};

// DOC_BAD_REF_KIND of linked sketch groups (document.md §5, sketch.md §2.6.1).
Status checkGroupKinds(const sketch::SketchData& s, const std::string& fp, size_t ei, size_t fi,
                       const std::map<std::string, EntityLoc>& entities,
                       const std::map<std::string, std::pair<size_t, size_t>>& constraintsAt) {
    auto bad = [](const std::string& path, const std::string& id, const char* expected) {
        return Status(docError("DOC_BAD_REF_KIND", path).with("id", id).with("expected", std::string(expected)));
    };
    auto here = [&](const std::string& id) {
        if (auto it = entities.find(id); it != entities.end()) return it->second.element == ei && it->second.feature == fi;
        if (auto it = constraintsAt.find(id); it != constraintsAt.end()) return it->second.first == ei && it->second.second == fi;
        return false;
    };
    std::map<std::string, size_t> memberOf;  // object → group number
    for (size_t k = 0; k < s.groups.size(); ++k) {
        const sketch::Group& g = s.groups[k];
        const std::string gp = fp + "/groups/" + std::to_string(k);
        std::set<std::string> seen;
        for (size_t r = 0; r < g.sources.size(); ++r) {
            const std::string path = gp + "/sources/" + std::to_string(r);
            if (!here(g.sources[r])) return bad(path, g.sources[r], "sameSketch");
            if (!seen.insert(g.sources[r]).second) return bad(path, g.sources[r], "distinct");
        }
        if (auto it = g.params.find("center"); it != g.params.end() && it->second.text != "origin") {
            const std::string& c = it->second.text;
            if (!here(c)) return bad(gp + "/params/center", c, "sameSketch");
            if (entities.at(c).entity->type != sketch::EntityType::Point) return bad(gp + "/params/center", c, "point");
        }
        if (auto it = g.params.find("line"); it != g.params.end()) {  // axis segment (S4-SKT-006)
            const std::string& l = it->second.text;
            if (!here(l)) return bad(gp + "/params/line", l, "sameSketch");
            if (entities.at(l).entity->type != sketch::EntityType::Line) return bad(gp + "/params/line", l, "line");
            if (g.hasMember(l)) return bad(gp + "/params/line", l, "member");
        }
        seen.clear();
        for (size_t r = 0; r < g.members.size(); ++r) {
            const std::string& id = g.members[r].id;
            const std::string path = gp + "/members/" + std::to_string(r) + "/id";
            if (!here(id)) return bad(path, id, "sameSketch");
            if (!seen.insert(id).second) return bad(path, id, "distinct");
            if (!memberOf.emplace(id, k).second) return bad(path, id, "member");
            if (std::find(g.sources.begin(), g.sources.end(), id) != g.sources.end()) return bad(path, id, "member");
        }
    }
    // Points of a member curve are members of the same group, and vice versa.
    for (const auto& e : s.entities) {
        std::vector<std::string> pts;
        if (e.type == sketch::EntityType::Line) pts = {e.p1, e.p2};
        else if (e.type == sketch::EntityType::Circle) pts = {e.center};
        else if (e.type == sketch::EntityType::Arc || e.type == sketch::EntityType::EllipseArc) pts = {e.center, e.start, e.end};
        else if (e.type == sketch::EntityType::Ellipse) pts = {e.center};
        else if (e.type == sketch::EntityType::Text) pts = {e.center};
        else if (sketch::isSplineType(e.type)) pts = e.poles;
        const auto own = memberOf.find(e.id);
        for (const auto& p : pts) {
            const auto pm = memberOf.find(p);
            const bool ownIn = own != memberOf.end(), ptIn = pm != memberOf.end();
            if (ownIn == ptIn && (!ownIn || own->second == pm->second)) continue;
            const auto& m = ownIn ? *own : *pm;
            const auto& g = s.groups[m.second];
            size_t r = 0;
            while (r < g.members.size() && g.members[r].id != m.first) ++r;
            return bad(fp + "/groups/" + std::to_string(m.second) + "/members/" + std::to_string(r) + "/id", m.first, "member");
        }
    }
    // Weak dimension anchor points (S4-WEAK-006): distinct points of this sketch.
    std::set<std::string> anchors;
    for (size_t r = 0; r < s.weakAnchors.size(); ++r) {
        const std::string& id = s.weakAnchors[r];
        const std::string path = fp + "/weakAnchors/" + std::to_string(r);
        if (!here(id)) return bad(path, id, "sameSketch");
        if (entities.at(id).entity->type != sketch::EntityType::Point) return bad(path, id, "point");
        if (!anchors.insert(id).second) return bad(path, id, "distinct");
    }
    return okStatus();
}

// Cut owned by a connection (§2.10о): an orthonormal frame, exactly one of revolve / box with valid numbers, positive
// thread and diameter.
Status checkConnectionCut(const Json& def, const std::string& fp) {
    auto bad = [&](const char* name) { return docError("DOC_SCHEMA", fp + "/" + name).with("name", std::string(name)); };
    const Json& fr = def["frame"];
    using V3 = std::array<double, 3>;
    auto vec = [&](const char* k) { return V3{fr[k][0].get<double>(), fr[k][1].get<double>(), fr[k][2].get<double>()}; };
    auto dot = [](const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const V3 x = vec("x"), y = vec("y"), z = vec("z");
    for (const V3* v : {&x, &y, &z})
        if (!(std::abs(std::sqrt(dot(*v, *v)) - 1) <= DOC_FRAME_TOL)) return bad("frame");
    if (!(std::abs(dot(x, y)) <= DOC_FRAME_TOL && std::abs(dot(y, z)) <= DOC_FRAME_TOL && std::abs(dot(z, x)) <= DOC_FRAME_TOL)) return bad("frame");
    const bool rev = def.contains("revolve"), box = def.contains("box");
    if (rev == box) return bad(rev ? "box" : "revolve");
    if (rev)
        for (const auto& v : def["revolve"])
            if (!(v[0].get<double>() >= 0)) return bad("revolve");
    if (box)
        for (const char* k : {"x", "y", "z"})
            if (!(def["box"][k][0].get<double>() < def["box"][k][1].get<double>())) return bad("box");
    if (def.contains("thread"))
        for (const char* k : {"d", "pitch", "length"})
            if (!(def["thread"][k].get<double>() > 0)) return bad("thread");
    if (def.contains("diameter") && !(def["diameter"].get<double>() > 0)) return bad("diameter");
    return okStatus();
}

Status semanticValidate(const Document& d) {
    // Element groups (§2.1а): a member refers to an existing group.
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        if (!d.elements[ei].group.empty() && !d.elementGroup(d.elements[ei].group))
            return docError("DOC_DANGLING_REF", "/elements/" + std::to_string(ei) + "/group").with("id", d.elements[ei].group);
    // Indexes.
    std::map<std::string, EntityLoc> entities;
    std::map<std::string, std::pair<size_t, size_t>> features;
    std::map<std::string, std::pair<size_t, size_t>> constraintsAt;  // constraint → sketch (group members, sketch.md §2.6)
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t fi = 0; fi < d.elements[ei].features.size(); ++fi) {
            const Feature& f = *d.elements[ei].features[fi];
            features.emplace(f.id, std::make_pair(ei, fi));
            if (const auto* s = f.sketchData()) {
                for (const auto& e : s->entities) entities.emplace(e.id, EntityLoc{ei, fi, &e});
                for (const auto& c : s->constraints) constraintsAt.emplace(c.id, std::make_pair(ei, fi));
            }
        }

    // DOC_SCHEMA (variables and tables, §5): list values, table cells and active row, instance variables.
    for (size_t i = 0; i < d.variables.size(); ++i) {
        const Variable& v = d.variables[i];
        const std::string vp = "/variables/" + std::to_string(i);
        if ((v.type == "list") != !v.values.empty()) return docError("DOC_SCHEMA", vp).with("name", std::string("values"));
        std::set<std::string> seen;
        for (const auto& x : v.values)
            if (!seen.insert(x).second) return docError("DOC_SCHEMA", vp + "/values").with("name", std::string("values"));
    }
    for (size_t ti = 0; ti < d.variantTables.size(); ++ti) {
        const VariantTable& t = d.variantTables[ti];
        const std::string tp = "/variantTables/" + std::to_string(ti);
        for (size_t r = 0; r < t.variants.size(); ++r)
            if (t.variants[r].values.size() != t.columns.size())
                return docError("DOC_SCHEMA", tp + "/variants/" + std::to_string(r) + "/values").with("name", std::string("values"));
        if (!t.row(t.active)) return docError("DOC_SCHEMA", tp + "/active").with("name", std::string("active"));
    }
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t ii = 0; ii < d.elements[ei].instances.size(); ++ii) {
            const Instance& in = d.elements[ei].instances[ii];
            const std::string ip = "/elements/" + std::to_string(ei) + "/instances/" + std::to_string(ii);
            std::set<std::string> tables, vars;
            for (const auto& r : in.variantRows)
                if (!tables.insert(r.table).second) return docError("DOC_SCHEMA", ip + "/variantRows").with("name", std::string("variantRows"));
            for (const auto& o : in.overrides)
                if (!vars.insert(o.variable).second) return docError("DOC_SCHEMA", ip + "/overrides").with("name", std::string("overrides"));
            std::set<std::string> paths;  // FX9-ASM-010: a path once, unit rotation
            for (size_t fi = 0; fi < in.form.size(); ++fi) {
                const auto& f = in.form[fi];
                const std::string fp = ip + "/form/" + std::to_string(fi);
                if (!paths.insert(f.path).second) return docError("DOC_SCHEMA", fp + "/path").with("name", std::string("path"));
                double q = 0;
                for (double c : f.rotation) q += c * c;
                if (std::abs(std::sqrt(q) - 1) > 1e-6) return docError("DOC_SCHEMA", fp + "/rotation").with("name", std::string("rotation"));
            }
        }

    // Exploded view (§2.4б, FX7-ASM-005): placements and suppressed mates without repeats.
    for (size_t ei = 0; ei < d.elements.size(); ++ei) {
        const auto& x = d.elements[ei].exploded;
        if (!x) continue;
        const std::string xp = "/elements/" + std::to_string(ei) + "/exploded";
        std::set<std::string> seen;
        if (x->placements)
            for (const auto& p : *x->placements)
                if (!seen.insert(p.instance).second) return docError("DOC_SCHEMA", xp + "/placements").with("name", std::string("placements"));
        seen.clear();
        for (const auto& m : x->suppressedMates)
            if (!seen.insert(m).second) return docError("DOC_SCHEMA", xp + "/suppressedMates").with("name", std::string("suppressedMates"));
        seen.clear();
        if (x->knownMates)
            for (const auto& m : *x->knownMates)
                if (!seen.insert(m).second) return docError("DOC_SCHEMA", xp + "/knownMates").with("name", std::string("knownMates"));
    }

    // Library part copies (§2.13): key, uniqueness, sizes table, self-contained copy; instance references to copies.
    {
        std::set<std::string> ids;
        for (size_t li = 0; li < d.libraryParts.size(); ++li) {
            const LibraryPart& lp = d.libraryParts[li];
            const std::string lpp = "/libraryParts/" + std::to_string(li);
            if (lp.id != lp.package + "@" + lp.fingerprint) return docError("DOC_SCHEMA", lpp + "/id").with("name", std::string("id"));
            if (!ids.insert(lp.id).second) return docError("DOC_DUPLICATE_ID", lpp + "/id").with("id", lp.id);
            const Document& c = *lp.document;
            bool links = !c.libraryParts.empty();
            for (const auto& el : c.elements) {
                for (const auto& in : el.instances) links = links || !in.refFile.empty();
                for (const auto& sh : el.sheets)
                    for (const auto& v : sh.views) links = links || !v.refFile.empty();
            }
            if (links) return docError("DOC_SCHEMA", lpp + "/document").with("name", std::string("document"));
            if (auto st = semanticValidate(c); !st) {
                Error e = st.error();
                e.at(lpp + "/document" + e.path.value_or(""));
                return e;
            }
            if (!c.variantTable(lp.sizesTable)) return docError("DOC_DANGLING_REF", lpp + "/sizesTable").with("id", lp.sizesTable);
        }
        for (size_t ei = 0; ei < d.elements.size(); ++ei)
            for (size_t ii = 0; ii < d.elements[ei].instances.size(); ++ii) {
                const std::string& ref = d.elements[ei].instances[ii].refFile;
                if (!isLibraryRef(ref)) continue;
                const std::string id = ref.substr(std::char_traits<char>::length(LIBRARY_REF_PREFIX));
                if (!d.libraryPart(id))
                    return docError("DOC_DANGLING_REF", "/elements/" + std::to_string(ei) + "/instances/" + std::to_string(ii) + "/ref/library")
                        .with("id", id);
            }
    }
    // Solution copies (§2.14): key, uniqueness; connection references to solution and library copies (§2.10н).
    {
        std::set<std::string> ids;
        for (size_t si = 0; si < d.solutions.size(); ++si) {
            const SolutionCopy& sc = d.solutions[si];
            const std::string sp = "/solutions/" + std::to_string(si);
            if (sc.id != sc.package + "@" + sc.fingerprint) return docError("DOC_SCHEMA", sp + "/id").with("name", std::string("id"));
            if (!ids.insert(sc.id).second) return docError("DOC_DUPLICATE_ID", sp + "/id").with("id", sc.id);
        }
        for (size_t ei = 0; ei < d.elements.size(); ++ei)
            for (size_t fi = 0; fi < d.elements[ei].features.size(); ++fi) {
                const Feature& f = *d.elements[ei].features[fi];
                if (f.type != FeatureType::Connection || !f.pattern()) continue;
                const std::string fp = "/elements/" + std::to_string(ei) + "/features/" + std::to_string(fi);
                const Json& def = f.pattern()->def;
                // Values: inputs — a stable name, a list of them (axes), a point [x, y, z] or the points of a sketch
                // {"sketch": id} (FX8-BLT-002);
                // choices — a string or a boolean; tables — copy IDs.
                for (auto it = def["inputs"].begin(); it != def["inputs"].end(); ++it) {
                    const Json& v = it.value();
                    const bool point = v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number();
                    const bool sketch = v.is_object() && v.size() == 1 && v.contains("sketch") && v["sketch"].is_string() &&
                                        !v["sketch"].get<std::string>().empty();
                    // A list of axes (solutions.md §7.11, FX10-WZ-005): one connection on several hole axes.
                    const bool axes = v.is_array() && !v.empty() && std::all_of(v.begin(), v.end(), [](const Json& a) {
                                          return a.is_string() && !a.get<std::string>().empty();
                                      });
                    if (!(v.is_string() && !v.get<std::string>().empty()) && !point && !sketch && !axes)
                        return docError("DOC_SCHEMA", fp + "/inputs/" + pointerSegment(it.key())).with("name", std::string("inputs"));
                }
                for (auto it = def["choices"].begin(); it != def["choices"].end(); ++it)
                    if (!it.value().is_string() && !it.value().is_boolean())
                        return docError("DOC_SCHEMA", fp + "/choices/" + pointerSegment(it.key())).with("name", std::string("choices"));
                if (def.contains("tables"))
                    for (auto it = def["tables"].begin(); it != def["tables"].end(); ++it)
                        if (!it.value().is_string())
                            return docError("DOC_SCHEMA", fp + "/tables/" + pointerSegment(it.key())).with("name", std::string("tables"));
                const std::string sid = def["solution"];
                if (!d.solution(sid)) return docError("DOC_DANGLING_REF", fp + "/solution").with("id", sid);
                if (def.contains("tables"))
                    for (auto it = def["tables"].begin(); it != def["tables"].end(); ++it)
                        if (!d.libraryPart(it.value().get<std::string>()))
                            return docError("DOC_DANGLING_REF", fp + "/tables/" + pointerSegment(it.key())).with("id", it.value().get<std::string>());
            }
    }

    // Traversal of all IDs in canonical order: (id, path, kind).
    struct IdRef {
        std::string id, path;
        IdKind kind;
    };
    std::vector<IdRef> allIds;
    for (size_t i = 0; i < d.variables.size(); ++i)
        allIds.push_back({d.variables[i].id, "/variables/" + std::to_string(i) + "/id", IdKind::Variable});
    for (size_t i = 0; i < d.variantTables.size(); ++i)
        allIds.push_back({d.variantTables[i].id, "/variantTables/" + std::to_string(i) + "/id", IdKind::Table});
    for (size_t i = 0; i < d.elementGroups.size(); ++i)  // §2.1а
        allIds.push_back({d.elementGroups[i].id, "/elementGroups/" + std::to_string(i) + "/id", IdKind::ElementGroup});
    for (size_t ei = 0; ei < d.elements.size(); ++ei) {
        const Element& el = d.elements[ei];
        allIds.push_back({el.id, elPath(ei) + "/id", IdKind::Element});
        for (size_t fi = 0; fi < el.features.size(); ++fi) {
            const Feature& f = *el.features[fi];
            allIds.push_back({f.id, fPath(ei, fi) + "/id", IdKind::Feature});
            if (const auto* s = f.sketchData()) {
                for (size_t k = 0; k < s->entities.size(); ++k)
                    allIds.push_back({s->entities[k].id, fPath(ei, fi) + "/entities/" + std::to_string(k) + "/id", IdKind::Entity});
                for (size_t k = 0; k < s->constraints.size(); ++k)
                    allIds.push_back({s->constraints[k].id, fPath(ei, fi) + "/constraints/" + std::to_string(k) + "/id",
                                      IdKind::Constraint});
                for (size_t k = 0; k < s->groups.size(); ++k)
                    allIds.push_back({s->groups[k].id, fPath(ei, fi) + "/groups/" + std::to_string(k) + "/id", IdKind::Group});
            }
        }
        for (size_t si = 0; si < el.sections.size(); ++si)
            allIds.push_back({el.sections[si].id, "/elements/" + std::to_string(ei) + "/sections/" + std::to_string(si) + "/id",
                              IdKind::Feature});
        for (size_t ii = 0; ii < el.instances.size(); ++ii)
            allIds.push_back({el.instances[ii].id, "/elements/" + std::to_string(ei) + "/instances/" + std::to_string(ii) + "/id",
                              IdKind::Instance});
        for (size_t mi = 0; mi < el.mates.size(); ++mi)
            allIds.push_back({el.mates[mi].id, "/elements/" + std::to_string(ei) + "/mates/" + std::to_string(mi) + "/id", IdKind::Mate});
        for (size_t wi = 0; wi < el.welds.size(); ++wi)
            allIds.push_back({el.welds[wi]["id"].get<std::string>(), "/elements/" + std::to_string(ei) + "/welds/" + std::to_string(wi) + "/id",
                              IdKind::Weld});
        for (size_t pi = 0; pi < el.patterns.size(); ++pi)
            allIds.push_back({el.patterns[pi].id, "/elements/" + std::to_string(ei) + "/patterns/" + std::to_string(pi) + "/id", IdKind::Pattern});
        for (size_t ai = 0; ai < el.annotations.size(); ++ai)
            allIds.push_back({el.annotations[ai]["id"].get<std::string>(),
                              "/elements/" + std::to_string(ei) + "/annotations/" + std::to_string(ai) + "/id", IdKind::Annotation});
        for (size_t si = 0; si < el.sheets.size(); ++si) {
            const std::string sp = "/elements/" + std::to_string(ei) + "/sheets/" + std::to_string(si);
            allIds.push_back({el.sheets[si].id, sp + "/id", IdKind::Sheet});
            for (size_t vi = 0; vi < el.sheets[si].views.size(); ++vi)
                allIds.push_back({el.sheets[si].views[vi].id, sp + "/views/" + std::to_string(vi) + "/id", IdKind::View});
            for (size_t ai = 0; ai < el.sheets[si].dimensions.size(); ++ai)
                allIds.push_back({el.sheets[si].dimensions[ai].id, sp + "/dimensions/" + std::to_string(ai) + "/id", IdKind::Dimension});
            for (size_t ti = 0; ti < el.sheets[si].texts.size(); ++ti)
                allIds.push_back({el.sheets[si].texts[ti].id, sp + "/texts/" + std::to_string(ti) + "/id", IdKind::Text});
            for (size_t mi = 0; mi < el.sheets[si].marks.size(); ++mi)
                allIds.push_back({el.sheets[si].marks[mi].id, sp + "/marks/" + std::to_string(mi) + "/id", IdKind::Mark});
            for (size_t ci = 0; ci < el.sheets[si].constructions.size(); ++ci)
                allIds.push_back({el.sheets[si].constructions[ci].id, sp + "/constructions/" + std::to_string(ci) + "/id", IdKind::Construction});
        }
    }
    // DOC_DUPLICATE_ID
    {
        std::set<std::string> seen;
        for (const auto& r : allIds)
            if (!seen.insert(r.id).second) return docError("DOC_DUPLICATE_ID", r.path).with("id", r.id);
    }
    // DOC_ID_COUNTER
    for (const auto& r : allIds) {
        int64_t counter = 0;
        switch (r.kind) {
            case IdKind::Element: counter = d.counters.element; break;
            case IdKind::Feature: counter = d.counters.feature; break;
            case IdKind::Entity: counter = d.counters.entity; break;
            case IdKind::Constraint: counter = d.counters.constraint; break;
            case IdKind::Variable: counter = d.counters.variable; break;
            case IdKind::Instance: counter = d.counters.instance; break;
            case IdKind::Mate: counter = d.counters.mate; break;
            case IdKind::Sheet: counter = d.counters.sheet; break;
            case IdKind::View: counter = d.counters.view; break;
            case IdKind::Annotation: counter = d.counters.annotation; break;
            case IdKind::Dimension: counter = d.counters.dimension; break;
            case IdKind::Group: counter = d.counters.group; break;
            case IdKind::Table: counter = d.counters.table; break;
            case IdKind::Weld: counter = d.counters.weld; break;
            case IdKind::Text: counter = d.counters.text; break;
            case IdKind::Pattern: counter = d.counters.pattern; break;
            case IdKind::Mark: counter = d.counters.mark; break;
            case IdKind::ElementGroup: counter = d.counters.elementGroup; break;
            case IdKind::Construction: counter = d.counters.construction; break;
        }
        if (idNumber(r.id) >= counter) return docError("DOC_ID_COUNTER", r.path).with("id", r.id);
    }
    // Scales of sheets and views — a, b > 0 (drawing.md §2.2).
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t si = 0; si < d.elements[ei].sheets.size(); ++si) {
            const auto& sh = d.elements[ei].sheets[si];
            const std::string sp = "/elements/" + std::to_string(ei) + "/sheets/" + std::to_string(si);
            if (!scaleValue(sh.scale)) return docError("DOC_SCHEMA", sp + "/scale").with("name", std::string("scale"));
            for (size_t vi = 0; vi < sh.views.size(); ++vi) {
                const auto& v = sh.views[vi];
                const std::string vp = sp + "/views/" + std::to_string(vi);
                if (v.scale && !scaleValue(*v.scale)) return docError("DOC_SCHEMA", vp + "/scale").with("name", std::string("scale"));
                // Orientation "plane" (FX7-DRW-007): plane, normal and up together; unit, mutually orthogonal vectors.
                const bool plane = v.orientation == "plane";
                if (plane != v.plane.has_value()) return docError("DOC_SCHEMA", vp + "/plane").with("name", std::string("plane"));
                if (plane != v.normal.has_value()) return docError("DOC_SCHEMA", vp + "/normal").with("name", std::string("normal"));
                if (plane != v.up.has_value()) return docError("DOC_SCHEMA", vp + "/up").with("name", std::string("up"));
                if (plane) {
                    const auto& n = *v.normal;
                    const auto& u = *v.up;
                    auto dot = [](const std::array<double, 3>& a, const std::array<double, 3>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
                    if (std::abs(std::sqrt(dot(n, n)) - 1) > VIEW_AXIS_TOL) return docError("DOC_SCHEMA", vp + "/normal").with("name", std::string("normal"));
                    if (std::abs(std::sqrt(dot(u, u)) - 1) > VIEW_AXIS_TOL || std::abs(dot(n, u)) > VIEW_AXIS_TOL)
                        return docError("DOC_SCHEMA", vp + "/up").with("name", std::string("up"));
                }
                // Section line binding points (FX9-DRW-002): exactly one of vertex / center / mid / edge; at — only with edge.
                if (v.lineBind && v.lineBind->contains("points"))
                    for (size_t pi = 0; pi < (*v.lineBind)["points"].size(); ++pi) {
                        const Json& p = (*v.lineBind)["points"][pi];
                        const std::string pp = vp + "/lineBind/points/" + std::to_string(pi);
                        int names = 0;
                        for (const char* key : {"vertex", "center", "mid", "edge"}) names += p.contains(key) ? 1 : 0;
                        if (names != 1) return docError("DOC_SCHEMA", pp).with("name", std::string("lineBind"));
                        if (p.contains("at") && !p.contains("edge")) return docError("DOC_SCHEMA", pp + "/at").with("name", std::string("at"));
                    }
            }
            // Tolerance set in the drawing (FX9-DRW-009): a valid field, or both deviations (upper > lower).
            for (size_t ai = 0; ai < sh.dimensions.size(); ++ai) {
                const auto& f = sh.dimensions[ai].fit;
                if (!f) continue;
                const std::string fp = sp + "/dimensions/" + std::to_string(ai) + "/fit";
                const bool dev = f->contains("upper") || f->contains("lower");
                if (f->contains("field") && !pmi::parseField((*f)["field"].get<std::string>()))
                    return docError("DOC_SCHEMA", fp + "/field").with("name", std::string("field"));
                if (dev && !(f->contains("upper") && f->contains("lower") && (*f)["upper"].get<double>() > (*f)["lower"].get<double>()))
                    return docError("DOC_SCHEMA", fp).with("name", std::string("fit"));
                if (!dev && !f->contains("field")) return docError("DOC_SCHEMA", fp).with("name", std::string("fit"));
            }
            // Sheet marks (FX9-DRW-008): source — required for weld and annotation without local, none for
            // restRoughness; local — annotation or restRoughness (FX12-DRW-007); def — exactly with local.
            for (size_t mi = 0; mi < sh.marks.size(); ++mi) {
                const auto& m = sh.marks[mi];
                const std::string mp = sp + "/marks/" + std::to_string(mi);
                auto bad = [&](const char* name) { return docError("DOC_SCHEMA", mp + "/" + name).with("name", std::string(name)); };
                const bool noSource = m.kind == "restRoughness" || m.kind == "partsList";
                if (noSource ? !m.source.empty() : (m.source.empty() && !m.local)) return bad("source");
                // view — at every kind but the parts list, which has none (FX14-DRW-003).
                if ((m.kind == "partsList") != m.view.empty()) return bad("view");
                if (m.local && m.kind != "annotation" && m.kind != "restRoughness") return bad("local");
                // The rest roughness set in the drawing: the record of the part property (§2.4, FX5-PMI-001).
                if (m.local && m.kind == "restRoughness" && m.def &&
                    (!m.def->is_object() || m.def->contains("parameter") != m.def->contains("value")))
                    return bad("def");
                if (m.local != m.def.has_value()) return bad("def");
            }
        }
    // Body — exactly one import feature; import only in a body or part (§2.4в, §2.10к; part — base body S4-REC-009).
    for (size_t ei = 0; ei < d.elements.size(); ++ei) {
        const auto& el = d.elements[ei];
        const std::string ep = "/elements/" + std::to_string(ei);
        if (el.isBody() && (el.features.size() != 1 || el.features[0]->type != FeatureType::Import))
            return docError("DOC_SCHEMA", ep + "/features").with("name", std::string("features"));
        if (!el.isBody() && el.kind != "part")
            for (size_t fi = 0; fi < el.features.size(); ++fi)
                if (el.features[fi]->type == FeatureType::Import)
                    return docError("DOC_SCHEMA", ep + "/features/" + std::to_string(fi) + "/type").with("name", std::string("type"));
    }
    // Thread: exactly one of designation / diameter; pitch only with diameter (§2.10и, S6-LIB-008).
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t fi = 0; fi < d.elements[ei].features.size(); ++fi) {
            const auto& f = *d.elements[ei].features[fi];
            // Dimension tolerances (§2.6, FX12-PMI-007): a valid field (with optional showDeviations), or both
            // deviations with upper > lower.
            if (f.tolerances)
                for (auto it = f.tolerances->begin(); it != f.tolerances->end(); ++it) {
                    const Json& t = it.value();
                    const std::string tp = "/elements/" + std::to_string(ei) + "/features/" + std::to_string(fi) + "/tolerances/" + it.key();
                    const bool dev = t.contains("upper") || t.contains("lower");
                    bool known = t.is_object();
                    for (auto k = t.begin(); known && k != t.end(); ++k)
                        known = k.key() == "field" || k.key() == "showDeviations" || k.key() == "upper" || k.key() == "lower";
                    if (!known || t.contains("field") == dev || (t.contains("showDeviations") && dev))
                        return docError("DOC_SCHEMA", tp).with("name", std::string("tolerances"));
                    if (!dev && !pmi::parseField(t["field"].get<std::string>()))
                        return docError("DOC_SCHEMA", tp + "/field").with("name", std::string("field"));
                    if (dev && !(t.contains("upper") && t.contains("lower") && t["upper"].get<double>() > t["lower"].get<double>()))
                        return docError("DOC_SCHEMA", tp).with("name", std::string("tolerances"));
                }
            // Chamfer (§2.10, FX7-PT-010): the mode's second value is present.
            if (const auto* e = f.edgeFeature(); e && !e->mode.empty() && e->size2.empty())
                return docError("DOC_SCHEMA", "/elements/" + std::to_string(ei) + "/features/" + std::to_string(fi))
                    .with("name", std::string(e->mode == "distanceAngle" ? "angle" : "distance2"));
            // Coil (§2.10п, FX7-PT-004): exactly one of turns / height.
            if (f.type == FeatureType::Coil && f.pattern()->def.contains("turns") == f.pattern()->def.contains("height"))
                return docError("DOC_SCHEMA", "/elements/" + std::to_string(ei) + "/features/" + std::to_string(fi))
                    .with("name", std::string("turns"));
            // Sheet parameters (§2.10л, S5-SHEET-006): thickness and radius are required without recognize.
            if (f.type == FeatureType::SheetMetal && !f.pattern()->def.contains("recognize"))
                for (const char* key : {"thickness", "radius"})
                    if (!f.pattern()->def.contains(key))
                        return docError("DOC_SCHEMA", "/elements/" + std::to_string(ei) + "/features/" + std::to_string(fi))
                            .with("name", std::string(key));
            if (f.type != FeatureType::Thread) continue;
            const Json& def = f.pattern()->def;
            const std::string fp = "/elements/" + std::to_string(ei) + "/features/" + std::to_string(fi);
            if (def.contains("designation") == def.contains("diameter"))
                return docError("DOC_SCHEMA", fp).with("name", std::string("designation"));
            if (def.contains("pitch") && !def.contains("diameter")) return docError("DOC_SCHEMA", fp).with("name", std::string("pitch"));
            if (def.contains("height") && !def.contains("pitch")) return docError("DOC_SCHEMA", fp).with("name", std::string("height"));
        }
    // Consistency of annotation fields (§2.4д, §5 "DOC_SCHEMA (annotations)", pmi.md §2).
    // Then restRoughness of the element (FX5-PMI-001): parameter and value — both or neither.
    for (size_t ei = 0; ei < d.elements.size(); ++ei) {
        for (size_t ai = 0; ai < d.elements[ei].annotations.size(); ++ai)
            if (auto s = checkAnnotation(d.elements[ei].annotations[ai], "/elements/" + std::to_string(ei) + "/annotations/" + std::to_string(ai)); !s)
                return s;
        if (const auto& r = d.elements[ei].restRoughness; r && r->contains("parameter") != r->contains("value"))
            return docError("DOC_SCHEMA", "/elements/" + std::to_string(ei) + "/restRoughness")
                .with("name", std::string(r->contains("parameter") ? "value" : "parameter"));
    }
    // Weld records against the weld type table (weld.md §3), assemblies in document order.
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t wi = 0; wi < d.elements[ei].welds.size(); ++wi)
            if (auto bad = pmi::weldCheck(d.elements[ei].welds[wi]))
                return docError("DOC_SCHEMA", "/elements/" + std::to_string(ei) + "/welds/" + std::to_string(wi)).with("name", *bad);
    // Snapshot of frozen references — parts without their own context (assembly.md §7в.5).
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        if (const auto& c = d.elements[ei].context; c && c->frozen)
            for (size_t fi = 0; fi < c->frozen->size(); ++fi)
                if ((*c->frozen)[fi].element.contains("context"))
                    return docError("DOC_SCHEMA", "/elements/" + std::to_string(ei) + "/context/frozen/" + std::to_string(fi) + "/element/context")
                        .with("name", std::string("context"));
    // Assembly features — plane, axis, sketch, cut and hole (assembly.md §7б, §7г.1); instance targets — assembly only.
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t fi = 0; fi < d.elements[ei].features.size(); ++fi) {
            const Feature& f = *d.elements[ei].features[fi];
            const std::string fp = "/elements/" + std::to_string(ei) + "/features/" + std::to_string(fi);
            const bool targets = (f.extrude() && !f.extrude()->targets.empty()) ||
                                 (f.type == FeatureType::Hole && f.pattern() && f.pattern()->def.contains("targets"));
            if (!d.elements[ei].isAssembly()) {
                if (targets) return docError("DOC_SCHEMA", fp).with("name", std::string("targets"));
                if (f.type == FeatureType::Hole && f.pattern() && f.pattern()->def.contains("cut"))  // FX8-ASM-007: assembly only
                    return docError("DOC_SCHEMA", fp).with("name", std::string("cut"));
                if (f.type == FeatureType::Connection) return docError("DOC_SCHEMA", fp + "/type").with("name", std::string("type"));
                if (f.type == FeatureType::ConnectionCut && f.pattern())
                    if (auto st = checkConnectionCut(f.pattern()->def, fp); !st) return st;
                continue;
            }
            const auto t = f.type;
            if (t != FeatureType::Plane && t != FeatureType::Axis && t != FeatureType::Sketch && t != FeatureType::Extrude &&
                t != FeatureType::Hole && t != FeatureType::Connection)
                return docError("DOC_SCHEMA", fp + "/type").with("name", std::string("type"));
            if (f.extrude() && f.extrude()->operation != "cut") return docError("DOC_SCHEMA", fp + "/operation").with("name", std::string("operation"));
        }
    // Instance quaternion — unit (assembly.md §1).
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t ii = 0; ii < d.elements[ei].instances.size(); ++ii) {
            const auto& q = d.elements[ei].instances[ii].rotation;
            const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (!(n >= 1 - 1e-6 && n <= 1 + 1e-6))
                return docError("DOC_SCHEMA", "/elements/" + std::to_string(ei) + "/instances/" + std::to_string(ii) + "/rotation")
                    .with("name", std::string("rotation"));
        }

    // Sketch entity references: (field, value).
    auto entityRefs = [](const sketch::Entity& e) {
        std::vector<std::pair<std::string, std::string>> refs;
        switch (e.type) {
            case sketch::EntityType::Line: refs = {{"p1", e.p1}, {"p2", e.p2}}; break;
            case sketch::EntityType::Circle: refs = {{"center", e.center}}; break;
            case sketch::EntityType::Ellipse: refs = {{"center", e.center}}; break;
            case sketch::EntityType::EllipseArc: refs = {{"center", e.center}, {"start", e.start}, {"end", e.end}}; break;
            case sketch::EntityType::Arc: refs = {{"center", e.center}, {"start", e.start}, {"end", e.end}}; break;
            case sketch::EntityType::Text: refs = {{"anchor", e.center}}; break;
            case sketch::EntityType::Bezier:  // poles (S4A-SPL-001)
            case sketch::EntityType::BSpline:
                for (size_t i = 0; i < e.poles.size(); ++i) refs.push_back({"poles/" + std::to_string(i), e.poles[i]});
                break;
            default: break;
        }
        return refs;
    };

    // DOC_DANGLING_REF: variant table columns (§2.3а) — first.
    for (size_t ti = 0; ti < d.variantTables.size(); ++ti)
        for (size_t k = 0; k < d.variantTables[ti].columns.size(); ++k)
            if (!d.variable(d.variantTables[ti].columns[k]))
                return docError("DOC_DANGLING_REF", "/variantTables/" + std::to_string(ti) + "/columns/" + std::to_string(k))
                    .with("id", d.variantTables[ti].columns[k]);
    // DOC_DANGLING_REF
    for (size_t ei = 0; ei < d.elements.size(); ++ei) {
        const Element& el = d.elements[ei];
        for (size_t fi = 0; fi < el.features.size(); ++fi) {
            const Feature& f = *el.features[fi];
            const std::string fp = fPath(ei, fi);
            auto checkFeatureRef = [&](const std::string& id, const std::string& path) -> Status {
                auto it = features.find(id);
                if (it == features.end()) return okStatus();  // nonexistent — not a load error
                if (it->second.first != ei || it->second.second >= fi) return docError("DOC_DANGLING_REF", path).with("id", id);
                return okStatus();
            };
            if (const auto* s = f.sketchData()) {
                for (size_t k = 0; k < s->entities.size(); ++k)
                    for (const auto& [field, ref] : entityRefs(s->entities[k]))
                        if (!entities.count(ref))
                            return docError("DOC_DANGLING_REF", fp + "/entities/" + std::to_string(k) + "/" + field).with("id", ref);
                for (size_t k = 0; k < s->constraints.size(); ++k)
                    for (size_t r = 0; r < s->constraints[k].refs.size(); ++r) {
                        const std::string& ref = s->constraints[k].refs[r];
                        if (ref != "origin" && !entities.count(ref))
                            return docError("DOC_DANGLING_REF",
                                            fp + "/constraints/" + std::to_string(k) + "/refs/" + std::to_string(r))
                                .with("id", ref);
                    }
                // Linked groups (sketch.md §2.6.1): sources, center, members.
                for (size_t k = 0; k < s->groups.size(); ++k) {
                    const sketch::Group& g = s->groups[k];
                    const std::string gp = fp + "/groups/" + std::to_string(k);
                    for (size_t r = 0; r < g.sources.size(); ++r)
                        if (!entities.count(g.sources[r]))
                            return docError("DOC_DANGLING_REF", gp + "/sources/" + std::to_string(r)).with("id", g.sources[r]);
                    if (auto it = g.params.find("center"); it != g.params.end() && it->second.text != "origin" &&
                                                           !entities.count(it->second.text))
                        return docError("DOC_DANGLING_REF", gp + "/params/center").with("id", it->second.text);
                    if (auto it = g.params.find("line"); it != g.params.end() && !entities.count(it->second.text))
                        return docError("DOC_DANGLING_REF", gp + "/params/line").with("id", it->second.text);
                    for (size_t r = 0; r < g.members.size(); ++r)
                        if (!entities.count(g.members[r].id) && !constraintsAt.count(g.members[r].id))
                            return docError("DOC_DANGLING_REF", gp + "/members/" + std::to_string(r) + "/id").with("id", g.members[r].id);
                }
                // Weak dimension anchor points (S4-WEAK-006).
                for (size_t r = 0; r < s->weakAnchors.size(); ++r)
                    if (!entities.count(s->weakAnchors[r]))
                        return docError("DOC_DANGLING_REF", fp + "/weakAnchors/" + std::to_string(r)).with("id", s->weakAnchors[r]);
            } else if (const auto* x = f.extrude()) {
                if (auto st = checkFeatureRef(x->sketch, fp + "/sketch"); !st) return st;
                for (size_t r = 0; r < x->upTo.size(); ++r)  // reference plane of the "up to" end (S7F-FEAT-001)
                    if (x->upTo[r].is_object() && x->upTo[r].contains("feature") && x->upTo[r]["feature"].is_string())
                        if (auto st = checkFeatureRef(x->upTo[r]["feature"].get<std::string>(), fp + "/end/to/" + std::to_string(r) + "/feature"); !st) return st;
            } else if (const auto* rv = f.revolve()) {
                if (auto st = checkFeatureRef(rv->sketch, fp + "/sketch"); !st) return st;
                if (!rv->axisFeature.empty())
                    if (auto st = checkFeatureRef(rv->axisFeature, fp + "/axis/feature"); !st) return st;
            } else if (const auto* g = f.refGeom()) {
                for (const auto& r : refGeomFeatureRefs(g->def))
                    if (auto st = checkFeatureRef(r.id, fp + r.path); !st) return st;
            } else if (const auto* pt = f.pattern()) {
                for (const auto& r : patternFeatureRefs(pt->def))
                    if (auto st = checkFeatureRef(r.id, fp + r.path); !st) return st;
            }
            if (const auto* s = f.sketchData(); s && s->plane.onFeature())
                if (auto st = checkFeatureRef(s->plane.feature, fp + "/plane/feature"); !st) return st;
        }
    }

    // DOC_BAD_REF_KIND: variable — a column of one table only, without repeats (§2.3а) — first.
    {
        std::set<std::string> used;
        for (size_t ti = 0; ti < d.variantTables.size(); ++ti)
            for (size_t k = 0; k < d.variantTables[ti].columns.size(); ++k)
                if (!used.insert(d.variantTables[ti].columns[k]).second)
                    return docError("DOC_BAD_REF_KIND", "/variantTables/" + std::to_string(ti) + "/columns/" + std::to_string(k))
                        .with("id", d.variantTables[ti].columns[k])
                        .with("expected", std::string("distinct"));
    }
    // DOC_BAD_REF_KIND
    for (size_t ei = 0; ei < d.elements.size(); ++ei) {
        const Element& el = d.elements[ei];
        for (size_t fi = 0; fi < el.features.size(); ++fi) {
            const Feature& f = *el.features[fi];
            const std::string fp = fPath(ei, fi);
            auto bad = [](const std::string& path, const std::string& id, const char* expected) {
                return Status(docError("DOC_BAD_REF_KIND", path).with("id", id).with("expected", std::string(expected)));
            };
            if (const auto* s = f.sketchData()) {
                if (s->plane.onFeature()) {
                    auto it = features.find(s->plane.feature);
                    if (it != features.end() && d.elements[it->second.first].features[it->second.second]->type != FeatureType::Plane)
                        return bad(fp + "/plane/feature", s->plane.feature, "plane");
                }
                for (size_t k = 0; k < s->entities.size(); ++k) {
                    const auto refs = entityRefs(s->entities[k]);
                    std::set<std::string> seen;
                    for (const auto& [field, ref] : refs) {
                        const std::string path = fp + "/entities/" + std::to_string(k) + "/" + field;
                        const EntityLoc& loc = entities.at(ref);
                        if (loc.element != ei || loc.feature != fi) return bad(path, ref, "sameSketch");
                        if (loc.entity->type != sketch::EntityType::Point) return bad(path, ref, "point");
                        if (!seen.insert(ref).second) return bad(path, ref, "distinct");
                    }
                }
                for (size_t k = 0; k < s->constraints.size(); ++k) {
                    std::set<std::string> seen;
                    for (size_t r = 0; r < s->constraints[k].refs.size(); ++r) {
                        const std::string& ref = s->constraints[k].refs[r];
                        const std::string path = fp + "/constraints/" + std::to_string(k) + "/refs/" + std::to_string(r);
                        if (ref != "origin") {
                            const EntityLoc& loc = entities.at(ref);
                            if (loc.element != ei || loc.feature != fi) return bad(path, ref, "sameSketch");
                        }
                        // translate: the axis segment may be the source itself (sketch.md §2.2, S4-SKT-006).
                        const bool axisIsSource = s->constraints[k].type == "translate" && r == 2 && ref == s->constraints[k].refs[0];
                        if (!seen.insert(ref).second && !axisIsSource) return bad(path, ref, "distinct");
                    }
                }
                if (auto st = checkGroupKinds(*s, fp, ei, fi, entities, constraintsAt); !st) return st;
            } else {
                // References of reference geometry and revolve to features of the required type.
                auto typeOf = [&](const std::string& id) -> std::optional<FeatureType> {
                    auto it = features.find(id);
                    if (it == features.end()) return std::nullopt;
                    return d.elements[it->second.first].features[it->second.second]->type;
                };
                if (const auto* g = f.refGeom())
                    for (const auto& r : refGeomFeatureRefs(g->def))
                        if (auto t = typeOf(r.id); t && featureTypeName(*t) != r.expected) return bad(fp + r.path, r.id, r.expected.c_str());
                if (const auto* pt = f.pattern())
                    for (const auto& r : patternFeatureRefs(pt->def)) {
                        const auto t = typeOf(r.id);
                        if (!t) continue;
                        const bool ok = r.expected == "tool" ? isToolFeature(*t) : featureTypeName(*t) == r.expected;
                        if (!ok) return bad(fp + r.path, r.id, r.expected.c_str());
                    }
                if (const auto* rv = f.revolve(); rv && !rv->axisFeature.empty())
                    if (auto t = typeOf(rv->axisFeature); t && *t != FeatureType::Axis) return bad(fp + "/axis/feature", rv->axisFeature, "axis");
                if (const auto* x = f.extrude())
                    for (size_t r = 0; r < x->upTo.size(); ++r)
                        if (x->upTo[r].is_object() && x->upTo[r].contains("feature") && x->upTo[r]["feature"].is_string())
                            if (auto t = typeOf(x->upTo[r]["feature"].get<std::string>()); t && *t != FeatureType::Plane)
                                return bad(fp + "/end/to/" + std::to_string(r) + "/feature", x->upTo[r]["feature"].get<std::string>(), "plane");
                std::string sk;
                if (const auto* x = f.extrude()) sk = x->sketch;
                if (const auto* rv = f.revolve()) sk = rv->sketch;
                if (!sk.empty()) {
                    auto it = features.find(sk);
                    if (it != features.end() && d.elements[it->second.first].features[it->second.second]->type != FeatureType::Sketch)
                        return bad(fp + "/sketch", sk, "sketch");
                }
                if (const auto* rv = f.revolve(); rv && !rv->axisSketchLine.empty()) {
                    auto it = entities.find(rv->axisSketchLine);
                    if (it != entities.end()) {
                        const std::string path = fp + "/axis/sketchLine";
                        if (it->second.entity->type != sketch::EntityType::Line) return bad(path, rv->axisSketchLine, "line");
                        auto sit = features.find(rv->sketch);
                        if (sit != features.end() &&
                            (it->second.element != sit->second.first || it->second.feature != sit->second.second))
                            return bad(path, rv->axisSketchLine, "sameSketch");
                    }
                }
            }
        }
    }

    // DOC_SCHEMA (spline structure, sketch.md §2.7.1, S4A-SPL-001): degree, knots, multiplicities, weights.
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t fi = 0; fi < d.elements[ei].features.size(); ++fi) {
            const auto* s = d.elements[ei].features[fi]->sketchData();
            if (!s) continue;
            for (size_t k = 0; k < s->entities.size(); ++k) {
                const sketch::Entity& e = s->entities[k];
                if (!sketch::isSplineType(e.type)) continue;
                const std::string bad = spline::validate(sketch::splineCurve(e, {}));
                if (!bad.empty()) return docError("DOC_SCHEMA", fPath(ei, fi) + "/entities/" + std::to_string(k)).with("name", bad);
            }
        }
    // DOC_SCHEMA (text, sketch.md §2.1, FX8-SK-007): no control characters except line breaks; style fragments within the
    // text (code points) with at least one style.
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t fi = 0; fi < d.elements[ei].features.size(); ++fi) {
            const auto* s = d.elements[ei].features[fi]->sketchData();
            if (!s) continue;
            for (size_t k = 0; k < s->entities.size(); ++k) {
                const sketch::Entity& e = s->entities[k];
                if (e.type != sketch::EntityType::Text) continue;
                const std::string ep = fPath(ei, fi) + "/entities/" + std::to_string(k);
                for (unsigned char ch : e.text)
                    if (ch < 32 && ch != '\n') return docError("DOC_SCHEMA", ep + "/text").with("name", std::string("text"));
                const int n = sketch::codePointCount(e.text);
                for (size_t i = 0; i < e.styles.size(); ++i) {
                    const auto& sp = e.styles[i];
                    if (!(sp.from < sp.to && sp.to <= n) || !(sp.bold || sp.italic || sp.underline))
                        return docError("DOC_SCHEMA", ep + "/styles/" + std::to_string(i)).with("name", std::string("styles"));
                }
            }
        }

    // DOC_SCHEMA (sheet text styles, drawing.md §6.23, FX9-DRW-006): fragments within the text, with a style.
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t si = 0; si < d.elements[ei].sheets.size(); ++si)
            for (size_t ti = 0; ti < d.elements[ei].sheets[si].texts.size(); ++ti) {
                const SheetText& t = d.elements[ei].sheets[si].texts[ti];
                const int n = sketch::codePointCount(t.text);
                for (size_t i = 0; i < t.styles.size(); ++i) {
                    const auto& sp = t.styles[i];
                    if (!(sp.from < sp.to && sp.to <= n) || !(sp.bold || sp.italic || sp.underline))
                        return docError("DOC_SCHEMA", "/elements/" + std::to_string(ei) + "/sheets/" + std::to_string(si) + "/texts/" +
                                                          std::to_string(ti) + "/styles/" + std::to_string(i))
                            .with("name", std::string("styles"));
                }
            }

    // DOC_SCHEMA (sheet constructions, drawing.md §6.29, FX14-DRW-006): number of points by kind, radius only of a circle
    // with one point, "at" only of an edge binding and in [0, 1].
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t si = 0; si < d.elements[ei].sheets.size(); ++si)
            for (size_t ci = 0; ci < d.elements[ei].sheets[si].constructions.size(); ++ci) {
                const SheetConstruction& c = d.elements[ei].sheets[si].constructions[ci];
                const std::string cp = "/elements/" + std::to_string(ei) + "/sheets/" + std::to_string(si) + "/constructions/" + std::to_string(ci);
                const size_t n = c.points.size();
                const bool count = c.kind == "line" || c.kind == "axis" ? n == 2
                                   : c.kind == "circle"                 ? (n == 1) == c.radius.has_value() && (n == 1 || n == 2)
                                   : c.kind == "arc"                    ? n == 3
                                                                        : n >= 3;
                if (!count) return docError("DOC_SCHEMA", cp + "/points").with("name", std::string("points"));
                if (c.radius && c.kind != "circle") return docError("DOC_SCHEMA", cp + "/radius").with("name", std::string("radius"));
                for (size_t pi = 0; pi < n; ++pi) {
                    const auto& pt = c.points[pi];
                    if (pt.at && (pt.bind != "edge" || !(*pt.at >= 0 && *pt.at <= 1)))
                        return docError("DOC_SCHEMA", cp + "/points/" + std::to_string(pi) + "/bind/at").with("name", std::string("at"));
                }
            }

    // DOC_SHARED_POINT
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t fi = 0; fi < d.elements[ei].features.size(); ++fi) {
            const auto* s = d.elements[ei].features[fi]->sketchData();
            if (!s) continue;
            std::map<std::string, std::vector<std::string>> users;
            for (size_t k = 0; k < s->entities.size(); ++k)
                for (const auto& [field, ref] : entityRefs(s->entities[k])) {
                    auto& u = users[ref];
                    u.push_back(s->entities[k].id);
                    if (u.size() == 2) {
                        // list of all referencing entities
                        std::vector<std::string> all;
                        for (const auto& e : s->entities)
                            for (const auto& [f2, r2] : entityRefs(e))
                                if (r2 == ref) all.push_back(e.id);
                        return docError("DOC_SHARED_POINT",
                                        fPath(ei, fi) + "/entities/" + std::to_string(k) + "/" + field)
                            .with("id", ref)
                            .with("ids", all);
                    }
                }
        }

    // DOC_SCHEMA (branch)
    for (size_t ei = 0; ei < d.elements.size(); ++ei)
        for (size_t fi = 0; fi < d.elements[ei].features.size(); ++fi) {
            const auto* s = d.elements[ei].features[fi]->sketchData();
            if (!s) continue;
            for (size_t k = 0; k < s->constraints.size(); ++k) {
                const sketch::Constraint& c = s->constraints[k];
                const std::string path = fPath(ei, fi) + "/constraints/" + std::to_string(k);
                auto kindOf = [&](size_t i) -> std::optional<sketch::EntityType> {
                    if (i >= c.refs.size() || c.refs[i] == "origin") return sketch::EntityType::Point;
                    auto it = entities.find(c.refs[i]);
                    if (it == entities.end()) return std::nullopt;
                    return it->second.entity->type;
                };
                auto branchError = [&]() { return Status(docError("DOC_SCHEMA", path).with("name", std::string("branch"))); };
                if (c.type == "distance" && c.refs.size() == 2) {
                    // side branch: to a line, to a curve from a point or line; arc — only to a curve (sketch.md §2.2).
                    const auto k0 = kindOf(0), k1 = kindOf(1);
                    auto curve = [](std::optional<sketch::EntityType> k) {
                        return k == sketch::EntityType::Circle || k == sketch::EntityType::Arc;
                    };
                    const bool needSide = k1 == sketch::EntityType::Line || (curve(k1) && !curve(k0));
                    if (k1 && k1 != sketch::EntityType::Projected && k0 && k0 != sketch::EntityType::Projected) {
                        if (needSide && (!c.branch || c.branch->kind != sketch::Branch::Kind::Side)) return branchError();
                        if (!needSide && c.branch) return branchError();
                        if (curve(k1) != c.arc.has_value()) return Status(docError("DOC_SCHEMA", path).with("name", std::string("arc")));
                        // Diameter dimension — point–segment (S2-SK-003) or point–point from an axis (FX9-SK-001).
                        if (c.diametral && !(k0 == sketch::EntityType::Point && (k1 == sketch::EntityType::Line || k1 == sketch::EntityType::Point)))
                            return Status(docError("DOC_SCHEMA", path).with("name", std::string("diametral")));
                    }
                }
                auto splineRef = [&](size_t i) {
                    const auto k = kindOf(i);
                    return k && sketch::isSplineType(*k);
                };
                if (c.type == "tangent" && c.refs.size() == 2 && splineRef(1)) {  // at a spline end — no branch (S4A-SPL-002)
                    if (c.branch) return branchError();
                } else if (c.type == "tangent" && !c.refs.empty()) {
                    const auto k0 = kindOf(0);
                    if (!c.branch) return branchError();
                    if (k0 == sketch::EntityType::Line && c.branch->kind != sketch::Branch::Kind::Side) return branchError();
                    if (k0 && k0 != sketch::EntityType::Line && c.branch->kind != sketch::Branch::Kind::Contact)
                        return branchError();
                }
            }
        }

    // DOC_DUPLICATE_NAME
    {
        std::set<std::string> names;
        for (size_t i = 0; i < d.variables.size(); ++i)
            if (!names.insert(d.variables[i].name).second)
                return docError("DOC_DUPLICATE_NAME", "/variables/" + std::to_string(i) + "/name")
                    .with("name", d.variables[i].name);
        for (size_t ti = 0; ti < d.variantTables.size(); ++ti) {  // variant names within a table (§2.3а)
            std::set<std::string> rows;
            for (size_t r = 0; r < d.variantTables[ti].variants.size(); ++r)
                if (!rows.insert(d.variantTables[ti].variants[r].name).second)
                    return docError("DOC_DUPLICATE_NAME", "/variantTables/" + std::to_string(ti) + "/variants/" + std::to_string(r) + "/name")
                        .with("name", d.variantTables[ti].variants[r].name);
        }
    }

    // DOC_BAD_ROLLBACK
    for (size_t ei = 0; ei < d.elements.size(); ++ei) {
        const Element& el = d.elements[ei];
        if (el.rollback.kind == Rollback::Kind::After && el.indexOf(el.rollback.feature) < 0)
            return docError("DOC_BAD_ROLLBACK", elPath(ei) + "/rollback").with("id", el.rollback.feature);
    }
    return okStatus();
}

}  // namespace

// ---------------------------------------------------------------- public API

Document createNew(const std::string& lengthUnit, const std::string& angleUnit) {
    Document d;
    d.lengthUnit = lengthUnit;
    d.angleUnit = angleUnit;
    Element e;
    e.id = allocate(d, IdKind::Element);
    e.name = defaultElementName(d);
    d.elements.push_back(std::move(e));
    return d;
}

Result<Document> parse(const std::string& text, const ParseOptions& options) {
    auto parsed = parseJsonStrict(text);
    if (!parsed) return parsed.error();
    Json j = *parsed;
    if (!j.is_object() || !j.contains("format") || j["format"] != "construe-document") return Error("DOC_FORMAT");
    const int minVersion = options.allowTestVersion0 ? 0 : 1;
    if (!j.contains("schemaVersion") || !j["schemaVersion"].is_number()) {
        Error e("DOC_SCHEMA");
        e.at("/schemaVersion").with("detail", std::string("schemaVersion missing or not a number"));
        return e;
    }
    const double ver = j["schemaVersion"].get<double>();
    // Integrality — via floor (casting a huge double to int64 is undefined behavior).
    if (!std::isfinite(ver) || std::floor(ver) != ver || ver < minVersion) {
        Error e("DOC_SCHEMA");
        e.at("/schemaVersion").with("detail", std::string("unsupported schemaVersion"));
        return e;
    }
    if (ver > CURRENT_SCHEMA_VERSION) {
        Error e("DOC_NEWER_VERSION");
        e.with("actual", ver).with("expected", static_cast<double>(CURRENT_SCHEMA_VERSION));
        return e;
    }
    // Migrations (§4.3, registry — doc/migrations.cpp).
    migrateToCurrent(j);
    if (auto s = Schema::document().validate(j); !s) return s.error();
    if (auto s = checkInstanceRefs(j); !s) return s.error();
    Document d = documentFrom(j);
    if (auto s = semanticValidate(d); !s) return s.error();
    normalizeElementGroups(d);  // §2.1а: members gathered, empty groups dropped
    return d;
}

std::string serialize(const Document& doc) { return Schema::document().writeCanonical(documentToJson(doc)); }

std::string serializeFeature(const Feature& feature) {
    return Schema::document().writeCompact(featureToJson(feature), "feature");
}

Status validate(const Document& doc) {
    if (auto s = Schema::document().validate(documentToJson(doc)); !s) return s;
    return semanticValidate(doc);
}

Json documentToJsonValue(const Document& doc) { return documentToJson(doc); }
Json elementToJsonValue(const Element& element) { return elementToJson(element); }
Element elementFromJsonValue(const Json& j) { return elementFrom(j); }
Json featureToJsonValue(const Feature& feature) { return featureToJson(feature); }
const Json* featureTolerance(const Feature& feature, const std::string& param) {
    if (!feature.tolerances || !feature.tolerances->is_object() || !feature.tolerances->contains(param)) return nullptr;
    return &(*feature.tolerances)[param];
}
Json sectionToJsonValue(const Section& section) { return sectionToJson(section); }
Json sheetToJsonValue(const Sheet& sheet) { return sheetToJson(sheet); }
Sheet sheetFromJsonValue(const Json& j) { return sheetFromJson(j); }
Json viewToJsonValue(const DrawingView& view) { return viewToJson(view); }
DrawingView viewFromJsonValue(const Json& j) { return viewFromJson(j); }
Json dimensionToJsonValue(const SheetDimension& a) { return dimensionToJson(a); }
Json sheetTextToJsonValue(const SheetText& t) { return sheetTextToJson(t); }
Json sheetConstructionToJsonValue(const SheetConstruction& c) { return constructionToJson(c); }
Json sheetMarkToJsonValue(const SheetMark& m) { return sheetMarkToJson(m); }
SheetDimension dimensionFromJsonValue(const Json& j) { return dimensionFromJson(j); }
Json mateToJsonValue(const Mate& mate) { return mateToJson(mate); }
Json instancePatternToJsonValue(const InstancePattern& p) { return instancePatternToJson(p); }
InstancePattern instancePatternFromJsonValue(const Json& j) { return instancePatternFromJson(j); }
Mate mateFromJsonValue(const Json& j) { return mateFromJson(j); }
Section sectionFromJsonValue(const Json& j) { return sectionFromJson(j); }
Json entityToJsonValue(const sketch::Entity& e) { return entityToJson(e); }
Json constraintToJsonValue(const sketch::Constraint& c) { return constraintToJson(c); }
Json groupToJsonValue(const sketch::Group& g) { return groupToJson(g); }

Result<Document> documentFromJson(const Json& j) {
    if (auto s = Schema::document().validate(j); !s) return s.error();
    if (auto s = checkInstanceRefs(j); !s) return s.error();
    Document d = documentFrom(j);
    if (auto s = semanticValidate(d); !s) return s.error();
    normalizeElementGroups(d);  // §2.1а: members gathered, empty groups dropped
    return d;
}

std::string allocate(Document& doc, IdKind kind) {
    switch (kind) {
        case IdKind::Element: return "el" + std::to_string(doc.counters.element++);
        case IdKind::Feature: return "f" + std::to_string(doc.counters.feature++);
        case IdKind::Entity: return "e" + std::to_string(doc.counters.entity++);
        case IdKind::Constraint: return "c" + std::to_string(doc.counters.constraint++);
        case IdKind::Variable: return "v" + std::to_string(doc.counters.variable++);
        case IdKind::Instance: return "i" + std::to_string(doc.counters.instance++);
        case IdKind::Mate: return "m" + std::to_string(doc.counters.mate++);
        case IdKind::Sheet: return "sh" + std::to_string(doc.counters.sheet++);
        case IdKind::View: return "dv" + std::to_string(doc.counters.view++);
        case IdKind::Annotation: return "a" + std::to_string(doc.counters.annotation++);
        case IdKind::Dimension: return "dim" + std::to_string(doc.counters.dimension++);
        case IdKind::Group: return "g" + std::to_string(doc.counters.group++);
        case IdKind::Table: return "t" + std::to_string(doc.counters.table++);
        case IdKind::Weld: return "w" + std::to_string(doc.counters.weld++);
        case IdKind::Text: return "tx" + std::to_string(doc.counters.text++);
        case IdKind::Pattern: return "ip" + std::to_string(doc.counters.pattern++);
        case IdKind::Mark: return "mk" + std::to_string(doc.counters.mark++);
        case IdKind::ElementGroup: return "eg" + std::to_string(doc.counters.elementGroup++);
        case IdKind::Construction: return "cs" + std::to_string(doc.counters.construction++);
    }
    return {};
}

namespace {
std::map<std::string, std::string>& clientNameBases() {
    static std::map<std::string, std::string> table;
    return table;
}
}  // namespace

const std::map<std::string, std::string>& builtinNameBases() {
    static const std::map<std::string, std::string> table = {
        {"element.part", "Деталь"}, {"element.assembly", "Сборка"}, {"element.body", "Тело"},
        {"element.drawing", "Чертёж"}, {"element.specification", "Спецификация"},
        {"feature.sketch", "Эскиз"}, {"feature.extrude", "Выдавливание"}, {"feature.cut", "Вырез"},
        {"feature.revolve", "Вращение"}, {"feature.fillet", "Скругление"}, {"feature.chamfer", "Фаска"},
        {"feature.plane", "Плоскость"}, {"feature.axis", "Ось"}, {"feature.pattern", "Массив"},
        {"feature.mirror", "Зеркало"}, {"feature.shell", "Оболочка"}, {"feature.draft", "Уклон"},
        {"feature.sweep", "Протягивание"}, {"feature.loft", "Лофт"}, {"feature.hole", "Отверстие"},
        {"feature.thread", "Резьба"}, {"feature.import", "Импорт"}, {"feature.sheetMetal", "Лист"},
        {"feature.sheetWall", "Стенка"}, {"feature.sheetFlange", "Отгиб"}, {"feature.sheetBend", "Гиб"},
        {"feature.sheetCut", "Вырез"}, {"feature.gear", "Венец"}, {"feature.coil", "Пружина"},
        {"feature.connection", "Соединение"}, {"feature.connectionCut", "Вырез соединения"},
        {"mate.coincident", "Совпадение"}, {"mate.concentric", "Концентричность"}, {"mate.distance", "Расстояние"},
        {"mate.angle", "Угол"}, {"mate.parallel", "Параллельность"}, {"mate.perpendicular", "Перпендикулярность"},
        {"mate.tangent", "Касание"}, {"mate.gear", "Зацепление"}, {"mate.lock", "Заделка"},
        {"section", "Разрез"}, {"sheet", "Лист "}, {"view", "Вид "}, {"table", "Таблица"},
        {"instancePattern", "Массив экземпляров"}, {"weld", "Шов "}, {"elementGroup", "Группа "},
    };
    return table;
}

void setNameBases(std::map<std::string, std::string> table) { clientNameBases() = std::move(table); }

std::string nameBase(const std::string& key) {
    const auto& client = clientNameBases();
    if (auto it = client.find(key); it != client.end() && !it->second.empty()) return it->second;
    const auto& builtin = builtinNameBases();
    auto it = builtin.find(key);
    return it == builtin.end() ? std::string() : it->second;
}

std::string firstFreeName(const std::string& base, const std::set<std::string>& taken) {
    for (int n = 1;; ++n) {
        std::string name = base + std::to_string(n);
        if (!taken.count(name)) return name;
    }
}

std::string defaultFeatureName(const Element& element, FeatureType t) { return defaultFeatureName(element, t, {}); }

std::string defaultFeatureBase(FeatureType t, const std::string& operation) {
    if (t == FeatureType::Extrude && (operation == "cut" || operation == "intersect")) return nameBase("feature.cut");
    const std::string base = nameBase(std::string("feature.") + featureTypeName(t));
    return base.empty() ? "?" : base;
}

std::string defaultFeatureName(const Element& element, FeatureType t, const std::string& operation, const std::string& exclude) {
    std::set<std::string> taken;
    for (const auto& f : element.features)
        if (exclude.empty() || f->id != exclude) taken.insert(f->name);
    const std::string base = defaultFeatureBase(t, operation);
    for (int n = 1;; ++n) {
        std::string name = base + std::to_string(n);
        if (!taken.count(name)) return name;
    }
}

int Document::elementIndex(const std::string& id) const {
    for (size_t i = 0; i < elements.size(); ++i)
        if (elements[i].id == id) return static_cast<int>(i);
    return -1;
}

const Variable* Document::variable(const std::string& id) const {
    for (const auto& v : variables)
        if (v.id == id) return &v;
    return nullptr;
}

bool isLibraryRef(const std::string& refFile) { return refFile.rfind(LIBRARY_REF_PREFIX, 0) == 0; }
std::string libraryRef(const std::string& libraryPartId) { return LIBRARY_REF_PREFIX + libraryPartId; }

const ElementGroup* Document::elementGroup(const std::string& id) const {
    for (const auto& g : elementGroups)
        if (g.id == id) return &g;
    return nullptr;
}

void normalizeElementGroups(Document& doc) {
    for (auto& e : doc.elements)
        if (!e.group.empty() && !doc.elementGroup(e.group)) e.group.clear();
    std::vector<size_t> order;
    order.reserve(doc.elements.size());
    std::set<std::string> placed;
    for (size_t i = 0; i < doc.elements.size(); ++i) {
        const std::string& g = doc.elements[i].group;
        if (g.empty()) {
            order.push_back(i);
            continue;
        }
        if (!placed.insert(g).second) continue;  // already gathered at its first member
        for (size_t k = i; k < doc.elements.size(); ++k)
            if (doc.elements[k].group == g) order.push_back(k);
    }
    std::vector<Element> out;
    out.reserve(order.size());
    for (size_t i : order) out.push_back(std::move(doc.elements[i]));
    doc.elements = std::move(out);
    std::erase_if(doc.elementGroups, [&](const ElementGroup& g) { return placed.count(g.id) == 0; });
}

const SolutionCopy* Document::solution(const std::string& id) const {
    for (const auto& s : solutions)
        if (s.id == id) return &s;
    return nullptr;
}

const LibraryPart* Document::libraryPart(const std::string& id) const {
    for (const auto& lp : libraryParts)
        if (lp.id == id) return &lp;
    return nullptr;
}

const VariantTable* Document::variantTable(const std::string& id) const {
    for (const auto& t : variantTables)
        if (t.id == id) return &t;
    return nullptr;
}

const VariantTable* Document::tableOfVariable(const std::string& variableId) const {
    for (const auto& t : variantTables)
        if (std::find(t.columns.begin(), t.columns.end(), variableId) != t.columns.end()) return &t;
    return nullptr;
}

std::string Document::effectiveExpr(const Variable& v, const std::string& rowName) const {
    const VariantTable* t = tableOfVariable(v.id);
    if (!t) return v.expr;
    const VariantRow* r = rowName.empty() ? nullptr : t->row(rowName);
    if (!r) r = t->row(t->active);
    if (!r) return v.expr;
    const size_t k = size_t(std::find(t->columns.begin(), t->columns.end(), v.id) - t->columns.begin());
    return k < r->values.size() ? r->values[k] : v.expr;
}

const VariantRow* VariantTable::row(const std::string& n) const {
    for (const auto& r : variants)
        if (r.name == n) return &r;
    return nullptr;
}

int Document::activeIndex() const {
    const int i = activeElement.empty() ? -1 : elementIndex(activeElement);
    return i < 0 ? 0 : i;
}

std::string defaultElementName(const Document& doc, const std::string& kind) {
    std::set<std::string> taken;
    for (const auto& e : doc.elements) taken.insert(e.name);
    for (int n = 1;; ++n) {
        std::string name = nameBase("element." + std::string(kind == "assembly" || kind == "body" || kind == "drawing" || kind == "specification" ? kind : "part")) +
                           std::to_string(n);
        if (!taken.count(name)) return name;
    }
}

int Element::annotationIndex(const std::string& id) const {
    for (size_t i = 0; i < annotations.size(); ++i)
        if (annotations[i].value("id", std::string()) == id) return static_cast<int>(i);
    return -1;
}

int Element::weldIndex(const std::string& id) const {
    for (size_t i = 0; i < welds.size(); ++i)
        if (welds[i].value("id", std::string()) == id) return static_cast<int>(i);
    return -1;
}

int Element::patternIndex(const std::string& id) const {
    for (size_t i = 0; i < patterns.size(); ++i)
        if (patterns[i].id == id) return static_cast<int>(i);
    return -1;
}

bool Element::mateSuppressedShown(const Mate& m) const {
    if (m.suppressed) return true;
    if (!explodedActive()) return false;
    const auto& s = exploded->suppressedMates;
    return std::find(s.begin(), s.end(), m.id) != s.end();
}

int Element::sheetIndex(const std::string& sheetId) const {
    for (size_t i = 0; i < sheets.size(); ++i)
        if (sheets[i].id == sheetId) return static_cast<int>(i);
    return -1;
}

int Sheet::viewIndex(const std::string& viewId) const {
    for (size_t i = 0; i < views.size(); ++i)
        if (views[i].id == viewId) return static_cast<int>(i);
    return -1;
}

int Sheet::dimensionIndex(const std::string& id) const {
    for (size_t i = 0; i < dimensions.size(); ++i)
        if (dimensions[i].id == id) return static_cast<int>(i);
    return -1;
}

int Sheet::constructionIndex(const std::string& id) const {
    for (size_t i = 0; i < constructions.size(); ++i)
        if (constructions[i].id == id) return static_cast<int>(i);
    return -1;
}

int Sheet::markIndex(const std::string& id) const {
    for (size_t i = 0; i < marks.size(); ++i)
        if (marks[i].id == id) return static_cast<int>(i);
    return -1;
}

int Sheet::textIndex(const std::string& id) const {
    for (size_t i = 0; i < texts.size(); ++i)
        if (texts[i].id == id) return static_cast<int>(i);
    return -1;
}

std::string defaultSheetName(const Element& drawing) {
    std::set<std::string> taken;
    for (const auto& s : drawing.sheets) taken.insert(s.name);
    return firstFreeName(nameBase("sheet"), taken);
}

std::string defaultTableName(const Document& doc) {
    std::set<std::string> taken;
    for (const auto& t : doc.variantTables) taken.insert(t.name);
    return firstFreeName(nameBase("table"), taken);
}

std::string defaultViewName(const Element& drawing) {
    std::set<std::string> taken;
    for (const auto& s : drawing.sheets)
        for (const auto& v : s.views) taken.insert(v.name);
    return firstFreeName(nameBase("view"), taken);
}

std::optional<double> scaleValue(const std::string& text) {
    const auto colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return std::nullopt;
    auto num = [](const std::string& s) -> std::optional<double> {
        if (s.empty()) return std::nullopt;
        int dots = 0;
        for (char ch : s) {
            if (ch == '.') ++dots;
            else if (ch < '0' || ch > '9') return std::nullopt;
        }
        if (dots > 1 || s.front() == '.' || s.back() == '.') return std::nullopt;
        return std::stod(s);
    };
    const auto a = num(text.substr(0, colon)), b = num(text.substr(colon + 1));
    if (!a || !b || *a <= 0 || *b <= 0) return std::nullopt;
    return *a / *b;
}

std::vector<std::string> effectiveViewSections(const Element& model, const std::optional<std::vector<std::string>>& sections) {
    std::vector<std::string> out;
    for (const auto& s : model.sections)
        if (sections ? std::find(sections->begin(), sections->end(), s.id) != sections->end() : !s.hidden) out.push_back(s.id);
    return out;
}

std::optional<std::vector<std::string>> viewSectionsList(const Sheet& sheet, const std::string& viewId) {
    std::string id = viewId;
    for (size_t steps = 0; steps <= sheet.views.size(); ++steps) {
        const int vi = sheet.viewIndex(id);
        if (vi < 0) return std::nullopt;
        const DrawingView& v = sheet.views[size_t(vi)];
        if ((v.kind == "base" || v.kind == "projected") && v.sections) return v.sections;
        if (v.kind == "base") return std::nullopt;
        id = v.parent;
    }
    return std::nullopt;  // cycle
}

const DrawingView* chainBaseView(const Sheet& sheet, const std::string& viewId) {
    std::string id = viewId;
    for (size_t steps = 0; steps <= sheet.views.size(); ++steps) {
        const int vi = sheet.viewIndex(id);
        if (vi < 0) return nullptr;
        const DrawingView& v = sheet.views[size_t(vi)];
        if (v.kind == "base") return &v;
        id = v.parent;
    }
    return nullptr;  // cycle
}

}  // namespace construe::doc
