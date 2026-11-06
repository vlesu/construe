// Session: sketches — creation, editing, fillets/chamfers, auto-dimensioning, trim, dragging (spec/core-api.md §4.6).
#include "api/session.h"
#include <numbers>
#include "regen/hole_tables.h"
#include <Eigen/Eigenvalues>

#include "sketch/trim.h"

#include "api/errors.h"
#include "api/feature_commands.h"
#include "api/sections.h"
#include "regen/mates.h"
#include "font/font.h"
#include "api/sketch_commands.h"
#include "api/sketch_groups.h"
#include "api/sketch_ops.h"

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

namespace {

bool writesCoordinates(sketch::SketchStatus s) {
    return s == sketch::SketchStatus::UnderConstrained || s == sketch::SketchStatus::FullyConstrained ||
           s == sketch::SketchStatus::OverConstrainedRedundant;
}

}  // namespace

// ---------------------------------------------------------------- §4.6

Result<std::string> Session::createSketch(const Json& plane, const std::vector<Json>& aux) {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (doc().active().isBody() || doc().active().isDrawing() || doc().active().isSpecification())  // body, drawing, specification (document.md §2.4в, §2.4г, §2.4е)
        return Error("API_NOT_PART").with("id", doc().active().id);
    Json def = Json::object();
    def["type"] = "sketch";
    def["plane"] = plane;
    def["entities"] = Json::array();
    def["constraints"] = Json::array();
    const auto prevCtx = ctx_;
    // The ID that addFeature will assign (after the owned features of aux, FX7-SEL-001).
    ctx_ = "f" + std::to_string(doc().counters.feature + static_cast<int64_t>(aux.size()));
    beginGroup("undo.createSketch");
    auto r = addFeature(def, aux);
    endGroup();
    if (!r) {
        ctx_ = prevCtx;
        return r.error();
    }
    ctx_ = *r;
    openSketch_.reset();
    if (doc::FeaturePtr f = doc().active().find(*r)) sketchEntry_ = *f;
    return r;
}

Status Session::enterSketch(const std::string& featureId) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::FeaturePtr f = doc().active().find(featureId);
    if (!f) return notFound(featureId);
    if (!f->sketchData()) return Error("API_NOT_SKETCH").with("id", featureId);
    ctx_ = featureId;
    openSketch_.reset();
    sketchEntry_ = *f;
    return okStatus();
}

Status Session::exitSketch() {
    if (auto s = checkNoDrag(); !s) return s;
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    ctx_.reset();
    openSketch_.reset();
    sketchEntry_.reset();
    return okStatus();
}

Status Session::sketchCancel() {
    if (auto s = checkNoDrag(); !s) return s;
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    // The sketch's data as it was opened; the entry of another sketch (the context changed by undo/redo) — nothing.
    doc::FeaturePtr f = doc().active().find(*ctx_);
    if (f && sketchEntry_ && sketchEntry_->id == *ctx_ && sketchEntry_->sketchData() && f->sketchData() &&
        doc::featureToJsonValue(*f) != doc::featureToJsonValue(*sketchEntry_)) {
        auto r = withSketch(doc(), *ctx_, *sketchEntry_->sketchData());
        if (!r) return r.error();
        if (auto s = commit(std::move(*r), "undo.sketchCancel", {*ctx_}, *ctx_); !s) return s;
    }
    return exitSketch();
}

bool Session::sketchChanged() const {
    if (!ctx_ || !sketchEntry_ || sketchEntry_->id != *ctx_) return false;
    doc::FeaturePtr f = doc().active().find(*ctx_);
    return f && doc::featureToJsonValue(*f) != doc::featureToJsonValue(*sketchEntry_);
}

Result<std::map<std::string, std::string>> Session::sketchAdd(const Json& items) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s.error();
    doc::Document nd = doc();
    auto added = sketchAddItems(nd, *ctx_, items, [this](const doc::Document& d, const doc::Feature& f, sketch::SketchData sd) {
        return withProjections(d, f, std::move(sd));
    });
    if (!added) return added.error();
    auto r = withSketch(nd, *ctx_, added->data);
    if (!r) return r.error();
    if (auto s = commit(std::move(*r), "undo.sketchAdd", {*ctx_}, *ctx_); !s) return s.error();
    return added->response;
}

Status Session::sketchUpdate(const Json& changes) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s;
    const doc::Document& d = doc();
    auto sd = sketchApplyUpdate(d, *ctx_, changes);
    if (!sd) return sd.error();
    auto r = withSketch(d, *ctx_, *sd);
    if (!r) return r.error();
    return commit(std::move(*r), "undo.sketchUpdate", {*ctx_}, *ctx_);
}

// ---------------------------------------------------------------- sketch fillet and chamfer (§4.6.2)

namespace {

struct Corner {
    std::string p, q, l1, l2;
    std::vector<std::string> coincident;              // coincident constraints of p or q inside the corner cluster
    std::vector<std::string> attached;                // other cluster points linked to p or q by them
    std::string far1, far2;  // far ends of the sides
    Point2 v, u1, u2, o1, o2;
    double len1 = 0, len2 = 0, theta = 0;
};

Result<Corner> cornerAt(const sketch::SketchData& sd, const std::string& point) {
    Corner c;
    auto bad = [&](const std::string& detail) { return Error("API_INVALID_ARGUMENT").with("name", std::string("point")).with("detail", detail); };
    // The corner is the coincidence cluster of the point; its sides are the two non-construction lines ending there,
    // construction curves meeting at the corner (sides of a previous fillet, a construction line under a side) do not
    // matter (FX7-SK-006).
    const auto f = sketchops::findCorner(sd, point);
    if (!f.problem.empty()) return bad(f.problem).with("count", double(f.lines));
    c.p = f.p, c.q = f.q, c.l1 = f.l1, c.l2 = f.l2;
    const std::set<std::string> cluster(f.cluster.begin(), f.cluster.end());
    for (const auto& k : sd.constraints) {
        if (k.type != "coincident" || k.refs.size() != 2 || !cluster.count(k.refs[0]) || !cluster.count(k.refs[1])) continue;
        const bool hasP = k.refs[0] == c.p || k.refs[1] == c.p, hasQ = k.refs[0] == c.q || k.refs[1] == c.q;
        if (!hasP && !hasQ) continue;
        c.coincident.push_back(k.id);
        for (const auto& r : k.refs)
            if (r != c.p && r != c.q && std::find(c.attached.begin(), c.attached.end(), r) == c.attached.end()) c.attached.push_back(r);
    }
    const auto* l1 = sd.findEntity(c.l1);
    const auto* l2 = sd.findEntity(c.l2);
    auto pos = [&](const std::string& id) { return Point2(sd.findEntity(id)->x, sd.findEntity(id)->y); };
    c.v = pos(c.p);
    c.far1 = l1->p1 == c.p ? l1->p2 : l1->p1, c.far2 = l2->p1 == c.q ? l2->p2 : l2->p1;
    const Point2 o1 = pos(c.far1), o2 = pos(c.far2);
    c.o1 = o1, c.o2 = o2;
    c.len1 = (o1 - c.v).norm(), c.len2 = (o2 - pos(c.q)).norm();
    if (c.len1 <= 0 || c.len2 <= 0) return bad("degenerate");
    c.u1 = (o1 - c.v) / c.len1, c.u2 = (o2 - c.v).normalized();
    c.theta = std::acos(std::clamp(c.u1.dot(c.u2), -1.0, 1.0));
    if (c.theta < 1e-6 || std::numbers::pi - c.theta < 1e-6) return bad("parallel");
    return c;
}

}  // namespace

Result<std::map<std::string, std::string>> Session::sketchFillet(const std::string& point, const std::string& radius) {
    return sketchCorner(point, radius, "", false, true);
}

Result<std::map<std::string, std::string>> Session::sketchChamfer(const std::string& point, const std::string& d1,
                                                                  const std::string& second, bool secondIsAngle) {
    return sketchCorner(point, d1, second, secondIsAngle, false);
}

namespace {
// Value of a measured dimension as text in document units (as sketchUpdate, core-api §4.6.2).
std::string measuredValueText(double v, bool angle, const expr::DocumentUnits& u) {
    expr::Quantity q{v, angle ? expr::ANGLE : expr::LENGTH};
    std::string t = expr::format(q, u, 6);
    // The value is an expression: it rejects the display "°", the unit "deg" is written (core-api §4.6.2, "30 deg").
    const std::string deg = "°";
    if (t.size() >= deg.size() && t.compare(t.size() - deg.size(), deg.size(), deg) == 0) t = t.substr(0, t.size() - deg.size()) + " deg";
    return t;
}
}  // namespace

// Constraint item from a weak one (sketch.md §4.4): type and refs; for a dimension — value as text in document units.
namespace {
Json weakItem(const sketch::Constraint& w, const SketchState& st, const std::function<std::string(double, bool)>& valueText) {
    Json item{{"type", w.type}, {"refs", w.refs}};
    if (sketch::isDimensionType(w.type)) {
        const auto it = st.result.reference.find(w.id);
        item["value"] = valueText(it != st.result.reference.end() ? it->second : 0.0, sketch::isAngleDimension(w.type));
        item["driving"] = true;
    }
    return item;
}
Json weakItem(const sketch::Constraint& w, const SketchState& st, const expr::DocumentUnits& units) {
    return weakItem(w, st, [&](double v, bool angle) { return measuredValueText(v, angle, units); });
}
}  // namespace

Result<std::vector<std::string>> Session::sketchFullyDefine(const FullyDefineLayout& layout, bool splines) {
    const expr::DocumentUnits units = regen::documentUnits(doc());
    return fullyDefineWith(layout, [units](double v, bool angle) { return measuredValueText(v, angle, units); }, splines);
}

Result<std::vector<std::string>> Session::fullyDefineWith(const FullyDefineLayout& layout,
                                                          const std::function<std::string(double, bool)>& valueText, bool splines) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s.error();
    // "Dimension all" (S4-WEAK-004, FX4-SK-001/002): the weak set of the open sketch (sketch.md §4.4) — as ordinary
    // dimensions and relations in a single undo step.
    auto st = sketchState(*ctx_);
    if (!st) return st.error();
    if (st->weak.empty()) return std::vector<std::string>{};
    const doc::Document& d = doc();
    const auto f = d.active().find(*ctx_);
    const sketch::SketchData base = withProjections(d, *f).value_or(*f->sketchData());
    // Without splines (FX7-SK-013): weak items referring to a spline or one of its poles stay weak.
    std::set<std::string> splineRefs;
    if (!splines)
        for (const auto& e : base.entities)
            if (sketch::isSplineType(e.type)) {
                splineRefs.insert(e.id);
                splineRefs.insert(e.poles.begin(), e.poles.end());
            }
    std::vector<Json> items;
    for (const auto& w : st->weak)
        if (std::none_of(w.refs.begin(), w.refs.end(), [&](const std::string& r) { return splineRefs.count(r) > 0; }))
            items.push_back(weakItem(w, *st, valueText));
    if (items.empty()) return std::vector<std::string>{};
    // Label layout (FX4-SK-001): label positions of the new dimensions.
    std::vector<sketch::Constraint> dims;
    std::vector<size_t> dimItem;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].contains("value")) {
            Json withId = items[i];
            withId["id"] = "c" + std::to_string(d.counters.constraint + 2000000 + i);
            dims.push_back(doc::constraintFromJson(withId));
            dimItem.push_back(i);
        }
    const auto labels = sketchops::layoutDimensionLabels(base, st->result.state, dims, layout);
    for (size_t k = 0; k < dims.size(); ++k)  // stored relative to the dimension anchor (FX8-SK-004)
        if (const auto off = sketchops::labelOffsetFor(base, st->result.state, dims[k], labels[k]))
            items[dimItem[k]]["labelOffset"] = Json::array({(*off)[0], (*off)[1]});
    Json batch = Json::array();
    for (size_t i = 0; i < items.size(); ++i) {
        items[i]["id"] = "t" + std::to_string(i + 1);  // temporary ID — the sketchAdd response gives the permanent one
        batch.push_back(items[i]);
    }
    beginGroup("undo.group.fullyDefine");
    auto added = sketchAdd(batch);
    auto end = endGroup();
    if (!added) return added.error();
    if (!end) return end.error();
    std::vector<std::string> ids;
    for (const auto& [tmp, id] : *added) ids.push_back(id);
    std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) { return compareIds(a, b) < 0; });
    return ids;
}

Result<std::string> Session::sketchStrengthen(const std::string& weakId, const std::optional<std::string>& value,
                                              const std::optional<Point2>& label) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s.error();
    auto st = sketchState(*ctx_);
    if (!st) return st.error();
    const auto w = std::find_if(st->weak.begin(), st->weak.end(), [&](const sketch::Constraint& c) { return c.id == weakId; });
    if (w == st->weak.end()) return notFound(weakId);
    Json item = weakItem(*w, *st, regen::documentUnits(doc()));
    item["id"] = "t1";
    if (value && sketch::isDimensionType(w->type)) {
        // New value: same side, negative — the other one (like editing a value, app-ux §6.9, FX1-SKG-003).
        auto q = checkExpression(*value, sketch::isAngleDimension(w->type) ? expr::ANGLE : expr::LENGTH);
        if (!q) return Error("API_INVALID_ARGUMENT").with("name", std::string("value"));
        sketch::Constraint c = *w;
        const auto it = st->result.reference.find(w->id);
        const double current = it != st->result.reference.end() ? it->second : 1.0;
        const Json ch = sketchops::dimensionValueChanges(c, current, *value, q->value)[0];
        item["value"] = ch["value"];
        if (ch.contains("refs")) item["refs"] = ch["refs"];
    }
    if (label && sketch::isDimensionType(w->type))  // FX8-SK-004: relative to the dimension anchor
        if (const auto d = withProjections(doc(), *doc().active().find(*ctx_)))
            if (const auto off = sketchops::labelOffsetFor(*d, st->result.state, *w, *label))
                item["labelOffset"] = Json::array({(*off)[0], (*off)[1]});
    beginGroup("undo.group.strengthen");
    auto added = sketchAdd(Json::array({item}));
    auto end = endGroup();
    if (!added) return added.error();
    if (!end) return end.error();
    return added->at("t1");
}

Result<std::map<std::string, std::string>> Session::sketchCorner(const std::string& point, const std::string& v1,
                                                                 const std::string& v2, bool v2Angle, bool fillet) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s.error();
    doc::Document nd = doc();
    const auto f = nd.active().find(*ctx_);
    sketch::SketchData sd = *f->sketchData();
    auto corner = cornerAt(sd, point);
    if (!corner) return corner.error();
    const Corner c = *corner;
    // Sides that are members of a linked group change only through the group (sketch.md §2.6.2); an offset contour is
    // unlinked first (S7F-SK-005).
    for (const auto& l : {c.l1, c.l2})
        if (const auto* g = sd.groupOfMember(l); g && g->type == "offset") {
            const std::string gid = g->id;
            sd = sketch::removeGroup(std::move(sd), gid, true);
        }
    if (sd.groupOfMember(c.l1) || sd.groupOfMember(c.l2)) return invalidItems(point, "group");
    auto value = [&](const std::string& text, expr::Dim dim, const char* name) -> Result<double> {
        auto q = checkExpression(text, dim);
        if (!q || !(q->value > 0)) return Error("API_INVALID_ARGUMENT").with("name", std::string(name));
        return q->value;
    };
    Point2 t1, t2, center;
    if (fillet) {
        auto r = value(v1, expr::LENGTH, "radius");
        if (!r) return r.error();
        const double t = *r / std::tan(c.theta / 2);
        if (t >= c.len1 || t >= c.len2) return Error("API_INVALID_ARGUMENT").with("name", std::string("radius")).with("actual", *r).with("detail", std::string("too large"));
        t1 = c.v + c.u1 * t, t2 = c.v + c.u2 * t;
        center = c.v + (c.u1 + c.u2).normalized() * (*r / std::sin(c.theta / 2));
    } else {
        auto a = value(v1, expr::LENGTH, "d1");
        if (!a) return a.error();
        double b = 0;
        if (v2Angle) {
            auto ang = value(v2, expr::ANGLE, "second");
            if (!ang) return ang.error();
            if (c.theta + *ang >= std::numbers::pi)
                return Error("API_INVALID_ARGUMENT").with("name", std::string("second")).with("detail", std::string("too large"));
            b = *a * std::sin(*ang) / std::sin(c.theta + *ang);
        } else {
            auto bb = value(v2, expr::LENGTH, "second");
            if (!bb) return bb.error();
            b = *bb;
        }
        if (*a >= c.len1) return Error("API_INVALID_ARGUMENT").with("name", std::string("d1")).with("actual", *a).with("detail", std::string("too large"));
        if (b >= c.len2) return Error("API_INVALID_ARGUMENT").with("name", std::string("second")).with("actual", b).with("detail", std::string("too large"));
        t1 = c.v + c.u1 * *a, t2 = c.v + c.u2 * b;
    }
    // Delete the corner coincident, move the ends to the tangency points (segments get shorter).
    std::erase_if(sd.constraints, [&](const sketch::Constraint& k) {
        return std::find(c.coincident.begin(), c.coincident.end(), k.id) != c.coincident.end();
    });
    for (auto& e : sd.entities) {
        if (e.id == c.p) e.x = t1.x(), e.y = t1.y();
        if (e.id == c.q) e.x = t2.x(), e.y = t2.y();
    }
    const std::set<std::string> oldConstraints = [&] {
        std::set<std::string> s;
        for (const auto& k : sd.constraints) s.insert(k.id);
        return s;
    }();
    {
        doc::Feature nf = *f;
        nf.data = sd;
        nd = replaceFeature(nd, nf);
    }
    auto pt = [](const std::string& id, const Point2& p) { return Json{{"id", id}, {"type", "point"}, {"x", p.x()}, {"y", p.y()}}; };
    auto con = [](const std::string& type, std::vector<std::string> refs) { return Json{{"type", type}, {"refs", refs}}; };
    Json items = Json::array();
    items.push_back(pt("t1", c.v));  // virtual vertex
    items.push_back(con("pointOnObject", {"t1", c.l1}));
    items.push_back(con("pointOnObject", {"t1", c.l2}));
    // Sides up to the virtual vertex in full — construction lines from the far ends (FX1-SKG-006): constraints
    // depending on side length are moved onto them — else a shortened side would drag the related dimensions along.
    items.push_back(pt("t20", c.o1)), items.push_back(pt("t21", c.v));
    items.push_back(Json{{"id", "t22"}, {"type", "line"}, {"p1", "t20"}, {"p2", "t21"}, {"construction", true}});
    items.push_back(con("coincident", {"t20", c.far1})), items.push_back(con("coincident", {"t21", "t1"}));
    items.push_back(pt("t23", c.o2)), items.push_back(pt("t24", c.v));
    items.push_back(Json{{"id", "t25"}, {"type", "line"}, {"p1", "t23"}, {"p2", "t24"}, {"construction", true}});
    items.push_back(con("coincident", {"t23", c.far2})), items.push_back(con("coincident", {"t24", "t1"}));
    // Other curves that met at the corner (construction lines, points) stay at the vertex (FX7-SK-006).
    for (const auto& a : c.attached) items.push_back(con("coincident", {a, "t1"}));
    if (fillet) {
        // Counterclockwise arc: start is the tangency point from which traversal to the other one is counterclockwise.
        const bool ccw = (t1 - center).x() * (t2 - center).y() - (t1 - center).y() * (t2 - center).x() > 0;
        items.push_back(pt("t2", center));
        items.push_back(pt("t3", ccw ? t1 : t2));
        items.push_back(pt("t4", ccw ? t2 : t1));
        items.push_back(Json{{"id", "t5"}, {"type", "arc"}, {"center", "t2"}, {"start", "t3"}, {"end", "t4"}});
        items.push_back(con("coincident", {"t3", ccw ? c.p : c.q}));
        items.push_back(con("coincident", {"t4", ccw ? c.q : c.p}));
        items.push_back(con("tangent", {c.l1, "t5"}));
        items.push_back(con("tangent", {c.l2, "t5"}));
        items.push_back(Json{{"type", "radius"}, {"refs", {"t5"}}, {"value", v1}, {"driving", true}});
    } else {
        items.push_back(pt("t2", t1)), items.push_back(pt("t3", t2));
        items.push_back(Json{{"id", "t4"}, {"type", "line"}, {"p1", "t2"}, {"p2", "t3"}});
        items.push_back(con("coincident", {"t2", c.p})), items.push_back(con("coincident", {"t3", c.q}));
        items.push_back(Json{{"type", "distance"}, {"refs", {"t1", c.p}}, {"value", v1}, {"driving", true}});
        if (v2Angle) items.push_back(Json{{"type", "angle"}, {"refs", {c.l1, "t4"}}, {"value", v2}, {"driving", true}});
        else items.push_back(Json{{"type", "distance"}, {"refs", {"t1", c.q}}, {"value", v2}, {"driving", true}});
    }
    auto added = sketchAddItems(nd, *ctx_, items, [this](const doc::Document& d, const doc::Feature& ff, sketch::SketchData x) {
        return withProjections(d, ff, std::move(x));
    });
    if (!added) return added.error();
    // Old constraints of the corner point go to the virtual vertex: side dimensions hold the vertex, fillet persists.
    const std::string vv = added->response.at("t1");
    auto otherEnd = [&](const std::string& line, const std::string& end) {
        const auto* l = added->data.findEntity(line);
        return l->p1 == end ? l->p2 : l->p1;
    };
    const std::string side1 = added->response.at("t22"), side2 = added->response.at("t25");
    for (auto& k : added->data.constraints) {
        if (!oldConstraints.count(k.id)) continue;
        for (auto& r : k.refs)
            if (r == c.p || r == c.q) r = vv;
        // Length and midpoint constraints of a shortened side go to the whole side (construction line up to vertex).
        if (k.type == "equal" || k.type == "midpoint")
            for (auto& r : k.refs) {
                if (r == c.l1) r = side1;
                else if (r == c.l2) r = side2;
            }
        // Length of a shortened side — distance from its other end to the virtual vertex.
        if (k.type == "length" && k.refs.size() == 1 && (k.refs[0] == c.l1 || k.refs[0] == c.l2)) {
            const std::string end = k.refs[0] == c.l1 ? otherEnd(c.l1, c.p) : otherEnd(c.l2, c.q);
            k.type = "distance";
            k.refs = {end, vv};
            k.branch.reset();
        }
    }
    auto r = withSketch(nd, *ctx_, added->data);
    if (!r) return r.error();
    if (auto s = commit(std::move(*r), fillet ? "undo.sketchFillet" : "undo.sketchChamfer", {*ctx_}, *ctx_); !s) return s.error();
    return added->response;
}

namespace {

// A constraint with the same type and set of refs (ref order does not matter).
bool sameConstraint(const sketch::Constraint& a, const sketch::Constraint& b) {
    if (a.type != b.type || a.refs.size() != b.refs.size()) return false;
    std::multiset<std::string> ra(a.refs.begin(), a.refs.end()), rb(b.refs.begin(), b.refs.end());
    return ra == rb;
}

// ---------------------------------------------------------------- duplicate curves (§4.6.2 sketchMergeDuplicate)

constexpr double DUPLICATE_TOL = 1e-6;  // mm (× max(1, curve size)): curves on the same path

// Owned points of a curve matched to the points of its duplicate by position: removed point → kept point.
std::optional<std::map<std::string, std::string>> duplicatePoints(const sketch::Entity& a, const sketch::Entity& b,
                                                                  const sketch::SolvedState& st, double tol) {
    auto P = [&](const std::string& id) {
        const auto it = st.points.find(id);
        return it != st.points.end() ? it->second : Point2(1e300, 1e300);
    };
    auto near = [&](const std::string& x, const std::string& y) { return (P(x) - P(y)).norm() <= tol; };
    auto R = [&](const sketch::Entity& e) {
        const auto it = st.radii.find(e.id);
        return it != st.radii.end() ? it->second : e.r;
    };
    using T = sketch::EntityType;
    if (a.type != b.type) return std::nullopt;
    switch (a.type) {
        case T::Line:
            if (near(a.p1, b.p1) && near(a.p2, b.p2)) return std::map<std::string, std::string>{{a.p1, b.p1}, {a.p2, b.p2}};
            if (near(a.p1, b.p2) && near(a.p2, b.p1)) return std::map<std::string, std::string>{{a.p1, b.p2}, {a.p2, b.p1}};
            return std::nullopt;
        case T::Circle:
            if (near(a.center, b.center) && std::abs(R(a) - R(b)) <= tol) return std::map<std::string, std::string>{{a.center, b.center}};
            return std::nullopt;
        case T::Arc:
            if (near(a.center, b.center) && near(a.start, b.start) && near(a.end, b.end))
                return std::map<std::string, std::string>{{a.center, b.center}, {a.start, b.start}, {a.end, b.end}};
            return std::nullopt;
        case T::Ellipse: {
            const auto ea = st.ellipses.find(a.id), eb = st.ellipses.find(b.id);
            if (ea == st.ellipses.end() || eb == st.ellipses.end() || !near(a.center, b.center)) return std::nullopt;
            const auto& x = ea->second;
            const auto& y = eb->second;
            if (std::abs(x.rx - y.rx) > tol || std::abs(x.ry - y.ry) > tol || std::abs(std::sin(x.angle - y.angle)) * x.rx > tol) return std::nullopt;
            return std::map<std::string, std::string>{{a.center, b.center}};
        }
        default: return std::nullopt;
    }
}

double curveSize(const sketch::Entity& e, const sketch::SolvedState& st) {
    auto P = [&](const std::string& id) {
        const auto it = st.points.find(id);
        return it != st.points.end() ? it->second : Point2(0, 0);
    };
    if (e.type == sketch::EntityType::Line) return (P(e.p2) - P(e.p1)).norm();
    if (e.type == sketch::EntityType::Arc) return (P(e.start) - P(e.center)).norm();
    if (const auto it = st.radii.find(e.id); it != st.radii.end()) return it->second;
    if (const auto it = st.ellipses.find(e.id); it != st.ellipses.end()) return std::max(it->second.rx, it->second.ry);
    return 1;
}

}  // namespace

Status Session::sketchDelete(const std::vector<std::string>& ids) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s;
    const doc::Document& d = doc();
    const int idx = d.active().indexOf(*ctx_);
    const doc::Feature& f = *d.active().features[idx];
    const sketch::SketchData& old = *f.sketchData();
    for (const auto& id : ids)
        if (id != "origin" && !old.findEntity(id) && !old.findConstraint(id) && !old.findGroup(id)) return notFound(id);
    // A group or its member — the group is deleted entirely (sketch.md §2.6.2); the rest — as before.
    auto [withoutGroups, restIds] = deleteGroupsOf(old, ids);
    std::vector<std::string> rest;
    for (const auto& id : restIds)
        if (id == "origin" || withoutGroups.findEntity(id) || withoutGroups.findConstraint(id)) rest.push_back(id);
    doc::Document nd = d;
    auto sd = sketchApplyDelete(withoutGroups, rest);
    if (!sd) return sd.error();
    // Projection geometry for rebuilding offsets of projections (sketch.md §2.6.5).
    if (auto wp = withProjections(d, f, *sd)) *sd = std::move(*wp);
    *sd = reconcileGroups(std::move(*sd), regen::documentUnits(d), regen::documentVariables(d).env(),
                          [&](bool entity) { return doc::allocate(nd, entity ? doc::IdKind::Entity : doc::IdKind::Constraint); });
    // Deleting a dimension keeps relations (FX4-SK-003, core-api §4.6.2): auto-constraints removed when it was set
    // (yielded) come back if their refs are intact, no such constraint exists, and it adds no conflict or redundancy.
    std::vector<sketch::Constraint> back;
    for (const auto& c : old.constraints)
        if (!c.yielded.empty() && !sd->findConstraint(c.id)) back.insert(back.end(), c.yielded.begin(), c.yielded.end());
    if (!back.empty()) {
        const auto vars = regen::documentVariables(d);
        const auto units = regen::documentUnits(d);
        const std::string path = "/elements/" + std::to_string(d.activeIndex()) + "/features/" + std::to_string(idx);
        auto solveData = [&](const sketch::SketchData& x) {
            const sketch::SketchData full = withProjections(d, f, x).value_or(x);
            return sketch::solve(full, regen::dimensionValues(full, vars, units, path));
        };
        sketch::SketchStatus status = solveData(*sd).status;
        for (sketch::Constraint y : back) {
            y.automatic = true;
            y.yielded.clear();
            if (sd->findConstraint(y.id)) continue;
            bool refsOk = true;
            for (const auto& r : y.refs) refsOk = refsOk && (r == "origin" || sd->findEntity(r));
            if (!refsOk) continue;
            if (std::any_of(sd->constraints.begin(), sd->constraints.end(), [&](const sketch::Constraint& c) { return sameConstraint(c, y); }))
                continue;
            sketch::SketchData trial = *sd;
            trial.constraints.push_back(y);
            const sketch::SketchResult r = solveData(trial);
            bool troubled = std::find(r.redundant.begin(), r.redundant.end(), y.id) != r.redundant.end();
            for (const auto& g : r.conflicts) troubled = troubled || std::find(g.ids.begin(), g.ids.end(), y.id) != g.ids.end();
            if (r.status == sketch::SketchStatus::NotConverged && status != sketch::SketchStatus::NotConverged) troubled = true;
            if (r.status == sketch::SketchStatus::Error && status != sketch::SketchStatus::Error) troubled = true;
            if (troubled) continue;
            *sd = std::move(trial);
            status = r.status;
        }
    }
    auto r = withSketch(nd, *ctx_, *sd);
    if (!r) return r.error();
    return commit(std::move(*r), "undo.sketchDelete", {*ctx_}, *ctx_);
}

// ---------------------------------------------------------------- linked groups (§4.6.2, sketch.md §2.6)

Result<std::string> Session::sketchGroupCreate(const std::string& type, const std::vector<std::string>& sources, const Json& params) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s.error();
    doc::Document nd = doc();
    // With projection geometry: projections can be offset sources (sketch.md §2.6.5).
    const auto f = nd.active().find(*ctx_);
    const sketch::SketchData sd = withProjections(nd, *f).value_or(*f->sketchData());
    const std::string id = doc::allocate(nd, doc::IdKind::Group);
    const Allocate alloc = [&](bool entity) { return doc::allocate(nd, entity ? doc::IdKind::Entity : doc::IdKind::Constraint); };
    auto r = rebuildGroup(sd, id, type, sources, params, regen::documentUnits(nd), regen::documentVariables(nd).env(), alloc);
    if (!r) return r.error();
    auto w = withSketch(nd, *ctx_, *r);
    if (!w) return w.error();
    if (auto c = commit(std::move(*w), "undo.sketchGroupCreate", {*ctx_}, *ctx_); !c) return c.error();
    return id;
}

Status Session::sketchGroupUpdate(const std::string& id, const std::vector<std::string>& sources, const Json& params) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    const auto f = nd.active().find(*ctx_);
    const sketch::SketchData sd = withProjections(nd, *f).value_or(*f->sketchData());
    const sketch::Group* g = sd.findGroup(id);
    if (!g) return notFound(id);
    const Allocate alloc = [&](bool entity) { return doc::allocate(nd, entity ? doc::IdKind::Entity : doc::IdKind::Constraint); };
    const auto units = regen::documentUnits(nd);
    const expr::Env env = regen::documentVariables(nd).env();
    auto r = rebuildGroup(sd, id, g->type, sources, params, units, env, alloc);
    if (!r) return r.error();
    // Cascade: groups on members of this group (sketch.md §2.6.2 "Reconciliation").
    *r = reconcileGroups(std::move(*r), units, env, alloc);
    auto w = withSketch(nd, *ctx_, *r);
    if (!w) return w.error();
    return commit(std::move(*w), "undo.sketchGroupUpdate", {*ctx_}, *ctx_);
}

Status Session::sketchGroupUnlink(const std::string& id) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s;
    const sketch::SketchData& sd = *doc().active().find(*ctx_)->sketchData();
    if (!sd.findGroup(id)) return notFound(id);
    auto w = withSketch(doc(), *ctx_, sketch::removeGroup(sd, id, true));
    if (!w) return w.error();
    return commit(std::move(*w), "undo.sketchGroupUnlink", {*ctx_}, *ctx_);
}

// ---------------------------------------------------------------- weak dimension anchor (§4.6.2, S4-WEAK-006)

Status Session::sketchSetWeakAnchor(const std::string& point) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s;
    sketch::SketchData sd = *doc().active().find(*ctx_)->sketchData();
    const sketch::Entity* e = sd.findEntity(point);
    if (!e) return notFound(point);
    if (e->type != sketch::EntityType::Point) return invalidItems(point, "not a point");
    std::erase(sd.weakAnchors, point);
    sd.weakAnchors.insert(sd.weakAnchors.begin(), point);
    auto w = withSketch(doc(), *ctx_, sd);
    if (!w) return w.error();
    return commit(std::move(*w), "undo.sketchSetWeakAnchor", {*ctx_}, *ctx_);
}

namespace {

// Incidence — position snap constraints (vision item 16).
bool isIncidence(const std::string& t) {
    return t == "coincident" || t == "pointOnObject" || t == "midpoint" || t == "concentric";
}

constexpr double YIELD_INCIDENCE_TOL = 1e-6;  // mm: a snap is implied if the point did not move without it

}  // namespace

Result<std::string> Session::sketchYieldAuto(const std::vector<std::string>& added) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s.error();
    const doc::Document& d = doc();
    const int idx = d.active().indexOf(*ctx_);
    const doc::Feature& f = *d.active().features[idx];
    const sketch::SketchData base = withProjections(d, f).value_or(*f.sketchData());
    const auto values = regen::dimensionValues(base, regen::documentVariables(d), regen::documentUnits(d),
                                               "/elements/" + std::to_string(d.activeIndex()) + "/features/" + std::to_string(idx));
    const sketch::SketchResult cur = sketch::solve(base, values);
    // How many new constraints (except except) are in conflict or redundant.
    auto troubled = [&](const sketch::SketchResult& r, const std::string& except) {
        // "Did not converge" without groups is not a fix: the problem merely became invisible (core-api §4.6).
        if (r.status == sketch::SketchStatus::NotConverged) return static_cast<int>(added.size()) + 1;
        std::set<std::string> bad(r.redundant.begin(), r.redundant.end());
        for (const auto& g : r.conflicts) bad.insert(g.ids.begin(), g.ids.end());
        int n = 0, all = 0;
        for (const auto& a : added) n += a != except && bad.count(a), all += a != except;
        // A conflict whose groups exclude the new constraints (group found from degenerate configuration, e.g. tangency
        // with an endpoint on a circle) is still caused by them: the sketch solved before they were set (part 05).
        if (r.status == sketch::SketchStatus::Conflict && n == 0) return all;
        return n;
    };
    const int before = troubled(cur, {});
    if (before == 0) return std::string();
    // Grounded points: origin, projections, fix and everything coincident with them (core-api §4.6).
    std::set<std::string> grounded{"origin"};
    for (const auto& e : base.entities)
        if (e.type == sketch::EntityType::Projected) grounded.insert(e.id);
    for (const auto& c : base.constraints)
        if (c.type == "fix") grounded.insert(c.refs.begin(), c.refs.end());
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& c : base.constraints)
            if (c.type == "coincident" && c.refs.size() == 2 && grounded.count(c.refs[0]) != grounded.count(c.refs[1])) {
                grounded.insert(c.refs.begin(), c.refs.end());
                grew = true;
            }
    }
    // Class: alignments (with support — later), directions, others, incidence — last (core-api §4.6).
    auto rank = [&](const sketch::Constraint& c) {
        // An auto equality is only an equality snap of a drawn size (FX7-SK-003) — the weakest hint, it yields first;
        // an equality of radii (arcs, circles) yields after the point alignments: in a rounded contour the alignments
        // of its ends are implied by the equal radii (FX10-SK-004).
        if (c.type == "equal") {
            const auto* e = c.refs.empty() ? nullptr : base.findEntity(c.refs[0]);
            const bool radii = e && (e->type == sketch::EntityType::Arc || e->type == sketch::EntityType::Circle);
            return radii ? 2 : -1;
        }
        if ((c.type == "horizontal" || c.type == "vertical") && c.refs.size() == 2)
            return grounded.count(c.refs[0]) || grounded.count(c.refs[1]) ? 1 : 0;
        if (c.type == "horizontal" || c.type == "vertical" || c.type == "parallel" || c.type == "perpendicular" || c.type == "tangent") return 3;
        if (c.type == "coincident" || c.type == "pointOnObject") return 5;
        return 4;
    };
    // Probe: driving dimensions among added are slightly changed — the score distinguishes candidates even for a
    // redundant dimension whose value is a measurement (core-api §4.6, YIELD_PROBE).
    sketch::Values probe = values;
    bool probed = false;
    for (const auto& a : added)
        if (auto it = probe.find(a); it != probe.end() && it->second) it->second = *it->second + 1e-3, probed = true;
    // Candidates — auto-constraints without which the new ones are not conflicting or redundant: class, then offset.
    struct Candidate {
        std::string id;
        int rank;
        double score;
    };
    std::vector<Candidate> cands;
    // Offset of the solution from the data coordinates: points and circle radii.
    auto displacement = [](const sketch::SketchData& d, const sketch::SketchResult& r) {
        double s = 0;
        for (const auto& e : d.entities) {
            if (e.type == sketch::EntityType::Point)
                if (auto it = r.state.points.find(e.id); it != r.state.points.end()) s += (it->second - Point2(e.x, e.y)).norm();
            if (e.type == sketch::EntityType::Circle)
                if (auto it = r.state.radii.find(e.id); it != r.state.radii.end()) s += std::abs(it->second - e.r);
        }
        return s;
    };
    for (const auto& c : base.constraints) {
        if (!c.automatic) continue;
        sketch::SketchData trial = base;
        std::erase_if(trial.constraints, [&](const sketch::Constraint& x) { return x.id == c.id; });
        const sketch::SketchResult r = sketch::solve(trial, values);
        if (troubled(r, c.id) >= before) continue;
        // A snap (incidence) is not removed if without it the point moves away from the target: only a snap implied
        // by the other constraints can be removed (vision item 16, core-api §4.6).
        // The check uses probe dimension values: a just-set dimension whose value is a measurement
        // coincides with the snap only by chance and does not imply it.
        if (isIncidence(c.type)) {
            const sketch::SketchResult ri = probed ? sketch::solve(trial, probe) : r;
            if (!writesCoordinates(ri.status)) continue;
            sketch::SketchData back = trial;
            for (auto& e : back.entities) {
                if (e.type == sketch::EntityType::Point)
                    if (auto it = ri.state.points.find(e.id); it != ri.state.points.end()) e.x = it->second.x(), e.y = it->second.y();
                if (e.type == sketch::EntityType::Circle)
                    if (auto it = ri.state.radii.find(e.id); it != ri.state.radii.end()) e.r = it->second;
            }
            back.constraints.push_back(c);
            const sketch::SketchResult rb = sketch::solve(back, probed ? probe : values);
            if (!writesCoordinates(rb.status) || displacement(back, rb) > YIELD_INCIDENCE_TOL) continue;
        }
        const sketch::SketchResult rp = probed ? sketch::solve(trial, probe) : r;
        double score = std::numeric_limits<double>::infinity();
        if (writesCoordinates(rp.status)) {
            score = 0;
            for (const auto& e : base.entities) {
                if (e.type == sketch::EntityType::Point)
                    if (auto it = rp.state.points.find(e.id); it != rp.state.points.end()) score += (it->second - Point2(e.x, e.y)).norm();
                if (e.type == sketch::EntityType::Circle)
                    if (auto it = rp.state.radii.find(e.id); it != rp.state.radii.end()) score += std::abs(it->second - e.r);
            }
        }
        cands.push_back({c.id, rank(c), score});
    }
    if (cands.empty()) return std::string();
    const Candidate* v = nullptr;
    for (const auto& c : cands)
        if (!v || c.rank < v->rank ||
            (c.rank == v->rank && (c.score < v->score - 1e-9 || (std::abs(c.score - v->score) <= 1e-9 && compareIds(v->id, c.id) < 0))))
            v = &c;
    const std::string victim = v->id;
    // Removed ones are kept in set dimensions (yielded): deleting one restores them (FX4-SK-003, core-api §4.6.2).
    sketch::SketchData sd = *f.sketchData();
    sketch::Constraint snap = *sd.findConstraint(victim);
    snap.yielded.clear();
    for (auto& c : sd.constraints)
        if (sketch::isDimensionType(c.type) && std::find(added.begin(), added.end(), c.id) != added.end()) c.yielded.push_back(snap);
    auto del = sketchApplyDelete(std::move(sd), {victim});
    if (!del) return del.error();
    auto nd = withSketch(d, *ctx_, *del);
    if (!nd) return nd.error();
    if (auto s = commit(std::move(*nd), "undo.sketchDelete", {*ctx_}, *ctx_); !s) return s.error();
    return victim;
}

Result<TrimResult> Session::sketchTrim(const std::string& curve, const Point2& pick) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s.error();
    doc::Document nd = doc();
    const sketch::SketchData sd0 = *nd.active().find(*ctx_)->sketchData();
    const auto* e = sd0.findEntity(curve);
    if (!e) return notFound(curve);
    if (e->type != sketch::EntityType::Line && e->type != sketch::EntityType::Circle && e->type != sketch::EntityType::Arc &&
        !sketch::isEllipseType(e->type) && !sketch::isSplineType(e->type))  // ellipses — S4-SKT-007, splines — S4A-SPL-003
        return invalidItems(curve, "not a curve");
    // An offset contour is unlinked first (sketch.md §2.6.2, S7F-SK-005); members of other groups change only through them.
    sketch::SketchData sd = sd0;
    if (const auto* g = sd.groupOfMember(curve); g && g->type == "offset") {
        const std::string gid = g->id;
        sd = sketch::removeGroup(std::move(sd), gid, true);
    }
    if (sd.groupOfMember(curve)) return invalidItems(curve, "group");  // member of a linked group (sketch.md §2.6.2)
    const Allocate alloc = [&](bool entity) { return doc::allocate(nd, entity ? doc::IdKind::Entity : doc::IdKind::Constraint); };
    auto t = sketch::trim(sd, curve, pick, alloc);
    if (!t) return t.error();
    if (auto wp = withProjections(nd, *nd.active().find(*ctx_), t->sketch)) t->sketch = std::move(*wp);  // offsets of projections
    t->sketch = reconcileGroups(std::move(t->sketch), regen::documentUnits(nd), regen::documentVariables(nd).env(), alloc);
    auto r = withSketch(nd, *ctx_, t->sketch);
    if (!r) return r.error();
    if (auto c = commit(std::move(*r), "undo.sketchTrim", {*ctx_}, *ctx_); !c) return c.error();
    return TrimResult{t->removedConstraints, t->created};
}

Status Session::sketchDragBegin(const std::string& entityId, const Point2& grabPoint) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s;
    const auto* sd = doc().active().find(*ctx_)->sketchData();
    if (entityId != "origin" && !sd->findEntity(entityId)) return notFound(entityId);
    auto st = sketchState(*ctx_);
    if (!st) return st.error();
    auto plan = sketch::dragBegin(*sd, st->result.state, entityId, grabPoint);
    if (!plan) return plan.error();
    sketch::SketchData data = *sd;
    data.projections = st->projections;
    drag_ = Drag{std::move(data), *plan, st->result.state, st->result.reference};
    return okStatus();
}

Result<SketchState> Session::sketchDragUpdate(const Point2& targetPoint) {
    if (!drag_) return apiError("API_NO_DRAG");
    const doc::Document& d = doc();
    const auto* sd = d.active().find(*ctx_)->sketchData();
    const int idx = d.active().indexOf(*ctx_);
    const auto values = regen::dimensionValues(*sd, regen::documentVariables(d), regen::documentUnits(d),
                                               "/elements/" + std::to_string(d.activeIndex()) + "/features/" + std::to_string(idx));
    const sketch::SketchDragResult r = sketch::drag(drag_->data, values, drag_->state, drag_->plan, targetPoint);
    if (r.status != sketch::SketchDragResult::Status::Failed) {
        drag_->state = r.state;
        drag_->reference = r.reference;
    }
    auto st = sketchState(*ctx_);
    if (!st) return st.error();
    SketchState out = *st;
    out.result.state = drag_->state;
    out.result.reference = drag_->reference;
    for (const auto& [id, v] : sketch::measureWeak(drag_->data, drag_->state, out.weak)) out.result.reference[id] = v;  // §4.4 item 4
    out.result.profiles = sketch::regions(drag_->data, drag_->state);
    return out;
}

Status Session::sketchDragEnd(bool commitDrag) {
    if (!drag_) return apiError("API_NO_DRAG");
    Drag dr = *drag_;
    drag_.reset();
    if (!commitDrag) return okStatus();
    doc::Document nd = doc();
    doc::Feature nf = *nd.active().find(*ctx_);
    nf.data = sketch::applySolution(*nf.sketchData(), dr.state);
    return commit(replaceFeature(nd, nf), "undo.sketchDragEnd", {*ctx_}, *ctx_);
}

Result<Session::DuplicateMerge> Session::sketchMergeDuplicate(const std::string& curve) {
    if (!ctx_) return apiError("API_NO_SKETCH_CONTEXT");
    if (auto s = checkNoDrag(); !s) return s.error();
    const doc::Document& d = doc();
    const int idx = d.active().indexOf(*ctx_);
    const doc::Feature& f = *d.active().features[idx];
    const sketch::SketchData& own = *f.sketchData();
    const auto* c = own.findEntity(curve);
    if (!c) return notFound(curve);
    const std::string path = "/elements/" + std::to_string(d.activeIndex()) + "/features/" + std::to_string(idx);
    const auto vars = regen::documentVariables(d);
    const auto units = regen::documentUnits(d);
    auto solveData = [&](const sketch::SketchData& x) {
        const sketch::SketchData full = withProjections(d, f, x).value_or(x);
        return sketch::solve(full, regen::dimensionValues(full, vars, units, path));
    };
    const sketch::SketchResult cur = solveData(own);
    DuplicateMerge out;
    // The duplicate: a curve of the same type on exactly the same path; at least one of the two is construction.
    const double tol = DUPLICATE_TOL * std::max(1.0, curveSize(*c, cur.state));
    const sketch::Entity* other = nullptr;
    std::map<std::string, std::string> points;  // removed point → kept point
    for (const auto& e : own.entities) {
        if (e.id == c->id || (!e.construction && !c->construction)) continue;
        if (auto m = duplicatePoints(*c, e, cur.state, tol)) {
            other = &e, points = *m;
            break;
        }
    }
    if (!other) return out;
    // The extra one is the construction one; of two construction curves — curve itself.
    const bool removeCurve = c->construction;
    const sketch::Entity& removed = removeCurve ? *c : *other;
    const sketch::Entity& kept = removeCurve ? *other : *c;
    if (!removeCurve) {  // point map from the other side
        std::map<std::string, std::string> inv;
        for (const auto& [a, b] : points) inv[b] = a;
        points = inv;
    }
    auto refuse = [&] {
        out.refused = true;
        return out;
    };
    // Members and sources of linked groups change only through the group (sketch.md §2.6.2).
    for (const auto& g : own.groups) {
        if (g.hasMember(removed.id) || std::find(g.sources.begin(), g.sources.end(), removed.id) != g.sources.end()) return refuse();
        for (const auto& [p, q] : points)
            if (g.hasMember(p)) return refuse();
    }
    std::map<std::string, std::string> map = points;
    map[removed.id] = kept.id;
    const std::set<std::string> keptPoints = [&] {
        std::set<std::string> v;
        for (const auto& p : sketch::ownedPoints(kept)) v.insert(p);
        return v;
    }();
    sketch::SketchData nd = own;
    std::erase_if(nd.entities, [&](const sketch::Entity& e) { return e.id == removed.id || points.count(e.id); });
    // Constraints of the kept geometry stay; those of the removed curve and its points move onto the kept ones.
    std::vector<sketch::Constraint> result, touchedOnes;
    for (const auto& k : own.constraints) {
        sketch::Constraint m = k;
        bool touched = false;
        for (auto& r : m.refs)
            if (const auto it = map.find(r); it != map.end()) r = it->second, touched = true;
        (touched ? touchedOnes : result).push_back(m);
    }
    for (const auto& m : touchedOnes) {
        // Constraints between the two copies are satisfied by the merge itself (coincident ends, equal, parallel …).
        const std::set<std::string> distinctRefs(m.refs.begin(), m.refs.end());
        if (distinctRefs.size() < m.refs.size()) {
            static const std::set<std::string> tautologies = {"coincident", "equal", "parallel", "collinear", "concentric"};
            if (tautologies.count(m.type)) continue;
            return refuse();
        }
        // An end of the kept curve on the kept curve itself: pointOnObject of an own point is satisfied.
        if (m.type == "pointOnObject" && m.refs.size() == 2 && m.refs[1] == kept.id && keptPoints.count(m.refs[0])) continue;
        // The same constraint already exists (a dimension — with the same value): the kept one holds it.
        const bool same = std::any_of(result.begin(), result.end(), [&](const sketch::Constraint& x) {
            return sameConstraint(x, m) && (!sketch::isDimensionType(m.type) || (x.value == m.value && x.driving == m.driving));
        });
        if (!same) result.push_back(m);
    }
    nd.constraints = result;
    std::erase_if(nd.weakAnchors, [&](const std::string& a) { return points.count(a) > 0; });
    for (auto& k : nd.constraints) {
        auto refs = sketch::normalizeRefs(nd, k);
        if (!refs) return refuse();
        k.refs = *refs;
    }
    // Without changing the drawing: the same status class, no new redundancy or conflict, every remaining point,
    // radius and ellipse where it was.
    const sketch::SketchResult r = solveData(nd);
    auto troubled = [](const sketch::SketchResult& x) {
        return x.status == sketch::SketchStatus::Conflict || x.status == sketch::SketchStatus::NotConverged || x.status == sketch::SketchStatus::Error;
    };
    if (troubled(r) && !troubled(cur)) return refuse();
    if (r.redundant.size() > cur.redundant.size() || !r.errors.empty()) return refuse();
    const double ptol = DUPLICATE_TOL * 10 * std::max(1.0, curveSize(kept, cur.state));
    for (const auto& [id, p] : r.state.points)
        if (const auto it = cur.state.points.find(id); it != cur.state.points.end() && (it->second - p).norm() > ptol) return refuse();
    for (const auto& [id, rr] : r.state.radii)
        if (const auto it = cur.state.radii.find(id); it != cur.state.radii.end() && std::abs(it->second - rr) > ptol) return refuse();
    // Dimension values stay as they were: every moved driving dimension still measures its value.
    // commit() pushes a new undo snapshot and may reallocate the history: references into the current document
    // (d, own, removed, kept) are not valid after it, so the ids are copied first.
    out.removed = removed.id, out.kept = kept.id;
    auto w = withSketch(d, *ctx_, nd);
    if (!w) return w.error();
    if (auto s = commit(std::move(*w), "undo.sketchMerge", {*ctx_}, *ctx_); !s) {
        out.removed.clear(), out.kept.clear();
        return s.error();
    }
    return out;
}

}  // namespace construe::api
