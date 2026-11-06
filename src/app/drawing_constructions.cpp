// Constructions on a drawing sheet (spec/app-ux.md §8б «Constructions», drawing.md §6.29, FX14-DRW-006).
#include "app/drawing_constructions.h"

#include "app/messages.h"
#include "app/sheet_print.h"
#include "app/theme.h"

#include "base/geom2.h"
#include "base/tolerance.h"
#include "drawing/dimensions.h"

#include <QFormLayout>
#include <QPainter>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <limits>

namespace construe::app {

namespace {

// Distance from a point to a line primitive (line, circle, arc), sheet mm.
double primDist(const drawing::Primitive& pr, const Point2& p) {
    using K = drawing::PrimitiveKind;
    switch (pr.kind) {
        case K::Line: return pr.points.size() == 2 ? geom2::distToSegment(p, pr.points[0], pr.points[1]) : 1e300;
        case K::Circle: return std::abs((p - pr.center).norm() - pr.radius);
        case K::Arc: {
            const Point2 v = p - pr.center;
            if (geom2::ccwSweep(pr.start, std::atan2(v.y(), v.x())) <= geom2::ccwSweepPositive(pr.start, pr.end)) return std::abs(v.norm() - pr.radius);
            const Point2 e0 = pr.center + pr.radius * Point2(std::cos(pr.start), std::sin(pr.start));
            const Point2 e1 = pr.center + pr.radius * Point2(std::cos(pr.end), std::sin(pr.end));
            return std::min((p - e0).norm(), (p - e1).norm());
        }
        default: return 1e300;
    }
}

void drawPrims(QPainter& p, const DrawingView& v, const std::vector<drawing::Primitive>& prims, const QColor& color) {
    const double z = v.zoom();
    for (const auto& pr : prims) {
        QPen pen(color);
        pen.setWidthF(std::max(1.5, drawing::LINE_THIN_MM * z));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        drawSheetGeometry(p, pr, z, [&](const Point2& q) { return v.sheetToWidget(q); });
    }
}

drawing::Primitive marker(const Point2& at, double r) {
    drawing::Primitive m;
    m.kind = drawing::PrimitiveKind::Circle;
    m.center = at;
    m.radius = r;
    return m;
}

// View entity as a highlight primitive.
std::vector<drawing::Primitive> entityPrims(const drawing::ViewEntity& e) {
    drawing::Primitive p;
    switch (e.kind) {
        case drawing::ViewEntity::Kind::Point: return {marker(e.a, 0.8)};
        case drawing::ViewEntity::Kind::Line:
            p.kind = drawing::PrimitiveKind::Line;
            p.points = {e.a, e.b};
            return {p};
        case drawing::ViewEntity::Kind::Circle:
            p.kind = drawing::PrimitiveKind::Circle;
            p.center = e.center, p.radius = e.radius;
            return {p};
        case drawing::ViewEntity::Kind::Arc:
            p.kind = drawing::PrimitiveKind::Arc;
            p.center = e.center, p.radius = e.radius, p.start = e.start, p.end = e.end;
            return {p};
        default: return {};
    }
}

const char* const LINE_TYPES[][2] = {{"solid", "ui.drawing.lineSolidThin"}, {"dashDot", "ui.drawing.lineDashDotThin"}, {"dashed", "ui.drawing.lineDashed"}};

api::Json pointJson(const doc::ConstructionPoint& p) {
    api::Json j{{"xy", api::Json::array({p.xy[0], p.xy[1]})}};
    if (!p.bind.empty()) {
        api::Json b{{p.bind, p.name}};
        if (p.at) b["at"] = *p.at;
        j["bind"] = b;
    }
    return j;
}

}  // namespace

ConstructionTool::ConstructionTool(DrawingView* view, Hooks hooks, QObject* parent) : QObject(parent), view_(view), h_(std::move(hooks)) {
    // «Construction» block of the drawing panel (app-ux §8б): the line type, the radius of a circle, the binding state.
    bar_ = new QWidget;
    bar_->setObjectName("consBar");
    auto* l = new QFormLayout(bar_);
    l->setContentsMargins(0, 6, 0, 0);
    line_ = new QComboBox;
    line_->setObjectName("consLineType");
    for (const auto& t : LINE_TYPES) line_->addItem(QString::fromUtf8(txt(t[1])), QString(t[0]));
    l->addRow(txt("ui.drawing.lineType"), line_);
    radius_ = new QLineEdit;
    radius_->setObjectName("consRadius");
    l->addRow(txt("ui.common.radius"), radius_);
    info_ = new QLabel;
    info_->setObjectName("consInfo");
    info_->setWordWrap(true);
    l->addRow(info_);
    bar_->hide();
    connect(line_, &QComboBox::activated, this, [this] {
        const auto d = document();
        const doc::SheetConstruction* c = d ? byId(*d, selected_) : nullptr;
        if (!c) return;
        const std::string v = line_->currentData().toString().toStdString();
        // The default of the kind — no field (drawing.md §6.29).
        const api::Json def{{"line", v == api::Session::defaultConstructionLine(c->kind) ? api::Json(nullptr) : api::Json(v)}};
        if (auto s = h_.session().updateConstruction(selected_, def); !s) h_.toast(errorText(s.error()), true);
    });
    connect(radius_, &QLineEdit::editingFinished, this, [this] {
        if (!radius_->isModified()) return;  // focus left without input (incl. on window close) — no session access
        radius_->setModified(false);
        const auto d = document();
        const doc::SheetConstruction* c = d ? byId(*d, selected_) : nullptr;
        if (!c || c->kind != "circle") return;
        bool ok = false;
        const double r = radius_->text().trimmed().replace(',', '.').toDouble(&ok);
        if (!ok || !(r > 0)) {
            refreshBar();
            return;
        }
        // A radius set by number: the circle is its center and the radius (the point on the circle is dropped).
        api::Json def{{"radius", r}, {"points", api::Json::array({pointJson(c->points.front())})}};
        if (auto s = h_.session().updateConstruction(selected_, def); !s) {
            h_.toast(errorText(s.error()), true);
            refreshBar();
        }
    });
}

std::shared_ptr<const doc::Document> ConstructionTool::document() const { return h_.session().document().document; }

const doc::Sheet* ConstructionTool::sheet(const doc::Document& d) const {
    if (!d.active().isDrawing()) return nullptr;
    const int si = d.active().sheetIndex(h_.currentSheet());
    return si < 0 ? nullptr : &d.active().sheets[size_t(si)];
}

const doc::SheetConstruction* ConstructionTool::byId(const doc::Document& d, const std::string& id) const {
    const doc::Sheet* sh = sheet(d);
    if (!sh || id.empty()) return nullptr;
    const int ci = sh->constructionIndex(id);
    return ci < 0 ? nullptr : &sh->constructions[size_t(ci)];
}

double ConstructionTool::tolMm(int px) const { return safeDiv(px, view_->zoom()); }

const std::vector<drawing::ViewEntity>& ConstructionTool::entitiesOf(const std::string& viewId) const {
    auto it = entities_.find(viewId);
    if (it == entities_.end()) {
        auto r = h_.session().viewEntities(h_.currentSheet(), viewId);
        it = entities_.emplace(viewId, r ? std::move(*r) : std::vector<drawing::ViewEntity>()).first;
    }
    return it->second;
}

const drawing::ViewPlacement* ConstructionTool::placementOf(const std::string& viewId) const {
    for (const auto& p : view_->graphics().placements)
        if (p.view == viewId) return &p;
    return nullptr;
}

std::optional<ConstructionTool::Snap> ConstructionTool::snapAt(const Point2& at) const {
    const auto d = document();
    const doc::Sheet* sh = d ? sheet(*d) : nullptr;
    if (!sh) return std::nullopt;
    // The view of the construction: the one of the first point (the others snap only to it).
    const std::string fixedView = points_.empty() ? std::string() : points_.front().view;
    auto toView = [&](const std::string& v, const Point2& q) -> std::optional<Point2> {
        const auto* pl = placementOf(v);
        if (!pl || !(pl->scale > 0)) return std::nullopt;
        return Point2((q - pl->offset) / pl->scale);
    };
    std::optional<Snap> best;
    double bestD = std::numeric_limits<double>::infinity();
    // Pass 1 — points (vertices, centers seen end-on) within PICK_POINT_PX; pass 2 — edges within PICK_CURVE_PX: a circle or
    // an arc — its center, a segment — its middle near it, otherwise the point under the cursor.
    for (int pass = 0; pass < 2 && !best; ++pass) {
        const double tol = tolMm(pass == 0 ? PICK_POINT_PX : PICK_CURVE_PX);
        for (const auto& v : sh->views) {
            if (!fixedView.empty() && v.id != fixedView) continue;
            for (const auto& e : entitiesOf(v.id)) {
                const bool point = e.kind == drawing::ViewEntity::Kind::Point;
                if (point != (pass == 0)) continue;
                if (point ? (e.ref != "vertex" && e.ref != "center") : e.ref != "edge") continue;
                double dist = 0;
                Snap s;
                s.view = v.id;
                s.highlight = entityPrims(e);
                if (point) {
                    dist = (at - e.a).norm();
                    s.point.bind = e.ref;
                    s.sheet = e.a;
                } else if (e.kind == drawing::ViewEntity::Kind::Circle || e.kind == drawing::ViewEntity::Kind::Arc) {
                    dist = s.highlight.empty() ? 1e300 : primDist(s.highlight.front(), at);
                    s.point.bind = "edge";
                    s.sheet = e.center;
                } else if (e.kind == drawing::ViewEntity::Kind::Line) {
                    dist = geom2::distToSegment(at, e.a, e.b);
                    s.point.bind = "edge";
                    const Point2 ab = e.b - e.a;
                    const double t = ab.squaredNorm() > 0 ? std::clamp((at - e.a).dot(ab) / ab.squaredNorm(), 0.0, 1.0) : 0.5;
                    const Point2 mid = (e.a + e.b) / 2;
                    if ((at - mid).norm() <= tolMm(PICK_POINT_PX)) {
                        s.sheet = mid;  // the middle: no parameter
                    } else {
                        s.point.at = t;
                        s.sheet = e.a + t * ab;
                    }
                    s.highlight.push_back(marker(s.sheet, 0.8));
                } else {
                    continue;  // curves — not bound
                }
                if (dist > tol || dist >= bestD) continue;
                const auto xy = toView(v.id, s.sheet);
                if (!xy) continue;
                s.point.name = e.name;
                s.point.xy = {xy->x(), xy->y()};
                best = std::move(s);
                bestD = dist;
            }
        }
    }
    if (best) return best;
    // A free point: on the view of the construction, otherwise on the view under the cursor.
    const std::string v = !fixedView.empty() ? fixedView : view_->viewAt(at);
    if (v.empty()) return std::nullopt;
    const auto xy = toView(v, at);
    if (!xy) return std::nullopt;
    Snap s;
    s.view = v;
    s.point.xy = {xy->x(), xy->y()};
    s.sheet = at;
    return s;
}

std::string ConstructionTool::snapKindAt(const Point2& at) const {
    const auto s = snapAt(at);
    if (!s) return {};
    return s->point.bind.empty() ? std::string("free") : s->point.bind;
}

size_t ConstructionTool::requiredPoints() const {
    if (kind_ == "line" || kind_ == "axis" || kind_ == "circle") return 2;
    if (kind_ == "arc") return 3;
    return 0;  // a pitch circle: three or more points, finished by Enter
}

doc::SheetConstruction ConstructionTool::candidate(const std::optional<Snap>& extra) const {
    doc::SheetConstruction c;
    c.id = "cs0";
    c.kind = kind_;
    c.view = points_.empty() ? (extra ? extra->view : std::string()) : points_.front().view;
    for (const auto& s : points_) c.points.push_back(s.point);
    if (extra) c.points.push_back(extra->point);
    return c;
}

void ConstructionTool::updatePreview() {
    preview_.clear();
    if (kind_.empty() || points_.empty()) return;
    const auto c = candidate(hover_);
    const size_t need = requiredPoints();
    if (c.points.size() < (need ? need : 3)) {
        // Not enough points for the shape yet: the segment from the first point to the cursor.
        if (hover_ && kind_ != "pitchCircle") {
            drawing::Primitive ln;
            ln.kind = drawing::PrimitiveKind::Line;
            ln.points = {points_.front().sheet, hover_->sheet};
            preview_.push_back(ln);
        }
        return;
    }
    if (auto r = h_.session().previewConstruction(h_.currentSheet(), c)) preview_ = std::move(r->primitives);
}

void ConstructionTool::prompt() {
    if (kind_.empty()) return;
    h_.status(text(kind_ == "pitchCircle" ? "ui.drawing.consPitchPrompt" : "ui.drawing.consPointPrompt"));
}

void ConstructionTool::setPlacing(const std::string& kind) {
    if (kind == kind_) return;
    kind_ = kind;
    points_.clear();
    hover_.reset();
    preview_.clear();
    entities_.clear();
    if (!kind.empty()) select({});
    if (kind.empty()) h_.status(QString());
    else prompt();
    view_->setCursor(kind.empty() ? Qt::ArrowCursor : Qt::CrossCursor);
    if (h_.placingChanged) h_.placingChanged(!kind.empty());
    view_->update();
}

void ConstructionTool::resetPoints() {
    points_.clear();
    preview_.clear();
    prompt();
    view_->update();
}

void ConstructionTool::select(const std::string& id) {
    if (id == selected_) return;
    selected_ = id;
    refreshBar();
    if (!id.empty() && h_.selected) h_.selected();
    view_->update();
}

void ConstructionTool::refresh() {
    entities_.clear();
    hover_.reset();
    const auto d = document();
    if (!selected_.empty() && (!d || !byId(*d, selected_))) selected_.clear();
    refreshBar();
    view_->update();
}

bool ConstructionTool::escape() {
    if (!kind_.empty()) {
        if (!points_.empty()) resetPoints();
        else setPlacing({});
        return true;
    }
    if (!selected_.empty()) {
        select({});
        return true;
    }
    return false;
}

std::string ConstructionTool::constructionAt(const Point2& at) const {
    const double tol = tolMm(PICK_CURVE_PX);
    const auto& prims = view_->graphics().primitives;
    for (auto it = prims.rbegin(); it != prims.rend(); ++it)
        if (it->role.rfind("cons:", 0) == 0 && primDist(*it, at) <= tol) return it->role.substr(5);
    return {};
}

void ConstructionTool::finish() {
    const auto c = candidate(std::nullopt);
    api::Json pts = api::Json::array();
    for (const auto& p : c.points) pts.push_back(pointJson(p));
    const api::Json def{{"kind", c.kind}, {"view", c.view}, {"points", pts}};
    auto r = h_.session().addConstruction(h_.currentSheet(), def);
    points_.clear();
    preview_.clear();
    if (!r) {
        h_.toast(errorText(r.error()), true);
        prompt();
        return;
    }
    prompt();  // the mode stays on for the next construction
    view_->update();
}

bool ConstructionTool::press(const Point2& at) {
    if (!kind_.empty()) return true;  // a point is taken on release
    if (h_.busy && h_.busy()) return false;  // dimension placement picks the construction itself
    // A free point of the selected construction: dragging it.
    if (!selected_.empty()) {
        const auto d = document();
        const doc::SheetConstruction* c = d ? byId(*d, selected_) : nullptr;
        for (size_t i = 0; c && i < selPoints_.size() && i < c->points.size(); ++i)
            if (c->points[i].bind.empty() && (selPoints_[i] - at).norm() <= tolMm(PICK_POINT_PX)) {
                pressed_ = true;
                dragging_ = false;
                dragPoint_ = int(i);
                pressAt_ = at;
                return true;
            }
    }
    const std::string id = constructionAt(at);
    if (id.empty()) return false;
    select(id);
    return true;
}

void ConstructionTool::move(const Point2& at, bool leftDown) {
    if (!kind_.empty()) {
        hover_ = snapAt(at);
        updatePreview();
        view_->update();
        return;
    }
    if (!leftDown || !pressed_) return;
    if (!dragging_ && (at - pressAt_).norm() * view_->zoom() > DRAG_THRESHOLD_PX) dragging_ = true;
    if (!dragging_) return;
    preview_.clear();
    const auto d = document();
    const doc::SheetConstruction* c = d ? byId(*d, selected_) : nullptr;
    const auto* pl = c ? placementOf(c->view) : nullptr;
    if (c && pl && pl->scale > 0) {
        doc::SheetConstruction m = *c;
        const Point2 xy = (at - pl->offset) / pl->scale;
        m.points[size_t(dragPoint_)].xy = {xy.x(), xy.y()};
        if (auto r = h_.session().previewConstruction(h_.currentSheet(), m)) preview_ = std::move(r->primitives);
    }
    view_->update();
}

void ConstructionTool::release(const Point2& at) {
    if (!kind_.empty()) {
        auto s = snapAt(at);
        if (!s) {
            h_.status(text("ui.drawing.consNoView"));
            return;
        }
        points_.push_back(std::move(*s));
        hover_.reset();
        if (const size_t need = requiredPoints(); need && points_.size() >= need) finish();
        else updatePreview();
        view_->update();
        return;
    }
    const bool dragged = pressed_ && dragging_;
    pressed_ = dragging_ = false;
    preview_.clear();
    view_->update();
    if (!dragged) {
        if (dragPoint_ >= 0) {  // a click on a free point without dragging — the construction stays selected
            dragPoint_ = -1;
        }
        return;
    }
    const auto d = document();
    const doc::SheetConstruction* c = d ? byId(*d, selected_) : nullptr;
    const auto* pl = c ? placementOf(c->view) : nullptr;
    const int i = std::exchange(dragPoint_, -1);
    if (!c || !pl || !(pl->scale > 0) || i < 0 || size_t(i) >= c->points.size()) return;
    const Point2 xy = (at - pl->offset) / pl->scale;
    api::Json pts = api::Json::array();
    for (size_t k = 0; k < c->points.size(); ++k) {
        doc::ConstructionPoint p = c->points[k];
        if (int(k) == i) p.xy = {xy.x(), xy.y()};
        pts.push_back(pointJson(p));
    }
    if (auto st = h_.session().updateConstruction(selected_, api::Json{{"points", pts}}); !st) h_.toast(errorText(st.error()), true);
}

bool ConstructionTool::key(int k) {
    if (!kind_.empty() && (k == Qt::Key_Return || k == Qt::Key_Enter)) {
        if (kind_ == "pitchCircle" && points_.size() >= 3) finish();
        return true;
    }
    if (k == Qt::Key_Delete && kind_.empty() && !selected_.empty()) {
        const std::string id = selected_;
        select({});
        if (auto s = h_.session().deleteConstruction(id); !s) h_.toast(errorText(s.error()), true);
        return true;
    }
    return false;
}

void ConstructionTool::paint(QPainter& p) {
    // Constructions with a binding error — in the error color over the sheet (drawing.md §6.29 «Errors»).
    std::vector<drawing::Primitive> broken, sel;
    for (const auto& e : view_->graphics().errors)
        if (e.view.rfind("cs", 0) == 0)
            for (const auto& pr : view_->graphics().primitives)
                if (pr.role == "cons:" + e.view) broken.push_back(pr);
    drawPrims(p, *view_, broken, COLOR_ERROR.color);
    if (!selected_.empty() && !dragging_) {
        for (const auto& pr : view_->graphics().primitives)
            if (pr.role == "cons:" + selected_) sel.push_back(pr);
        for (const auto& q : selPoints_) sel.push_back(marker(q, tolMm(3)));
        drawPrims(p, *view_, sel, COLOR_SELECTED.color);
    }
    for (const auto& s : points_) drawPrims(p, *view_, s.highlight.empty() ? std::vector{marker(s.sheet, tolMm(3))} : s.highlight, COLOR_SELECTED.color);
    if (hover_) drawPrims(p, *view_, hover_->highlight.empty() ? std::vector{marker(hover_->sheet, tolMm(3))} : hover_->highlight, COLOR_HOVER.color);
    drawPrims(p, *view_, preview_, COLOR_HOVER.color);
}

void ConstructionTool::refreshBar() {
    selPoints_.clear();
    const auto d = document();
    const doc::SheetConstruction* c = d ? byId(*d, selected_) : nullptr;
    bar_->setVisible(c != nullptr);
    if (!c) return;
    {
        const QSignalBlocker b(line_);
        const std::string l = c->line.empty() ? api::Session::defaultConstructionLine(c->kind) : c->line;
        line_->setCurrentIndex(std::max(0, line_->findData(QString::fromStdString(l))));
    }
    QString state;
    double radius = 0;
    if (auto r = h_.session().previewConstruction(h_.currentSheet(), *c)) {
        selPoints_ = r->points;
        for (const auto& pr : r->primitives)
            if (pr.kind != drawing::PrimitiveKind::Line) radius = pr.radius;
        if (r->error) state = errorText(*r->error);
    } else {
        state = errorText(r.error());
    }
    const auto* pl = placementOf(c->view);
    if (pl && pl->scale > 0) radius /= pl->scale;  // model mm
    const bool circle = c->kind == "circle";
    radius_->setEnabled(circle);
    if (!(radius_->hasFocus() && radius_->isModified())) radius_->setText(radius > 0 ? QString::number(radius, 'g', 10) : QString());
    const int bound = int(std::count_if(c->points.begin(), c->points.end(), [](const doc::ConstructionPoint& q) { return !q.bind.empty(); }));
    const QString counts = text("ui.drawing.consBound", {{"bound", QString::number(bound)}, {"count", QString::number(c->points.size())}});
    info_->setText(state.isEmpty() ? counts : counts + "\n" + state);
}

}  // namespace construe::app
