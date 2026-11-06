// Dimensions on a drawing sheet (spec/app-ux.md §8б «Dimensions», drawing.md §6а, S3-DRW-006).
#include "app/drawing_dims.h"

#include "app/messages.h"
#include "app/theme.h"
#include "app/sheet_print.h"
#include "app/symbol_field.h"

#include "drawing/render.h"
#include "base/geom2.h"

#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>
#include <limits>

namespace construe::app {

namespace {

bool inArc(double a0, double a1, double x) {
    return geom2::ccwSweep(a0, x) <= geom2::ccwSweepPositive(a0, a1);
}

// Distance from a point to a dimension sheet primitive (mm); text — 0 inside the label extent.
double primDist(const drawing::Primitive& pr, const Point2& p) {
    using K = drawing::PrimitiveKind;
    switch (pr.kind) {
        case K::Line: return pr.points.size() == 2 ? geom2::distToSegment(p, pr.points[0], pr.points[1]) : 1e300;
        case K::Polyline: {
            double d = 1e300;
            const size_t n = pr.points.size();
            for (size_t i = 0; i + 1 < n + (pr.closed ? 1 : 0); ++i) d = std::min(d, geom2::distToSegment(p, pr.points[i], pr.points[(i + 1) % n]));
            return d;
        }
        case K::Circle: return std::abs((p - pr.center).norm() - pr.radius);
        case K::Arc: {
            const Point2 v = p - pr.center;
            if (inArc(pr.start, pr.end, std::atan2(v.y(), v.x()))) return std::abs(v.norm() - pr.radius);
            const Point2 e0 = pr.center + pr.radius * Point2(std::cos(pr.start), std::sin(pr.start));
            const Point2 e1 = pr.center + pr.radius * Point2(std::cos(pr.end), std::sin(pr.end));
            return std::min((p - e0).norm(), (p - e1).norm());
        }
        case K::Text: {
            // Label extent: x by glyph outlines (drawing.md §3.1), y from the baseline up to the height; in label axes.
            const auto [x0, x1] = textExtentAlong(pr).value_or(std::pair(0.0, 0.0));
            const Point2 q = textLocal(pr, p);
            const double dx = q.x() < x0 ? x0 - q.x() : std::max(0.0, q.x() - x1);
            const double dy = q.y() < 0 ? -q.y() : std::max(0.0, q.y() - pr.height);
            return std::hypot(dx, dy);
        }
    }
    return 1e300;
}

// Primitives on top of the sheet in a single color (highlight, preview, selection).
void drawPrims(QPainter& p, const DrawingView& v, const std::vector<drawing::Primitive>& prims, const QColor& color) {
    const double z = v.zoom();
    for (const auto& pr : prims) {
        QPen pen(color);
        pen.setWidthF(std::max(1.5, drawing::LINE_THIN_MM * z));
        p.setPen(pen);
        p.setBrush(pr.filled ? QBrush(color) : QBrush(Qt::NoBrush));
        if (drawSheetGeometry(p, pr, z, [&](const Point2& q) { return v.sheetToWidget(q); })) continue;
        if (pr.kind == drawing::PrimitiveKind::Text) p.fillPath(v.sheetTransform().map(sheetTextPath(pr)), color);
    }
}

// View entity as primitives (selection highlight).
std::vector<drawing::Primitive> entityPrims(const drawing::ViewEntity& e) {
    drawing::Primitive p;
    switch (e.kind) {
        case drawing::ViewEntity::Kind::Curve: {  // its polylines (FX7-DRW-010)
            std::vector<drawing::Primitive> out;
            for (const auto& pl : e.polylines) {
                p.kind = drawing::PrimitiveKind::Polyline;
                p.points = pl;
                out.push_back(p);
            }
            return out;
        }
        case drawing::ViewEntity::Kind::Point:
            p.kind = drawing::PrimitiveKind::Circle;
            p.center = e.a;
            p.radius = 0.8;
            break;
        case drawing::ViewEntity::Kind::Line:
            p.kind = drawing::PrimitiveKind::Line;
            p.points = {e.a, e.b};
            break;
        case drawing::ViewEntity::Kind::Circle:
            p.kind = drawing::PrimitiveKind::Circle;
            p.center = e.center;
            p.radius = e.radius;
            break;
        case drawing::ViewEntity::Kind::Arc:
            p.kind = drawing::PrimitiveKind::Arc;
            p.center = e.center;
            p.radius = e.radius;
            p.start = e.start;
            p.end = e.end;
            break;
    }
    return {p};
}

// Distance from a point to a view entity, sheet mm.
double entityDist(const drawing::ViewEntity& e, const Point2& p) {
    const auto prims = entityPrims(e);
    if (e.kind == drawing::ViewEntity::Kind::Point) return (p - e.a).norm();
    double d = std::numeric_limits<double>::infinity();
    for (const auto& pr : prims) d = std::min(d, primDist(pr, p));
    return d;
}

const char* const LINEAR_TYPES[][2] = {{"horizontal", "ui.drawingDim.horizontal"}, {"vertical", "ui.drawingDim.vertical"}, {"aligned", "ui.drawingDim.aligned"}};
const char* const CIRCLE_TYPES[][2] = {{"diameter", "ui.common.diameter"}, {"radius", "ui.common.radius"}};
constexpr const char* DEFAULT_LOCAL_FIT = "H7";  // «Set in drawing» with an empty field (FX9-DRW-009)
// The default (no `tolerance` field) — «Field with deviations» (FX13-TOL-004); there is no «As in model».
const char* const TOLERANCE_MODES[][2] = {{"", "ui.drawingDim.fieldWithDeviations"}, {"hide", "ui.common.hide"}, {"field", "ui.drawingDim.fieldOnly"},
                                          {"deviations", "ui.common.deviations"}};

}  // namespace

DimensionTool::DimensionTool(DrawingView* view, Hooks hooks, QObject* parent) : QObject(parent), view_(view), h_(std::move(hooks)) {
    // «Dimension» block of the title-block panel (app-ux §8б, FX1-UI-007): header and fields as a form.
    bar_ = new QWidget;
    bar_->setObjectName("dimBar");
    auto* l = new QFormLayout(bar_);
    l->setContentsMargins(0, 6, 0, 0);
    auto* hdr = new QLabel(txt("ui.common.dimension"));
    hdr->setObjectName("dimPropsHeader");
    QFont hf = hdr->font();
    hf.setBold(true);
    hdr->setFont(hf);
    l->addRow(hdr);
    type_ = new QComboBox;
    type_->setObjectName("dimType");
    l->addRow(txt("ui.common.type"), type_);
    // Prefix, text and suffix — inscription fields with the standard symbols (FX14-UI-001, app-ux §6.11г).
    auto field = [&](const char* label, const char* name) {
        auto* e = new SymbolLineEdit;
        e->setObjectName(name);
        l->addRow(QString::fromUtf8(label), e);
        return e;
    };
    prefix_ = field(txt("ui.drawingDim.prefix"), "dimPrefix");
    text_ = field(txt("ui.common.text"), "dimText");
    suffix_ = field(txt("ui.drawingDim.suffix"), "dimSuffix");
    // Showing the tolerance from the model (FX3-DRW-001, app-ux §8б): «Field with deviations» — no field (FX13-TOL-004).
    tolerance_ = new QComboBox;
    tolerance_->setObjectName("dimTolerance");
    for (const auto& t : TOLERANCE_MODES) tolerance_->addItem(QString::fromUtf8(txt(t[1])), QString(t[0]));
    break_ = new QCheckBox(txt("ui.drawingDim.broken"));
    break_->setObjectName("dimBreak");
    l->addRow(break_);
    // Tolerance set in the drawing (FX9-DRW-009, app-ux §8б): on — the field below acts only on this dimension.
    local_ = new QCheckBox(txt("ui.drawingDim.setInDrawing"));
    local_->setObjectName("dimLocal");
    l->addRow(local_);
    // «Tolerance» after the check box (FX14-DRW-007): it is seen to be edited with the check box.
    l->addRow(txt("ui.common.tolerance"), tolerance_);
    fit_ = new QLineEdit;
    fit_->setObjectName("dimFit");
    l->addRow(txt("ui.common.toleranceField"), fit_);
    bar_->hide();
    connect(local_, &QCheckBox::clicked, this, [this](bool on) {
        const QString f = fit_->text().trimmed();
        const api::Json def{{"fit", on ? api::Json{{"field", f.isEmpty() ? std::string(DEFAULT_LOCAL_FIT) : f.toStdString()}} : api::Json(nullptr)}};
        if (auto s = h_.session().updateDimension(selected_, def); !s) {
            h_.toast(errorText(s.error()), true);
            refreshBar();
        }
    });
    connect(fit_, &QLineEdit::editingFinished, this, [this] {
        if (!fit_->isModified()) return;
        fit_->setModified(false);
        const auto d = document();
        const doc::SheetDimension* a = d ? dimensionById(*d, selected_) : nullptr;
        const std::string v = fit_->text().trimmed().toStdString();
        if (!a || !a->fit || v.empty() || a->fit->value("field", std::string()) == v) return;
        if (auto s = h_.session().updateDimension(selected_, api::Json{{"fit", {{"field", v}}}}); !s) {
            h_.toast(errorText(s.error()), true);
            refreshBar();
        }
    });
    connect(type_, &QComboBox::activated, this, [this] {
        const auto d = document();
        const doc::SheetDimension* a = d ? dimensionById(*d, selected_) : nullptr;
        if (!a) return;
        const std::string v = type_->currentData().toString().toStdString();
        const api::Json def = a->kind == "linear" ? api::Json{{"orientation", v}} : api::Json{{"kind", v}};
        if (auto s = h_.session().updateDimension(selected_, def); !s) h_.toast(errorText(s.error()), true);
    });
    connect(tolerance_, &QComboBox::activated, this, [this] {
        const std::string v = tolerance_->currentData().toString().toStdString();
        const api::Json def{{"tolerance", v.empty() ? api::Json(nullptr) : api::Json(v)}};
        if (auto s = h_.session().updateDimension(selected_, def); !s) h_.toast(errorText(s.error()), true);
    });
    connect(break_, &QCheckBox::clicked, this, [this](bool on) {
        const api::Json def{{"break", on ? api::Json(true) : api::Json(nullptr)}};
        if (auto s = h_.session().updateDimension(selected_, def); !s) h_.toast(errorText(s.error()), true);
    });
    connect(prefix_, &QLineEdit::editingFinished, this, [this] { applyText(prefix_, "prefix"); });
    connect(text_, &QLineEdit::editingFinished, this, [this] { applyText(text_, "text"); });
    connect(suffix_, &QLineEdit::editingFinished, this, [this] { applyText(suffix_, "suffix"); });
}

std::shared_ptr<const doc::Document> DimensionTool::document() const { return h_.session().document().document; }

const doc::Sheet* DimensionTool::sheet(const doc::Document& d) const {
    if (!d.active().isDrawing()) return nullptr;
    const int si = d.active().sheetIndex(h_.currentSheet());
    return si < 0 ? nullptr : &d.active().sheets[size_t(si)];
}

const doc::SheetDimension* DimensionTool::dimensionById(const doc::Document& d, const std::string& id) const {
    const doc::Sheet* sh = sheet(d);
    if (!sh || id.empty()) return nullptr;
    const int ai = sh->dimensionIndex(id);
    return ai < 0 ? nullptr : &sh->dimensions[size_t(ai)];
}

double DimensionTool::tolMm(int px) const { return safeDiv(px, view_->zoom()); }

const std::vector<drawing::ViewEntity>& DimensionTool::entitiesOf(const std::string& viewId) const {
    auto it = entities_.find(viewId);
    if (it == entities_.end()) {
        auto r = h_.session().viewEntities(h_.currentSheet(), viewId);
        it = entities_.emplace(viewId, r ? std::move(*r) : std::vector<drawing::ViewEntity>()).first;
    }
    return it->second;
}

std::optional<DimensionTool::Pick> DimensionTool::entityAt(const Point2& at) const {
    const auto d = document();
    const doc::Sheet* sh = d ? sheet(*d) : nullptr;
    if (!sh) return std::nullopt;
    // Vertices within PICK_POINT_PX win over edges (like points in the sketcher); among equally close ones — an axis or a
    // center (FX9-DRW-001), then the one nearer to the viewer (drawing.md §6а.7).
    auto axial = [](const drawing::ViewEntity& e) { return e.ref == "axis" || e.ref == "center"; };
    // An axis or a center wins only over what coincides with it: a point, or a parallel line (not a crossing one).
    auto coincide = [](const drawing::ViewEntity& a, const drawing::ViewEntity& b) {
        using K = drawing::ViewEntity::Kind;
        if (a.kind == K::Point || b.kind == K::Point) return a.kind == b.kind;
        if (a.kind != K::Line || b.kind != K::Line) return false;
        const Point2 ta = (a.b - a.a).normalized(), tb = (b.b - b.a).normalized();
        return std::abs(ta.x() * tb.y() - ta.y() * tb.x()) <= 1e-9;
    };
    std::optional<Pick> best;
    double bestD = 0;
    for (int pass = 0; pass < 2 && !best; ++pass) {
        const bool vertices = pass == 0;
        const double tol = tolMm(vertices ? PICK_POINT_PX : PICK_CURVE_PX);
        for (const auto& v : sh->views)
            for (const auto& e : entitiesOf(v.id)) {
                if ((e.kind == drawing::ViewEntity::Kind::Point) != vertices) continue;
                const double dist = entityDist(e, at);
                if (dist > tol) continue;
                const bool tie = best && std::abs(dist - bestD) <= drawing::DIM_PICK_TIE;
                // Of an axial and a model entity in a tie: the axial one if they coincide, else the model one.
                const bool mixed = tie && axial(e) != axial(best->e);
                const bool wins = mixed && (coincide(e, best->e) ? axial(e) : !axial(e));
                const bool loses = mixed && !wins;
                if (!best || dist < bestD - drawing::DIM_PICK_TIE || wins || (tie && !loses && !wins && e.depth > best->e.depth)) {
                    best = Pick{v.id, e};
                    bestD = dist;
                }
            }
    }
    return best;
}

std::optional<std::pair<std::string, std::string>> DimensionTool::faceAt(const Point2& at) const {
    const auto d = document();
    const doc::Sheet* sh = d ? sheet(*d) : nullptr;
    if (!sh) return std::nullopt;
    const double tol = tolMm(PICK_CURVE_PX);
    std::optional<std::pair<std::string, std::string>> best;
    double bestD = 0, bestDepth = 0;
    for (const auto& v : sh->views)
        for (const auto& e : entitiesOf(v.id)) {
            if (e.kind == drawing::ViewEntity::Kind::Point || e.face.empty()) continue;
            const double dist = entityDist(e, at);
            if (dist > tol) continue;
            const bool tie = best && std::abs(dist - bestD) <= drawing::DIM_PICK_TIE;
            if (!best || dist < bestD - drawing::DIM_PICK_TIE || (tie && e.depth > bestDepth)) {  // nearer to the viewer wins a tie
                best = std::make_pair(v.id, e.face);
                bestD = dist;
                bestDepth = e.depth;
            }
        }
    return best;
}

std::optional<doc::SheetDimension> DimensionTool::fromPicks() const {
    if (picks_.empty()) return std::nullopt;
    doc::SheetDimension a;
    a.view = picks_.front().view;
    using K = drawing::ViewEntity::Kind;
    // A curve — up to its far extreme point (drawing.md §6а.4, FX7-DRW-010): overall size, the greatest diameter.
    for (const auto& p : picks_) a.refs.push_back({p.e.ref, p.e.name, p.e.side, p.e.kind == K::Curve ? "far" : ""});
    const auto& e0 = picks_.front().e;
    if (picks_.size() == 1) {
        if (e0.kind == K::Circle) a.kind = "diameter";
        else if (e0.kind == K::Arc) a.kind = "radius";
        else if (e0.kind == K::Line) a.kind = "linear";
        else return std::nullopt;  // a single vertex or curve — a second object is needed
        return a;
    }
    const auto& e1 = picks_[1].e;
    if (e0.kind == K::Line && e1.kind == K::Line) {
        const Point2 t1 = (e0.b - e0.a).normalized(), t2 = (e1.b - e1.a).normalized();
        if (std::abs(t1.x() * t2.y() - t1.y() * t2.x()) > drawing::DIM_PARALLEL_TOL) {
            a.kind = "angle";
            a.offset = drawing::DIM_MIN_OFFSET;
            return a;
        }
    }
    a.kind = "linear";
    return a;
}

void DimensionTool::updatePreview(const Point2& at) {
    preview_.clear();
    previewDim_.reset();
    const auto a = fromPicks();
    if (!a) return;
    auto r = h_.session().previewDimension(h_.currentSheet(), *a, at);
    if (!r) {
        h_.status(errorText(r.error()));
        return;
    }
    preview_ = std::move(r->primitives);
    previewDim_ = r->dimension;
    h_.status(text("ui.status.placeDimension"));
}

void DimensionTool::resetPicks() {
    picks_.clear();
    preview_.clear();
    previewDim_.reset();
    if (active_) h_.status(text("ui.status.pickDimension"));
    view_->update();
}

void DimensionTool::setActive(bool on) {
    if (on == active_) return;
    active_ = on;
    hover_.reset();
    picks_.clear();
    preview_.clear();
    previewDim_.reset();
    if (on) select({});
    h_.status(on ? text("ui.status.pickDimension") : QString());
    view_->setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    if (h_.activeChanged) h_.activeChanged(on);
    view_->update();
}

void DimensionTool::select(const std::string& id) {
    if (id == selected_) return;
    selected_ = id;
    refreshBar();
    if (!id.empty() && h_.selected) h_.selected();
    view_->update();
}

void DimensionTool::setShowUndimensioned(bool on) {
    if (on == showUndimensioned_) return;
    showUndimensioned_ = on;
    recomputeUndimensioned();
    view_->update();
}

void DimensionTool::recomputeUndimensioned() {
    undimensioned_.clear();
    unplacedMarks_.clear();
    if (!showUndimensioned_ || h_.currentSheet().empty()) return;
    const auto d = document();
    if (!d || !sheet(*d)) return;
    // By the selected view (FX14-DRW-001, FX14-DRW-005, app-ux §8б): the selected view, else the view of the selected
    // dimension (after a double click the new dimension is selected — the blue stay on its view); none — no filter.
    std::vector<std::string> views;
    if (!view_->selectedView().empty()) views.push_back(view_->selectedView());
    else if (const doc::SheetDimension* a = selected_.empty() ? nullptr : dimensionById(*d, selected_)) views.push_back(a->view);
    if (auto r = h_.session().undimensioned(h_.currentSheet(), views)) undimensioned_ = std::move(*r);
    if (auto r = h_.session().unplacedMarks(h_.currentSheet(), views)) unplacedMarks_ = std::move(*r);  // FX12-DRW-004
}

int DimensionTool::unplacedMarkAt(const Point2& at) const {
    const double tol = tolMm(PICK_CURVE_PX);
    for (int i = int(unplacedMarks_.size()) - 1; i >= 0; --i)
        for (const auto& pr : unplacedMarks_[size_t(i)].primitives)
            if (primDist(pr, at) <= (pr.kind == drawing::PrimitiveKind::Text ? 0.0 : tol)) return i;
    return -1;
}

int DimensionTool::undimensionedAt(const Point2& at) const {
    const double tol = tolMm(PICK_CURVE_PX);
    for (int i = int(undimensioned_.size()) - 1; i >= 0; --i)
        for (const auto& pr : undimensioned_[size_t(i)].primitives)
            if (primDist(pr, at) <= (pr.kind == drawing::PrimitiveKind::Text ? 0.0 : tol)) return i;
    return -1;
}

bool DimensionTool::adoptUndimensioned(const Point2& at) {
    if (!showUndimensioned_ || active_) return false;
    const int i = undimensionedAt(at);
    if (i < 0) {
        const int mi = unplacedMarkAt(at);  // a blue mark (FX12-DRW-004): placed as the command places it
        if (mi < 0) return false;
        if (auto r = h_.session().addModelMark(h_.currentSheet(), unplacedMarks_[size_t(mi)].mark); !r) h_.toast(errorText(r.error()), true);
        return true;
    }
    auto r = h_.session().addDimension(h_.currentSheet(), undimensioned_[size_t(i)].dimension);
    if (!r) {
        h_.toast(errorText(r.error()), true);
        return true;
    }
    select(*r);
    return true;
}

void DimensionTool::refresh() {
    entities_.clear();
    hover_.reset();
    recomputeUndimensioned();
    const auto d = document();
    if (!selected_.empty() && (!d || !dimensionById(*d, selected_))) selected_.clear();
    refreshBar();
    view_->update();
}

bool DimensionTool::escape() {
    if (active_) {
        if (!picks_.empty()) resetPicks();
        else setActive(false);
        return true;
    }
    if (!selected_.empty()) {
        select({});
        return true;
    }
    return false;
}

std::string DimensionTool::dimensionAt(const Point2& at) const {
    const double tol = tolMm(PICK_CURVE_PX);
    const auto& prims = view_->graphics().primitives;
    for (auto it = prims.rbegin(); it != prims.rend(); ++it) {
        if (it->role.rfind("dim:", 0) != 0) continue;
        if (primDist(*it, at) <= (it->kind == drawing::PrimitiveKind::Text ? 0.0 : tol)) return it->role.substr(4);
    }
    return {};
}

bool DimensionTool::contextMenu(const Point2& at, const QPoint& globalPos) {
    if (active_) return false;
    const std::string id = dimensionAt(at);
    if (id.empty()) return false;
    select(id);
    QMenu menu(view_);
    menu.setObjectName("dimContextMenu");
    menu.addAction(txt("ui.common.delete"), this, [this, id] {
        if (auto s = h_.session().deleteDimension(id); !s) h_.toast(errorText(s.error()), true);
    });
    menu.exec(globalPos);
    return true;
}

bool DimensionTool::press(const Point2& at) {
    if (active_) return true;  // placement happens on release
    const std::string id = dimensionAt(at);
    // A press on a blue dimension or mark is consumed (FX14-DRW-001): the selection — and so the blue — stay between
    // the clicks of a double click.
    if (id.empty()) return showUndimensioned_ && (undimensionedAt(at) >= 0 || unplacedMarkAt(at) >= 0);
    select(id);
    pressed_ = true;
    dragging_ = false;
    pressAt_ = at;
    dragId_ = id;
    return true;
}

void DimensionTool::move(const Point2& at, bool leftDown) {
    if (active_) {
        hover_ = entityAt(at);
        if (!picks_.empty()) updatePreview(at);
        view_->update();
        return;
    }
    if (!leftDown || !pressed_) return;
    if (!dragging_ && (at - pressAt_).norm() * view_->zoom() > DRAG_THRESHOLD_PX) dragging_ = true;
    if (!dragging_) return;
    preview_.clear();
    previewDim_.reset();
    const auto d = document();
    if (const doc::SheetDimension* a = d ? dimensionById(*d, dragId_) : nullptr)
        if (auto r = h_.session().previewDimension(h_.currentSheet(), *a, at)) {
            preview_ = std::move(r->primitives);
            previewDim_ = r->dimension;
        }
    view_->update();
}

void DimensionTool::release(const Point2& at) {
    if (active_) {
        clickPlace(at);
        return;
    }
    pressed_ = false;
    if (!dragging_) return;
    dragging_ = false;
    const auto placed = previewDim_;
    preview_.clear();
    previewDim_.reset();
    view_->update();
    if (!placed) return;
    api::Json def{{"offset", placed->offset}};
    if (placed->kind != "linear" && placed->kind != "arcLength") def["angle"] = placed->angle;
    if (auto s = h_.session().updateDimension(dragId_, def); !s) h_.toast(errorText(s.error()), true);
}

void DimensionTool::clickPlace(const Point2& at) {
    const auto ent = entityAt(at);
    // Same entity: view, reference view, name and side (two silhouettes of a face are different entities, FX3-DRW-002).
    const bool same = ent && !picks_.empty() && std::any_of(picks_.begin(), picks_.end(), [&](const Pick& p) {
                          return p.view == ent->view && p.e.ref == ent->e.ref && p.e.name == ent->e.name && p.e.side == ent->e.side;
                      });
    if (picks_.empty()) {
        if (!ent) return;
        picks_.push_back(*ent);
        updatePreview(at);
        view_->update();
        return;
    }
    if (ent && !same && ent->view != picks_.front().view) {  // a different view — start the dimension anew
        picks_.assign(1, *ent);
        updatePreview(at);
        view_->update();
        return;
    }
    if (ent && !same && picks_.size() == 1) {
        picks_.push_back(*ent);
        updatePreview(at);
        view_->update();
        return;
    }
    // Dimension line location.
    updatePreview(at);
    if (!previewDim_) return;
    // Dimension definition (document.md §2.4г) — preview fields.
    const doc::SheetDimension& pa = *previewDim_;
    api::Json refs = api::Json::array();
    for (const auto& rf : pa.refs) {
        api::Json rj{{rf.kind, rf.name}};
        if (rf.side != 0) rj["side"] = rf.side;  // silhouette and cut — side of the generatrix (FX3-DRW-002)
        refs.push_back(rj);
    }
    api::Json def{{"kind", pa.kind}, {"view", pa.view}, {"refs", refs}, {"offset", pa.offset}};
    if (pa.kind == "linear") def["orientation"] = pa.orientation;
    else def["angle"] = pa.angle;
    auto r = h_.session().addDimension(h_.currentSheet(), def);
    resetPicks();
    if (!r) {
        h_.toast(errorText(r.error()), true);
        return;
    }
    select(*r);  // the new dimension is selected; the mode stays on for the next one
}

bool DimensionTool::key(int k) {
    if (k == Qt::Key_Delete && !active_ && !selected_.empty()) {
        const std::string id = selected_;
        if (auto s = h_.session().deleteDimension(id); !s) h_.toast(errorText(s.error()), true);
        return true;
    }
    return false;
}

void DimensionTool::paint(QPainter& p) {
    // Undimensioned model dimensions — blue, under the selection and the placement preview (FX7-DRW-003).
    for (const auto& u : undimensioned_) drawPrims(p, *view_, u.primitives, COLOR_SK_UNDER.color);
    for (const auto& u : unplacedMarks_) drawPrims(p, *view_, u.primitives, COLOR_SK_UNDER.color);  // FX12-DRW-004
    if (!selected_.empty() && !dragging_) {
        std::vector<drawing::Primitive> sel;
        for (const auto& pr : view_->graphics().primitives)
            if (pr.role == "dim:" + selected_) sel.push_back(pr);
        drawPrims(p, *view_, sel, COLOR_SELECTED.color);
    }
    for (const auto& pk : picks_) drawPrims(p, *view_, entityPrims(pk.e), COLOR_SELECTED.color);
    if (hover_) drawPrims(p, *view_, entityPrims(hover_->e), COLOR_HOVER.color);
    drawPrims(p, *view_, preview_, COLOR_HOVER.color);
}

void DimensionTool::refreshBar() {
    const auto d = document();
    const doc::SheetDimension* a = d ? dimensionById(*d, selected_) : nullptr;
    bar_->setVisible(a != nullptr);
    if (!a) return;
    {
        const QSignalBlocker b(type_);
        type_->clear();
        if (a->kind == "linear") {
            for (const auto& t : LINEAR_TYPES) type_->addItem(QString::fromUtf8(txt(t[1])), QString(t[0]));
        } else if (a->kind == "angle") {
            type_->addItem(txt("ui.drawingDim.angular"), QString("angle"));
        } else if (a->kind == "arcLength") {  // FX2-DRW-001: placed by «Dimensions from model»
            type_->addItem(txt("ui.common.arcLength"), QString("arcLength"));
        } else if (a->kind == "pathLength") {  // FX3-DRW-004: placed by «Dimensions from model»
            type_->addItem(txt("ui.drawingDim.pathLength"), QString("pathLength"));
        } else {
            for (const auto& t : CIRCLE_TYPES) type_->addItem(QString::fromUtf8(txt(t[1])), QString(t[0]));
        }
        type_->setEnabled(a->kind != "angle" && a->kind != "arcLength" && a->kind != "pathLength");
        type_->setCurrentIndex(std::max(0, type_->findData(QString::fromStdString(a->kind == "linear" ? a->orientation : a->kind))));
    }
    auto setField = [](QLineEdit* e, const std::optional<std::string>& v, const QString& hint) {
        const QString t = v ? QString::fromStdString(*v) : QString();
        if (!(e->hasFocus() && e->isModified()) && e->text() != t) e->setText(t);
        e->setPlaceholderText(hint);
    };
    // Hints: default sign and measured value (without overrides).
    doc::SheetDimension plain = *a;
    plain.prefix = std::string();
    plain.text.reset();
    plain.suffix.reset();
    QString measured;
    bool modelTolerance = false;
    if (auto r = h_.session().previewDimension(h_.currentSheet(), plain, std::nullopt)) {
        measured = QString::fromStdString(r->text);
        modelTolerance = r->modelTolerance;
    }
    {
        // Tolerance display choice — for a dimension with a tolerance from the model; break — for linear and diameter.
        const QSignalBlocker b1(tolerance_), b2(break_);
        tolerance_->setEnabled(modelTolerance);
        // A stored «fieldDeviations» is the default item (FX13-TOL-004).
        const std::string tm = a->tolerance.value_or(std::string());
        tolerance_->setCurrentIndex(std::max(0, tolerance_->findData(QString::fromStdString(tm == "fieldDeviations" ? std::string() : tm))));
        break_->setEnabled(a->kind == "linear" || a->kind == "diameter");
        break_->setChecked(a->brk);
        const QSignalBlocker b3(local_);
        local_->setChecked(a->fit.has_value());
        fit_->setEnabled(a->fit.has_value());
        const QString f = a->fit ? QString::fromStdString(a->fit->value("field", std::string())) : QString();
        if (!(fit_->hasFocus() && fit_->isModified()) && fit_->text() != f) fit_->setText(f);
    }
    setField(prefix_, a->prefix,
             a->kind == "diameter"    ? QString::fromUtf8("\xC3\x98")
             : a->kind == "radius"    ? QString("R")
             : a->kind == "arcLength" ? QString::fromUtf8("\xE2\x8C\x92")
                                      : QString());
    setField(text_, a->text, measured);
    setField(suffix_, a->suffix, QString());
}

void DimensionTool::applyText(QLineEdit* e, const char* field) {
    if (!e->isModified()) return;  // focus left without input (incl. when the window is closing) — no session access
    const auto d = document();
    const doc::SheetDimension* a = d ? dimensionById(*d, selected_) : nullptr;
    e->setModified(false);
    if (!a) return;
    const std::string v = e->text().trimmed().toStdString();
    const std::string f = field;
    const std::optional<std::string>& cur = f == "prefix" ? a->prefix : f == "text" ? a->text : a->suffix;
    if (v.empty() ? !cur : cur && *cur == v) return;  // unchanged — no undo step
    const api::Json def{{field, v.empty() ? api::Json(nullptr) : api::Json(v)}};
    if (auto s = h_.session().updateDimension(selected_, def); !s) {
        h_.toast(errorText(s.error()), true);
        refreshBar();
    }
}

}  // namespace construe::app
