// CoreApi sketch commands (api/sketch_commands.h).
#include "api/sketch_commands.h"

#include "api/errors.h"
#include "api/feature_commands.h"
#include "regen/naming.h"
#include "regen/regen.h"

#include <array>
#include <cmath>
#include <regex>
#include <set>

namespace construe::api {

namespace {

bool isTempId(const std::string& s) {
    static const std::regex re("t[1-9][0-9]*");
    return std::regex_match(s, re);
}

bool isEntityType(const std::string& t) {
    return t == "point" || t == "line" || t == "circle" || t == "arc" || t == "projected" || t == "text" || t == "ellipse" ||
           t == "ellipseArc" || t == "bezier" || t == "bspline";
}

std::string measuredValueText(double v, const std::string& type, const expr::DocumentUnits& u) {
    const bool angle = sketch::isAngleDimension(type);
    expr::Quantity q{v, angle ? expr::ANGLE : sketch::isNumberDimension(type) ? expr::DIMLESS : expr::LENGTH};
    std::string t = expr::format(q, u, 6);
    // The value is an expression: it rejects the display "°", the unit "deg" is written (core-api §4.6.2, "30 deg").
    const std::string deg = "°";
    if (t.size() >= deg.size() && t.compare(t.size() - deg.size(), deg.size(), deg) == 0) t = t.substr(0, t.size() - deg.size()) + " deg";
    return t;
}

}  // namespace

Result<doc::Document> withSketch(const doc::Document& base, const std::string& featureId, const sketch::SketchData& s) {
    doc::FeaturePtr f = base.active().find(featureId);
    doc::Feature nf = *f;
    sketch::SketchData sd = s;
    // Weak anchor points of removed points are dropped (core-api §4.6.2, S4-WEAK-006).
    std::erase_if(sd.weakAnchors, [&](const std::string& id) {
        const sketch::Entity* e = sd.findEntity(id);
        return !e || e->type != sketch::EntityType::Point;
    });
    nf.data = std::move(sd);
    doc::Document nd = replaceFeature(base, nf);
    if (auto v = doc::validate(nd); !v) {
        const std::string* detail = v.error().stringParam("detail");
        const std::string* rid = v.error().stringParam("id");
        return invalidItems(rid ? *rid : "", v.error().code + (detail ? ": " + *detail : std::string()));
    }
    return nd;
}

Result<SketchAddResult> sketchAddItems(doc::Document& nd, const std::string& featureId, const Json& items,
                                       const ProjectionResolver& projections) {
    if (!items.is_array()) return invalidItems("", "items must be an array");
    doc::FeaturePtr f = nd.active().find(featureId);
    sketch::SketchData sd = *f->sketchData();

    // Temporary IDs → permanent.
    std::map<std::string, std::string> map;
    std::map<std::string, std::string> tempType;  // entity type of a temporary ID
    for (const auto& it : items) {
        if (!it.is_object() || !it.contains("type") || !it["type"].is_string()) return invalidItems("", "type required");
        const std::string type = it["type"];
        const bool entity = isEntityType(type);
        if (it.contains("id")) {
            if (!it["id"].is_string() || !isTempId(it["id"])) return invalidItems("", "bad temporary id");
            const std::string tid = it["id"];
            if (map.count(tid)) return invalidItems(tid, "duplicate temporary id");
            map[tid] = doc::allocate(nd, entity ? doc::IdKind::Entity : doc::IdKind::Constraint);
            if (entity) tempType[tid] = type;
        } else if (entity) {
            return invalidItems("", "entity id required");
        } else {
            map["#" + std::to_string(&it - &items[0])] = doc::allocate(nd, doc::IdKind::Constraint);
        }
    }
    // Refs.
    std::set<std::string> ownedPoints;
    auto resolveRef = [&](const std::string& ref, bool asOwnedPoint, const std::string& itemId) -> Result<std::string> {
        if (asOwnedPoint) {
            auto t = tempType.find(ref);
            if (t == tempType.end() || t->second != "point") return invalidItems(itemId, "curve point must be a new point of this batch");
            if (!ownedPoints.insert(ref).second) return invalidItems(itemId, "point already owned by another curve");
            return map.at(ref);
        }
        if (ref == "origin") return ref;
        if (map.count(ref) && tempType.count(ref)) return map.at(ref);
        if (isTempId(ref)) return invalidItems(itemId, "unknown temporary id " + ref);
        if (sd.findEntity(ref)) return ref;
        return invalidItems(itemId, "unknown entity " + ref);
    };
    SketchAddResult out;
    std::vector<sketch::Constraint> newConstraints;
    try {
        for (size_t idx = 0; idx < items.size(); ++idx) {
            Json it = items[idx];
            const std::string type = it["type"];
            const std::string tid = it.contains("id") ? it["id"].get<std::string>() : "#" + std::to_string(idx);
            const std::string pid = map.at(tid);
            if (it.contains("id")) out.response[tid] = pid;
            it["id"] = pid;
            if (isEntityType(type)) {
                for (const char* key : {"p1", "p2", "center", "start", "end", "anchor"}) {
                    if (!it.contains(key)) continue;
                    if (!it[key].is_string()) return invalidItems(tid, std::string(key) + " must be a string");
                    auto r = resolveRef(it[key], true, tid);
                    if (!r) return r.error();
                    it[key] = *r;
                }
                if (it.contains("poles")) {  // spline poles — new points of the batch, owned (S4A-SPL-001)
                    if (!it["poles"].is_array()) return invalidItems(tid, "poles must be an array");
                    for (auto& q : it["poles"]) {
                        if (!q.is_string()) return invalidItems(tid, "pole must be a string");
                        auto r = resolveRef(q, true, tid);
                        if (!r) return r.error();
                        q = *r;
                    }
                }
                if (type == "projected") {
                    // source — an edge or vertex name per the grammar of naming §2.3 (core-api §4.6.2).
                    if (!it.contains("source") || !it["source"].is_string()) return invalidItems(tid, "source required");
                    // or a sketch axis "sketch:x" / "sketch:y" (sketch.md §2.5, S1-SK-021).
                    const std::string src = it["source"].get<std::string>();
                    if (src.rfind("pierce:", 0) == 0) {
                        // Piercing: sketch of active element (not this one) or edge name (sketch.md §2.5, S2-FEAT-006).
                        const std::string ref = src.substr(7);
                        const auto sf = nd.active().find(ref);
                        auto pn = naming::parse(naming::splitPath(ref).leaf);  // assembly: "<path>/<name>" (naming.md §5.3а)
                        const bool sketchRef = sf && sf->sketchData() && ref != featureId;
                        const bool edgeRef = pn && pn->kind == naming::EntityKind::Edge;
                        if (!sketchRef && !edgeRef) return invalidItems(tid, "pierce source must be another sketch or an edge name");
                    } else if (sketch::isSilhouetteSource(src) || sketch::isExtremeSource(src)) {
                        // Silhouette line / extreme point of a surface of revolution — a face name (sketch.md §2.5, FX8-SK-002).
                        auto pn = naming::parse(naming::splitPath(sketch::projectionSourceRef(src)).leaf);
                        if (!pn || pn->kind != naming::EntityKind::Face) return invalidItems(tid, "silhouette source must be a face name");
                    } else if (sketch::isRefAxisSource(src)) {
                        // A reference axis feature of the active element, always construction (sketch.md §2.5, FX9-SK-001).
                        const auto af = nd.active().find(sketch::refAxisFeature(src));
                        if (!af || af->type != doc::FeatureType::Axis) return invalidItems(tid, "refaxis source must be an axis feature");
                        if (!it.value("construction", false)) return invalidItems(tid, "reference axis projection is always construction");
                    } else if (src != "sketch:x" && src != "sketch:y") {
                        auto pn = naming::parse(naming::splitPath(src).leaf);  // in an assembly — "<path>/<name>"
                        if (!pn || pn->kind == naming::EntityKind::Face) return invalidItems(tid, "source must be an edge or vertex name");
                    }
                }
                sd.entities.push_back(doc::entityFromJson(it));
            } else {
                if (!it.contains("refs") || !it["refs"].is_array()) return invalidItems(tid, "refs required");
                for (auto& ref : it["refs"]) {
                    if (!ref.is_string()) return invalidItems(tid, "ref must be a string");
                    auto r = resolveRef(ref, false, tid);
                    if (!r) return r.error();
                    ref = *r;
                }
                newConstraints.push_back(doc::constraintFromJson(it));
                newConstraints.back().yielded.clear();  // written only by sketchYieldAuto (core-api §4.6.2)
            }
        }
    } catch (const std::exception& e) {
        return invalidItems("", e.what());
    }
    // Constraints: refs normalization, target, branch (§4.6.2).
    const sketch::SolvedState cur = sketch::stateOf(sd);
    // Ref kinds to projections — by their geometry (sketch.md §2.5): compute before refs normalization.
    if (auto wp = projections(nd, *f, sd)) sd.projections = wp->projections;
    for (auto& c : newConstraints) {
        auto refs = sketch::normalizeRefs(sd, c);
        if (!refs) {
            // A ref to a projection without geometry (source not found in the model) — its own error, not "unsuitable
            // elements" (core-api §4.6.2; user finding 2026-10-15).
            for (const auto& r : c.refs)
                if (const auto* e = sd.findEntity(r); e && e->type == sketch::EntityType::Projected && !sd.projections.count(r))
                    return Error("SKETCH_PROJECTION_LOST").with("id", r);
            return Error("SKETCH_BAD_REFS").with("id", c.id).with("detail", std::string("refs"));
        }
        c.refs = *refs;
        if (sketch::isDimensionType(c.type) && !c.driving) c.driving = true;
        if (c.type == "fix" && c.target.empty()) {
            const sketch::Entity* e = sd.findEntity(c.refs.at(0));
            if (!e) return Error("SKETCH_BAD_REFS").with("id", c.id).with("detail", std::string("refs"));
            if (e->type == sketch::EntityType::Point) {
                const Point2 p = cur.points.at(e->id);
                c.target = {p.x(), p.y()};
            } else if (e->type == sketch::EntityType::Circle) {
                const Point2 p = cur.points.at(e->center);
                c.target = {p.x(), p.y(), e->r};
            } else if (e->type == sketch::EntityType::Ellipse) {
                // Ellipse (FX7-SK-009, sketch.md §2.2): center, semi-axis rx vector, ry — as the solver's target.
                const Point2 p = cur.points.at(e->center);
                const auto it = cur.ellipses.find(e->id);
                const double rx = it != cur.ellipses.end() ? it->second.rx : e->rx, ry = it != cur.ellipses.end() ? it->second.ry : e->ry;
                const double a = it != cur.ellipses.end() ? it->second.angle : e->angle;
                c.target = {p.x(), p.y(), rx * std::cos(a), rx * std::sin(a), ry};
            } else {
                return Error("SKETCH_BAD_REFS").with("id", c.id).with("detail", std::string("target"));
            }
        }
        // At a spline end — no branch (sketch.md §2.2, S4A-SPL-002).
        const sketch::Entity* r1 = c.refs.size() == 2 ? sd.findEntity(c.refs[1]) : nullptr;
        bool needsBranch = (c.type == "tangent" && !(r1 && sketch::isSplineType(r1->type))) || c.type == "angle";
        if (c.type == "distance" && c.refs.size() == 2) {
            // Ref kind accounting for projections (sketch.md §2.5): 0 point, 1 segment, 2 curve.
            auto kindOf = [&](const std::string& r) {
                const auto* e = r == "origin" ? nullptr : sd.findEntity(r);
                if (!e) return 0;
                auto t = e->type;
                if (t == sketch::EntityType::Projected) {
                    const auto pj = sd.projections.find(r);
                    if (pj == sd.projections.end()) return 0;
                    if (pj->second.kind == sketch::Projection::Kind::Line) return 1;
                    return pj->second.kind == sketch::Projection::Kind::Point ? 0 : 2;
                }
                return t == sketch::EntityType::Line ? 1 : (t == sketch::EntityType::Point ? 0 : 2);
            };
            const int k0 = kindOf(c.refs[0]), k1 = kindOf(c.refs[1]);
            // Side branch: to a line; to a curve from a point or a line. arc — only to a curve, near by default (§2.2).
            needsBranch = k1 == 1 || (k1 == 2 && k0 != 2);
            if (k1 == 2 && !c.arc) c.arc = "near";
            if (k1 != 2 && c.arc) return invalidItems(c.id, "arc applies only to a circle or arc");
            if (c.arc && *c.arc != "near" && *c.arc != "far") return invalidItems(c.id, "arc must be near or far");
        }
        if (needsBranch && !c.branch) {
            // Dimension value, if computable, — to choose the branch (angle sector, sketch.md §4); for a reference
            // one — its stored expression (becomes the value when switched to driving; the client makes the dimension
            // reference and then driving, app-ux §6.9, FX1-SKI-008).
            std::optional<double> value;
            if (sketch::isDimensionType(c.type) && c.value) {
                sketch::SketchData one = sd;
                sketch::Constraint cc = c;
                cc.driving = true;
                one.constraints = {cc};
                const auto vals = regen::dimensionValues(one, regen::documentVariables(nd), regen::documentUnits(nd), "");
                if (auto it = vals.find(c.id); it != vals.end() && it->second) value = *it->second;
            }
            auto b = sketch::inferBranch(sd, c, value);
            if (!b) return b.error();
            c.branch = *b;
        }
        sd.constraints.push_back(c);
    }
    out.data = std::move(sd);
    return out;
}

Result<sketch::SketchData> sketchApplyUpdate(const doc::Document& nd, const std::string& featureId, const Json& changes) {
    if (!changes.is_array()) return invalidItems("", "changes must be an array");
    sketch::SketchData sd = *nd.active().find(featureId)->sketchData();
    const auto units = regen::documentUnits(nd);
    for (const auto& ch : changes) {
        if (!ch.is_object() || !ch.contains("id") || !ch["id"].is_string()) return invalidItems("", "id required");
        const std::string id = ch["id"];
        sketch::Entity* e = nullptr;
        for (auto& x : sd.entities)
            if (x.id == id) e = &x;
        sketch::Constraint* c = nullptr;
        for (auto& x : sd.constraints)
            if (x.id == id) c = &x;
        if (!e && !c) return notFound(id);
        try {
            for (auto it = ch.begin(); it != ch.end(); ++it) {
                const std::string& k = it.key();
                const Json& v = it.value();
                if (k == "id") continue;
                if (e) {
                    // A sketch axis is always construction (sketch.md §2.5): it is not switched to regular geometry.
                    if (k == "construction" && !v.get<bool>() && e->type == sketch::EntityType::Projected &&
                        (e->source == "sketch:x" || e->source == "sketch:y" || sketch::isRefAxisSource(e->source)))
                        return invalidItems(e->id, "sketch axis is always construction");
                    if (k == "construction") e->construction = v.get<bool>();
                    else if ((k == "x" || k == "y") && e->type == sketch::EntityType::Point) (k == "x" ? e->x : e->y) = v.get<double>();
                    else if (k == "r" && e->type == sketch::EntityType::Circle) {
                        const double r = v.get<double>();
                        if (!(r > 0)) return invalidItems(id, "r must be > 0");
                        e->r = r;
                    } else if (sketch::isEllipseType(e->type) && (k == "rx" || k == "ry")) {  // S4-SKT-004, -007
                        const double r = v.get<double>();
                        if (!(r > 0)) return invalidItems(id, k + " must be > 0");
                        (k == "rx" ? e->rx : e->ry) = r;
                    } else if (sketch::isEllipseType(e->type) && k == "angle") {
                        e->angle = v.get<double>();
                    } else if (e->type == sketch::EntityType::Text && (k == "text" || k == "font")) {  // S2-SK-013
                        const std::string sv = v.get<std::string>();
                        if (sv.empty()) return invalidItems(id, k + " must be non-empty");
                        if (k == "text") {
                            for (unsigned char ch : sv)  // line breaks allowed (FX8-SK-007)
                                if (ch < 32 && ch != '\n') return invalidItems(id, "text must not contain control characters");
                            e->text = sv;
                            // Styles outside the new text are cut to it (empty ones are dropped).
                            const int n = sketch::codePointCount(sv);
                            std::erase_if(e->styles, [n](font::StyleSpan& sp) {
                                sp.to = std::min(sp.to, n);
                                return sp.from >= sp.to;
                            });
                        } else {
                            e->font = sv;
                        }
                    } else if (e->type == sketch::EntityType::Text && k == "styles") {  // FX8-SK-007
                        std::vector<font::StyleSpan> st;
                        if (!v.is_null()) {
                            if (!v.is_array()) return invalidItems(id, "styles must be an array");
                            for (const auto& o : v) {
                                if (!o.is_object() || !o.contains("from") || !o.contains("to") || !o["from"].is_number_integer() ||
                                    !o["to"].is_number_integer())
                                    return invalidItems(id, "style must have integer from, to");
                                font::StyleSpan sp;
                                sp.from = o["from"].get<int>(), sp.to = o["to"].get<int>();
                                sp.bold = o.value("bold", false), sp.italic = o.value("italic", false), sp.underline = o.value("underline", false);
                                st.push_back(sp);
                            }
                        }
                        e->styles = std::move(st);
                    } else if (e->type == sketch::EntityType::Text && k == "height") {
                        const double h = v.get<double>();
                        if (!(h > 0)) return invalidItems(id, "height must be > 0");
                        e->height = h;
                    } else if (e->type == sketch::EntityType::Text && k == "angle") {
                        e->angle = v.get<double>();
                    } else return invalidItems(id, "field not applicable: " + k);
                    continue;
                }
                const bool dim = sketch::isDimensionType(c->type);
                if (k == "value" && dim) c->value = v.get<std::string>();
                else if (k == "driving" && dim) {
                    const bool nv = v.get<bool>();
                    if (c->driving.value_or(true) && !nv) {
                        // Measurement from the current document coordinates, without solving (sketch.md §2.2, §4).
                        if (auto mv = sketch::measure(sd, id)) c->value = measuredValueText(*mv, c->type, units);
                    }
                    c->driving = nv;
                } else if (k == "type" && dim) {
                    const std::string nt = v.get<std::string>();
                    // Radius/diameter; arc: also arc length, central angle (core-api §4.6, S1-SOLVE-010, S2-SK-005).
                    const auto* re0 = sd.findEntity(c->refs.at(0));
                    const bool arc = re0 && re0->type == sketch::EntityType::Arc;
                    std::set<std::string> rd{"radius", "diameter"};
                    if (arc) rd.insert("arcLength"), rd.insert("arcAngle");
                    const std::set<std::string> dd{"distance", "distanceX", "distanceY"};
                    const std::set<std::string> ed{"semiMajor", "semiMinor", "ellipseAngle"};  // ellipse (S4-SKT-004)
                    bool ok = (rd.count(c->type) && rd.count(nt)) || (dd.count(c->type) && dd.count(nt)) || (ed.count(c->type) && ed.count(nt));
                    if (ok && dd.count(nt)) {
                        for (const auto& r : c->refs) {
                            const auto* re = sd.findEntity(r);
                            if (r != "origin" && (!re || re->type != sketch::EntityType::Point)) ok = false;
                        }
                    }
                    if (!ok) return invalidItems(id, "type change not allowed");
                    c->type = nt;
                    c->branch.reset();
                    if (!ch.contains("value"))
                        if (auto mv = sketch::measure(sd, id)) c->value = measuredValueText(*mv, nt, units);
                } else if (k == "labelOffset" && dim) {  // FX8-SK-004: offset from the dimension anchor
                    if (v.is_null()) c->labelOffset.reset();
                    else c->labelOffset = std::array<double, 2>{v.at(0).get<double>(), v.at(1).get<double>()};
                } else if (k == "arc" && c->type == "distance" && c->arc) {
                    const std::string a = v.get<std::string>();
                    if (a != "near" && a != "far") return invalidItems(id, "arc must be near or far");
                    c->arc = a;
                } else if (k == "diametral" && (c->type == "distance" || c->type == "distanceX" || c->type == "distanceY") && c->refs.size() == 2) {
                    // Diameter / radius from an axis (S2-SK-003): the value is doubled or halved — same geometry.
                    const bool nv = v.get<bool>();
                    if (nv != c->diametral && c->value && !ch.contains("value")) {
                        if (auto mv = sketch::measure(sd, id)) c->value = measuredValueText(std::abs(nv ? 2 * *mv : *mv / 2), c->type, units);
                    }
                    c->diametral = nv;
                } else if (k == "refs" && (c->type == "distanceX" || c->type == "distanceY") && c->refs.size() == 2) {
                    // The same two refs in reverse order: dimension on the other side (core-api §4.6.2, FX1-SKG-003).
                    if (!v.is_array() || v.size() != 2 || v[0].get<std::string>() != c->refs[1] || v[1].get<std::string>() != c->refs[0])
                        return invalidItems(id, "refs must be the same two references swapped");
                    std::swap(c->refs[0], c->refs[1]);
                } else if (k == "auto" && !dim && c->type != "fix") {
                    if (v.get<bool>()) return invalidItems(id, "auto can only be cleared");
                    c->automatic = false;
                } else if (k == "branch") {
                    Json cj = doc::constraintToJsonValue(*c);
                    cj["branch"] = v;
                    c->branch = doc::constraintFromJson(cj).branch;
                } else {
                    return invalidItems(id, "field not applicable: " + k);
                }
            }
        } catch (const std::exception& ex) {
            return invalidItems(id, ex.what());
        }
    }
    return sd;
}

Result<sketch::SketchData> sketchApplyDelete(sketch::SketchData sd, const std::vector<std::string>& ids) {
    std::set<std::string> owned;
    for (const auto& e : sd.entities)
        for (const auto* p : {&e.p1, &e.p2, &e.center, &e.start, &e.end})
            if (!p->empty()) owned.insert(*p);
    for (const auto& e : sd.entities) owned.insert(e.poles.begin(), e.poles.end());  // spline poles (S4A-SPL-001)
    std::set<std::string> delEntities, delConstraints;
    for (const auto& id : ids) {
        if (id == "origin") return invalidItems(id, "origin cannot be deleted");
        if (const auto* e = sd.findEntity(id)) {
            if (e->type == sketch::EntityType::Point && owned.count(id)) return invalidItems(id, "point belongs to a curve");
            delEntities.insert(id);
            for (const auto* p : {&e->p1, &e->p2, &e->center, &e->start, &e->end})
                if (!p->empty()) delEntities.insert(*p);
            delEntities.insert(e->poles.begin(), e->poles.end());
        } else if (sd.findConstraint(id)) {
            delConstraints.insert(id);
        } else {
            return notFound(id);
        }
    }
    std::erase_if(sd.entities, [&](const sketch::Entity& e) { return delEntities.count(e.id) > 0; });
    std::erase_if(sd.constraints, [&](const sketch::Constraint& c) {
        if (delConstraints.count(c.id)) return true;
        for (const auto& r : c.refs)
            if (delEntities.count(r)) return true;
        return false;
    });
    return sd;
}

}  // namespace construe::api
