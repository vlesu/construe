// Sketcher: offset as a linked group (spec/app-ux.md §6.3 «Offset», «Offset edit»; sketch.md §2.6.5;
// S2-SK-009, FX7-SK-007, S7F-SK-005).
#include "app/sketch_editor.h"

#include "api/sketch_ops.h"
#include "base/geom2.h"
#include "app/app_model.h"
#include "app/expr_input.h"
#include "app/messages.h"
#include "app/sketch_picker.h"
#include "app/theme.h"
#include "expr/expr.h"
#include "sketch/offset.h"

#include <QMenu>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace construe::app {

namespace {

// Geometry of an offset source: a line, an arc (counterclockwise a → b) or a circle (sketch.md §2.6.5).
struct CurveGeom {
    enum Kind { Line, Arc, Circle } kind = Line;
    Point2 a = Point2::Zero(), b = Point2::Zero(), c = Point2::Zero();
    double r = 0;
};

std::optional<CurveGeom> curveGeom(const sketch::SketchData& d, const std::string& id) {
    const auto* e = d.findEntity(id);
    if (!e) return std::nullopt;
    auto P = [&](const std::string& p) {
        const auto* q = d.findEntity(p);
        return q ? Point2(q->x, q->y) : Point2(0, 0);
    };
    CurveGeom g;
    if (e->type == sketch::EntityType::Line) {
        g.kind = CurveGeom::Line, g.a = P(e->p1), g.b = P(e->p2);
    } else if (e->type == sketch::EntityType::Arc) {
        g.kind = CurveGeom::Arc, g.c = P(e->center), g.a = P(e->start), g.b = P(e->end), g.r = (g.a - g.c).norm();
    } else if (e->type == sketch::EntityType::Circle) {
        g.kind = CurveGeom::Circle, g.c = P(e->center), g.r = e->r;
    } else if (e->type == sketch::EntityType::Projected) {
        const auto it = d.projections.find(id);
        if (it == d.projections.end()) return std::nullopt;
        const auto& p = it->second;
        if (p.kind == sketch::Projection::Kind::Line) g.kind = CurveGeom::Line, g.a = Point2(p.x1, p.y1), g.b = Point2(p.x2, p.y2);
        else if (p.kind == sketch::Projection::Kind::Arc)
            g.kind = CurveGeom::Arc, g.c = Point2(p.cx, p.cy), g.r = p.r, g.a = Point2(p.sx, p.sy), g.b = Point2(p.ex, p.ey);
        else if (p.kind == sketch::Projection::Kind::Circle) g.kind = CurveGeom::Circle, g.c = Point2(p.cx, p.cy), g.r = p.r;
        else return std::nullopt;
    } else {
        return std::nullopt;
    }
    return g;
}

// Polyline of a curve geometry in sketch coordinates.
std::vector<Point2> polyline(const CurveGeom& g) {
    if (g.kind == CurveGeom::Line) return {g.a, g.b};
    double a0 = 0, sw = 2 * std::numbers::pi;
    if (g.kind == CurveGeom::Arc) {
        a0 = std::atan2((g.a - g.c).y(), (g.a - g.c).x());
        sw = geom2::ccwSweepPositive(a0, std::atan2((g.b - g.c).y(), (g.b - g.c).x()));
    }
    std::vector<Point2> v;
    for (int i = 0; i <= 64; ++i) v.push_back(g.c + g.r * Point2(std::cos(a0 + sw * i / 64), std::sin(a0 + sw * i / 64)));
    return v;
}

}  // namespace

std::string SketchEditor::offsetCurveAt(const QPointF& s) const {
    const Hit h = hitAt(s);
    const sketch::SketchData d = dataWithProjections();
    const auto* e = h.kind == Hit::Kind::Curve ? d.findEntity(h.id) : nullptr;
    if (!e || !curveGeom(d, e->id)) return {};
    // A construction curve under a real one (sides of a fillet lie under the sides): the real one is meant.
    if (e->construction) {
        const auto g = geo();
        for (const auto& c : d.entities)
            if (!c.construction && c.type != sketch::EntityType::Projected && curveGeom(d, c.id) && g.distToEntity(c, s) <= PICK_CURVE_PX)
                return c.id;
    }
    return e->id;
}

void SketchEditor::startOffset() {
    const sketch::SketchData d = dataWithProjections();
    std::vector<std::string> sel;
    for (const auto& id : selection_)
        if (curveGeom(d, id)) sel.push_back(id);
    setTool(Tool::Offset);
    pending_.clear();
    offsetSide_ = 1;
    if (sel.empty()) {  // a click on an element: «Line» / «Contour»; on a body edge — its contour (S7F-SK-005)
        emit hint(text("ui.offset.pick"));
        return;
    }
    pending_ = sel;
    for (const auto& id : sel) selection_.insert(id);  // the selection stays highlighted
    emit hint(text("ui.offset.side"));
    view_.update();
}

void SketchEditor::startOffsetEdit(const std::string& groupId, const QPointF& at) {
    const auto* g = data() ? data()->findGroup(groupId) : nullptr;
    if (!g || g->type != "offset") return;
    const std::vector<std::string> sources = g->sources;
    const auto it = g->params.find("distance");
    const QString value = it == g->params.end() ? QString() : QString::fromStdString(it->second.text);
    setTool(Tool::Offset);
    offsetEditing_ = groupId;
    pending_ = sources;
    selection_.clear();
    for (const auto& id : sources) selection_.insert(id);
    offsetContour_ = true;
    offsetSide_ = 1;
    openOffsetValue(at, value, "ui.offset.edit");
}

void SketchEditor::openOffsetValue(const QPointF& s, const QString& value, const char* hintKey) {
    if (const auto p = toLocal(s)) clicks_ = {*p};
    cursor_ = s;
    openDrawInput(value);
    drawInput_->selectAll();
    connect(drawInput_, &QLineEdit::textChanged, &view_, static_cast<void (QWidget::*)()>(&QWidget::update), Qt::UniqueConnection);
    emit hint(text(hintKey));
    view_.update();
}

void SketchEditor::offsetChoice(const std::string& curve, const QPointF& s) {
    const auto contour = api::sketchops::contourOf(dataWithProjections(), curve);
    QMenu menu(&view_);
    menu.setObjectName("offsetMenu");
    QAction* seg = menu.addAction(txt("ui.common.line2"));
    seg->setObjectName("offsetSegment");
    QAction* con = menu.addAction(txt("ui.sketch.contour"));
    con->setObjectName("offsetContour");
    con->setEnabled(!contour.empty());
    if (contour.empty()) con->setToolTip(text("ui.offset.notClosed"));
    QAction* chosen = menu.exec(view_.mapToGlobal(s.toPoint()));
    if (chosen == seg) {
        pending_ = {curve};
        selection_ = {curve};
        emit hint(text("ui.offset.side"));
    } else if (chosen == con) {
        // The whole closed contour: the value field at once — positive outward, negative inward (sketch.md §2.6.5).
        pending_ = contour;
        selection_.clear();
        for (const auto& id : contour) selection_.insert(id);
        offsetContour_ = true;
        offsetSide_ = 1;
        const auto u = model_.session().units();
        openOffsetValue(s, formatNumber(2 / expr::lengthFactor(u.length)), "ui.offset.contourValue");  // 2 mm
    }
    view_.update();
}

void SketchEditor::setOffsetEdges(std::vector<std::string> edges) {
    offsetEdges_ = std::move(edges);
    offsetEdgeGeometry_.clear();
    if (offsetEdges_.empty()) return;
    if (auto g = model_.session().projectionPreview(feature_, offsetEdges_))
        for (size_t i = 0; i < offsetEdges_.size() && i < g->size(); ++i)
            if ((*g)[i]) offsetEdgeGeometry_[offsetEdges_[i]] = *(*g)[i];
}

void SketchEditor::clickOffset(const QPointF& s) {
    const auto p = toLocal(s);
    if (!data() || !p) return;
    if (offsetContour_) {  // the value field is open: Ctrl+click adds / removes a contour element (S7F-SK-005)
        if (ctrlHeld_) toggleOffsetSource(s);
        if (drawInput_) drawInput_->setFocus();
        return;
    }
    if (pending_.empty()) {
        if (const std::string curve = offsetCurveAt(s); !curve.empty()) {
            offsetChoice(curve, s);
            return;
        }
        // A body edge: the whole visible contour is projected and offset (S7F-SK-005).
        const PickTarget t = view_.pickAt(s.toPoint(), false, true, false);
        if (t.kind == PickTarget::Kind::Edge) {
            auto c = model_.session().sketchEdgeContour(feature_, t.name);
            if (!c || c->empty()) {
                emit hint(text("ui.offset.notClosed"));
                return;
            }
            // Edges already projected into the sketch are used as they are.
            std::vector<std::string> edges;
            for (const auto& name : *c) {
                std::string proj;
                for (const auto& e : data()->entities)
                    if (e.type == sketch::EntityType::Projected && e.source == name) proj = e.id;
                if (proj.empty()) edges.push_back(name);
                else pending_.push_back(proj);
            }
            setOffsetEdges(edges);
            selection_.clear();
            for (const auto& id : pending_) selection_.insert(id);
            offsetContour_ = true;
            offsetSide_ = 1;
            const auto u = model_.session().units();
            openOffsetValue(s, formatNumber(2 / expr::lengthFactor(u.length)), "ui.offset.contourValue");
            return;
        }
        emit hint(text("ui.offset.pick"));
        return;
    }
    // Side — by the source nearest to the click (relative to its own direction), the sign — for the whole set.
    const sketch::SketchData d = dataWithProjections();
    std::string best;
    double bestD = 1e300, dist = 0;
    int side = 1;
    for (const auto& id : pending_) {
        const auto g = curveGeom(d, id);
        if (!g) continue;
        double dd = 0, signedD = 0;
        if (g->kind == CurveGeom::Line) {
            const Point2 ab = g->b - g->a, u = ab.normalized(), w = *p - g->a;
            const double t = std::clamp(w.dot(ab) / std::max(ab.squaredNorm(), 1e-300), 0.0, 1.0);
            dd = (*p - (g->a + ab * t)).norm();
            signedD = u.x() * w.y() - u.y() * w.x();  // > 0 — left of the direction
        } else {
            signedD = g->r - (*p - g->c).norm();  // > 0 — towards the center: "left" of an arc / a circle
            dd = std::abs(signedD);
        }
        if (dd < bestD) bestD = dd, best = id, side = signedD >= 0 ? 1 : -1, dist = std::abs(signedD);
    }
    if (best.empty()) return;
    offsetSide_ = api::sketchops::offsetSign(d, pending_, best, side);
    if (offsetSide_ == 0) {
        emit hint(text("ui.offset.pick"));
        return;
    }
    const auto u = model_.session().units();
    openOffsetValue(s, formatNumber(std::max(dist, 1e-3) / expr::lengthFactor(u.length)), "ui.offset.side");
}

void SketchEditor::toggleOffsetSource(const QPointF& s) {
    const auto* d = data();
    const auto* edited = d && !offsetEditing_.empty() ? d->findGroup(offsetEditing_) : nullptr;
    auto toggle = [&](const std::string& id) {
        if (edited && edited->hasMember(id)) return;  // the offset contour itself is not its source
        const auto it = std::find(pending_.begin(), pending_.end(), id);
        if (it == pending_.end()) {
            pending_.push_back(id);
            selection_.insert(id);
        } else if (pending_.size() + offsetEdges_.size() > 1) {  // the last source stays
            pending_.erase(it);
            selection_.erase(id);
        }
    };
    // The curve under the cursor as it is (a construction source under its offset is meant).
    const Hit h = hitAt(s);
    if (const sketch::SketchData dd = dataWithProjections(); h.kind == Hit::Kind::Curve && curveGeom(dd, h.id)) {
        toggle(h.id);
    } else if (const PickTarget t = view_.pickAt(s.toPoint(), false, true, false); t.kind == PickTarget::Kind::Edge) {
        std::string proj;
        for (const auto& e : d->entities)
            if (e.type == sketch::EntityType::Projected && e.source == t.name) proj = e.id;
        if (!proj.empty()) {
            toggle(proj);
        } else {
            std::vector<std::string> edges = offsetEdges_;
            const auto it = std::find(edges.begin(), edges.end(), t.name);
            if (it == edges.end()) edges.push_back(t.name);
            else if (pending_.size() + edges.size() > 1) edges.erase(it);
            setOffsetEdges(edges);
        }
    }
    view_.update();
}

sketch::SketchData SketchEditor::offsetData(std::vector<std::string>& sources) const {
    sketch::SketchData sd = dataWithProjections();
    sources = pending_;
    int k = 0;
    for (const auto& name : offsetEdges_) {
        const auto it = offsetEdgeGeometry_.find(name);
        if (it == offsetEdgeGeometry_.end()) continue;
        sketch::Entity e;
        e.id = "~offset" + std::to_string(++k);
        e.type = sketch::EntityType::Projected;
        e.source = name;
        e.construction = true;
        sd.entities.push_back(e);
        sd.projections[e.id] = it->second;
        sources.push_back(e.id);
    }
    return sd;
}

namespace {
// The signed distance text: the entered text with the side multiplier (−1 — the opposite side).
std::string signedText(const std::string& typed, double value, int side, api::Session& s) {
    if (side > 0) return typed;
    std::string t = typed;
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    if (!t.empty() && t.front() == '-')
        if (auto q = s.checkExpression(t.substr(1), expr::LENGTH); q && std::abs(q->value + value) <= 1e-9 * std::max(1.0, std::abs(value)))
            return t.substr(1);
    return "-(" + typed + ")";
}
}  // namespace

std::vector<std::vector<QPointF>> SketchEditor::offsetPreview(std::vector<std::vector<QPointF>>& edges) const {
    std::vector<std::vector<QPointF>> out;
    auto screen = [&](const std::vector<Point2>& v) {
        std::vector<QPointF> pl;
        for (const auto& q : v) pl.push_back(toScreen(q));
        return pl;
    };
    std::vector<std::string> sources;
    const sketch::SketchData sd = offsetData(sources);
    for (size_t i = pending_.size(); i < sources.size(); ++i)
        if (const auto g = curveGeom(sd, sources[i])) edges.push_back(screen(polyline(*g)));
    if (!drawInput_ || !drawInput_->isVisible() || sources.empty()) return out;
    auto& s = model_.session();
    const std::string typed = normalizeExprInput(drawInput_->text());
    const auto q = s.checkExpression(typed, expr::LENGTH);
    if (!q) return out;
    auto items = api::sketchops::groupItems(sd, "offset", sources, doc::Json{{"distance", signedText(typed, q->value, offsetSide_, s)}},
                                            s.units(), s.variableValues().env());
    if (!items) return out;
    std::map<std::string, Point2> pts;  // "@role" → position
    for (const auto& it : *items)
        if (it.entity && it.entity->type == sketch::EntityType::Point) pts["@" + it.role] = Point2(it.entity->x, it.entity->y);
    auto P = [&](const std::string& ref) { return pts.count(ref) ? pts.at(ref) : Point2(0, 0); };
    for (const auto& it : *items) {
        if (!it.entity) continue;
        const auto& e = *it.entity;
        CurveGeom g;
        if (e.type == sketch::EntityType::Line) g.kind = CurveGeom::Line, g.a = P(e.p1), g.b = P(e.p2);
        else if (e.type == sketch::EntityType::Arc) g.kind = CurveGeom::Arc, g.c = P(e.center), g.a = P(e.start), g.b = P(e.end), g.r = (g.a - g.c).norm();
        else if (e.type == sketch::EntityType::Circle) g.kind = CurveGeom::Circle, g.c = P(e.center), g.r = e.r;
        else continue;
        out.push_back(screen(polyline(g)));
    }
    return out;
}

void SketchEditor::applyOffsetInput() {
    auto& s = model_.session();
    const std::string typed = normalizeExprInput(drawInput_->text());
    const auto q = s.checkExpression(typed, expr::LENGTH);
    if (!q || !(std::abs(q->value) > sketch::OFFSET_MIN)) {
        emit hint(q ? text("ui.status.tooSmall") : errorText(q.error()));
        return;
    }
    const doc::Json params = {{"distance", signedText(typed, q->value, offsetSide_, s)}};
    // Checked before any change: a vanishing radius keeps the field open (no projections are created either).
    std::vector<std::string> trial;
    const sketch::SketchData sd = offsetData(trial);
    if (auto items = api::sketchops::groupItems(sd, "offset", trial, params, s.units(), s.variableValues().env()); !items) {
        emit error(items.error().code == "API_INVALID_ARGUMENT" ? text("ui.offset.impossible") : errorText(items.error(), s.document().document.get()));
        return;
    }
    drawInput_->hide();
    drawInput_->deleteLater();
    s.beginGroup("undo.group.offset");
    std::vector<std::string> sources = pending_;
    Status st = okStatus();
    if (!offsetEdges_.empty()) {  // projections of the body edges (auxiliary, as by «Project»)
        doc::Json items = doc::Json::array();
        for (size_t i = 0; i < offsetEdges_.size(); ++i)
            items.push_back({{"id", "t" + std::to_string(i + 1)}, {"type", "projected"}, {"source", offsetEdges_[i]}, {"construction", true}});
        if (auto r = s.sketchAdd(items)) {
            for (size_t i = 0; i < offsetEdges_.size(); ++i)
                if (const auto it = r->find("t" + std::to_string(i + 1)); it != r->end()) sources.push_back(it->second);
        } else {
            st = r.error();
        }
    }
    if (st) {
        if (offsetEditing_.empty()) {
            if (auto r = s.sketchGroupCreate("offset", sources, params); !r) st = r.error();
        } else {
            st = s.sketchGroupUpdate(offsetEditing_, sources, params);
        }
    }
    s.endGroup();
    if (!st) emit error(st.error().code == "API_INVALID_ARGUMENT" ? text("ui.offset.impossible") : errorText(st.error(), s.document().document.get()));
    setTool(Tool::Select);
    view_.setFocus();
    refresh();
}

}  // namespace construe::app
