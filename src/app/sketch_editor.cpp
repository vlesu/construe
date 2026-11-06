// Sketcher (spec/app-ux.md §6).
#include "app/sketch_editor.h"

#include "api/sketch_ops.h"
#include "app/sketch_picker.h"

#include "app/snap.h"
#include "base/geom2.h"
#include "base/ids.h"

#include <QCursor>
#include <QPixmap>

#include "app/app_model.h"
#include "app/expr_input.h"
#include "app/messages.h"
#include "app/theme.h"
#include "expr/expr.h"

#include "app/keys.h"

#include <QKeyEvent>
#include <QActionGroup>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace construe::app {

using Json = doc::Json;

namespace {

std::string tid(int n) { return "t" + std::to_string(n); }

constexpr int PROJECTION_TEMP_ID_BASE = 900;  // temporary IDs of auto projections (§6.4)

Json pointItem(const std::string& id, const Point2& p) { return Json{{"id", id}, {"type", "point"}, {"x", p.x()}, {"y", p.y()}}; }

Json constraintItem(const std::string& type, std::vector<std::string> refs) {
    Json r = Json::array();
    for (auto& x : refs) r.push_back(x);
    return Json{{"type", type}, {"refs", r}};
}

bool isCurve(const sketch::Entity* e) {
    return e && (e->type == sketch::EntityType::Circle || e->type == sketch::EntityType::Arc);
}

// Drawing tools (app-ux §6.3): a click creates geometry.
bool isDrawingTool(SketchEditor::Tool t) {
    using T = SketchEditor::Tool;
    switch (t) {
        case T::Point: case T::Line: case T::Rectangle: case T::Circle: case T::Arc3: case T::ArcCenter: case T::TangentArc:
        case T::CenterRectangle: case T::Polygon: case T::Circle3: case T::Ellipse: case T::Star: case T::SplineControl:
        case T::SplineThrough: case T::Text: return true;
        default: return false;
    }
}

}  // namespace

SketchEditor::SketchEditor(AppModel& model, Viewport& view, QObject* parent)
    : QObject(parent), model_(model), view_(view), inlineEdit_(model.session(), view) {
    connect(&inlineEdit_, &InlineDimensionEdit::applied, this, &SketchEditor::refresh);
    connect(&inlineEdit_, &InlineDimensionEdit::hint, this, &SketchEditor::hint);
    connect(&inlineEdit_, &InlineDimensionEdit::error, this, &SketchEditor::error);
    textBox_ = new TextBox(model_, &view_);
    connect(textBox_, &TextBox::accepted, this, [this](const TextBox::Values& v) { textAccepted(v); });
    patternBox_ = new PatternBox(&view_);  // sketch pattern (§6.3, S4-SKT-001/002)
    connect(patternBox_, &PatternBox::changed, this, [this] { view_.update(); });
    connect(patternBox_, &PatternBox::accepted, this, [this] { patternAccepted(); });
    connect(patternBox_, &PatternBox::cancelled, this, [this] { patternClosed(); });
    connect(patternBox_, &PatternBox::linePickRequested, this, [this](bool on) {  // S4-SKT-006
        emit hint(on ? txt("ui.sketch.clickALinePatternDirection") : "");
        view_.setFocus();
    });
    connect(patternBox_, &PatternBox::unlinkRequested, this, [this] {
        const std::string g = patternEditing_;
        patternClosed();
        auto& s = model_.session();
        s.beginGroup("undo.group.sketchUnlink");
        auto r = s.sketchGroupUnlink(g);
        s.endGroup();
        if (!r) emit error(errorText(r.error(), s.document().document.get()));
        refresh();
    });
    primitiveBox_ = new PrimitiveBox(&view_);  // primitive dialog (§6.3, S4-SKT-003)
    connect(primitiveBox_, &PrimitiveBox::accepted, this, [this] { primitiveAccepted(); });
    connect(primitiveBox_, &PrimitiveBox::cancelled, this, [this] { primitiveClosed(); });
    connect(primitiveBox_, &PrimitiveBox::unlinkRequested, this, [this] {
        const std::string g = primitiveEditing_;
        primitiveClosed();
        auto& s = model_.session();
        s.beginGroup("undo.group.sketchUnlink");
        auto r = s.sketchGroupUnlink(g);
        s.endGroup();
        if (!r) emit error(errorText(r.error(), s.document().document.get()));
        refresh();
    });
    toleranceBox_ = new ToleranceBox(&view_);  // dimension tolerance (§6.6)
    connect(toleranceBox_, &ToleranceBox::accepted, this, [this](const doc::Json& t) { toleranceAccepted(t); });
    connect(toleranceBox_, &ToleranceBox::removed, this, [this] { toleranceRemoved(); });
    // Dimension preview after a mouse pause (§6.9, FX2-SK-002).
    dimPreviewTimer_ = new QTimer(this);
    dimPreviewTimer_->setSingleShot(true);
    dimPreviewTimer_->setInterval(DIM_PREVIEW_DELAY_MS);
    connect(dimPreviewTimer_, &QTimer::timeout, this, [this] {
        dimPreview_ = computeDimensionPreview(dimPreviewAt_);
        view_.update();
    });
}

void SketchEditor::open(const std::string& featureId) {
    feature_ = featureId;
    resetToolState();
    selection_.clear();
    tool_ = Tool::Select;
    drawConstruction_ = false;  // the construction mode lasts for the sketch session (§6.3, FX7-SK-001)
    refresh();
}

void SketchEditor::close() {
    finishInlineEdit(false);
    toleranceBox_->hide();
    feature_.clear();
    state_.reset();
    resetToolState();
    selection_.clear();
    emit statusChanged();
}

void SketchEditor::refresh() {
    if (feature_.empty()) return;
    auto st = model_.session().sketchState(feature_);
    if (st) state_ = *st;
    else state_.reset();
    // Remove vanished objects from the selection.
    if (const auto* d = data()) {
        OrderedSelection keep;
        for (const auto& id : selection_)
            if (d->findEntity(id) || d->findConstraint(id)) keep.insert(id);
        selection_ = keep;
    }
    view_.update();
    emit statusChanged();
}

SketchGeometry SketchEditor::geo() const {
    const api::SketchState* base = state_ ? &*state_ : nullptr;
    const api::SketchState* active = dragState_ ? &*dragState_ : base;
    return SketchGeometry(displayData(), base, active, view_.camera());
}

SketchAnnotations SketchEditor::annotations() const {
    std::optional<std::pair<std::string, Point2>> over;
    if (!labelDrag_.empty()) over = std::pair{labelDrag_, labelDragAt_};  // a dimension label is being dragged (§6.6)
    return SketchAnnotations(geo(), model_.session(), [this](const std::string& id) { return selection_.count(id); }, hover_, showConstraints_,
                             over);
}

QPointF SketchEditor::toScreen(const Point2& local) const { return geo().toScreen(local); }

std::optional<Point2> SketchEditor::toLocal(const QPointF& s) const { return geo().toLocal(s); }

Snap SketchEditor::snapAt(const QPointF& s, bool suppress, const std::set<std::string>& exclude) const {
    // Body geometry off the sketch plane is a target at any view direction through the implicit targets (§6.4, FX8-SK-010).
    return app::snapAt(geo(), view_.bodies(), s, suppress, exclude, bodyTargets());
}

const std::vector<DrawBody>* SketchEditor::bodyTargets() const {
    // Implicit targets: projections of edges and vertices off the plane (FX8-SK-010), silhouettes and extremes of bodies
    // of revolution (FX8-SK-002; app-ux §6.4), cached by the session.
    if (!state_ || feature_.empty()) return nullptr;
    auto t = model_.session().sketchBodyTargets(feature_);
    if (!t || !*t || (*t)->empty()) return nullptr;
    if (targetsKey_ != t->get()) {
        targetsKey_ = t->get();
        targetsHold_ = *t;
        targetsBodies_ = bodyTargetsBodies(**t, state_->frame);
    }
    return &targetsBodies_;
}

std::optional<std::pair<std::string, bool>> SketchEditor::bodyTargetAt(const QPointF& s) const {
    // A silhouette line (within PICK_CURVE_PX) or an extreme point (within PICK_POINT_PX) under the cursor (FX8-SK-002):
    // its source and whether it is a point.
    const auto* bodies = bodyTargets();
    if (!bodies) return std::nullopt;
    const DrawBody* b = &bodies->at(1);
    const Plane& pl = state_->frame;
    auto screen = [&](const float* p) { return toScreen(pl.toLocal(Point3(p[0], p[1], p[2]))); };
    for (const auto& v : b->mesh.vertices) {
        const QPointF q = screen(v.position.data());
        if (std::hypot(q.x() - s.x(), q.y() - s.y()) <= PICK_POINT_PX) return std::pair{v.name, true};
    }
    std::optional<std::pair<std::string, bool>> best;
    double bd = PICK_CURVE_PX;
    for (const auto& e : b->mesh.edges) {
        if (e.polylines.empty() || e.polylines[0].size() != 6) continue;
        const double dd = segDist(s, screen(e.polylines[0].data()), screen(e.polylines[0].data() + 3));
        if (dd <= bd) bd = dd, best = std::pair{e.name, false};
    }
    return best;
}

Hit SketchEditor::hitAt(const QPointF& s) const { return app::hitAt(geo(), annotations(), s); }

std::string SketchEditor::trimCurveAt(const QPointF& screen) const { return geo().trimCurveAt(screen); }

std::optional<QRectF> SketchEditor::labelRect(const std::string& id) const { return annotations().labelRect(id); }

std::optional<DimPrims> SketchEditor::dimensionPrims(const std::string& id) const {
    const auto* d = data();
    const auto* c = d ? d->findConstraint(id) : nullptr;
    if (!c || !sketch::isDimensionType(c->type)) return std::nullopt;
    const auto a = annotations();
    return a.dimensionPrims(*c, a.labelPos(*c));
}

api::FullyDefineLayout SketchEditor::dimensionLabelLayout() const {
    const double pxPerMm = std::hypot(toScreen(Point2(1, 0)).x() - toScreen(Point2::Zero()).x(),
                                      toScreen(Point2(1, 0)).y() - toScreen(Point2::Zero()).y());
    if (!(pxPerMm > 0)) return {};
    QFont f;
    f.setPixelSize(DIM_TEXT_PX);
    const QFontMetricsF fm(f);
    // Like labelRect (sketch_annotations): height — fm.height() + 2; character width — the widest digit.
    double digit = 0;
    for (char ch = '0'; ch <= '9'; ++ch) digit = std::max(digit, fm.horizontalAdvance(QChar(ch)));
    return {(fm.height() + 2) / pxPerMm, digit / pxPerMm};
}

std::optional<QRectF> SketchEditor::glyphRect(const std::string& constraintId) const { return annotations().glyphRect(constraintId); }

std::optional<QPointF> SketchEditor::screenPosOfEntity(const std::string& id) const {
    const auto g = geo();
    return app::screenPosOfEntity(g, annotations(), id);
}

void SketchEditor::openInlineEdit(const std::string& cid) {
    const auto* d = sketch::isWeakId(cid) ? displayData() : data();
    const auto* c = d ? d->findConstraint(cid) : nullptr;
    if (!c) return;
    const auto a = annotations();
    const auto r = a.labelRect(cid).value_or(QRectF(a.labelPos(*c), QSizeF(10, 10)));
    if (sketch::isWeakId(cid)) {  // weak dimension: current value, Enter — regular (§6.6а, S4-WEAK-002)
        const double v = a.dimValue(*c).value_or(0);
        const bool ang = sketch::isAngleDimension(c->type);
        std::optional<std::array<double, 2>> at;
        if (const auto lp = api::sketchops::dimensionLabelPoint(dataWithProjections(), state_->result.state, *c)) at = std::array<double, 2>{lp->x(), lp->y()};
        inlineEdit_.open(cid, ang ? angleText(v) : lengthText(v), ang, r, at);
        return;
    }
    inlineEdit_.open(cid, c->value.value_or(""), sketch::isAngleDimension(c->type), r);
}

bool SketchEditor::isWeak(const std::string& id) const {
    if (!state_ || !sketch::isWeakId(id)) return false;
    return std::any_of(state_->weak.begin(), state_->weak.end(), [&](const sketch::Constraint& c) { return c.id == id; });
}

void SketchEditor::strengthenWeak(const std::string& weakId) {
    if (!isWeak(weakId)) return;
    const auto* c = displayData()->findConstraint(weakId);
    if (c && sketch::isDimensionType(c->type)) {
        openInlineEdit(weakId);
        return;
    }
    auto& s = model_.session();
    if (auto r = s.sketchStrengthen(weakId); !r) emit error(errorText(r.error(), s.document().document.get()));
    refresh();
}

const sketch::SketchData* SketchEditor::displayData() const {
    const auto* d = data();
    if (!d || !state_ || state_->weak.empty() || buildingDisplay_) return d;
    if (display_ && displayKey_ == d && displayRev_ == state_->revision) return display_.get();
    buildingDisplay_ = true;  // the layout below converts coordinates via geo(), i.e. by data()
    auto dd = std::make_shared<sketch::SketchData>(*d);
    // Weak dimension labels — by the «Dimension all» (FX4-SK-001) layout at the current view scale.
    std::vector<sketch::Constraint> dims;
    std::vector<size_t> at;
    for (const auto& w : state_->weak) {
        sketch::Constraint c = w;
        if (sketch::isDimensionType(c.type)) {
            const auto it = state_->result.reference.find(c.id);
            const double v = it != state_->result.reference.end() ? it->second : 0.0;
            c.value = sketch::isAngleDimension(c.type) ? angleText(v) : lengthText(v);
            c.driving = true;
            dims.push_back(c);
            at.push_back(dd->constraints.size());
        }
        dd->constraints.push_back(c);
    }
    const sketch::SketchData base = dataWithProjections();
    api::FullyDefineLayout layout = dimensionLabelLayout();
    layout.maxRows = WEAK_LAYOUT_ROWS;  // layout after every command — fewer candidates (§6.6а)
    const auto labels = api::sketchops::layoutDimensionLabels(base, state_->result.state, dims, layout);
    for (size_t k = 0; k < at.size() && k < labels.size(); ++k) {
        auto& c = dd->constraints[at[k]];
        c.labelOffset = api::sketchops::labelOffsetFor(base, state_->result.state, c, labels[k]);
        c.driving = false;  // value taken from the reference state (as for a reference dimension)
    }
    buildingDisplay_ = false;
    display_ = dd;
    displayKey_ = d;
    displayRev_ = state_->revision;
    return display_.get();
}

const sketch::SketchData* SketchEditor::data() const {
    if (feature_.empty()) return nullptr;
    auto info = model_.session().document();
    static thread_local std::shared_ptr<const doc::Document> holder;
    holder = info.document;
    const auto f = holder->active().find(feature_);
    return f ? f->sketchData() : nullptr;
}

std::string SketchEditor::axisAt(const QPointF& s) const {
    const auto p = toLocal(s);
    if (!p) return {};
    std::string best;
    double bestD = PICK_CURVE_PX;
    for (const auto& [name, q] : {std::pair<std::string, Point2>{"sketch:x", Point2(p->x(), 0)}, {"sketch:y", Point2(0, p->y())}}) {
        const QPointF qs = toScreen(q);
        const double d = std::hypot(qs.x() - s.x(), qs.y() - s.y());
        if (d <= bestD) bestD = d, best = name;
    }
    return best;
}

sketch::SketchData SketchEditor::dataWithProjections() const {
    sketch::SketchData sd = data() ? *data() : sketch::SketchData{};
    if (state_) sd.projections = state_->projections;
    return sd;
}

void SketchEditor::clickProject(const QPointF& s) {
    // Body edge or vertex under the cursor (view ID buffer; vertex priority — §6.11а).
    const PickTarget t = view_.pickAt(s.toPoint(), false, true, false);
    if (t.kind != PickTarget::Kind::Edge && t.kind != PickTarget::Kind::Vertex) return;
    if (const auto* d = data())
        for (const auto& e : d->entities)
            if (e.type == sketch::EntityType::Projected && e.source == t.name) {
                emit hint(text("ui.status.alreadyProjected"));
                return;
            }
    // A projection is auxiliary (Р-020, §6.11а); it becomes regular via the «Construction» command.
    auto r = model_.session().sketchAdd(Json::array({Json{{"id", "t1"}, {"type", "projected"}, {"source", t.name}, {"construction", true}}}));
    if (!r) emit error(errorText(r.error()));
}

void SketchEditor::clickPierce(const QPointF& s) {
    if (piercePoint_.empty()) {
        const Hit h = hitAt(s);
        const auto* d = data();
        const auto* e = d && h.kind == Hit::Kind::Point ? d->findEntity(h.id) : nullptr;
        if (!e) {
            emit hint(txt("ui.sketch.requiredSketchPoint"));
            return;
        }
        piercePoint_ = e->id;
        emit hint(txt("ui.sketch.selectAPathBodyEdge"));
        return;
    }
    const PickTarget t = view_.pickAt(s.toPoint(), false, true, false);
    if (t.kind != PickTarget::Kind::Edge) {
        emit hint(txt("ui.sketch.selectAPathBodyEdge"));
        return;
    }
    pierceSource("pierce:" + t.name);
}

void SketchEditor::pierceSource(const std::string& source) {
    if (tool_ != Tool::Pierce || piercePoint_.empty()) return;
    // A projection with the same source is reused; otherwise a new auxiliary one (§6.11б).
    std::string proj;
    if (const auto* d = data())
        for (const auto& e : d->entities)
            if (e.type == sketch::EntityType::Projected && e.source == source) proj = e.id;
    Json items = Json::array();
    if (proj.empty()) {
        items.push_back(Json{{"id", "t1"}, {"type", "projected"}, {"source", source}, {"construction", true}});
        proj = "t1";
    }
    items.push_back(Json{{"type", "coincident"}, {"refs", {piercePoint_, proj}}});
    auto r = model_.session().sketchAdd(items);
    piercePoint_.clear();
    if (!r) emit error(errorText(r.error()));
    else emit hint(txt("ui.sketch.requiredSketchPoint"));
    refresh();
}

void SketchEditor::snapConstraint(Json& items, const std::string& tempId, const Snap& snap) const {
    // Hint constraints are "weak" (auto, S1-SK-016): they are marked on exit.
    const size_t before = items.size();
    struct MarkAuto {
        Json& items;
        size_t from;
        ~MarkAuto() {
            for (size_t i = from; i < items.size(); ++i)
                if (items[i].contains("refs")) items[i]["auto"] = true;
        }
    } mark{items, before};
    std::string target = snap.target;
    if (!snap.source.empty() && snap.kind != Snap::Kind::Center) target = projectionFor(items, snap.source);
    switch (snap.kind) {
        case Snap::Kind::Point: items.push_back(constraintItem("coincident", {tempId, target})); break;
        case Snap::Kind::Midpoint: items.push_back(constraintItem("midpoint", {tempId, target})); break;
        case Snap::Kind::Center: break;  // center is not a sketch point; for a circle center — concentric (clickCircle)
        case Snap::Kind::Intersection: {  // point on both curves (§6.4 item 1г, vision item 16)
            const std::string second = snap.source2.empty() ? snap.target2 : projectionFor(items, snap.source2);
            items.push_back(constraintItem("pointOnObject", {tempId, target}));
            items.push_back(constraintItem("pointOnObject", {tempId, second}));
            break;
        }
        case Snap::Kind::Curve:
            items.push_back(constraintItem("pointOnObject", {tempId, target}));
            // "Onto curve" with alignment along one axis (§6.4, S1-SK-015).
            if (!snap.alignXSource.empty()) items.push_back(constraintItem("vertical", {tempId, projectionFor(items, snap.alignXSource)}));
            else if (!snap.alignXId.empty()) items.push_back(constraintItem("vertical", {tempId, snap.alignXId}));
            if (!snap.alignYSource.empty()) items.push_back(constraintItem("horizontal", {tempId, projectionFor(items, snap.alignYSource)}));
            else if (!snap.alignYId.empty()) items.push_back(constraintItem("horizontal", {tempId, snap.alignYId}));
            break;
        case Snap::Kind::None:
            // Hints without a position snap become constraints too (§6.4, rule М9).
            // Target — a point ([new, point]) or a segment midpoint ([new, segment]); on a body — via a projection.
            if (!snap.alignXSource.empty()) items.push_back(constraintItem("vertical", {tempId, projectionFor(items, snap.alignXSource)}));
            else if (!snap.alignXId.empty()) items.push_back(constraintItem("vertical", {tempId, snap.alignXId}));
            if (!snap.alignYSource.empty()) items.push_back(constraintItem("horizontal", {tempId, projectionFor(items, snap.alignYSource)}));
            else if (!snap.alignYId.empty()) items.push_back(constraintItem("horizontal", {tempId, snap.alignYId}));
            if (!snap.extLine.empty()) items.push_back(constraintItem("pointOnObject", {tempId, snap.extLine}));
            break;
    }
}

std::string SketchEditor::projectionFor(Json& items, const std::string& source) const {
    // A projection with the same source, already in the sketch or already in this placement, is used (§6.4).
    if (const auto* d = data())
        for (const auto& e : d->entities)
            if (e.type == sketch::EntityType::Projected && e.source == source) return e.id;
    // Temporary projection IDs are in a separate range: the caller assigns IDs of its elements after
    // the snap (t1, t2, …), and they must not collide.
    int count = 0;
    for (const auto& it : items)
        if (it.value("type", "") == "projected") {
            if (it.value("source", "") == source) return it["id"].get<std::string>();
            ++count;
        }
    const std::string id = tid(PROJECTION_TEMP_ID_BASE + count);
    items.push_back(Json{{"id", id}, {"type", "projected"}, {"source", source}, {"construction", true}});
    return id;
}

std::optional<api::sketchops::EqualMatch> SketchEditor::equalSnap(bool radius, double value, bool raw, double tolScale) const {
    const auto* d = data();
    if (!d || !state_) return std::nullopt;
    // The tolerance is the common snap one (FX8-SK-006): SNAP_POINT_PX on screen at the cursor, in mm.
    double absTol = 0;
    if (const auto at = toLocal(cursor_)) {
        const QPointF a = toScreen(*at), b = toScreen(*at + Point2(1, 0)), c = toScreen(*at + Point2(0, 1));
        const double pxPerMm = std::max(std::hypot(b.x() - a.x(), b.y() - a.y()), std::hypot(c.x() - a.x(), c.y() - a.y()));
        if (pxPerMm > 0) absTol = SNAP_POINT_PX / pxPerMm;
    }
    auto m = api::sketchops::equalTarget(*d, state_->result.state, radius, value, absTol * std::max(1.0, tolScale));
    if (m && !raw && hintDismissed("equal:" + m->id)) return std::nullopt;  // dismissed by Esc (FX10-SK-005)
    return m;
}

std::vector<std::string> SketchEditor::arc3Tangents(const Point2& a, const Point2& b, const Point2& m, bool raw) const {
    // Arc through a, b (ends) and m (FX7-SK-002): the ends are the first two clicks.
    const auto c = api::sketchops::circumcenter(a, b, m);
    if (!c || clickSnaps_.size() < 2) return {};
    return arcEndTangents(*c, a, clickSnaps_[0], b, clickSnaps_[1], raw);
}

std::vector<std::string> SketchEditor::arcEndTangents(const Point2& c, const Point2& a, const Snap& sa, const Point2& b,
                                                      const Snap& sb, bool raw) const {
    // At an end snapped to the end of a line / arc, the arc's tangent close to that curve's direction (within
    // SNAP_TANGENT_DEG: the arc's tangent from inexact clicks is off by several degrees) — tangency (§6.4, FX10-SK-004).
    std::vector<std::string> out;
    const auto* d = data();
    if (!d || !state_) return out;
    for (int i = 0; i < 2; ++i) {
        const Snap& sn = i == 0 ? sa : sb;
        if (sn.kind != Snap::Kind::Point || sn.target.empty() || sn.target == "origin" || !sn.source.empty()) continue;
        const Point2 r = (i == 0 ? a : b) - c;
        const std::string t = api::sketchops::tangentAtJoin(*d, state_->result.state, sn.target, Point2(-r.y(), r.x()), SNAP_TANGENT_DEG * PI / 180);
        if (!t.empty() && (raw || !hintDismissed("tangent:" + t)) && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    return out;
}

std::string SketchEditor::arcEnteredAt(const std::string& point, const Point2& dir, bool raw) const {
    const auto* d = data();
    if (!d || point.empty() || point == "origin") return {};
    for (const auto& e : d->entities)
        if (e.type == sketch::EntityType::Arc && (e.start == point || e.end == point)) {
            // The segment arrives at the arc's end and goes on into the arc: against the arc's continuation there.
            const Point2 t = snap::arcContinuation(pointPos(e.center), pointPos(point), e.start == point);
            if (snap::alongTangent(dir, -t, SNAP_TANGENT_DEG * PI / 180) && (raw || !hintDismissed("tangent:" + e.id))) return e.id;
        }
    return {};
}

QString SketchEditor::equalHint(bool raw) const {
    // The element a click at the cursor would make the drawn radius / length equal to (FX7-SK-003).
    if (!cursorValid_ || ctrlHeld_ || clicks_.empty() || hoverSnap_.kind != Snap::Kind::None) return {};
    const auto at = toLocal(cursor_);
    if (!at) return {};
    std::optional<api::sketchops::EqualMatch> m;
    if ((tool_ == Tool::Circle || tool_ == Tool::ArcCenter) && clicks_.size() == 1) m = equalSnap(true, (*at - clicks_[0]).norm(), raw);
    else if (tool_ == Tool::Line) m = equalSnap(false, (*at - clicks_.back()).norm(), raw);
    else if (tool_ == Tool::Arc3 && clicks_.size() == 2)
        if (const auto c = api::sketchops::circumcenter(clicks_[0], clicks_[1], *at))
            m = equalSnap(true, (clicks_[0] - *c).norm(), raw, snap::arc3RadiusSensitivity(clicks_[0], clicks_[1], *at));
    return m ? QString::fromStdString(m->id) : QString();
}

QString SketchEditor::arcTangentHint(bool raw) const {
    if (tool_ != Tool::Arc3 || clicks_.size() != 2 || !cursorValid_ || ctrlHeld_) return {};
    const auto at = toLocal(cursor_);
    if (!at) return {};
    const auto t = arc3Tangents(clicks_[0], clicks_[1], hoverSnap_.kind != Snap::Kind::None ? hoverSnap_.at : *at, raw);
    return t.empty() ? QString() : QString::fromStdString(t.front());
}

std::string SketchEditor::directionHintKey(bool raw) const {
    // While a segment (or centerline) is being drawn and Ctrl is not held (FX7-SK-011): the direction the click would
    // snap to, as for the click itself (§6.4 items 4–6; with a position snap — horizontal/vertical to the basis only).
    if (tool_ != Tool::Line || clicks_.empty() || !cursorValid_ || ctrlHeld_) return {};
    const auto at = toLocal(cursor_);
    if (!at) return {};
    const Point2 b = hoverSnap_.kind != Snap::Kind::None ? hoverSnap_.at : *at;
    const DirectionSnap d = directionSnap(clicks_.back(), b, raw);
    if (hoverSnap_.kind != Snap::Kind::None && d.type != "horizontal" && d.type != "vertical") {
        // An end on an arc's end entering it smoothly — tangency at the joint (§6.4, FX10-SK-004).
        if (hoverSnap_.kind == Snap::Kind::Point && hoverSnap_.source.empty())
            if (const std::string arc = arcEnteredAt(hoverSnap_.target, b - clicks_.back(), raw); !arc.empty()) return "tangent:" + arc;
        return {};
    }
    return d.type.empty() ? snap::directionHintKey(d.alsoTangent.empty() ? "" : "tangent", d.alsoTangent) : snap::directionHintKey(d.type, d.other);
}

QString SketchEditor::directionHint() const {
    const std::string k = directionHintKey(false);
    if (k.rfind("tangent:", 0) == 0) return "tangent";
    if (k.rfind("dir:", 0) == 0) return QString::fromStdString(k.substr(4, k.find(':', 4) - 4));
    return {};
}

std::vector<std::string> SketchEditor::shownHintKeys(bool raw) const {
    std::vector<std::string> out;
    auto add = [&](const std::string& k) {
        if (!k.empty() && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
    };
    if (const QString e = equalHint(raw); !e.isEmpty()) add("equal:" + e.toStdString());
    if (const QString t = arcTangentHint(raw); !t.isEmpty()) add("tangent:" + t.toStdString());
    add(directionHintKey(raw));
    return out;
}

bool SketchEditor::dismissShownHints() {
    // Esc while an auto-constraint is offered at the cursor (§6.13 item 0, FX10-SK-005): the next click goes without it,
    // the tool and the started element stay.
    if (!dismissedHints_.dismiss(shownHintKeys(false))) return false;
    emit hint(text("ui.sketch.hintDismissed"));
    view_.update();
    return true;
}

SketchEditor::DirectionSnap SketchEditor::directionSnap(const Point2& a, const Point2& b, bool raw) const {
    const auto* d = data();
    std::optional<Point2> woken, tangent;
    std::string arcId;
    if (const auto* w = wokenLine_.empty() ? nullptr : d->findEntity(wokenLine_); w && w->type == sketch::EntityType::Line)
        woken = pointPos(w->p2) - pointPos(w->p1);
    if (const auto* arc = chainEndCurve_.empty() ? nullptr : d->findEntity(chainEndCurve_);
        arc && arc->type == sketch::EntityType::Arc && (arc->start == chainEndPoint_ || arc->end == chainEndPoint_)) {
        tangent = snap::arcContinuation(pointPos(arc->center), pointPos(chainEndPoint_), arc->start == chainEndPoint_);
        arcId = arc->id;
    } else if (chainEndPoint_.empty() && clicks_.size() == 1 && !clickSnaps_.empty() && clickSnaps_.back().kind == Snap::Kind::Point) {
        // A segment started at the end of an arc (not a chain continuation): the arc's tangent there (FX10-SK-002).
        const std::string& from = clickSnaps_.back().target;
        for (const auto& e : d->entities)
            if (e.type == sketch::EntityType::Arc && !from.empty() && (e.start == from || e.end == from)) {
                tangent = snap::arcContinuation(pointPos(e.center), pointPos(from), e.start == from);
                arcId = e.id;
                break;
            }
    }
    const auto r = snap::snapDirection(a, b, SNAP_ANGLE_DEG * PI / 180, woken, tangent);
    DirectionSnap ds;
    ds.end = r.end;
    switch (r.kind) {
        case snap::Direction::Horizontal: ds.type = "horizontal"; break;
        case snap::Direction::Vertical: ds.type = "vertical"; break;
        case snap::Direction::Parallel: ds.type = "parallel", ds.other = wokenLine_; break;
        case snap::Direction::Perpendicular: ds.type = "perpendicular", ds.other = wokenLine_; break;
        case snap::Direction::Tangent: ds.type = "tangent", ds.other = arcId; break;
        case snap::Direction::None: break;
    }
    // Horizontal/vertical from an arc end, coinciding with its tangent — tangency as well (§6.4); no direction snap but
    // the segment goes on along the arc within SNAP_TANGENT_DEG — tangency without turning (FX10-SK-004).
    if ((ds.type == "horizontal" || ds.type == "vertical" || ds.type.empty()) && tangent && tangent->squaredNorm() > 0) {
        const Point2 dir = ds.type == "horizontal" ? Point2(1, 0) : ds.type == "vertical" ? Point2(0, 1) : Point2(b - a);
        if (snap::alongTangent(dir, *tangent, SNAP_TANGENT_DEG * PI / 180) || (!ds.type.empty() && snap::alongTangent(-dir, *tangent, SNAP_TANGENT_DEG * PI / 180)))
            ds.alsoTangent = arcId;
    }
    // Hints dismissed by Esc (FX10-SK-005): the direction does not snap; a dismissed direction leaves its tangency, which
    // then is the shown hint (the next Esc dismisses it too).
    if (!raw) {
        if (hintDismissed(snap::directionHintKey(ds.type, ds.other))) ds.type.clear(), ds.other.clear(), ds.end = b;
        if (!ds.alsoTangent.empty() && hintDismissed("tangent:" + ds.alsoTangent)) ds.alsoTangent.clear();
    }
    return ds;
}

// ---------------------------------------------------------------- dimensions: text and position

// Value of a new dimension — a number in document units without a unit (§5.6).
std::string SketchEditor::lengthText(double mm) const {
    const auto u = model_.session().units();
    return formatNumber(mm / expr::lengthFactor(u.length)).toStdString();
}

std::string SketchEditor::angleText(double rad) const {
    const auto u = model_.session().units();
    return formatNumber(rad / expr::angleFactor(u.angle)).toStdString();
}

// ---------------------------------------------------------------- colors (§6.5)

const char* SketchEditor::entityColorToken(const std::string& id) const {
    if (selection_.count(id)) return COLOR_SELECTED.name;
    if (hover_.id == id && hover_.kind != Hit::Kind::None) return COLOR_HOVER.name;
    if (!state_) return COLOR_SK_UNDER.name;
    auto it = state_->result.entityState.find(id);
    if (it == state_->result.entityState.end()) return COLOR_SK_UNDER.name;
    switch (it->second) {
        case sketch::EntityState::Fixed: return COLOR_SK_FULL.name;
        case sketch::EntityState::Conflict: return COLOR_SK_CONFLICT.name;
        default: return COLOR_SK_UNDER.name;
    }
}

QColor SketchEditor::entityColor(const std::string& id) const {
    const std::string t = entityColorToken(id);
    for (const ColorToken* c : {&COLOR_SELECTED, &COLOR_HOVER, &COLOR_SK_FULL, &COLOR_SK_CONFLICT, &COLOR_SK_UNDER})
        if (t == c->name) return c->color;
    return COLOR_SK_UNDER.color;
}

QString SketchEditor::dofText() const {
    if (!state_) return {};
    const auto& r = state_->result;
    switch (r.status) {
        case sketch::SketchStatus::UnderConstrained: {
            // Weak dimensions (§6.10, S4-WEAK-001): «DOF: <without weak> + <number of weak> weak» (non-weak DOF + weak).
            // weakDof also counts the implicit holding of axis ends (FX7-SK-011): only an axis left — fully defined.
            const int n = int(state_->weak.size());
            const int rest = std::max(0, r.dof.value_or(0) - state_->weakDof);
            if (n == 0) return rest == 0 ? QString(txt("ui.sketch.fullyDefined")) : QString(txt("ui.sketch.underDefinedDofN")).arg(rest);
            const QString word = n % 10 == 1 && n % 100 != 11 ? txt("ui.sketch.weak") : txt("ui.sketch.weak2");
            return QString("%1 · DOF: %2 + %3 %4").arg(rest == 0 ? txt("ui.sketch.defined") : txt("ui.sketch.underDefined")).arg(rest).arg(n).arg(word);
        }
        case sketch::SketchStatus::FullyConstrained: return txt("ui.sketch.fullyDefined");
        case sketch::SketchStatus::OverConstrainedRedundant: return QString(txt("ui.sketch.redundantConstraintsN")).arg(r.redundant.size());
        case sketch::SketchStatus::Conflict: return txt("ui.sketch.constraintConflict");
        case sketch::SketchStatus::NotConverged: return txt("ui.sketch.notSolved");
        case sketch::SketchStatus::Error: return txt("ui.sketch.sketchError");
    }
    return {};
}

QColor SketchEditor::dofColor() const {
    if (!state_) return COLOR_SK_UNDER.color;
    switch (state_->result.status) {
        case sketch::SketchStatus::UnderConstrained:
            // Everything is covered by weak dimensions — shown as defined (§6.10, S4-WEAK-001).
            return state_->result.dof.value_or(0) <= state_->weakDof ? COLOR_SK_FULL.color : COLOR_SK_UNDER.color;
        case sketch::SketchStatus::FullyConstrained: return COLOR_SK_FULL.color;
        case sketch::SketchStatus::OverConstrainedRedundant: return COLOR_WARNING.color;
        default: return COLOR_SK_CONFLICT.color;
    }
}

// ---------------------------------------------------------------- commands

std::map<std::string, double> SketchEditor::curveSizes() const {
    std::map<std::string, double> out;
    const auto* d = data();
    if (!d || !state_) return out;
    for (const auto& e : d->entities) {
        if (e.type == sketch::EntityType::Line) out[e.id] = (pointPos(e.p2) - pointPos(e.p1)).norm();
        if (e.type == sketch::EntityType::Arc) out[e.id] = (pointPos(e.start) - pointPos(e.center)).norm();
        if (e.type == sketch::EntityType::Circle)
            if (const auto it = state_->result.state.radii.find(e.id); it != state_->result.state.radii.end()) out[e.id] = it->second;
    }
    return out;
}

bool SketchEditor::commitItems(const Json& itemsIn, const std::string& groupKey, std::map<std::string, std::string>* ids) {
    const auto sizesBefore = curveSizes();
    // Auto-constraints get temporary IDs so that their persistent IDs are known from the reply.
    Json items = itemsIn;
    // Centerline and «Draw as construction» (§6.3, FX2-SK-001): curves (and texts, with
    // the toggle on) are created as construction.
    markNewConstruction(items, centerline_ || drawConstruction_, drawConstruction_);
    int next = 1;
    for (const auto& it : items)
        if (it.contains("id")) next = std::max(next, std::stoi(it["id"].get<std::string>().substr(1)) + 1);
    std::vector<std::string> autoTemp;
    for (auto& it : items)
        if (!it.contains("id")) {
            it["id"] = tid(next++);
            autoTemp.push_back(it["id"]);
        }
    auto& s = model_.session();
    s.beginGroup(groupKey);
    auto r = s.sketchAdd(items);
    const bool ok = r.ok();
    if (!ok) {
        emit error(errorText(r.error(), s.document().document.get()));
    } else {
        if (ids) *ids = *r;
        // Entities just drawn — their dimensions are opened by a click from the drawing tool (FX7-SK-008).
        lastDrawn_.clear();
        for (const auto& [t, id] : *r)
            if (!id.empty() && id[0] == 'e') lastDrawn_.insert(id);
        // Redundant/conflicting auto-constraints are removed in the same group (§6.4): the choice is made in the core
        // (sketchYieldAuto, by class among all auto-constraints), one at a time while there is something to remove.
        std::vector<std::string> autoIds;
        for (const auto& t : autoTemp)
            if (auto it = r->find(t); it != r->end()) autoIds.push_back(it->second);
        const auto names = autoConstraintNames();
        QStringList removed;
        for (int guard = 0; guard < 64 && !autoIds.empty(); ++guard) {
            const auto victim = s.sketchYieldAuto(autoIds);
            if (!victim || victim->empty()) break;
            // Only previously existing auto-constraints are reported: new ones simply are not added (FX8-SK-003).
            if (std::find(autoIds.begin(), autoIds.end(), *victim) == autoIds.end())
                if (auto it = names.find(*victim); it != names.end()) removed << it->second;
            std::erase(autoIds, *victim);
        }
        if (!removed.isEmpty()) emit hint(text("ui.sketch.autoYielded", {{"what", removed.join("; ")}}));
    }
    s.endGroup();
    refresh();
    // The solver may satisfy the hints by a degenerate solution — e.g. tangent arcs shrunk to their centers (FX10-SK-004):
    // a curve that existed collapsed — the placement is undone and repeated without its weakest hint (§6.4).
    if (ok) {
        bool collapsed = false;
        const auto after = curveSizes();
        for (const auto& [id, size] : sizesBefore)
            if (const auto it = after.find(id); size > 0 && it != after.end() && it->second < AUTO_COLLAPSE_RATIO * size) collapsed = true;
        if (collapsed) {
            // Class as in sketchYieldAuto (core-api §4.6): equalities first, then alignments, directions and tangencies;
            // within a class the last hint.
            // An equality of radii: its first reference is a drawn or an existing arc / circle.
            auto equalOfRadii = [&](const Json& it) {
                if (!it.contains("refs") || it["refs"].empty()) return false;
                const std::string r = it["refs"][0].get<std::string>();
                for (const auto& x : itemsIn)
                    if (x.value("id", "") == r) return x.value("type", "") == "arc" || x.value("type", "") == "circle";
                const auto* e = data() ? data()->findEntity(r) : nullptr;
                return e && (e->type == sketch::EntityType::Arc || e->type == sketch::EntityType::Circle);
            };
            auto rank = [&](const Json& it) {
                const std::string t = it.value("type", "");
                if (t == "equal") return equalOfRadii(it) ? 1 : -1;  // radii — after the alignments (FX10-SK-004)
                if ((t == "horizontal" || t == "vertical") && it.contains("refs") && it["refs"].size() == 2) return 0;
                if (t == "horizontal" || t == "vertical" || t == "parallel" || t == "perpendicular" || t == "tangent") return 2;
                if (t == "coincident" || t == "pointOnObject") return 4;
                return 3;
            };
            int weakest = -1;
            for (int i = 0; i < int(itemsIn.size()); ++i)
                if (itemsIn[i].value("auto", false) && rank(itemsIn[i]) < 4 && (weakest < 0 || rank(itemsIn[i]) <= rank(itemsIn[weakest])))
                    weakest = i;  // incidence (the snaps of the drawn points) stays
            if (weakest >= 0 && s.undo()) {
                Json fewer = itemsIn;
                fewer.erase(fewer.begin() + weakest);
                refresh();
                return commitItems(fewer, groupKey, ids);
            }
        }
    }
    return ok;
}

std::map<std::string, QString> SketchEditor::autoConstraintNames() const {
    std::map<std::string, QString> out;
    const auto doc = model_.session().document().document;
    if (const auto* d = data())
        for (const auto& c : d->constraints)
            if (c.automatic) {
                QString n = app::objectName(doc.get(), c.id);
                // Alignment with a segment midpoint is named as such (FX8-SK-003).
                if ((c.type == "horizontal" || c.type == "vertical") && c.refs.size() == 2)
                    if (const auto* l = d->findEntity(c.refs[1]); l && l->type != sketch::EntityType::Point)
                        n += " — " + text("ui.sketch.alignMidpoint");
                out[c.id] = n;
            }
    return out;
}

std::vector<doc::Json> SketchEditor::yieldAutoConstraints(const std::vector<std::string>& added) {
    std::vector<Json> removed;
    // Explicit constraints displace auto-constraints of their conflicting/redundant group (§6.4, S1-SK-016): one
    // at a time, starting from the latest, while the new ones are in the group.
    // The choice is made in the core (core-api sketchYieldAuto): one at a time while there are candidates.
    auto& s = model_.session();
    const auto names = autoConstraintNames();
    QStringList what;
    for (int guard = 0; guard < 64; ++guard) {
        Json before;
        if (const auto* d = data())
            for (const auto& c : d->constraints) before[c.id] = Json{{"type", c.type}, {"refs", c.refs}, {"auto", true}};
        const auto victim = s.sketchYieldAuto(added);
        if (!victim || victim->empty()) break;
        if (before.contains(*victim)) removed.push_back(before[*victim]);
        if (auto it = names.find(*victim); it != names.end()) what << it->second;
    }
    refresh();
    // The removed auto-relation is named in the status bar (FX8-SK-003): an auto-relation may have defined the sketch.
    if (!what.isEmpty()) emit hint(text("ui.sketch.autoYielded", {{"what", what.join("; ")}}));
    return removed;
}

void SketchEditor::addPointWithSnap(Json& items, const std::string& tempId, const Point2& p, const Snap& snap) {
    items.push_back(pointItem(tempId, p));
    snapConstraint(items, tempId, snap);
}

void SketchEditor::clickPoint(const QPointF& s, bool suppress) {
    const Snap sn = snapAt(s, suppress);
    if (sn.kind == Snap::Kind::Point && sn.source.empty()) return;  // the point already exists
    Json items = Json::array();
    addPointWithSnap(items, "t1", sn.at, sn);
    commitItems(items, "undo.group.point");
}

// Text (§6.11в): anchor point with snaps, then the text dialog near the cursor.
void SketchEditor::clickText(const QPointF& s, bool suppress) {
    textSnap_ = snapAt(s, suppress);
    textEditing_.clear();
    textBox_->open(s.toPoint(), {"", api::Session::defaultFont(), 5, 0});
}

namespace {
// Text style fragments in the document form (sketch.md §2.1, FX8-SK-007); none — null (the field is cleared).
Json stylesJson(const std::vector<font::StyleSpan>& styles) {
    if (styles.empty()) return nullptr;
    Json out = Json::array();
    for (const auto& sp : styles) {
        Json o{{"from", sp.from}, {"to", sp.to}};
        if (sp.bold) o["bold"] = true;
        if (sp.italic) o["italic"] = true;
        if (sp.underline) o["underline"] = true;
        out.push_back(o);
    }
    return out;
}
}  // namespace

void SketchEditor::textAccepted(const TextBox::Values& v) {
    if (!textEditing_.empty()) {
        Json up = Json::array({{{"id", textEditing_}, {"text", v.text}, {"font", v.font}, {"height", v.heightMm}, {"angle", v.angleRad},
                                {"styles", stylesJson(v.styles)}}});
        if (auto r = model_.session().sketchUpdate(up); !r) emit error(errorText(r.error()));
        textEditing_.clear();
        refresh();
        view_.setFocus();
        return;
    }
    Json items = Json::array();
    if (textSnap_.kind == Snap::Kind::Point && textSnap_.source.empty()) {
        items.push_back(pointItem("t1", textSnap_.at));  // on an existing point — own anchor point and a coincidence
        items.push_back(constraintItem("coincident", {"t1", textSnap_.target}));
    } else {
        addPointWithSnap(items, "t1", textSnap_.at, textSnap_);
    }
    Json t = {{"id", "t2"}, {"type", "text"}, {"anchor", "t1"}, {"text", v.text}, {"font", v.font}, {"height", v.heightMm}};
    if (v.angleRad != 0) t["angle"] = v.angleRad;
    if (!v.styles.empty()) t["styles"] = stylesJson(v.styles);  // FX8-SK-007
    items.push_back(t);
    commitItems(items, "undo.group.text");
    view_.setFocus();
}

void SketchEditor::clickLine(const QPointF& s, bool suppress) {
    Snap sn = snapAt(s, suppress);
    if (clicks_.empty()) {
        clicks_.push_back(sn.at);
        clickSnaps_.push_back(sn);
        return;
    }
    const Point2 a = clicks_.back();
    Point2 b = sn.at;
    if ((toScreen(b) - toScreen(a)).manhattanLength() < MIN_ELEMENT_PX) {
        emit hint(text("ui.status.tooSmall"));
        return;
    }
    Json items = Json::array();
    // Start: chain continuation or snap of the first click.
    items.push_back(pointItem("t1", a));
    if (!chainEndPoint_.empty()) items.push_back(constraintItem("coincident", {"t1", chainEndPoint_}));
    else snapConstraint(items, "t1", clickSnaps_.back());
    // Direction (§6.4 items 4–6) — if the end is not snapped to a point/midpoint/curve.
    DirectionSnap ds;
    // "Onto curve" + segment horizontal/vertical: the end goes to the curve's intersection with the line from start.
    if (sn.kind == Snap::Kind::Curve && !sn.target.empty() && !suppress) {
        const DirectionSnap d0 = directionSnap(a, b);
        if (d0.type == "horizontal" || d0.type == "vertical") {
            const int axis = d0.type == "vertical" ? 0 : 1;
            if (auto q = curveAxisPoint(geo(), sn.target, axis, a[axis], b)) {
                const QPointF qs = toScreen(*q), cs = s;
                if (std::hypot(qs.x() - cs.x(), qs.y() - cs.y()) <= SNAP_CURVE_PX) {
                    ds = d0;
                    b = *q;
                    sn.at = b;
                    sn.alignX.reset(), sn.alignY.reset();
                    sn.alignXId.clear(), sn.alignYId.clear(), sn.alignXSource.clear(), sn.alignYSource.clear();
                }
            }
        }
    }
    if (sn.kind == Snap::Kind::None && !suppress) {
        ds = directionSnap(a, b);
        // Direction — if snapping does not move a point already aligned or placed on an extension (§6.4).
        const bool fixedByHints = sn.alignX || sn.alignY || !sn.extLine.empty();
        if (fixedByHints && (ds.end - b).norm() > 1e-9) ds = DirectionSnap{};
        else b = ds.end;
    }
    // Horizontal/vertical to the basis — always, even with a position snap of the end (§6.4, FX1-SKG-001): the solver
    // finishes the position; an incompatible direction is removed as a redundant auto-constraint.
    if (ds.type.empty() && sn.kind != Snap::Kind::None && !suppress) {
        const DirectionSnap d0 = directionSnap(a, b);
        if (d0.type == "horizontal" || d0.type == "vertical") ds.type = d0.type, ds.alsoTangent = d0.alsoTangent;
        else if (d0.type == "tangent") ds.alsoTangent = d0.other;
        else if (d0.type.empty()) ds.alsoTangent = d0.alsoTangent;
    }
    // A length close to an existing segment's — equal to it (FX7-SK-003); an end snapped to a position stays there.
    const auto eqLen = sn.kind != Snap::Kind::None || suppress || (b - a).norm() <= 0 ? std::nullopt : equalSnap(false, (b - a).norm());
    if (eqLen) b = a + (b - a) * (eqLen->value / (b - a).norm());
    items.push_back(pointItem("t2", b));
    items.push_back(Json{{"id", "t3"}, {"type", "line"}, {"p1", "t1"}, {"p2", "t2"}});
    if (eqLen) items.push_back(constraintItem("equal", {"t3", eqLen->id})), items.back()["auto"] = true;
    // Segment direction goes before the end hints: among constraints with the same meaning the later one is
    // deemed redundant, and the segment keeps its own "horizontal"/"vertical" (§6.4).
    // A direction from a hint is "weak" (auto, S1-SK-016).
    if (ds.type == "horizontal" || ds.type == "vertical") items.push_back(constraintItem(ds.type, {"t3"})), items.back()["auto"] = true;
    else if (!ds.type.empty()) items.push_back(constraintItem(ds.type, {"t3", ds.other})), items.back()["auto"] = true;
    if (!ds.alsoTangent.empty()) items.push_back(constraintItem("tangent", {"t3", ds.alsoTangent})), items.back()["auto"] = true;
    // The end on the end of an arc, entering it smoothly — tangency at the joint, e.g. a contour closed by a segment
    // (§6.4, FX10-SK-004).
    if (!suppress && sn.kind == Snap::Kind::Point && sn.source.empty())
        if (const std::string arc = arcEnteredAt(sn.target, b - a); !arc.empty() && arc != ds.alsoTangent && arc != ds.other)
            items.push_back(constraintItem("tangent", {"t3", arc})), items.back()["auto"] = true;
    snapConstraint(items, "t2", sn);
    std::map<std::string, std::string> ids;
    if (!commitItems(items, "undo.group.line", &ids)) return;
    const bool closed = sn.kind == Snap::Kind::Point && !chainStartPoint_.empty() && sn.target == chainStartPoint_;
    if (chainStartPoint_.empty()) chainStartPoint_ = ids["t1"];
    chainEndPoint_ = ids["t2"];
    chainEndCurve_ = ids["t3"];
    wokenLine_ = ids["t3"];  // the previous chain segment is the woken one
    clicks_ = {b};
    clickSnaps_ = {Snap{}};
    if (closed) resetToolState();
}

void SketchEditor::clickRectangle(const QPointF& s, bool suppress) {
    const Snap sn = snapAt(s, suppress);
    if (clicks_.empty()) {
        clicks_.push_back(sn.at);
        clickSnaps_.push_back(sn);
        return;
    }
    const Point2 a = clicks_[0], c = sn.at;
    const QPointF d = toScreen(c) - toScreen(a);
    if (std::abs(d.x()) < MIN_ELEMENT_PX || std::abs(d.y()) < MIN_ELEMENT_PX) {
        emit hint(text("ui.status.tooSmall"));
        return;
    }
    auto b = api::sketchops::rectangle(a, c);
    snapConstraint(b.items, b.roles["corner0"], clickSnaps_[0]);
    snapConstraint(b.items, b.roles["corner2"], sn);
    Json& items = b.items;
    commitItems(items, "undo.group.rectangle");
    resetToolState();
}

void SketchEditor::clickCircle(const QPointF& s, bool suppress) {
    const Snap sn = snapAt(s, suppress);
    if (clicks_.empty()) {
        clicks_.push_back(sn.at);
        clickSnaps_.push_back(sn);
        return;
    }
    const Point2 c = clicks_[0];
    double r = (sn.at - c).norm();
    if ((toScreen(sn.at) - toScreen(c)).manhattanLength() < MIN_ELEMENT_PX || r <= 0) {
        emit hint(text("ui.status.tooSmall"));
        return;
    }
    // A radius close to an existing one — equal to it (FX7-SK-003); a rim on a point keeps the point.
    const auto eq = sn.kind == Snap::Kind::Point || suppress ? std::nullopt : equalSnap(true, r);
    if (eq) r = eq->value;
    Json items = Json::array();
    addPointWithSnap(items, "t1", c, clickSnaps_[0]);
    items.push_back(Json{{"id", "t2"}, {"type", "circle"}, {"center", "t1"}, {"r", r}});
    if (eq) items.push_back(constraintItem("equal", {"t2", eq->id})), items.back()["auto"] = true;
    // The rim caught a point — the circle passes through it (vision item 16).
    if (sn.kind == Snap::Kind::Point) {
        const std::string through = sn.source.empty() ? sn.target : projectionFor(items, sn.source);
        items.push_back(constraintItem("pointOnObject", {through, "t2"}));
        items.back()["auto"] = true;
    }
    // Center at the center of a projection circle or circular body edge: concentricity (§6.4 item 1в, S1-SK-017).
    if (const Snap& cs = clickSnaps_[0]; cs.kind == Snap::Kind::Center) {
        const std::string target = cs.source.empty() ? cs.target : projectionFor(items, cs.source);
        items.push_back(constraintItem("concentric", {"t2", target}));
        items.back()["auto"] = true;
    }
    commitItems(items, "undo.group.circle");
    resetToolState();
}

namespace {

using geom2::angleOf;
using geom2::ccwSweep;

}  // namespace

void SketchEditor::clickCenterRectangle(const QPointF& s, bool suppress) {
    const Snap sn = snapAt(s, suppress);
    if (clicks_.empty()) {
        clicks_.push_back(sn.at);
        clickSnaps_.push_back(sn);
        return;
    }
    const Point2 c = clicks_[0], k = sn.at;
    const QPointF d = toScreen(k) - toScreen(c);
    if (std::abs(d.x()) < MIN_ELEMENT_PX || std::abs(d.y()) < MIN_ELEMENT_PX) {
        emit hint(text("ui.status.tooSmall"));
        return;
    }
    // Rectangle from center (§6.3) — api::sketchops::centerRectangle.
    auto b = api::sketchops::centerRectangle(c, k);
    snapConstraint(b.items, b.roles["center"], clickSnaps_[0]);
    snapConstraint(b.items, b.roles["corner2"], sn);
    Json& items = b.items;
    commitItems(items, "undo.group.rectangle");
    resetToolState();
}

void SketchEditor::setPolygonTool(bool inscribed) {
    polygonInscribed_ = inscribed;
    setTool(Tool::Polygon);
}

void SketchEditor::setPolygonSides(int n) {
    polygonSides_ = std::clamp(n, 3, 40);
    view_.update();
    emit statusChanged();
}

void SketchEditor::clickCircle3(const QPointF& s, bool suppress) {
    const Snap sn = snapAt(s, suppress);
    clicks_.push_back(sn.at);
    clickSnaps_.push_back(sn);
    if (clicks_.size() < 3) return;
    auto b = api::sketchops::circleThreePoints(clicks_[0], clicks_[1], clicks_[2]);
    if (!b) {
        emit hint(text("ui.status.tooSmall"));
        resetToolState();
        return;
    }
    // Clicks that caught points — the circle passes through them (S1-SK-019).
    for (const Snap& k : clickSnaps_)
        if (k.kind == Snap::Kind::Point) {
            const std::string through = k.source.empty() ? k.target : projectionFor(b->items, k.source);
            b->items.push_back(constraintItem("pointOnObject", {through, b->roles["circle"]}));
            b->items.back()["auto"] = true;
        }
    Json& items = b->items;
    commitItems(items, "undo.group.circle");
    resetToolState();
}

void SketchEditor::clickEllipse(const QPointF& s, bool suppress) {
    // Center → end of the first semi-axis → second semi-axis (app-ux §6.3, S4-SKT-004).
    const Snap sn = snapAt(s, suppress);
    if (clicks_.size() == 1 && (toScreen(sn.at) - toScreen(clicks_[0])).manhattanLength() < MIN_ELEMENT_PX) {
        emit hint(text("ui.status.tooSmall"));
        return;
    }
    if (clicks_.size() < 2) {
        clicks_.push_back(sn.at);
        clickSnaps_.push_back(sn);
        return;
    }
    const Point2 c = clicks_[0], a = clicks_[1];
    const Point2 u = (a - c).normalized();
    const Point2 foot = c + u * (sn.at - c).dot(u);
    if ((toScreen(sn.at) - toScreen(foot)).manhattanLength() < MIN_ELEMENT_PX) {
        emit hint(text("ui.status.tooSmall"));
        return;
    }
    auto b = api::sketchops::ellipse(c, a, sn.at);
    if (!b) return;
    snapConstraint(b->items, b->roles["center"], clickSnaps_[0]);
    // Center at the center of a projection circle or circular body edge: concentricity (as for a circle, §6.4 item 1в).
    if (const Snap& cs = clickSnaps_[0]; cs.kind == Snap::Kind::Center) {
        const std::string target = cs.source.empty() ? cs.target : projectionFor(b->items, cs.source);
        b->items.push_back(constraintItem("concentric", {b->roles["ellipse"], target}));
        b->items.back()["auto"] = true;
    }
    // The third click caught a point — the ellipse passes through it.
    if (sn.kind == Snap::Kind::Point) {
        const std::string through = sn.source.empty() ? sn.target : projectionFor(b->items, sn.source);
        b->items.push_back(constraintItem("pointOnObject", {through, b->roles["ellipse"]}));
        b->items.back()["auto"] = true;
    }
    Json& items = b->items;
    commitItems(items, "undo.group.ellipse");
    resetToolState();
}

void SketchEditor::clickSpline(const QPointF& s, bool suppress) {
    // Points one by one (app-ux §6.3, S4A-SPL-001): a click on the last point finishes an open spline, a click on the
    // first one (≥ 3 points) — a closed one; a double click and Enter finish too.
    const Snap sn = snapAt(s, suppress);
    auto closeTo = [&](const Point2& q) { return (toScreen(sn.at) - toScreen(q)).manhattanLength() < MIN_ELEMENT_PX; };
    if (!clicks_.empty() && closeTo(clicks_.back())) {
        finishSpline(false);
        return;
    }
    if (clicks_.size() >= 3 && closeTo(clicks_.front())) {
        finishSpline(true);
        return;
    }
    clicks_.push_back(sn.at);
    clickSnaps_.push_back(sn);
    view_.update();
}

void SketchEditor::finishSpline(bool closed) {
    if (clicks_.size() < 2) {
        emit hint(text("ui.status.tooSmall"));
        resetToolState();
        return;
    }
    const bool through = tool_ == Tool::SplineThrough;
    std::vector<Point2> pts = clicks_;
    std::vector<Snap> snaps = clickSnaps_;
    // Through points, closed: the last point is the first one again (the ends coincide).
    if (through && closed) pts.push_back(pts.front()), snaps.push_back(snaps.front());
    auto b = through ? api::sketchops::splineThrough(pts) : api::sketchops::splineByPoles(pts, closed);
    if (!b) {
        emit hint(text("ui.status.tooSmall"));
        resetToolState();
        return;
    }
    const std::string sp = b->roles["spline"];
    const size_t nPoles = b->items.size() - 1;
    if (!through) {
        // Each control point caught by a snap is bound to it (§6.4).
        for (size_t i = 0; i < snaps.size() && i < nPoles; ++i) snapConstraint(b->items, b->roles["pole" + std::to_string(i)], snaps[i]);
    } else {
        // The ends are poles; interior points caught on a sketch point — the spline passes through it.
        snapConstraint(b->items, b->roles["pole0"], snaps.front());
        const std::string last = b->roles["pole" + std::to_string(nPoles - 1)];
        if (closed) b->items.push_back(constraintItem("coincident", {b->roles["pole0"], last})), b->items.back()["auto"] = true;
        else snapConstraint(b->items, last, snaps.back());
        for (size_t i = 1; i + 1 < snaps.size(); ++i)
            if (snaps[i].kind == Snap::Kind::Point) {
                const std::string on = snaps[i].source.empty() ? snaps[i].target : projectionFor(b->items, snaps[i].source);
                b->items.push_back(constraintItem("pointOnObject", {on, sp}));
                b->items.back()["auto"] = true;
            }
    }
    Json& items = b->items;
    commitItems(items, "undo.group.spline");
    resetToolState();
}

void SketchEditor::clickArc3(const QPointF& s, bool suppress) {
    Snap sn = snapAt(s, suppress);
    if (clicks_.size() == 2)
        if (const auto raw = toLocal(s)) sn = arcThroughPointSnap(sn, *raw);  // the third point: a point snap only (§6.3)
    clicks_.push_back(sn.at);
    clickSnaps_.push_back(sn);
    if (clicks_.size() < 3) return;
    // Start, end, point on the arc (§6.3); in the data an arc is always counter-clockwise (sketch.md §2.1).
    const Point2 a = clicks_[0], bpt = clicks_[1];
    auto b = api::sketchops::arcThreePoints(a, bpt, clicks_[2]);
    if (!b || (toScreen(a) - toScreen(bpt)).manhattanLength() < MIN_ELEMENT_PX) {
        emit hint(text("ui.status.tooSmall"));
        resetToolState();
        return;
    }
    snapConstraint(b->items, b->roles["a"], clickSnaps_[0]);
    snapConstraint(b->items, b->roles["b"], clickSnaps_[1]);
    // Ends on the end of a line / arc with the arc's tangent close to its direction — tangency (FX7-SK-002); a radius
    // close to an existing one — equality (FX7-SK-003); the solver finishes the position.
    if (!suppress) {
        const auto center = api::sketchops::circumcenter(a, bpt, clicks_[2]);
        for (const auto& t : arc3Tangents(a, bpt, clicks_[2])) b->items.push_back(constraintItem("tangent", {t, b->roles["arc"]})), b->items.back()["auto"] = true;
        if (center)
            if (const auto eq = equalSnap(true, (a - *center).norm(), false, snap::arc3RadiusSensitivity(a, bpt, clicks_[2]))) {
                b->items.push_back(constraintItem("equal", {b->roles["arc"], eq->id})), b->items.back()["auto"] = true;
                // The arc is drawn with the sample's radius through the same ends (its center moves along the chord's
                // bisector, on its side): the solver starts from a consistent arc — otherwise it may satisfy the
                // tangencies and equalities of a rounded contour by shrinking all arcs (FX10-SK-004).
                const Point2 mid = (a + bpt) / 2, chord = bpt - a;
                const double h2 = eq->value * eq->value - chord.squaredNorm() / 4;
                if (h2 >= 0 && chord.norm() > 0) {
                    Point2 n(-chord.y(), chord.x());
                    n.normalize();
                    if (n.dot(*center - mid) < 0) n = -n;
                    const Point2 c2 = mid + std::sqrt(h2) * n;
                    for (auto& it : b->items)
                        if (it.value("id", "") == b->roles["center"]) it["x"] = c2.x(), it["y"] = c2.y();
                }
            }
    }
    // The third point caught a point — the arc passes through it (vision item 16).
    if (const Snap& m = clickSnaps_[2]; m.kind == Snap::Kind::Point) {
        const std::string through = m.source.empty() ? m.target : projectionFor(b->items, m.source);
        b->items.push_back(constraintItem("pointOnObject", {through, b->roles["arc"]}));
        b->items.back()["auto"] = true;
    }
    Json& items = b->items;
    commitItems(items, "undo.group.arc");
    resetToolState();
}

void SketchEditor::clickArcCenter(const QPointF& s, bool suppress) {
    const Snap sn = snapAt(s, suppress);
    if (clicks_.size() < 2) {
        clicks_.push_back(sn.at);
        clickSnaps_.push_back(sn);
        arcSweep_ = 0;
        lastArcAngle_.reset();
        if (clicks_.size() == 2) lastArcAngle_ = angleOf(clicks_[0], clicks_[1]);
        return;
    }
    const Point2 c = clicks_[0];
    Point2 start = clicks_[1];
    double r = (start - c).norm();
    if ((toScreen(start) - toScreen(c)).manhattanLength() < MIN_ELEMENT_PX) {
        emit hint(text("ui.status.tooSmall"));
        resetToolState();
        return;
    }
    // A radius close to an existing one — equal to it (FX7-SK-003); a start on a snapped point stays there.
    const auto eq = clickSnaps_[1].kind != Snap::Kind::None || suppress ? std::nullopt : equalSnap(true, r);
    if (eq) start = c + (start - c) * (eq->value / r), r = eq->value;
    // End — by the cursor angle, radius fixed; direction — by the sign of the sweep around the center (§6.3).
    const double ae = angleOf(c, sn.at);
    Point2 end = c + r * Point2(std::cos(ae), std::sin(ae));
    // The end caught a point/curve near the arc's circle — the end goes there, with all snaps (vision item 16).
    Snap sEnd;
    if (sn.kind != Snap::Kind::None && sn.kind != Snap::Kind::Center) {
        const QPointF on = toScreen(end), at = toScreen(sn.at);
        if (std::hypot(on.x() - at.x(), on.y() - at.y()) <= SNAP_POINT_PX) end = sn.at, sEnd = sn;
    }
    if (lastArcAngle_) {
        double d = ae - *lastArcAngle_;
        while (d > PI) d -= 2 * PI;
        while (d < -PI) d += 2 * PI;
        arcSweep_ += d;
    }
    auto b = api::sketchops::arcCenterStartEnd(c, start, end, arcSweep_ >= 0);
    snapConstraint(b.items, b.roles["center"], clickSnaps_[0]);
    snapConstraint(b.items, b.roles["start"], clickSnaps_[1]);
    snapConstraint(b.items, b.roles["end"], sEnd);
    if (eq) b.items.push_back(constraintItem("equal", {b.roles["arc"], eq->id})), b.items.back()["auto"] = true;
    // Ends on the ends of lines / arcs, tangent there — tangency at the joint (§6.4, FX10-SK-004).
    if (!suppress)
        for (const auto& t : arcEndTangents(c, start, clickSnaps_[1], end, sEnd))
            b.items.push_back(constraintItem("tangent", {t, b.roles["arc"]})), b.items.back()["auto"] = true;
    Json& items = b.items;
    commitItems(items, "undo.group.arc");
    resetToolState();
}

bool SketchEditor::createTangentArc(const std::string& fromPoint, const std::string& fromCurve, const Point2& tangent,
                                    Point2 end, const Snap& endSnap, std::string* newEnd) {
    const Point2 p = pointPos(fromPoint);
    auto b = api::sketchops::tangentArc(p, tangent, end, fromPoint, fromCurve);
    if (!b || (toScreen(end) - toScreen(p)).manhattanLength() < MIN_ELEMENT_PX) {
        emit hint(text("ui.status.tooSmall"));
        return false;
    }
    const std::string atEnd = b->roles["end"];
    snapConstraint(b->items, atEnd, endSnap);
    Json& items = b->items;
    // Center at an existing point or the origin — coincidence (hint, §6.3).
    {
        const Point2 c(items[0]["x"].get<double>(), items[0]["y"].get<double>());
        const QPointF cs = toScreen(c);
        std::string near;
        double best = SNAP_POINT_PX;
        auto consider = [&](const std::string& id, const Point2& q) {
            const QPointF qs = toScreen(q);
            const double dd = std::hypot(qs.x() - cs.x(), qs.y() - cs.y());
            if (dd <= best) best = dd, near = id;
        };
        consider("origin", Point2::Zero());
        for (const auto& e : data()->entities)
            if (e.type == sketch::EntityType::Point) consider(e.id, pointPos(e.id));
        if (!near.empty()) {
            items.push_back(constraintItem("coincident", {items[0]["id"].get<std::string>(), near}));
            items.back()["auto"] = true;
        }
    }
    // End at the end of another segment/arc tangent to the arc there — tangency as well (hint, §6.3).
    if (endSnap.kind == Snap::Kind::Point && !endSnap.target.empty() && endSnap.target != "origin") {
        const Point2 c(items[0]["x"].get<double>(), items[0]["y"].get<double>());
        const Point2 rad = end - c;
        const Point2 arcDir = Point2(-rad.y(), rad.x()).normalized();
        for (const auto& e : data()->entities) {
            if (e.id == fromCurve) continue;
            std::optional<Point2> dir;
            if (e.type == sketch::EntityType::Line && (e.p1 == endSnap.target || e.p2 == endSnap.target))
                dir = (pointPos(e.p2) - pointPos(e.p1)).normalized();
            if (e.type == sketch::EntityType::Arc && (e.start == endSnap.target || e.end == endSnap.target)) {
                const Point2 r2 = end - pointPos(e.center);
                dir = Point2(-r2.y(), r2.x()).normalized();
            }
            if (!dir || rad.squaredNorm() <= 0) continue;
            if (std::abs(arcDir.x() * dir->y() - arcDir.y() * dir->x()) <= std::sin(SNAP_TANGENT_DEG * PI / 180)) {
                items.push_back(constraintItem("tangent", {e.id, b->roles["arc"]}));
                items.back()["auto"] = true;
            }
        }
    }
    std::map<std::string, std::string> ids;
    if (!commitItems(items, "undo.group.arc", &ids)) return false;
    if (newEnd) *newEnd = ids[atEnd];
    chainEndCurve_ = ids[b->roles["arc"]];
    wokenLine_.clear();  // the previous chain segment is an arc
    return true;
}

void SketchEditor::clickTangentArc(const QPointF& s, bool suppress) {
    const auto* d = data();
    if (chainEndPoint_.empty()) {
        // The first click is the end of an existing segment or arc (§6.3).
        const Snap sn = snapAt(s, false);
        if (sn.kind != Snap::Kind::Point || sn.target.empty() || sn.target == "origin") return;
        for (const auto& e : d->entities) {
            if (e.type == sketch::EntityType::Line && (e.p1 == sn.target || e.p2 == sn.target)) chainEndCurve_ = e.id;
            if (e.type == sketch::EntityType::Arc && (e.start == sn.target || e.end == sn.target)) chainEndCurve_ = e.id;
        }
        if (chainEndCurve_.empty()) return;
        chainEndPoint_ = sn.target;
        return;
    }
    const auto tg = chainTangent();
    if (!tg) {
        resetToolState();
        return;
    }
    const Point2 tangent = *tg;
    const Snap sn = snapAt(s, suppress, {chainEndPoint_});
    std::string newEnd;
    const std::string fromCurve = chainEndCurve_;
    if (!createTangentArc(chainEndPoint_, fromCurve, tangent, sn.at, sn, &newEnd)) return;
    // End at the chain start — closure: the chain is finished (§6.3).
    const bool closed = sn.kind == Snap::Kind::Point && !chainStartPoint_.empty() && sn.target == chainStartPoint_;
    if (returnToLine_ && closed) {
        tool_ = Tool::Line;
        returnToLine_ = false;
        resetToolState();
        emit statusChanged();
        return;
    }
    if (returnToLine_) {
        // From a chain of segments: after the arc — a segment again from its end (§6.3).
        tool_ = Tool::Line;
        returnToLine_ = false;
        chainEndPoint_ = newEnd;
        clicks_ = {pointPos(newEnd)};
        clickSnaps_ = {Snap{}};
        emit statusChanged();
        return;
    }
    resetToolState();
}

std::optional<Point2> SketchEditor::chainTangent() const {
    const auto* d = data();
    const auto* e = d && !chainEndCurve_.empty() ? d->findEntity(chainEndCurve_) : nullptr;
    if (!e || chainEndPoint_.empty()) return std::nullopt;
    // Tangent at the curve end pointing "forward" (outwards from the curve).
    if (e->type == sketch::EntityType::Line)
        return e->p2 == chainEndPoint_ ? Point2(pointPos(e->p2) - pointPos(e->p1)) : Point2(pointPos(e->p1) - pointPos(e->p2));
    if (e->type != sketch::EntityType::Arc) return std::nullopt;
    const Point2 c = pointPos(e->center), q = pointPos(chainEndPoint_) - c;
    const Point2 ccwDir(-q.y(), q.x());  // direction of increasing angle
    return e->end == chainEndPoint_ ? ccwDir : Point2(-ccwDir);
}

std::vector<std::vector<QPointF>> SketchEditor::rubberPolylines(const Point2& cur) const {
    std::vector<std::vector<QPointF>> out;
    auto poly = [&](std::initializer_list<Point2> pts) {
        std::vector<QPointF> v;
        for (const auto& q : pts) v.push_back(toScreen(q));
        out.push_back(std::move(v));
    };
    // Circle arc (center, radius, start angle, signed sweep) — as points in the sketch plane.
    auto arc = [&](const Point2& c, double r, double a0, double sweep) {
        std::vector<QPointF> v;
        const int n = std::max(8, int(std::abs(sweep) / (2 * PI) * 96));
        for (int i = 0; i <= n; ++i) v.push_back(toScreen(c + r * Point2(std::cos(a0 + sweep * i / n), std::sin(a0 + sweep * i / n))));
        out.push_back(std::move(v));
    };
    const Point2 a = clicks_.empty() ? cur : clicks_.front();
    switch (tool_) {
        case Tool::Line:
            if (!clicks_.empty()) poly({clicks_.back(), cur});
            break;
        case Tool::Rectangle:
            poly({a, Point2(cur.x(), a.y()), cur, Point2(a.x(), cur.y()), a});
            break;
        case Tool::CenterRectangle: {
            const Point2 dv = cur - a;
            poly({a - dv, Point2(a.x() + dv.x(), a.y() - dv.y()), a + dv, Point2(a.x() - dv.x(), a.y() + dv.y()), a - dv});
            break;
        }
        case Tool::Circle: arc(a, (cur - a).norm(), 0, 2 * PI); break;
        case Tool::Ellipse:  // S4-SKT-004: axis, then the ellipse through the cursor
            if (clicks_.size() == 1) poly({a, cur});
            else if (clicks_.size() == 2)
                if (auto b = api::sketchops::ellipse(a, clicks_[1], cur)) {
                    const auto& it = b->items[1];
                    std::vector<QPointF> v;
                    for (const auto& q : ellipsePolyline(a, {it["rx"].get<double>(), it["ry"].get<double>(), it["angle"].get<double>()}, 96))
                        v.push_back(toScreen(q));
                    out.push_back(std::move(v));
                }
            break;
        case Tool::SplineControl:
        case Tool::SplineThrough:  // S4A-SPL-001: control polygon and the curve with the cursor as the next point
            if (!clicks_.empty()) {
                std::vector<Point2> pts = clicks_;
                pts.push_back(cur);
                std::vector<QPointF> poly;
                for (const auto& q : pts) poly.push_back(toScreen(q));
                if (tool_ == Tool::SplineControl) out.push_back(poly);
                auto b = tool_ == Tool::SplineThrough ? api::sketchops::splineThrough(pts) : api::sketchops::splineByPoles(pts, false);
                if (b) {
                    // The batch: points t1…tn, then the spline (sketchops.splineByPoles / splineThrough).
                    std::map<std::string, Point2> at;
                    sketch::Entity e;
                    for (const auto& it : b->items) {
                        if (it["type"] == "point") {
                            at[it["id"].get<std::string>()] = Point2(it["x"].get<double>(), it["y"].get<double>());
                            continue;
                        }
                        e.type = it["type"] == "bezier" ? sketch::EntityType::Bezier : sketch::EntityType::BSpline;
                        for (const auto& q : it["poles"]) e.poles.push_back(q.get<std::string>());
                        if (it.contains("degree")) e.degree = it["degree"].get<int>();
                        if (it.contains("knots")) e.knots = it["knots"].get<std::vector<double>>();
                        if (it.contains("mults")) e.mults = it["mults"].get<std::vector<int>>();
                    }
                    std::vector<QPointF> v;
                    for (const auto& q : splinePolyline(e, [&](const std::string& id) { return at[id]; })) v.push_back(toScreen(q));
                    out.push_back(std::move(v));
                }
            }
            break;
        case Tool::Polygon:
            if (!clicks_.empty())
                if (auto b = api::sketchops::polygon(a, cur, polygonSides_, polygonInscribed_)) {
                    std::vector<Point2> pts;
                    for (const auto& it : b->items)
                        if (it["type"] == "point" && it["id"] != b->roles.at("center")) pts.push_back(Point2(it["x"].get<double>(), it["y"].get<double>()));
                    std::vector<QPointF> v;
                    for (size_t i = 0; i < pts.size(); i += 2) v.push_back(toScreen(pts[i]));
                    if (!v.empty()) v.push_back(v.front());
                    out.push_back(std::move(v));
                }
            break;
        case Tool::Star:  // S4-SKT-003: star outline through the cursor (group generator)
            if (!clicks_.empty() && (cur - a).norm() > 0)
                if (const auto* d = data()) {
                    const Point2 v = cur - a;
                    const doc::Json prm = {{"rays", starRays_}, {"d1", lengthText(2 * v.norm())}, {"d2", lengthText(v.norm())},
                                           {"x", a.x()}, {"y", a.y()}, {"angle", std::atan2(v.y(), v.x())}};
                    if (auto items = api::sketchops::groupItems(*d, "star", {}, prm, model_.session().units(), model_.session().variableValues().env())) {
                        std::vector<QPointF> pl;
                        for (int j = 0; j <= 2 * starRays_; ++j)
                            for (const auto& it : *items)
                                if (it.entity && it.role == "edge" + std::to_string(j % (2 * starRays_)) + "/p1")
                                    pl.push_back(toScreen(Point2(it.entity->x, it.entity->y)));
                        out.push_back(std::move(pl));
                    }
                }
            break;
        case Tool::Circle3:
            if (clicks_.size() == 1) poly({a, cur});
            else if (clicks_.size() == 2) {
                if (const auto c = api::sketchops::circumcenter(a, clicks_[1], cur)) arc(*c, (a - *c).norm(), 0, 2 * PI);
                else poly({a, clicks_[1]});
            }
            break;
        case Tool::Arc3:
            if (clicks_.size() == 1) poly({a, cur});
            else if (clicks_.size() == 2) {
                const Point2 b = clicks_[1];
                if (const auto c = api::sketchops::circumcenter(a, b, cur)) {
                    const double r = (a - *c).norm(), aa = angleOf(*c, a), ab = angleOf(*c, b), am = angleOf(*c, cur);
                    // Arc from start to end through the cursor: counter-clockwise if the cursor is on this side.
                    if (ccwSweep(aa, am) <= ccwSweep(aa, ab)) arc(*c, r, aa, ccwSweep(aa, ab));
                    else arc(*c, r, aa, -ccwSweep(ab, aa));
                } else {
                    poly({a, b});
                }
            }
            break;
        case Tool::ArcCenter:
            if (clicks_.size() == 1) {
                poly({a, cur});
                arc(a, (cur - a).norm(), 0, 2 * PI);
            } else if (clicks_.size() == 2) {
                const double r = (clicks_[1] - a).norm();
                arc(a, r, angleOf(a, clicks_[1]), std::clamp(arcSweep_, -2 * PI, 2 * PI));
            }
            break;
        case Tool::TangentArc:
            if (const auto tg = chainTangent()) {
                const Point2 p = pointPos(chainEndPoint_), t = tg->normalized(), n(-t.y(), t.x()), v = cur - p;
                const double den = 2 * v.dot(n);
                if (std::abs(den) < 1e-12) {
                    poly({p, cur});
                } else {
                    const double sgn = v.squaredNorm() / den;
                    const Point2 c = p + sgn * n;
                    const double r = std::abs(sgn), ap = angleOf(c, p), ae = angleOf(c, cur);
                    if (sgn > 0) arc(c, r, ap, ccwSweep(ap, ae));
                    else arc(c, r, ap, -ccwSweep(ae, ap));
                }
            }
            break;
        default: break;
    }
    return out;
}

void SketchEditor::toggleConstruction() {
    const auto* d = data();
    std::vector<std::string> curves;
    bool anyNormal = false;
    for (const auto& id : selection_)
        if (const auto* e = d->findEntity(id); e && e->type != sketch::EntityType::Point) {
            if (e->type == sketch::EntityType::Projected && e->source.rfind("sketch:", 0) == 0) continue;  // axis: always auxiliary
            curves.push_back(id);
            anyNormal = anyNormal || !e->construction;
        }
    if (curves.empty()) {  // «Construction geometry» mode (§6.3, FX1-UI-005, FX7-SK-001)
        setConstructionMode(!drawConstruction_);
        return;
    }
    setCurvesConstruction(curves, anyNormal);
}

void SketchEditor::setConstructionMode(bool on) {
    if (drawConstruction_ == on) return;
    drawConstruction_ = on;
    // Without a drawing tool the mode is Tool::Construction (a click toggles an element), otherwise the tool stays.
    if (on && tool_ == Tool::Select) setTool(Tool::Construction);
    else if (!on && tool_ == Tool::Construction) setTool(Tool::Select);
    emit statusChanged();
}

void SketchEditor::setCurvesConstruction(const std::vector<std::string>& curves, bool construction) {
    Json changes = Json::array();
    for (const auto& id : curves) changes.push_back({{"id", id}, {"construction", construction}});
    auto& s = model_.session();
    s.beginGroup("undo.group.construction");
    auto r = s.sketchUpdate(changes);
    // Simplification without changing the drawing (§6.3, FX7-SK-001): a curve that now lies on the same path as another
    // one (construction under a real one, two construction ones) merges into it, in the same undo step.
    bool kept = false;
    if (r)
        for (const auto& id : curves) {
            auto m = s.sketchMergeDuplicate(id);
            if (m && m->refused) kept = true;
        }
    s.endGroup();
    if (!r) emit error(errorText(r.error(), s.document().document.get()));
    else if (kept) emit hint(text("ui.construction.keptDuplicate"));
    refresh();
}

void SketchEditor::clickConstructionMode(const QPointF& s) {
    // «Construction geometry» mode (§6.3, FX1-UI-005): a click on a curve toggles its construction flag.
    const Hit h = hitAt(s);
    const auto* d = data();
    const auto* e = d && h.kind == Hit::Kind::Curve ? d->findEntity(h.id) : nullptr;
    if (!e || e->type == sketch::EntityType::Point) return;
    if (e->type == sketch::EntityType::Projected && e->source.rfind("sketch:", 0) == 0) return;  // axis: always auxiliary
    setCurvesConstruction({e->id}, !e->construction);
}

std::optional<std::pair<Point2, Point2>> SketchEditor::localBounds() const {
    // Extent of the sketch geometry (§6.1, FX1-UI-010).
    const auto* d = data();
    if (!d || !state_) return std::nullopt;
    const auto g = geo();
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    auto add = [&](const Point2& p) {
        x0 = std::min(x0, p.x()), y0 = std::min(y0, p.y()), x1 = std::max(x1, p.x()), y1 = std::max(y1, p.y());
    };
    for (const auto& e : d->entities) {
        switch (e.type) {
            case sketch::EntityType::Point: add(g.pointPos(e.id)); break;
            case sketch::EntityType::Circle:
            case sketch::EntityType::Arc: {
                const Point2 c = g.pointPos(e.center);
                const double r = g.radius(e);
                add(c - Point2(r, r)), add(c + Point2(r, r));
                break;
            }
            case sketch::EntityType::Text:
                for (const auto& pl : textPolylines(e, g.pointPos(e.center)))
                    for (const auto& q : pl) add(q);
                break;
            case sketch::EntityType::Ellipse:
            case sketch::EntityType::EllipseArc:
                if (const auto el = g.ellipseOf(e.id)) {
                    const double m = std::max(el->second.rx, el->second.ry);
                    add(el->first - Point2(m, m)), add(el->first + Point2(m, m));
                }
                break;
            case sketch::EntityType::Projected: {
                if (e.source.rfind("sketch:", 0) == 0) break;  // sketch axes are infinite for fitting
                if (const auto le = g.lineEnds(e.id)) add(le->first), add(le->second);
                else if (const auto ci = g.circleOf(e.id)) add(ci->first - Point2(ci->second, ci->second)), add(ci->first + Point2(ci->second, ci->second));
                else if (const auto it = state_->projections.find(e.id); it != state_->projections.end() && it->second.kind == sketch::Projection::Kind::Point)
                    add(Point2(it->second.px, it->second.py));
                break;
            }
            default: break;
        }
    }
    if (x0 > x1) return std::nullopt;
    return std::pair{Point2(x0, y0), Point2(x1, y1)};
}

std::optional<std::string> SketchEditor::dimensionKind(const std::vector<std::string>& ids, const QPointF& at) const {
    const auto* d = data();
    if (!d) return std::nullopt;
    auto view = [&](const Point2& p) {
        const QPointF q = toScreen(p);
        return Point2(q.x(), q.y());
    };
    return api::sketchops::dimensionKind(dataWithProjections(), [&](const std::string& id) { return pointPos(id); }, ids, Point2(at.x(), at.y()), view,
                                         PARALLEL_DIM_TOL_DEG);
}

ToolTarget SketchEditor::toolTargetAt(const QPointF& s) const {
    const auto* d = data();
    return app::toolTargetAt(tool_ == Tool::Dimension, picked_,
                             {[&] { return hitAt(s); }, [&](const std::string& id) { return d ? d->findEntity(id) : nullptr; },
                              [&] { return view_.pickAt(s.toPoint(), false, true, false); }, [&] { return bodyTargetAt(s); },
                              [&] { return view_.refAxisAt(s.toPoint()); }, [&] { return axisAt(s); }});
}

std::string SketchEditor::toolHoverId() const { return toolTargetId(toolHover_); }

void SketchEditor::clickDimension(const QPointF& s) {
    // Clicks on objects accumulate the selection, a click on a place dimensions the selection there (§6.9, FX2-SK-002).
    // The target — the same choice as the hover highlight (FX9-SK-006).
    Hit h = toolTargetAt(s).hit;
    const std::string pick = h.id;
    // Nothing picked yet: a click on a dimension's label (regular or weak) selects (highlights) it, so that Del deletes
    // it without leaving the tool (app-ux §6.7, FX9-SK-003), and shows its bar (S7F-SK-003).
    if (h.kind == Hit::Kind::Label && picked_.empty()) {
        const auto* c = isWeak(h.id) ? displayData()->findConstraint(h.id) : data() ? data()->findConstraint(h.id) : nullptr;
        if (c && sketch::isDimensionType(c->type)) {
            if (inlineEdit_.isOpen() && !inlineEdit_.finish(true)) return;
            selection_ = {h.id};
            dimAutoSelected_ = false;
            view_.update();
            emit statusChanged();
            emit selectionPicked(s);
            return;
        }
    }
    if (!selection_.empty()) selection_.clear();  // any other click of the tool drops the selected dimension
    if (h.kind == Hit::Kind::Point || h.kind == Hit::Kind::Curve) {
        // The open field of the previous dimension is applied; the object is added to the selection.
        if (inlineEdit_.isOpen() && !inlineEdit_.finish(true)) return;
        const auto next = addDimensionPick(picked_, pick);
        if (next.size() < picked_.size() || (next.size() == 1 && next != picked_)) pickClicks_.clear();
        if (next != picked_) pickClicks_.push_back(s);
        picked_ = next;
        restartDimensionPreview();
        view_.update();
        return;
    }
    // A constraint icon or another dimension's label is not picked by the «Dimension» tool: a click on them
    // places the dimension, as on an empty place (§6.9).
    if (h.kind != Hit::Kind::None && h.kind != Hit::Kind::Label) return;
    if (inlineEdit_.isOpen() && !inlineEdit_.finish(true)) return;
    if (!data() || !dimensionPlaceable(dataWithProjections(), picked_)) return;  // nothing or one point: wait
    dimPreview_.reset();
    createDimension(s, s);
}

bool SketchEditor::resolveBodyPicks() { return resolveBodyPicks(picked_); }

bool SketchEditor::resolveBodyPicks(std::vector<std::string>& ids) {
    // Body edge/vertex labels → construction projections (an existing one with the same source is used, §6.9).
    for (auto& id : ids) {
        const bool v = id.rfind("srcv:", 0) == 0, e = id.rfind("srce:", 0) == 0;
        if (!v && !e) continue;
        const std::string source = id.substr(5);
        std::string found;
        if (const auto* d = data())
            for (const auto& en : d->entities)
                if (en.type == sketch::EntityType::Projected && en.source == source) found = en.id;
        if (found.empty()) {
            auto r = model_.session().sketchAdd(Json::array({Json{{"id", "t1"}, {"type", "projected"}, {"source", source}, {"construction", true}}}));
            if (!r) {
                emit error(errorText(r.error()));
                return false;
            }
            found = r->at("t1");
            refresh();
        }
        id = found;
    }
    return true;
}

void SketchEditor::createDimension(const std::optional<QPointF>& kindAt, const QPointF& placeAt) {
    // Projections of body edges/vertices and the dimension itself — one undo step.
    auto& session = model_.session();
    session.beginGroup("undo.group.dimension");
    struct GroupEnd {
        api::Session& s;
        ~GroupEnd() { s.endGroup(); }
    } groupEnd{session};
    if (!resolveBodyPicks()) {
        picked_.clear(), pickClicks_.clear();
        return;
    }
    // Type: by the position kindAt; without it — default (a point outside both ranges, §6.9).
    const auto kind = dimensionKind(picked_, kindAt.value_or(QPointF(1e9, 1e9)));
    if (!kind) {
        picked_.clear(), pickClicks_.clear();
        return;
    }
    const std::string& k = *kind;
    const auto plan = api::sketchops::dimensionFor(dataWithProjections(), [&](const std::string& id) { return pointPos(id); },
                                                   state_->result.state.radii, picked_, k, toLocal(placeAt));
    if (!plan) {
        picked_.clear(), pickClicks_.clear();
        return;
    }
    const std::vector<std::string>& refs = plan->refs;
    const bool angle = plan->angle;
    const auto lbl = toLocal(placeAt).value_or(Point2::Zero());
    // The dimension is placed as reference, its value is the solver's measurement, then it becomes driving: placement
    // does not move the geometry (§6.9, FX1-SKI-008).
    Json item = constraintItem(k, refs);
    item["id"] = "t1";
    item["value"] = angle ? angleText(plan->value) : plan->number ? formatNumber(plan->value).toStdString() : lengthText(plan->value);
    item["driving"] = false;
    {  // label relative to the dimension anchor (FX8-SK-004)
        sketch::Constraint probe;
        probe.type = k, probe.refs = refs;
        if (const auto off = api::sketchops::labelOffsetFor(dataWithProjections(), state_->result.state, probe, lbl))
            item["labelOffset"] = Json::array({(*off)[0], (*off)[1]});
    }
    if (!plan->arc.empty()) item["arc"] = plan->arc;
    if (plan->side != 0) item["branch"] = Json{{"side", plan->side}};
    auto& s = model_.session();
    s.beginGroup("undo.group.dimension");
    auto r = s.sketchAdd(Json::array({item}));
    std::string cid;
    bool madeDriven = false;
    std::vector<Json> yielded;
    if (!r) {
        emit error(errorText(r.error(), s.document().document.get()));
    } else {
        cid = r->at("t1");
        double measured = plan->value;
        if (auto st0 = s.sketchState(feature_))
            if (auto it = st0->result.reference.find(cid); it != st0->result.reference.end()) measured = it->second;
        const bool signedKind = k == "distanceX" || k == "distanceY";
        const std::string valueText = angle            ? angleText(measured)
                                      : plan->number ? formatNumber(measured).toStdString()
                                                     : lengthText(signedKind ? measured : std::abs(measured));
        if (auto u = s.sketchUpdate(Json::array({{{"id", cid}, {"driving", true}, {"value", valueText}}})); !u)
            emit error(errorText(u.error(), s.document().document.get()));
        yielded = yieldAutoConstraints({cid});
        auto st = s.sketchState(feature_);
        if (st) {
            bool over = false;
            for (const auto& g : st->result.conflicts)
                if (std::find(g.ids.begin(), g.ids.end(), cid) != g.ids.end()) over = true;
            if (std::find(st->result.redundant.begin(), st->result.redundant.end(), cid) != st->result.redundant.end()) over = true;
            if (over) {
                s.sketchUpdate(Json::array({{{"id", cid}, {"driving", false}}}));
                madeDriven = true;
            }
        }
    }
    s.endGroup();
    picked_.clear(), pickClicks_.clear();
    refresh();
    if (!cid.empty()) {
        const QString dimName = "D" + QString::fromStdString(cid.substr(1));
        if (madeDriven) emit this->madeDriven(text("ui.dimension.madeDriven", {{"dim", dimName}}), QString::fromStdString(cid));
        else openInlineEdit(cid);
        if (!madeDriven && !yielded.empty())
            emit yieldedAuto(text("ui.dimension.yieldedAuto", {{"dim", dimName}, {"count", QString::number(yielded.size())}}),
                             QString::fromStdString(cid), QList<Json>(yielded.begin(), yielded.end()));
        // The new dimension is selected and its bar is shown right away — «Diameter / radius from axis» and Shift+D without
        // leaving the tool (app-ux §6.7, S7F-SK-002).
        if (tool_ == Tool::Dimension) {
            selection_ = {cid};
            dimAutoSelected_ = true;
            emit statusChanged();
            emit selectionPicked(placeAt);
        }
    }
}

void SketchEditor::applyConstraint(const std::string& type) {
    const auto* d = data();
    if (!d) return;
    // Body edge/vertex labels → projections in the same command group as the constraint (§6.8).
    std::vector<std::string> sel(selection_.begin(), selection_.end());
    auto& s0 = model_.session();
    const bool body = std::any_of(sel.begin(), sel.end(), [](const std::string& id) { return id.rfind("src", 0) == 0; });
    if (body) s0.beginGroup("undo.group.constraint");
    struct GroupEnd {
        api::Session& s;
        bool on;
        ~GroupEnd() { if (on) s.endGroup(); }
    } groupEnd{s0, body};
    if (body && !resolveBodyPicks(sel)) return;
    d = data();
    // Projection kinds — by their geometry (sketch.md §2.5): a vertex projection is a point, not a curve; without
    // geometry «Coincident» with a vertex projection became "point on curve" and was rejected by the
    // core (user finding 2026-10-15).
    const auto plan = api::sketchops::constraintsForSelection(dataWithProjections(), type, sel);
    switch (plan.outcome) {
        case api::sketchops::ConstraintOutcome::PickEntities: setTool(Tool::Constraint, type); return;
        case api::sketchops::ConstraintOutcome::FixArcUnsupported: emit hint(text("ui.status.fixArc")); return;
        case api::sketchops::ConstraintOutcome::AlreadyExists: {
            // An explicit placement of an existing auto-constraint strengthens it (§6.4, S1-SK-016).
            Json up = Json::array();
            for (const auto& c : d->constraints)
                if (c.automatic && c.type == type) {
                    std::set<std::string> refs(c.refs.begin(), c.refs.end());
                    bool all = !selection_.empty();
                    for (const auto& id : selection_) all = all && refs.count(id);
                    if (all) up.push_back({{"id", c.id}, {"auto", false}});
                }
            if (up.empty()) emit hint(text("ui.status.constraintExists"));
            else model_.session().sketchUpdate(up), refresh();
            return;
        }
        case api::sketchops::ConstraintOutcome::Items: break;
    }
    Json filtered = plan.items;
    int nextTemp = 1;
    for (auto& it : filtered) it["id"] = tid(nextTemp++);
    auto& s = model_.session();
    s.beginGroup("undo.group.constraint");
    auto r = s.sketchAdd(filtered);
    if (r) {
        std::vector<std::string> added;
        for (const auto& [t, id] : *r) added.push_back(id);
        yieldAutoConstraints(added);
    }
    s.endGroup();
    if (!r) emit error(errorText(r.error(), s.document().document.get()));
    refresh();
}

void SketchEditor::clickConstraint(const QPointF& s) {
    // A text is its anchor point; no sketch entity — a body edge or vertex, a silhouette, an axis (§6.8): the same
    // choice as the hover highlight (FX9-SK-006).
    const Hit h = toolTargetAt(s).hit;
    if (h.kind != Hit::Kind::Point && h.kind != Hit::Kind::Curve) return;
    selection_.insert(h.id);
    const auto* d = data();
    int need = 2;
    const std::string& t = constraintType_;
    if (t == "fix") need = 1;
    else if (t == "symmetric") need = 3;
    else if ((t == "horizontal" || t == "vertical") && h.id != "origin" && d->findEntity(h.id) &&
             d->findEntity(h.id)->type == sketch::EntityType::Line && selection_.size() == 1)
        need = 1;
    if (int(selection_.size()) >= need) {
        applyConstraint(t);
        selection_.clear();
    }
    view_.update();
}

void SketchEditor::deleteSelection() {
    const auto* d = data();
    if (!d || selection_.empty()) return;
    std::set<std::string> owned;
    for (const auto& e : d->entities)
        for (const auto* p : {&e.p1, &e.p2, &e.center, &e.start, &e.end})
            if (!p->empty()) owned.insert(*p);
    for (const auto& e : d->entities) owned.insert(e.poles.begin(), e.poles.end());  // spline poles (S4A-SPL-001)
    std::vector<std::string> ids;
    for (const auto& id : selection_)
        if (id != "origin" && !owned.count(id)) ids.push_back(id);
    if (ids.empty()) {
        emit hint(text("ui.status.cannotDeleteEndpoint"));
        return;
    }
    auto r = model_.session().sketchDelete(ids);
    if (!r) emit error(errorText(r.error(), model_.session().document().document.get()));
    selection_.clear();
    refresh();
}

void SketchEditor::selectAll() {
    const auto* d = data();
    if (!d) return;
    selection_.clear();
    for (const auto& e : d->entities) selection_.insert(e.id);
    view_.update();
}

void SketchEditor::startTangentArc() {
    if (tool_ == Tool::Line && !chainEndPoint_.empty()) {
        // In a chain of segments: the next segment is a tangent arc from the current end (§6.3).
        const std::string from = chainEndPoint_, curve = chainEndCurve_;
        tool_ = Tool::TangentArc;
        chainEndPoint_ = from;
        chainEndCurve_ = curve;
        returnToLine_ = true;
        clicks_.clear();
        view_.update();
        emit statusChanged();
    } else {
        setTool(Tool::TangentArc);
    }
}

void SketchEditor::setTool(Tool t, const std::string& constraintType) {
    lastDrawn_.clear();
    // The construction mode outlives tools (FX7-SK-001): "Select" while it is on is the construction click mode.
    if (t == Tool::Select && drawConstruction_) t = Tool::Construction;
    // A dimension selected in «Dimension» (FX9-SK-003, S7F-SK-002/003) is the tool's own selection: leaving the tool drops it.
    if (tool_ == Tool::Dimension && t != Tool::Dimension) selection_.clear();
    resetToolState();
    textBox_->hide();
    patternBox_->hide();
    patternEditing_.clear();
    primitiveBox_->hide();
    primitiveEditing_.clear();
    offsetContour_ = false;  // offset state (S7F-SK-005)
    offsetEdges_.clear();
    offsetEdgeGeometry_.clear();
    offsetEditing_.clear();
    tool_ = t;
    centerline_ = false;
    returnToLine_ = false;
    constraintType_ = constraintType;
    if (t != Tool::Select && t != Tool::Constraint) selection_.clear();
    // Trim cursor — scissors (§6.11).
    if (t == Tool::Trim) {
        QPixmap pm(24, 24);
        pm.fill(Qt::transparent);
        QPainter pp(&pm);
        QFont f = pp.font();
        f.setPixelSize(20);
        pp.setFont(f);
        pp.setPen(COLOR_SK_CONFLICT.color);
        pp.drawText(pm.rect(), Qt::AlignCenter, QString::fromUtf8("✂"));
        pp.end();
        view_.setCursor(QCursor(pm, 12, 12));
    } else {
        view_.unsetCursor();
    }
    view_.update();
    // The status line says what the tool waits for (S7F-UI-006); leaving a tool for «Select» clears only that hint — a
    // message the tool's last command left (an auto-relation removed, …) stays.
    if (const QString h = toolHint(t); !h.isEmpty()) {
        if (h != toolHint_) emit hint(h);
        toolHint_ = h;
    } else if (!toolHint_.isEmpty()) {
        emit toolHintEnded(toolHint_);
        toolHint_.clear();
    }
    emit statusChanged();
}

QString SketchEditor::toolHint(Tool t) {
    switch (t) {
        case Tool::Select: return {};
        case Tool::Dimension: return text("ui.wait.sketch.dimension");
        case Tool::Constraint: return text("ui.wait.sketch.constraint");
        case Tool::Trim: return text("ui.wait.sketch.trim");
        case Tool::Project:
        case Tool::Pierce: return text("ui.wait.sketch.project");
        case Tool::SketchFillet:
        case Tool::SketchChamfer: return text("ui.wait.sketch.corner");
        case Tool::Construction: return text("ui.wait.sketch.construction");
        case Tool::Mirror:
        case Tool::Offset:
        case Tool::PatternLinear:
        case Tool::PatternCircular: return text("ui.wait.sketch.selection");
        default: return text("ui.wait.sketch.draw");
    }
}

void SketchEditor::setConstraintsShown(bool on) {
    showConstraints_ = on;
    view_.update();
    emit statusChanged();
}

void SketchEditor::updateTrimPreview() {
    trimPreview_.reset();
    if (tool_ != Tool::Trim || !cursorValid_) return;
    const auto* d = data();
    const std::string c = trimCurveAt(cursor_);
    const auto lp = toLocal(cursor_);
    if (!d || c.empty() || !lp) return;
    if (auto plan = model_.session().sketchTrimPreview(c, *lp)) trimPreview_ = *plan;
}

void SketchEditor::clickTrim(const QPointF& s) {
    cursor_ = s;
    cursorValid_ = true;
    updateTrimPreview();
    if (!trimPreview_) return;
    const auto lp = toLocal(s);
    auto r = model_.session().sketchTrim(trimPreview_->curve, *lp);
    if (!r) {
        emit error(errorText(r.error()));
        return;
    }
    // Inapplicable constraints were removed — a notification with the «Undo» action (§6.11).
    if (!r->removedConstraints.empty())
        emit notice(text("ui.trim.removedConstraints", {{"count", QString::number(r->removedConstraints.size())}}), true);
    updateTrimPreview();
}

void SketchEditor::resetToolState() {
    dismissedHints_.clear();
    piercePoint_.clear();
    clicks_.clear();
    clickSnaps_.clear();
    chainEndPoint_.clear();
    chainStartPoint_.clear();
    chainEndCurve_.clear();
    wokenLine_.clear();
    trimPreview_.reset();
    view_.setHover({});
    arcSweep_ = 0;
    lastArcAngle_.reset();
    picked_.clear();
    pickClicks_.clear();
    dimPreview_.reset();
    if (dimPreviewTimer_) dimPreviewTimer_->stop();
}

bool SketchEditor::escape() {
    if (dismissShownHints()) return true;  // an offered auto-constraint (§6.13 item 0, FX10-SK-005)
    if (inlineEdit_.isOpen()) {
        finishInlineEdit(false);
        return true;
    }
    if (primitiveBox_->isVisible()) {  // primitive dialog — close without changes (§6.13)
        primitiveClosed();
        return true;
    }
    if (patternBox_->isVisible()) {  // pattern dialog — close without changes (§6.13)
        patternClosed();
        return true;
    }
    if (boxing_) {
        boxing_ = false;
        pressed_ = false;
        view_.update();
        return true;
    }
    if (!labelDrag_.empty()) {  // dragging a dimension label — the label goes back (§6.6)
        labelDrag_.clear();
        pressed_ = false;
        view_.update();
        return true;
    }
    if (dragging_) {
        model_.session().sketchDragEnd(false);
        dragging_ = false;
        pressed_ = false;
        dragState_.reset();
        view_.update();
        return true;
    }
    // A dimension selected in the «Dimension» tool — drop the selection, the mode stays (FX9-SK-003); the
    // automatic selection of a just placed dimension (S7F-SK-002) is not a ladder step.
    if (tool_ == Tool::Dimension && picked_.empty() && !selection_.empty() && dimAutoSelected_) selection_.clear();
    if (tool_ == Tool::Dimension && picked_.empty() && !selection_.empty()) {
        selection_.clear();
        view_.update();
        return true;
    }
    // Objects picked by the «Dimension» tool — reset the picks, the mode stays (§6.13 item 4, FX2-SK-002).
    if (tool_ == Tool::Dimension && !picked_.empty()) {
        picked_.clear(), pickClicks_.clear();
        dimPreview_.reset();
        view_.update();
        return true;
    }
    if (tool_ == Tool::Construction) {  // Esc in the construction mode turns it off (§6.13 item 4а, FX7-SK-001)
        setConstructionMode(false);
        return true;
    }
    if (tool_ != Tool::Select) {
        setTool(Tool::Select);
        return true;
    }
    if (!selection_.empty()) {
        selection_.clear();
        view_.update();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- input

void SketchEditor::dimensionMenu(const std::string& cid, const QPoint& globalPos) {
    const auto* d = data();
    const auto* c = d ? d->findConstraint(cid) : nullptr;
    if (!c) return;
    selection_ = {cid};
    view_.update();
    QMenu menu(&view_);
    menu.setObjectName("dimensionMenu");
    menu.addAction(txt("ui.common.editValue"), [this, cid] { openInlineEdit(cid); });
    const bool driving = c->driving.value_or(true);
    menu.addAction(driving ? txt("ui.sketch.reference") : txt("ui.sketch.driving"), [this, cid] { toggleDriven(cid); });
    // Radius / diameter from an axis (S2-SK-003, FX8-SK-001): two exclusive checkable items, the current one is checked.
    if (diametralApplicable(cid)) {
        menu.addSeparator();
        auto* group = new QActionGroup(&menu);
        for (const bool dia : {false, true}) {
            QAction* a = menu.addAction(dia ? txt("ui.sketch.diameterFromAxis") : txt("ui.sketch.radiusFromAxis"), [this, cid, dia] {
                const auto* cc = data() ? data()->findConstraint(cid) : nullptr;
                if (cc && cc->diametral != dia) toggleDiametral(cid);
            });
            a->setObjectName(dia ? "dimDiametral" : "dimRadial");
            a->setCheckable(true);
            a->setChecked(c->diametral == dia);
            group->addAction(a);
        }
        menu.addSeparator();
    }
    auto retype = [this, cid](const std::string& to) {
        auto& s = model_.session();
        s.beginGroup("undo.group.dimension");
        auto r = s.sketchUpdate(Json::array({{{"id", cid}, {"type", to}}}));
        s.endGroup();
        if (!r) emit error(errorText(r.error(), s.document().document.get()));
        refresh();
    };
    if (c->type == "radius" || c->type == "diameter") {
        const std::string other = c->type == "radius" ? "diameter" : "radius";
        menu.addAction(c->type == "radius" ? txt("ui.common.diameter") : txt("ui.common.radius"), [retype, other] { retype(other); })->setObjectName("dimToggleRadius");
    }
    // For an arc — arc length ↔ radius (§6.9, S1-SOLVE-010).
    const auto* e0 = c->refs.empty() ? nullptr : d->findEntity(c->refs[0]);
    // For an arc — also the central angle (S2-SK-005).
    if (e0 && e0->type == sketch::EntityType::Arc && (c->type == "radius" || c->type == "diameter")) {
        menu.addAction(txt("ui.common.arcLength"), [retype] { retype("arcLength"); })->setObjectName("dimArcLength");
        menu.addAction(txt("ui.sketch.centralAngle"), [retype] { retype("arcAngle"); })->setObjectName("dimArcAngle");
    }
    if (c->type == "arcLength" || c->type == "arcAngle")
        menu.addAction(txt("ui.common.radius"), [retype] { retype("radius"); })->setObjectName("dimToggleRadius");
    // Ellipse: semi-axes and rotation (§6.9, S4-SKT-004); the current type is not shown.
    if (c->type == "semiMajor" || c->type == "semiMinor" || c->type == "ellipseAngle") {
        const std::pair<const char*, const char*> kinds[] = {{"semiMajor", txt("ui.sketch.majorSemiAxis")}, {"semiMinor", txt("ui.sketch.minorSemiAxis")},
                                                             {"ellipseAngle", txt("ui.sketch.rotation")}};
        const char* names[] = {"dimEllipseMajor", "dimEllipseMinor", "dimEllipseAngle"};
        for (int i = 0; i < 3; ++i)
            if (c->type != kinds[i].first)
                menu.addAction(kinds[i].second, [retype, to = std::string(kinds[i].first)] { retype(to); })->setObjectName(names[i]);
    }
    // To a circle/arc — nearest ↔ farthest (§6.9, S1-SOLVE-010).
    if (c->type == "distance" && c->arc) {
        const std::string to = *c->arc == "far" ? "near" : "far";
        menu.addAction(to == "far" ? txt("ui.sketch.far") : txt("ui.sketch.nearest"), [this, cid, to] {
            auto& s = model_.session();
            s.beginGroup("undo.group.dimension");
            auto r = s.sketchUpdate(Json::array({{{"id", cid}, {"arc", to}}}));
            s.endGroup();
            if (!r) emit error(errorText(r.error(), s.document().document.get()));
            refresh();
        })->setObjectName("dimArcFarNear");
    }
    if (pmi::toleranceDimension(c->type))  // dimension tolerance (§6.6, S3-PMI-002)
        menu.addAction(txt("ui.sketch.tolerance"), [this, cid] { openToleranceBox(cid); })->setObjectName("dimTolerance");
    menu.addSeparator();
    menu.addAction(txt("ui.common.delete"), [this] { deleteSelection(); });
    menu.exec(globalPos);
}

// Point context menu (§6.6а, S4-WEAK-006): «Make anchor» — the weak coordinates of the point's loop
// move to it; available while the sketch has weak dimensions (otherwise the choice changes nothing).
void SketchEditor::pointMenu(const std::string& pid, const QPoint& globalPos) {
    selection_ = {pid};
    view_.update();
    QMenu menu(&view_);
    menu.setObjectName("pointMenu");
    QAction* anchor = menu.addAction(txt("ui.sketch.makeAnchor"), [this, pid] {
        auto& s = model_.session();
        if (auto r = s.sketchSetWeakAnchor(pid); !r) emit error(errorText(r.error(), s.document().document.get()));
        refresh();
    });
    anchor->setObjectName("pointWeakAnchor");
    anchor->setEnabled(state_ && !state_->weak.empty());
    menu.addSeparator();
    menu.addAction(txt("ui.common.delete"), [this] { deleteSelection(); });
    menu.exec(globalPos);
}

// Dimension tolerance dialog (§6.6): at the label; OK — fit designation with dimension, «Remove» deletes it.
void SketchEditor::openToleranceBox(const std::string& cid) {
    const auto* c = data() ? data()->findConstraint(cid) : nullptr;
    if (!c) return;
    const SketchAnnotations an = annotations();
    const auto existing = an.dimTolerance(*c);
    const auto v = an.dimValue(*c);
    toleranceDim_ = cid;
    const auto r = an.labelRect(cid);
    const QPoint at = r ? r->bottomLeft().toPoint() + QPoint(0, 4) : QPoint(20, 20);
    toleranceBox_->open(at, existing.value_or(doc::Json::object()), existing.has_value(), pmi::sizeDimension(c->type),
                        v ? std::optional<double>(std::abs(*v)) : std::nullopt, an.dimBaseText(*c));
}

void SketchEditor::toleranceAccepted(const doc::Json& tol) {
    const auto* c = data() ? data()->findConstraint(toleranceDim_) : nullptr;
    if (!c) return;
    auto& s = model_.session();
    doc::Json def = {{"type", "fit"}, {"dimension", {{"sketch", feature_}, {"constraint", toleranceDim_}}}};
    for (auto it = tol.begin(); it != tol.end(); ++it) def[it.key()] = it.value();
    const auto existing = annotations().dimTolerance(*c);
    Status r = okStatus();
    if (existing) r = s.updateAnnotation((*existing)["id"].get<std::string>(), def);
    else if (auto a = s.addAnnotation(def); !a) r = a.error();
    if (!r) emit error(errorText(r.error(), s.document().document.get()));
    refresh();
}

void SketchEditor::toleranceRemoved() {
    const auto* c = data() ? data()->findConstraint(toleranceDim_) : nullptr;
    if (!c) return;
    if (const auto existing = annotations().dimTolerance(*c)) {
        auto& s = model_.session();
        if (auto r = s.deleteAnnotation((*existing)["id"].get<std::string>()); !r) emit error(errorText(r.error(), s.document().document.get()));
    }
    refresh();
}

double SketchEditor::pointMarkerPx(bool standalone, bool emphasized) {
    if (standalone) return emphasized ? SK_STANDALONE_POINT_PX + 2 : SK_STANDALONE_POINT_PX;
    return emphasized ? VERTEX_SELECTED_PX : SK_POINT_PX;
}

double SketchEditor::pointMarkerPx(const std::string& pointId) const {
    const auto* d = data();
    const auto* e = d ? d->findEntity(pointId) : nullptr;
    if (!e || e->type != sketch::EntityType::Point) return 0;
    for (const auto& o : d->entities)
        if (o.center == pointId) return 0;
    const bool emph = selection_.count(pointId) || hover_.id == pointId ||
                      std::find(picked_.begin(), picked_.end(), pointId) != picked_.end();
    return pointMarkerPx(!api::sketchops::ownedPoints(*d).count(pointId), emph);
}

QString SketchEditor::dimensionText(const std::string& constraintId) const {
    const auto* c = displayData() ? displayData()->findConstraint(constraintId) : nullptr;
    return c ? annotations().dimText(*c) : QString();
}

// Tooltip above a dimension label (§6.6): name D<N>, expression and tolerance.
QString SketchEditor::toolTipAt(const QPoint& at) const {
    if (!state_ || !data()) return {};
    const Hit h = hitAt(QPointF(at));
    if (h.kind != Hit::Kind::Label) return {};
    if (isWeak(h.id)) return text("ui.weak.tooltip");  // §6.6а
    const auto* c = data()->findConstraint(h.id);
    if (!c || !sketch::isDimensionType(c->type)) return {};
    QStringList lines{"D" + QString::fromStdString(c->id.substr(1))};
    if (c->value) lines << QString::fromStdString(*c->value);
    const SketchAnnotations an = annotations();
    if (const auto t = an.dimTolerance(*c)) {
        if (t->contains("field")) {
            const std::string field = (*t)["field"].get<std::string>();
            const auto v = an.dimValue(*c);
            const auto d = api::Session::fitDeviations(field, v ? std::abs(*v) : 0.0);
            if (d)
                lines << QString(txt("ui.sketch.toleranceNNN"))
                             .arg(QString::fromStdString(field), QString::fromStdString(pmi::formatDeviation(d->upper)),
                                  QString::fromStdString(pmi::formatDeviation(d->lower)));
            else
                lines << txt("ui.sketch.tolerance2") + errorText(d.error());
        } else {
            lines << QString(txt("ui.sketch.toleranceNN"))
                         .arg(QString::fromStdString(pmi::formatDeviation((*t)["upper"].get<double>())),
                              QString::fromStdString(pmi::formatDeviation((*t)["lower"].get<double>())));
        }
    }
    return lines.join(QChar('\n'));
}

bool SketchEditor::mousePress(QMouseEvent* e) {
    if (state_ && e->button() == Qt::RightButton && tool_ == Tool::Select) {
        const Hit h = hitAt(e->position());
        if (h.kind == Hit::Kind::Label) {
            if (const auto* c = data()->findConstraint(h.id); c && sketch::isDimensionType(c->type))
                dimensionMenu(h.id, e->globalPosition().toPoint());
            return true;
        }
        if (h.kind == Hit::Kind::Point && data()->findEntity(h.id)) {  // point menu (§6.6а, S4-WEAK-006)
            pointMenu(h.id, e->globalPosition().toPoint());
            return true;
        }
        return false;
    }
    if (!state_ || e->button() != Qt::LeftButton) return false;
    cursor_ = e->position();
    ctrlHeld_ = e->modifiers() & Qt::ControlModifier;
    const bool suppress = ctrlHeld_;
    // A click on a dimension (regular or weak) right after drawing — no element started — leaves the drawing tool for
    // «Select» and opens the dimension's value field (FX7-SK-008).
    if (isDrawingTool(tool_) && clicks_.empty() && chainEndPoint_.empty()) {
        const Hit h = hitAt(cursor_);
        const auto* c = h.kind != Hit::Kind::Label ? nullptr : isWeak(h.id) ? displayData()->findConstraint(h.id) : data()->findConstraint(h.id);
        // Only a dimension of the element just drawn (a label of other geometry under a new click is not meant).
        const bool ofLast = c && std::any_of(c->refs.begin(), c->refs.end(), [&](const std::string& r) { return lastDrawn_.count(r) > 0; });
        if (c && ofLast && sketch::isDimensionType(c->type)) {
            const std::string id = c->id;
            setTool(Tool::Select);
            if (isWeak(id)) strengthenWeak(id);
            else openInlineEdit(id);
            view_.update();
            return true;
        }
    }
    switch (tool_) {
        case Tool::Point: clickPoint(cursor_, suppress); break;
        case Tool::Text: clickText(cursor_, suppress); break;
        case Tool::Line: clickLine(cursor_, suppress); break;
        case Tool::Rectangle: clickRectangle(cursor_, suppress); break;
        case Tool::Circle: clickCircle(cursor_, suppress); break;
        case Tool::Arc3: clickArc3(cursor_, suppress); break;
        case Tool::CenterRectangle: clickCenterRectangle(cursor_, suppress); break;
        case Tool::Polygon: clickPolygon(cursor_, suppress); break;
        case Tool::Circle3: clickCircle3(cursor_, suppress); break;
        case Tool::Ellipse: clickEllipse(cursor_, suppress); break;
        case Tool::SplineControl:
        case Tool::SplineThrough: clickSpline(cursor_, suppress); break;
        case Tool::Star: clickStar(cursor_, suppress); break;
        case Tool::ArcCenter: clickArcCenter(cursor_, suppress); break;
        case Tool::TangentArc: clickTangentArc(cursor_, suppress); break;
        case Tool::Dimension:
            // A press on a dimension label with nothing picked: a click selects it on release, a drag moves the label
            // (app-ux §6.7, S7F-SK-003).
            if (picked_.empty())
                if (const Hit h = toolTargetAt(cursor_).hit;
                    h.kind == Hit::Kind::Label && (isWeak(h.id) || (data() && data()->findConstraint(h.id)))) {
                    pressed_ = true;
                    pressPos_ = cursor_;
                    pressHit_ = h;
                    break;
                }
            clickDimension(cursor_);
            break;
        case Tool::Constraint: clickConstraint(cursor_); break;
        case Tool::Trim: clickTrim(cursor_); break;
        case Tool::Mirror: clickMirror(cursor_); break;
        case Tool::PatternCircular: clickPatternCenter(cursor_); break;
        case Tool::PatternLinear: clickPatternLine(cursor_); break;  // the dialog is open; «By line» picks the axis
        case Tool::Offset: clickOffset(cursor_); break;
        case Tool::SketchFillet:
        case Tool::SketchChamfer: clickCorner(cursor_); break;
        case Tool::Project: clickProject(cursor_); break;
        case Tool::Pierce: clickPierce(cursor_); break;
        case Tool::Construction: clickConstructionMode(cursor_); break;
        case Tool::Select:
            pressed_ = true;
            pressPos_ = cursor_;
            pressHit_ = hitAt(cursor_);
            break;
    }
    dismissedHints_.clear();  // a dismissal holds for the next click only (FX10-SK-005)
    view_.update();
    return true;
}

bool SketchEditor::mouseMove(QMouseEvent* e) {
    if (!state_) return false;
    cursor_ = e->position();
    cursorValid_ = true;
    // Mouse movement removes the dimension preview and restarts the pause countdown (§6.9, FX2-SK-002).
    if (tool_ == Tool::Dimension && !picked_.empty() && (cursor_ - dimPreviewAt_).manhattanLength() > DIM_PREVIEW_MOVE_PX) {
        const bool shown = dimPreview_.has_value();
        restartDimensionPreview();
        if (shown) view_.update();
    }
    if (tool_ == Tool::Select && pressed_ && !dragging_ && pressHit_.kind != Hit::Kind::None &&
        pressHit_.kind != Hit::Kind::Label && (cursor_ - pressPos_).manhattanLength() > DRAG_THRESHOLD_PX) {
        const auto grab = toLocal(pressPos_).value_or(Point2::Zero());
        if (model_.session().sketchDragBegin(pressHit_.id, grab).ok()) dragging_ = true;
    }
    // A dimension label is grabbed by its text or dimension line (§6.6, FX1-SKI-001): the label follows the cursor.
    if ((tool_ == Tool::Select || tool_ == Tool::Dimension) && pressed_ && labelDrag_.empty() && pressHit_.kind == Hit::Kind::Label &&
        (cursor_ - pressPos_).manhattanLength() > DRAG_THRESHOLD_PX)
        // A weak (gray) dimension is dragged like a regular one (FX7-SK-005).
        if (const auto* c = isWeak(pressHit_.id) ? displayData()->findConstraint(pressHit_.id) : data() ? data()->findConstraint(pressHit_.id) : nullptr;
            c && sketch::isDimensionType(c->type))
            if (const auto from = toLocal(annotations().labelPos(*c))) labelDrag_ = c->id, labelDragFrom_ = *from, labelDragAt_ = *from;
    if (!labelDrag_.empty()) {
        if (const auto a = toLocal(pressPos_), b = toLocal(cursor_); a && b) labelDragAt_ = labelDragFrom_ + (*b - *a);
        view_.update();
        return true;
    }
    // Box: LMB drag from an empty place (§6.3).
    if (tool_ == Tool::Select && pressed_ && !dragging_ && !boxing_ && pressHit_.kind == Hit::Kind::None &&
        (cursor_ - pressPos_).manhattanLength() > DRAG_THRESHOLD_PX)
        boxing_ = true;
    if (boxing_) {
        view_.update();
        return true;
    }
    if (dragging_) {
        if (auto lp = toLocal(cursor_)) {
            auto r = model_.session().sketchDragUpdate(*lp);
            if (r) dragState_ = *r;
        }
        view_.update();
        return true;
    }
    if (tool_ == Tool::ArcCenter && clicks_.size() == 2) {
        // Accumulating the sweep angle around the center — the arc direction (§6.3).
        if (auto lp = toLocal(cursor_)) {
            const double a = angleOf(clicks_[0], *lp);
            if (lastArcAngle_) {
                double d = a - *lastArcAngle_;
                while (d > PI) d -= 2 * PI;
                while (d < -PI) d += 2 * PI;
                arcSweep_ += d;
            }
            lastArcAngle_ = a;
        }
    }
    if (tool_ == Tool::Point || tool_ == Tool::Line || tool_ == Tool::Rectangle || tool_ == Tool::Circle ||
        tool_ == Tool::Arc3 || tool_ == Tool::ArcCenter || tool_ == Tool::TangentArc || tool_ == Tool::CenterRectangle ||
        tool_ == Tool::Polygon || tool_ == Tool::Circle3 || tool_ == Tool::Text || tool_ == Tool::Ellipse || tool_ == Tool::Star ||
        tool_ == Tool::SplineControl || tool_ == Tool::SplineThrough)
        hoverSnap_ = snapAt(cursor_, e->modifiers() & Qt::ControlModifier);
    if (tool_ == Tool::Arc3 && clicks_.size() == 2)
        if (const auto raw = toLocal(cursor_)) hoverSnap_ = arcThroughPointSnap(hoverSnap_, *raw);  // as the click (§6.3)
    hover_ = hitAt(cursor_);
    // «Dimension» / «Constraint»: the target a click would take is highlighted before the click (FX9-SK-006) — a body
    // edge or vertex in the view, a silhouette or an axis in the sketch overlay.
    if (tool_ == Tool::Dimension || tool_ == Tool::Constraint) {
        const ToolTarget t = toolTargetAt(cursor_);
        if (t.view.kind != toolHover_.view.kind || t.view.name != toolHover_.view.name) view_.setHover(t.view);
        toolHover_ = t;
    } else if (toolHover_.hit.kind != Hit::Kind::None) {
        if (toolHover_.view.kind != PickTarget::Kind::None) view_.setHover({});
        toolHover_ = {};
    }
    // A segment under the cursor ≥ WAKE_HOVER_MS while a tool is working is "woken" (§6.4 item 5).
    if (!clock_.isValid()) clock_.start();
    const auto* dd = data();
    const auto* he = hover_.kind == Hit::Kind::Curve && dd ? dd->findEntity(hover_.id) : nullptr;
    if (he && he->type == sketch::EntityType::Line) {
        if (hoverLine_ != he->id) hoverLine_ = he->id, hoverLineSince_ = clock_.elapsed();
        else if (tool_ != Tool::Select && clock_.elapsed() - hoverLineSince_ >= WAKE_HOVER_MS) wokenLine_ = hoverLine_;
    } else {
        hoverLine_.clear();
    }
    updateTrimPreview();
    if (tool_ == Tool::Project || (tool_ == Tool::Pierce && !piercePoint_.empty())) view_.setHover(view_.pickAt(cursor_.toPoint(), false, true, false));
    // A dismissed hint (FX10-SK-005) comes back once the cursor has left its target and returned.
    if (!dismissedHints_.empty()) dismissedHints_.retain(shownHintKeys(true));
    view_.update();
    return true;
}

bool SketchEditor::mouseRelease(QMouseEvent* e) {
    if (!state_ || e->button() != Qt::LeftButton) return false;
    if (!labelDrag_.empty()) {  // §6.6, FX1-SKI-001: label position — one undo step
        const std::string cid = labelDrag_;
        const Point2 at = labelDragAt_;
        labelDrag_.clear();
        pressed_ = false;
        if (isWeak(cid)) {  // a dragged weak dimension stays where it was put: it becomes regular (FX7-SK-005)
            auto r = model_.session().sketchStrengthen(cid, std::nullopt, at);
            if (!r) emit error(errorText(r.error(), model_.session().document().document.get()));
            refresh();
            return true;
        }
        // Stored relative to the dimension anchor (FX8-SK-004): the label then moves with its points.
        const auto* c = data() ? data()->findConstraint(cid) : nullptr;
        const auto off = c ? api::sketchops::labelOffsetFor(dataWithProjections(), state_->result.state, *c, at) : std::nullopt;
        if (!off) return true;
        auto r = model_.session().sketchUpdate(Json::array({{{"id", cid}, {"labelOffset", Json::array({(*off)[0], (*off)[1]})}}}));
        if (!r) emit error(errorText(r.error(), model_.session().document().document.get()));
        refresh();
        return true;
    }
    if (dragging_) {
        dragging_ = false;
        pressed_ = false;
        auto& s = model_.session();
        s.sketchDragEnd(true);
        dragState_.reset();
        refresh();
        return true;
    }
    if (boxing_) {
        cursor_ = e->position();
        finishBox(e->modifiers() & Qt::ControlModifier);
        return true;
    }
    if (tool_ == Tool::Dimension && pressed_) {  // a click on a dimension label in «Dimension» (S7F-SK-003)
        pressed_ = false;
        clickDimension(pressPos_);
        view_.update();
        return true;
    }
    if (tool_ == Tool::Select && pressed_) {
        pressed_ = false;
        const bool add = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        // Click on a weak relation — make it regular (§6.6а, S4-WEAK-002); a weak dimension behaves like a regular one:
        // a click without a drag does nothing, a double click opens the value field (FX7-SK-005).
        // A click on a weak dimension selects it and shows the dimension bar (§7.2, FX9-SK-002).
        if (pressHit_.kind == Hit::Kind::Label && isWeak(pressHit_.id)) {
            const auto* c = displayData()->findConstraint(pressHit_.id);
            if (!(c && sketch::isDimensionType(c->type))) {
                strengthenWeak(pressHit_.id);
                view_.update();
                return true;
            }
        }
        if (pressHit_.kind == Hit::Kind::None) {
            if (!add) selection_.clear();
        } else if (add) {
            if (!selection_.erase(pressHit_.id)) selection_.insert(pressHit_.id);
        } else {
            selection_ = {pressHit_.id};
        }
        view_.update();
        emit statusChanged();
        emit selectionPicked(e->position());
    }
    return true;
}

void SketchEditor::startDimensionFromSelection() {
    // "Selection → command" (§6.9): the selection becomes the tool's picks, a click on a place sets the dimension.
    setTool(Tool::Dimension);
    for (const auto& id : selection_) {
        picked_.push_back(id);
        pickClicks_.push_back(screenPosOfEntity(id).value_or(cursor_));
    }
    selection_.clear();
    restartDimensionPreview();
}

void SketchEditor::restartDimensionPreview() {
    dimPreview_.reset();
    dimPreviewAt_ = cursor_;
    if (tool_ == Tool::Dimension && !picked_.empty()) dimPreviewTimer_->start();
    else dimPreviewTimer_->stop();
}

std::optional<SketchEditor::DimPreview> SketchEditor::computeDimensionPreview(const QPointF& at) const {
    // The dimension a click at `at` would create (§6.9, FX2-SK-002): only over an empty place (or an icon / label),
    // with the field closed. Not yet projected body edges and vertices and sketch axes — via temporary projections
    // (FX3-SK-001): a projected entity with the same source and its geometry in the "before sketch" state, doc intact.
    if (tool_ != Tool::Dimension || !state_ || !cursorValid_ || inlineEdit_.isOpen()) return std::nullopt;
    const Hit h = hitAt(at);
    if (h.kind == Hit::Kind::Point || h.kind == Hit::Kind::Curve) return std::nullopt;
    if (!dimensionPlaceable(dataWithProjections(), picked_)) return std::nullopt;
    auto sd = std::make_shared<sketch::SketchData>(dataWithProjections());
    std::vector<std::string> picks = picked_;
    std::vector<std::string> temps;
    for (auto& id : picks) {
        if (id.rfind("srce:", 0) != 0 && id.rfind("srcv:", 0) != 0) continue;
        const std::string source = id.substr(5);
        std::string found;
        for (const auto& en : sd->entities)
            if (en.type == sketch::EntityType::Projected && en.source == source) found = en.id;
        if (found.empty()) {
            sketch::Entity e;
            e.id = "preview:" + std::to_string(temps.size() + 1);  // does not collide with document IDs (e<N>)
            e.type = sketch::EntityType::Projected;
            e.source = source;
            e.construction = true;
            sd->entities.push_back(e);
            temps.push_back(e.id);
            found = e.id;
        }
        id = found;
    }
    std::shared_ptr<api::SketchState> st;
    if (!temps.empty()) {
        std::vector<std::string> sources;
        for (const auto& t : temps) sources.push_back(sd->findEntity(t)->source);
        auto pr = model_.session().projectionPreview(feature_, sources);
        if (!pr) return std::nullopt;
        st = std::make_shared<api::SketchState>(*state_);
        for (size_t i = 0; i < temps.size(); ++i) {
            if (!(*pr)[i]) return std::nullopt;  // the source does not project into a sketch curve
            const sketch::Projection& g = *(*pr)[i];
            sd->projections[temps[i]] = g;
            if (g.kind == sketch::Projection::Kind::Point) st->result.state.points[temps[i]] = Point2(g.px, g.py);
        }
        st->projections = sd->projections;
    }
    const SketchGeometry g(sd.get(), st ? st.get() : &*state_, st ? st.get() : &*state_, view_.camera());
    auto posOf = [&](const std::string& id) { return g.pointPos(id); };
    auto view = [&](const Point2& p) {
        const QPointF q = toScreen(p);
        return Point2(q.x(), q.y());
    };
    const auto kind = api::sketchops::dimensionKind(*sd, posOf, picks, Point2(at.x(), at.y()), view, PARALLEL_DIM_TOL_DEG);
    if (!kind) return std::nullopt;
    const auto lbl = toLocal(at);
    if (!lbl) return std::nullopt;
    const auto plan = api::sketchops::dimensionFor(*sd, posOf, state_->result.state.radii, picks, *kind, *lbl);
    if (!plan) return std::nullopt;
    DimPreview pv;
    if (st) pv.data = sd, pv.state = st;
    pv.c.id = "preview";
    pv.c.type = *kind;
    pv.c.refs = plan->refs;
    const bool signedKind = *kind == "distanceX" || *kind == "distanceY";
    pv.c.value = plan->angle ? angleText(plan->value)
                 : plan->number ? formatNumber(plan->value).toStdString()
                                : lengthText(signedKind ? plan->value : std::abs(plan->value));
    pv.c.driving = true;
    if (!plan->arc.empty()) pv.c.arc = plan->arc;
    if (plan->side != 0) {
        sketch::Branch br;
        br.side = plan->side;
        pv.c.branch = br;
    }
    pv.label = at;
    return pv;
}

void SketchEditor::toggleDriven(const std::string& cid) {
    const auto* d = data();
    const auto* c = d ? d->findConstraint(cid) : nullptr;
    if (!c) return;
    const bool driving = c->driving.value_or(true);
    auto& s = model_.session();
    s.beginGroup("undo.group.dimension");
    auto r = s.sketchUpdate(Json::array({{{"id", cid}, {"driving", !driving}}}));
    s.endGroup();
    if (!r) emit error(errorText(r.error(), s.document().document.get()));
    refresh();
}

const sketch::Constraint* SketchEditor::dimensionConstraint(const std::string& cid) const {
    const auto* d = isWeak(cid) ? displayData() : data();
    const auto* c = d ? d->findConstraint(cid) : nullptr;
    return c && sketch::isDimensionType(c->type) ? c : nullptr;
}

bool SketchEditor::diametralApplicable(const std::string& cid) const {
    const auto* c = dimensionConstraint(cid);
    if (!c || !state_) return false;
    // Axes of revolve features that use this sketch (FX9-SK-001): a sketch segment or a datum axis in the sketch plane.
    std::vector<api::sketchops::DiametralAxis> axes;
    const auto docPtr = model_.session().document().document;
    const auto& pts = state_->result.state.points;
    for (const auto& f : docPtr->active().features) {
        const auto* rv = f && !f->suppressed ? f->revolve() : nullptr;
        if (!rv || rv->sketch != feature_) continue;
        if (const auto* l = data() ? data()->findEntity(rv->axisSketchLine) : nullptr; l && l->type == sketch::EntityType::Line) {
            const auto a = pts.find(l->p1), b = pts.find(l->p2);
            if (a != pts.end() && b != pts.end()) axes.push_back({a->second, Point2(b->second - a->second)});
        } else if (!rv->axisFeature.empty()) {
            // A reference axis feature lying in the sketch plane (app-ux §7.2, FX9-SK-001).
            if (const auto g = model_.session().referenceGeometry(api::StateRef::final())) {
                if (const auto ax = g->axes.find(rv->axisFeature); ax != g->axes.end()) {
                    const Vec3 n = state_->frame.normal;
                    if (std::abs(ax->second.dir.dot(n)) <= 1e-6 && std::abs((ax->second.point - state_->frame.origin).dot(n)) <= 1e-6) {
                        const Point2 o = state_->frame.toLocal(ax->second.point);
                        axes.push_back({o, Point2(state_->frame.toLocal(Point3(ax->second.point + ax->second.dir)) - o)});
                    }
                }
            }
        } else if (rv->axisDatum.size() == 6 && rv->axisDatum.rfind("axis:", 0) == 0) {
            const char k = rv->axisDatum[5];
            const Vec3 dir = k == 'x' ? Vec3::UnitX() : k == 'y' ? Vec3::UnitY() : Vec3::UnitZ();
            const Point2 o = state_->frame.toLocal(Point3::Zero()), e = state_->frame.toLocal(Point3(dir));
            if ((e - o).norm() > 0.5) axes.push_back({o, Point2(e - o)});  // the axis lies in (or near) the plane
        }
    }
    return api::sketchops::diametralApplicable(dataWithProjections(), state_->result.state, *c, axes);
}

void SketchEditor::toggleDiametral(const std::string& cid) {
    // The open value field of a just placed dimension is applied first (as Enter, S7F-SK-002).
    if (inlineEdit_.isOpen() && !inlineEdit_.finish(true)) return;
    const auto* c = dimensionConstraint(cid);
    if (!c || !diametralApplicable(cid)) return;
    const bool to = !c->diametral;
    auto& s = model_.session();
    s.beginGroup("undo.group.dimension");
    std::string id = cid;
    if (isWeak(cid)) {  // a weak dimension becomes regular with its current value first (FX9-SK-002)
        auto sr = s.sketchStrengthen(cid, std::nullopt, std::nullopt);
        if (!sr) {
            s.endGroup();
            emit error(errorText(sr.error(), s.document().document.get()));
            refresh();
            return;
        }
        id = *sr;
        if (selection_.erase(cid)) selection_.insert(id);
    }
    auto r = s.sketchUpdate(Json::array({{{"id", id}, {"diametral", to}}}));
    s.endGroup();
    if (!r) emit error(errorText(r.error(), s.document().document.get()));
    refresh();
}

void SketchEditor::finishBox(bool add) {
    boxing_ = false;
    pressed_ = false;
    const Point2 c0(pressPos_.x(), pressPos_.y()), c1(cursor_.x(), cursor_.y());
    const bool window = cursor_.x() >= pressPos_.x();  // left to right — entirely inside; right to left — touched
    if (!add) selection_.clear();
    const auto g = geo();
    if (const auto* d = data())
        for (const auto& e : d->entities) {
            const auto pl = g.screenPolyline(e);
            if (pl.empty()) continue;
            if (window ? snap::polylineInside(pl, c0, c1) : snap::polylineTouches(pl, c0, c1)) selection_.insert(e.id);
        }
    view_.update();
    emit statusChanged();
    emit selectionPicked(cursor_);
}

bool SketchEditor::mouseDoubleClick(QMouseEvent* e) {
    if (!state_) return false;
    const Hit h = hitAt(e->position());
    // Double click on a weak dimension — its value field, the number selected (§6.6а, FX7-SK-005).
    if (h.kind == Hit::Kind::Label && isWeak(h.id)) {
        if (tool_ != Tool::Select && tool_ != Tool::Dimension && clicks_.size() == 1 && chainEndPoint_.empty()) clicks_.clear(), clickSnaps_.clear();
        strengthenWeak(h.id);
        return true;
    }
    // Double click on a dimension — value editing in any mode; the mode does not change (§6.9, FX1-SKI-006).
    if (h.kind == Hit::Kind::Label)
        if (const auto* c = data()->findConstraint(h.id); c && sketch::isDimensionType(c->type)) {
            // An element started by the first click of a double click is discarded.
            if (tool_ != Tool::Select && tool_ != Tool::Dimension && clicks_.size() == 1 && chainEndPoint_.empty()) clicks_.clear(), clickSnaps_.clear();
            if (tool_ == Tool::Dimension) picked_.clear(), pickClicks_.clear(), dimPreview_.reset();
            openInlineEdit(h.id);
            return true;
        }
    if (tool_ == Tool::Select && h.kind == Hit::Kind::Label) return true;
    if (tool_ == Tool::Select && h.kind == Hit::Kind::Curve)  // text editing (§6.11в)
        if (const auto* te = data()->findEntity(h.id); te && te->type == sketch::EntityType::Text) {
            textEditing_ = te->id;
            textBox_->open(e->position().toPoint(), {te->text, te->font, te->height, te->angle, te->styles});
            return true;
        }
    // Double click on a primitive member — primitive dialog (§6.3 «Primitive window», S4-SKT-003).
    if (tool_ == Tool::Select && (h.kind == Hit::Kind::Curve || h.kind == Hit::Kind::Point) && openPrimitiveBox(h.id, e->position()))
        return true;
    // Double click on an offset contour element — offset editing (§6.3 «Offset edit», S7F-SK-005).
    if (tool_ == Tool::Select && (h.kind == Hit::Kind::Curve || h.kind == Hit::Kind::Point))
        if (const auto* g = data()->groupOfMember(h.id); g && g->type == "offset") {
            startOffsetEdit(g->id, e->position());
            return true;
        }
    // Double click on a pattern element — pattern edit dialog (§6.3 «Sketch pattern», S4-SKT-001/002).
    if (tool_ == Tool::Select && (h.kind == Hit::Kind::Curve || h.kind == Hit::Kind::Point))
        if (const auto* g = data()->groupOfMember(h.id); g && (g->type == "linearPattern" || g->type == "circularPattern")) {
            PatternBox::Values v;
            v.circular = g->type == "circularPattern";
            // Count: a number or an expression (S4-SKT-006).
            auto num = [&](const char* k, int def) {
                const auto it = g->params.find(k);
                if (it == g->params.end()) return QString::number(def);
                return it->second.kind == sketch::GroupParam::Kind::Text ? QString::fromStdString(it->second.text)
                                                                         : QString::number(int(it->second.number));
            };
            auto txt = [&](const char* k) {
                const auto it = g->params.find(k);
                return it == g->params.end() ? QString() : QString::fromStdString(it->second.text);
            };
            v.count1 = num("count1", 2), v.count2 = num("count2", 1), v.count = num("count", 2);
            v.step1 = txt("step1"), v.angle1 = txt("angle1"), v.step2 = txt("step2"), v.angle2 = txt("angle2"), v.angle = txt("angle");
            v.center = txt("center").toStdString();
            v.line = txt("line").toStdString();
            v.line2 = txt("line2").toStdString();  // FX12-FEAT-001
            const std::string gid = g->id;
            setTool(v.circular ? Tool::PatternCircular : Tool::PatternLinear);
            pending_ = data()->findGroup(gid)->sources;
            cursor_ = e->position();
            openPatternBox(v, gid);
            return true;
        }
    if (tool_ == Tool::Line && !clicks_.empty()) {
        resetToolState();  // a double click finishes the chain
        return true;
    }
    if ((tool_ == Tool::SplineControl || tool_ == Tool::SplineThrough) && !clicks_.empty()) {
        finishSpline(false);  // a double click finishes the spline (its first press placed the last point)
        return true;
    }
    return mousePress(e);
}

bool SketchEditor::eventFilter(QObject* o, QEvent* ev) {
    // Esc in the dimension input field while drawing — close without changes (§6.3).
    if (o == drawInput_ && ev->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
        drawInput_->hide();
        drawInput_->deleteLater();
        view_.setFocus();
        if (tool_ == Tool::Offset) setTool(Tool::Select);  // offset: Esc leaves without changes (§6.3, S7F-SK-005)
        return true;
    }
    return QObject::eventFilter(o, ev);
}

void SketchEditor::openDrawInput(const QString& first) {
    if (!drawInput_) {
        drawInput_ = new QLineEdit(&view_);
        drawInput_->setObjectName("drawInput");
        drawInput_->setFixedWidth(DIM_FIELD_W_PX);
        connect(drawInput_, &QLineEdit::returnPressed, this, [this] { applyDrawInput(); });
        drawInput_->installEventFilter(this);
    }
    drawInput_->setText(first);
    drawInput_->move((cursor_ + QPointF(16, 16)).toPoint());
    drawInput_->show();
    drawInput_->setFocus();
}

void SketchEditor::startMirror() {
    std::vector<std::string> sel(selection_.begin(), selection_.end());
    if (sel.empty()) {
        emit hint(txt("ui.sketch.firstSelectElementsToMirror"));
        return;
    }
    setTool(Tool::Mirror);
    pending_ = sel;
    for (const auto& id : sel) selection_.insert(id);  // the selection stays highlighted
    emit hint(txt("ui.sketch.selectAnAxisSketchLine"));
    view_.update();
}

void SketchEditor::clickMirror(const QPointF& s) {
    const auto d = dataWithProjections();
    std::string axis;
    Point2 a, b;
    Json items = Json::array();
    const Hit h = hitAt(s);
    const auto* e = h.kind == Hit::Kind::Curve ? d.findEntity(h.id) : nullptr;
    if (e && e->type == sketch::EntityType::Line) {
        axis = e->id;
        a = Point2(d.findEntity(e->p1)->x, d.findEntity(e->p1)->y), b = Point2(d.findEntity(e->p2)->x, d.findEntity(e->p2)->y);
    } else if (e && e->type == sketch::EntityType::Projected && d.projections.count(e->id) &&
               d.projections.at(e->id).kind == sketch::Projection::Kind::Line) {
        const auto& pg = d.projections.at(e->id);
        axis = e->id, a = Point2(pg.x1, pg.y1), b = Point2(pg.x2, pg.y2);
    } else if (const std::string src = axisAt(s); !src.empty()) {
        axis = projectionFor(items, src);  // sketch axis is implicit: projection in the same placement (S1-SK-021)
        a = src == "sketch:x" ? Point2(-1, 0) : Point2(0, -1);
        b = src == "sketch:x" ? Point2(1, 0) : Point2(0, 1);
    }
    if (axis.empty()) {
        emit hint(txt("ui.sketch.requiredSketchLineOrAxis"));
        return;
    }
    auto batch = api::sketchops::mirror(d, pending_, axis, a, b);
    if (!batch) return;
    for (auto& it : batch->items) items.push_back(it);
    commitItems(items, "undo.group.mirror");
    setTool(Tool::Select);
}

void SketchEditor::clickCorner(const QPointF& s) {
    const Hit h = hitAt(s);
    const auto* d = data();
    const auto* e = d && h.kind == Hit::Kind::Point ? d->findEntity(h.id) : nullptr;
    if (!e) {
        emit hint(text("ui.corner.notPoint"));
        return;
    }
    // The corner is checked at the click, so an impossible fillet says why right away (FX7-SK-006).
    if (const auto f = api::sketchops::findCorner(*d, e->id); !f.problem.empty()) {
        emit error(cornerProblemText(f.problem, f.lines));
        return;
    }
    cornerPoint_ = e->id;
    clicks_ = {Point2(e->x, e->y)};
    cursor_ = s;
    const auto u = model_.session().units();
    // Fillet: the radius of the nearest fillet of the same contour (FX8-SK-005), otherwise SKETCH_FILLET_DEFAULT = 2 mm.
    double mm = 2;
    if (tool_ == Tool::SketchFillet && state_)
        if (const auto r = api::sketchops::neighborFilletRadius(*d, state_->result.state, e->id)) mm = *r;
    openDrawInput(formatNumber(mm / expr::lengthFactor(u.length)));
    drawInput_->selectAll();
}

void SketchEditor::applyDrawInput() {
    if (drawInput_ && (tool_ == Tool::SketchFillet || tool_ == Tool::SketchChamfer)) {
        // Sketch fillet / chamfer (§6.3, S2-SK-001, S2-SK-002); chamfer: "d", "d1; d2", "d; a°".
        const QString t = drawInput_->text().trimmed();
        auto& s = model_.session();
        Result<std::map<std::string, std::string>> r = std::map<std::string, std::string>{};
        if (tool_ == Tool::SketchFillet) {
            r = s.sketchFillet(cornerPoint_, normalizeExprInput(t));
        } else {
            const QStringList parts = t.split(';');
            const std::string d1 = normalizeExprInput(parts.value(0));
            QString second = parts.size() > 1 ? parts.value(1).trimmed() : parts.value(0).trimmed();
            const bool angle = second.contains(QChar(0x00B0)) || second.contains("deg");
            second.replace(QChar(0x00B0), " deg");
            r = s.sketchChamfer(cornerPoint_, d1, normalizeExprInput(second), angle);
        }
        if (!r) {
            emit error(cornerErrorText(r.error(), s.document().document.get()));
            return;
        }
        drawInput_->hide();
        drawInput_->deleteLater();
        clicks_.clear();
        view_.setFocus();
        refresh();
        return;
    }
    if (drawInput_ && tool_ == Tool::Offset) {  // offset group (§6.3, S7F-SK-005)
        applyOffsetInput();
        return;
    }
    if (!drawInput_ || clicks_.size() != 1) return;
    const std::string typed = normalizeExprInput(drawInput_->text());
    auto q = model_.session().checkExpression(typed, expr::LENGTH);
    if (!q || !(q->value > 0)) {
        emit hint(q ? text("ui.status.tooSmall") : errorText(q.error()));
        return;
    }
    drawInput_->hide();
    drawInput_->deleteLater();
    const Point2 a = clicks_[0];
    auto& s = model_.session();
    std::set<std::string> before;
    if (const auto* d = data())
        for (const auto& en : d->entities) before.insert(en.id);
    const bool line = tool_ == Tool::Line;
    s.beginGroup(line ? "undo.group.line" : "undo.group.circle");
    if (line) {
        const auto cur = toLocal(cursor_);
        Point2 dir = cur ? Point2(*cur - a) : Point2(1, 0);
        if (geom2::isZeroVec(dir)) dir = Point2(1, 0);
        clickLine(toScreen(a + dir.normalized() * q->value), true);
    } else {
        clickCircle(toScreen(a + Point2(q->value / 2, 0)), true);
    }
    // A new segment or circle gets a dimension with the entered text (§6.3).
    std::string created;
    if (const auto* d = data())
        for (const auto& en : d->entities)
            if (!before.count(en.id) && en.type == (line ? sketch::EntityType::Line : sketch::EntityType::Circle)) created = en.id;
    if (!created.empty()) {
        auto r = s.sketchAdd(Json::array({{{"type", line ? "length" : "diameter"}, {"refs", {created}}, {"value", typed}, {"driving", true}}}));
        if (!r) emit error(errorText(r.error(), s.document().document.get()));
    }
    s.endGroup();
    view_.setFocus();
    refresh();
}

bool SketchEditor::keyPress(QKeyEvent* e) {
    if (!state_) return false;
    const Qt::KeyboardModifiers m = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
    const bool shift = m == Qt::ShiftModifier;
    const bool none = m == Qt::NoModifier;
    // Dimension input while drawing (§6.3, S2-SK-010): a digit after the first click of a segment or circle.
    if ((tool_ == Tool::Line || tool_ == Tool::Circle) && clicks_.size() == 1 && (none || shift) && !e->text().isEmpty()) {
        const QChar ch = e->text().at(0);
        if (ch.isDigit() || ch == '.' || ch == ',' || ch == '-') {
            openDrawInput(e->text());
            return true;
        }
    }
    if ((tool_ == Tool::SplineControl || tool_ == Tool::SplineThrough) && !clicks_.empty() &&
        (physicalKey(e) == Qt::Key_Return || physicalKey(e) == Qt::Key_Enter)) {
        finishSpline(false);  // Enter finishes the spline (§6.3)
        return true;
    }
    switch (physicalKey(e)) {
        case Qt::Key_Escape:
            if (!escape()) emit escapeBottom();
            return true;
        case Qt::Key_Delete: deleteSelection(); return true;
        case Qt::Key_P: if (none) { setTool(Tool::Point); return true; } break;
        case Qt::Key_L:
            if (none) { setTool(Tool::Line); return true; }
            if (shift) { applyConstraint("parallel"); return true; }
            break;
        case Qt::Key_R:
            if (none) { setTool(Tool::Rectangle); return true; }
            if (shift) { setTool(Tool::CenterRectangle); return true; }
            break;
        case Qt::Key_C:
            if (none) { setTool(Tool::Circle); return true; }
            if (shift) { setConstraintsShown(!showConstraints_); return true; }
            break;
        case Qt::Key_D:
            if (none) {
                startDimensionFromSelection();
                return true;
            }
            if (shift && selection_.size() == 1) {  // diameter / radius from an axis for the selected dim (S2-SK-003)
                toggleDiametral(*selection_.begin());
                return true;
            }
            break;
        case Qt::Key_I: if (none) { applyConstraint("coincident"); return true; } break;
        case Qt::Key_H: if (none) { applyConstraint("horizontal"); return true; } break;
        case Qt::Key_V: if (none) { applyConstraint("vertical"); return true; } break;
        case Qt::Key_E: if (none) { applyConstraint("equal"); return true; } break;
        case Qt::Key_T:
            if (shift) { applyConstraint("perpendicular"); return true; }
            if (none) {
                startTangentArc();
                return true;
            }
            break;
        case Qt::Key_Q: if (none) { toggleConstruction(); return true; } break;
        case Qt::Key_U: if (none) { setTool(Tool::Project); return true; } break;
        case Qt::Key_N: if (none) { applyConstraint("tangent"); return true; } break;
        case Qt::Key_O: if (none) { applyConstraint("concentric"); return true; } break;
        case Qt::Key_M:
            if (shift) { applyConstraint("midpoint"); return true; }
            if (none) { setTool(Tool::Trim); return true; }
            break;
        case Qt::Key_Y: if (none) { applyConstraint("symmetric"); return true; } break;
        case Qt::Key_X: if (none) { applyConstraint("fix"); return true; } break;
        case Qt::Key_A:
            if (m == Qt::ControlModifier) { selectAll(); return true; }
            if (none) { setTool(Tool::Arc3); return true; }
            if (shift) { setTool(Tool::ArcCenter); return true; }
            break;
        default: break;
    }
    return false;
}

void SketchEditor::leave() {
    cursorValid_ = false;
    hover_ = {};
    view_.update();
}

// ---------------------------------------------------------------- drawing

void SketchEditor::paint(QPainter& p) {
    const auto* d = data();
    if (!d || !state_) return;
    const auto g = geo();
    const auto ann = annotations();
    const auto& st = dragState_ ? *dragState_ : *state_;
    auto radius = [&](const sketch::Entity& e) {
        auto it = st.result.state.radii.find(e.id);
        return it != st.result.state.radii.end() ? it->second : e.r;
    };
    auto pen = [&](const std::string& id, bool construction) {
        bool emph = selection_.count(id) || (hover_.id == id && hover_.kind != Hit::Kind::None) ||
                    std::find(picked_.begin(), picked_.end(), id) != picked_.end();
        if (hover_.kind == Hit::Kind::Label)
            if (const auto* hc = g.data()->findConstraint(hover_.id))
                emph = emph || std::find(hc->refs.begin(), hc->refs.end(), id) != hc->refs.end();
        QPen pe(std::find(picked_.begin(), picked_.end(), id) != picked_.end() ? COLOR_SELECTED.color : entityColor(id),
                emph ? EDGE_SELECTED_WIDTH_PX : SK_LINE_PX);
        if (construction) pe.setDashPattern({4, 2});
        return pe;
    };
    // Projections (§6.11а): COLOR_SK_FULL dash-dotted PROJECTED_DASH; selection/hover — as for entities.
    for (const auto& e : d->entities) {
        if (e.type != sketch::EntityType::Projected) continue;
        const auto pl = g.projectedScreen(e.id);
        if (pl.empty()) continue;
        QPen pe = pen(e.id, false);
        if (!selection_.count(e.id) && hover_.id != e.id) pe.setColor(COLOR_SK_FULL.color);
        if (pl.size() == 1) {
            p.setPen(Qt::NoPen);
            p.setBrush(pe.color());
            p.drawEllipse(pl[0], SK_POINT_PX / 2.0 + 1, SK_POINT_PX / 2.0 + 1);
            p.setBrush(Qt::NoBrush);
            continue;
        }
        if (e.construction) pe.setDashPattern({10, 3, 2, 3});  // a regular projection is solid (§6.5)
        p.setPen(pe);
        p.drawPolyline(pl.data(), int(pl.size()));
    }
    // Curves.
    for (const auto& e : d->entities) {
        if (e.type == sketch::EntityType::Text) {  // text outlines (§6.11в)
            p.setPen(pen(e.id, e.construction));
            for (const auto& pl : textPolylines(e, pointPos(e.center))) {
                QPainterPath path;
                for (size_t i = 0; i < pl.size(); ++i) {
                    const QPointF q = toScreen(pl[i]);
                    if (i == 0) path.moveTo(q);
                    else path.lineTo(q);
                }
                p.drawPath(path);
            }
        } else if (e.type == sketch::EntityType::Line) {
            p.setPen(pen(e.id, e.construction));
            p.drawLine(toScreen(pointPos(e.p1)), toScreen(pointPos(e.p2)));
        } else if (sketch::isSplineType(e.type)) {  // S4A-SPL-001: the curve and its control polygon (dashed, thin)
            QPen poly(COLOR_SK_FULL.color, 1);
            poly.setDashPattern({3, 3});
            p.setPen(poly);
            QPainterPath pp;
            for (size_t i = 0; i < e.poles.size(); ++i) i == 0 ? pp.moveTo(toScreen(pointPos(e.poles[i]))) : pp.lineTo(toScreen(pointPos(e.poles[i])));
            if (e.periodic && !e.poles.empty()) pp.lineTo(toScreen(pointPos(e.poles.front())));
            p.drawPath(pp);
            p.setPen(pen(e.id, e.construction));
            if (const auto pl = geo().splineEntityPolyline(e); !pl.empty()) {
                QPainterPath path;
                for (size_t i = 0; i < pl.size(); ++i) i == 0 ? path.moveTo(toScreen(pl[i])) : path.lineTo(toScreen(pl[i]));
                p.drawPath(path);
            }
        } else if (sketch::isEllipseType(e.type)) {  // S4-SKT-004, -007
            p.setPen(pen(e.id, e.construction));
            if (const auto pl = geo().ellipseEntityPolyline(e, 128); !pl.empty()) {
                QPainterPath path;
                for (size_t i = 0; i < pl.size(); ++i) i == 0 ? path.moveTo(toScreen(pl[i])) : path.lineTo(toScreen(pl[i]));
                p.drawPath(path);
            }
        } else if (e.type == sketch::EntityType::Circle || e.type == sketch::EntityType::Arc) {
            p.setPen(pen(e.id, e.construction));
            const Point2 c = pointPos(e.center);
            double r = e.type == sketch::EntityType::Circle ? radius(e) : (pointPos(e.start) - c).norm();
            double a0 = 0, a1 = 2 * PI;
            if (e.type == sketch::EntityType::Arc) {
                const Point2 s0 = pointPos(e.start) - c, s1 = pointPos(e.end) - c;
                a0 = std::atan2(s0.y(), s0.x());
                a1 = std::atan2(s1.y(), s1.x());
                if (a1 <= a0) a1 += 2 * PI;
            }
            QPainterPath path;
            const int N = 128;
            for (int i = 0; i <= N; ++i) {
                const double a = a0 + (a1 - a0) * i / N;
                const QPointF q = toScreen(c + Point2(r * std::cos(a), r * std::sin(a)));
                if (i == 0) path.moveTo(q);
                else path.lineTo(q);
            }
            p.drawPath(path);
        }
    }
    // Points.
    std::set<std::string> centers;
    for (const auto& e : d->entities)
        if (!e.center.empty()) centers.insert(e.center);
    const std::set<std::string> owned = api::sketchops::ownedPoints(*d);
    for (const auto& e : d->entities) {
        if (e.type != sketch::EntityType::Point) continue;
        const QPointF q = toScreen(pointPos(e.id));
        const bool emph = selection_.count(e.id) || hover_.id == e.id ||
                          std::find(picked_.begin(), picked_.end(), e.id) != picked_.end();
        const QColor col = std::find(picked_.begin(), picked_.end(), e.id) != picked_.end() ? COLOR_SELECTED.color : entityColor(e.id);
        if (centers.count(e.id)) {
            p.setPen(QPen(col, emph ? 2 : 1));
            const double h = SK_CENTER_PX / 2.0;
            p.drawLine(q + QPointF(-h, 0), q + QPointF(h, 0));
            p.drawLine(q + QPointF(0, -h), q + QPointF(0, h));
        } else if (!owned.count(e.id)) {
            // Standalone point: a filled circle with a contrasting outline, larger than curve ends (FX9-SK-007).
            const double r = pointMarkerPx(true, emph) / 2.0;
            p.setPen(QPen(COLOR_SK_POINT_OUTLINE.color, SK_POINT_OUTLINE_PX));
            p.setBrush(col);
            p.drawEllipse(q, r, r);
            p.setBrush(Qt::NoBrush);
        } else {
            const double h = pointMarkerPx(false, emph) / 2.0;
            p.fillRect(QRectF(q.x() - h, q.y() - h, 2 * h, 2 * h), col);
        }
    }
    // Sketch origin.
    {
        const QPointF o = toScreen(Point2::Zero());
        p.setPen(Qt::NoPen);
        p.setBrush(hover_.id == "origin" || selection_.count("origin") ? COLOR_HOVER.color : COLOR_ORIGIN_POINT.color);
        p.drawEllipse(o, 3.5, 3.5);
        p.setBrush(Qt::NoBrush);
    }
    // Constraint icons (§6.6).
    {
        QFont gf;
        gf.setPixelSize(GLYPH_PX - 5);
        p.setFont(gf);
        std::set<std::string> conflict(st.result.redundant.begin(), st.result.redundant.end()), redundant = conflict;
        conflict.clear();
        for (const auto& grp : st.result.conflicts) conflict.insert(grp.ids.begin(), grp.ids.end());
        for (const auto& gl : ann.glyphs()) {
            QColor bg = COLOR_GLYPH_BG.color, fg = COLOR_GLYPH_FG.color;
            if (sketch::isWeakId(gl.constraint)) fg = COLOR_DIM_WEAK.color;  // weak relation — pale (§6.6а)
            if (conflict.count(gl.constraint)) bg = COLOR_SK_CONFLICT.color, fg = Qt::white;
            else if (redundant.count(gl.constraint)) bg = COLOR_WARNING.color, fg = Qt::white;
            if (selection_.count(gl.constraint)) bg = COLOR_SELECTED.color, fg = Qt::white;
            else if (hover_.id == gl.constraint) bg = COLOR_HOVER.color, fg = Qt::white;
            if (gl.leader) {  // alignment with a segment midpoint (FX8-SK-003)
                p.setPen(QPen(bg == COLOR_GLYPH_BG.color ? COLOR_GLYPH_FG.color : bg, 1, Qt::DashLine));
                p.drawLine(*gl.leader);
            }
            p.fillRect(gl.rect, bg);
            p.setPen(fg);
            p.drawText(gl.rect, Qt::AlignCenter, gl.symbol);
        }
    }
    // Dimensions.
    QFont df;
    df.setPixelSize(DIM_TEXT_PX);
    p.setFont(df);
    for (const auto& c : g.data()->constraints) {  // together with the weak ones (§6.6а)
        if (!sketch::isDimensionType(c.type)) continue;
        const auto rect = ann.labelRect(c.id);
        if (!rect) continue;
        const bool weak = sketch::isWeakId(c.id);
        const bool driven = !c.driving.value_or(true) && !weak;
        bool err = false;
        for (const auto& e : st.result.errors)
            if (const auto* id = e.stringParam("id"); id && *id == c.id) err = true;
        QColor col = err ? COLOR_SK_CONFLICT.color : weak ? COLOR_DIM_WEAK.color : driven ? COLOR_DIM_DRIVEN.color : COLOR_DIM.color;
        if (selection_.count(c.id)) col = COLOR_SELECTED.color;
        else if (hover_.id == c.id) col = COLOR_HOVER.color;
        // Extension line to the middle of the geometry.
        Point2 m = Point2::Zero();
        int n = 0;
        for (const auto& r : c.refs) {
            const auto* e = r == "origin" ? nullptr : g.data()->findEntity(r);
            if (!e || e->type == sketch::EntityType::Point) m += pointPos(r);
            else if (e->type == sketch::EntityType::Line) m += (pointPos(e->p1) + pointPos(e->p2)) / 2;
            else m += pointPos(e->center);
            ++n;
        }
        if (n) m /= n;
        if (!ann.drawDimensionGeometry(p, c, rect->center(), col)) {
            p.setPen(QPen(col, 1, Qt::DashLine));
            p.drawLine(toScreen(m), rect->center());
        }
        p.fillRect(*rect, QColor(255, 255, 255, 200));
        p.setPen(col);
        p.drawText(*rect, Qt::AlignCenter, ann.dimText(c));
    }
    // Dimension preview under the cursor (§6.9, FX2-SK-002): semi-transparent, label at the future click point.
    if (dimPreview_) {
        QColor col = COLOR_DIM.color;
        col.setAlpha(DIM_PREVIEW_ALPHA);
        // With temporary projections (FX3-SK-001) — geometry and labels from the preview data.
        std::optional<SketchAnnotations> own;
        if (dimPreview_->data)
            own.emplace(SketchGeometry(dimPreview_->data.get(), dimPreview_->state.get(), dimPreview_->state.get(), view_.camera()),
                        model_.session(), [](const std::string&) { return false; }, Hit{}, false);
        const SketchAnnotations& pa = own ? *own : ann;
        pa.drawDimensionGeometry(p, dimPreview_->c, dimPreview_->label, col);
        const QString t = pa.dimText(dimPreview_->c);
        QRectF r = QFontMetricsF(df).boundingRect(t);
        r.adjust(-3, -1, 3, 1);
        r.moveCenter(dimPreview_->label);
        p.fillRect(r, QColor(255, 255, 255, 160));
        p.setPen(col);
        p.drawText(r, Qt::AlignCenter, t);
    }
    // Rubber-band geometry is in the sketch plane (app-ux §6.1): in isometric view a circle looks like an ellipse.
    if (cursorValid_ && (!clicks_.empty() || (tool_ == Tool::TangentArc && !chainEndPoint_.empty()))) {
        QPen rp(COLOR_SK_UNDER.color, 1, Qt::DashLine);
        rp.setDashPattern({4, 4});
        p.setPen(rp);
        p.setBrush(Qt::NoBrush);
        const Point2 fallback = clicks_.empty() ? pointPos(chainEndPoint_) : clicks_.back();
        Point2 cur = hoverSnap_.kind != Snap::Kind::None ? hoverSnap_.at : toLocal(cursor_).value_or(fallback);
        // Direction hint of a segment or centerline (§6.4 items 4–6, FX7-SK-011): the rubber end turns to the snapped
        // direction and its symbol is shown at the cursor — the same as the click would set.
        const QString dir = directionHint();
        if (!dir.isEmpty() && hoverSnap_.kind == Snap::Kind::None) cur = directionSnap(clicks_.back(), cur).end;
        for (const auto& pl : rubberPolylines(cur)) p.drawPolyline(pl.data(), int(pl.size()));
        // Hint symbols are large and bold (FX8-SK-006): SNAP_HINT_PX, bold font.
        QFont hf = p.font();
        hf.setPixelSize(SNAP_HINT_PX);
        hf.setBold(true);
        // Equality and arc tangency hints (FX7-SK-003, FX7-SK-002): "=" / "◠" at the cursor and at the other element;
        // the sample element of the equality is highlighted (FX8-SK-006).
        const QString eqTarget = equalHint();
        if (!eqTarget.isEmpty())
            if (const auto* te = d->findEntity(eqTarget.toStdString())) {
                p.setPen(QPen(COLOR_SNAP.color, SNAP_SAMPLE_WIDTH_PX));
                if (te->type == sketch::EntityType::Line) {
                    p.drawLine(toScreen(pointPos(te->p1)), toScreen(pointPos(te->p2)));
                } else {
                    const Point2 c = pointPos(te->center);
                    const double r = te->type == sketch::EntityType::Circle ? radius(*te) : (pointPos(te->start) - c).norm();
                    double a0 = 0, a1 = 2 * PI;
                    if (te->type == sketch::EntityType::Arc) {
                        const Point2 s0 = pointPos(te->start) - c, s1 = pointPos(te->end) - c;
                        a0 = std::atan2(s0.y(), s0.x());
                        a1 = std::atan2(s1.y(), s1.x());
                        if (a1 <= a0) a1 += 2 * PI;
                    }
                    QPainterPath path;
                    for (int i = 0; i <= 96; ++i) {
                        const double a = a0 + (a1 - a0) * i / 96;
                        const QPointF q = toScreen(c + Point2(r * std::cos(a), r * std::sin(a)));
                        i == 0 ? path.moveTo(q) : path.lineTo(q);
                    }
                    p.drawPath(path);
                }
            }
        p.setFont(hf);
        for (const auto& [target, sym] : {std::pair<QString, QString>{eqTarget, "="}, {arcTangentHint(), QString::fromUtf8("◠")}}) {
            if (target.isEmpty()) continue;
            p.setPen(QPen(COLOR_SNAP.color, 2));
            p.drawText(QRectF(cursor_ + QPointF(SNAP_HINT_PX / 2, -SNAP_HINT_PX * 3 / 2), QSizeF(SNAP_HINT_PX, SNAP_HINT_PX)), Qt::AlignCenter, sym);
            if (const auto q = screenPosOfEntity(target.toStdString()))
                p.drawText(QRectF(*q + QPointF(SNAP_HINT_PX / 2, -SNAP_HINT_PX), QSizeF(SNAP_HINT_PX, SNAP_HINT_PX)), Qt::AlignCenter, sym);
        }
        if (!dir.isEmpty()) {
            static const std::map<QString, QString> symbol = {{"horizontal", "H"}, {"vertical", "V"}, {"parallel", "∥"},
                                                              {"perpendicular", "⊥"}, {"tangent", "◠"}};
            p.setPen(QPen(COLOR_SNAP.color, 2));
            p.drawText(QRectF(cursor_ + QPointF(SNAP_HINT_PX / 2, SNAP_HINT_PX / 2), QSizeF(SNAP_HINT_PX, SNAP_HINT_PX)), Qt::AlignCenter,
                       symbol.count(dir) ? symbol.at(dir) : dir);
        }
    }
    // Offset (§6.3, S7F-SK-005): body edges of the contour — as selected, the result — dashed preview.
    if (tool_ == Tool::Offset) {
        std::vector<std::vector<QPointF>> edges;
        const auto preview = offsetPreview(edges);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(COLOR_SELECTED.color, EDGE_SELECTED_WIDTH_PX));
        for (const auto& pl : edges) p.drawPolyline(pl.data(), int(pl.size()));
        QPen rp(COLOR_SK_UNDER.color, 1, Qt::DashLine);
        rp.setDashPattern({4, 4});
        p.setPen(rp);
        for (const auto& pl : preview) p.drawPolyline(pl.data(), int(pl.size()));
    }
    // Pattern preview — dashed in the sketch plane (§6.3 «Sketch pattern»).
    if (patternBox_->isVisible()) {
        QPen rp(COLOR_SK_UNDER.color, 1, Qt::DashLine);
        rp.setDashPattern({4, 4});
        p.setPen(rp);
        p.setBrush(Qt::NoBrush);
        for (const auto& pl : patternPreview()) p.drawPolyline(pl.data(), int(pl.size()));
    }
    // Selection box: left to right — solid, right to left — dashed (§6.3).
    if (boxing_) {
        p.setPen(QPen(COLOR_SELECTED.color, 1, cursor_.x() >= pressPos_.x() ? Qt::SolidLine : Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(pressPos_, cursor_).normalized());
    }
    // The piece removed by trimming — dashed COLOR_SK_CONFLICT (§6.11).
    if (trimPreview_) {
        QPen tp(COLOR_SK_CONFLICT.color, EDGE_SELECTED_WIDTH_PX, Qt::DashLine);
        p.setPen(tp);
        const auto& rp = trimPreview_->removed;
        if (rp.kind == sketch::TrimPiece::Kind::Line) {
            p.drawLine(toScreen(rp.a), toScreen(rp.b));
        } else if (rp.kind == sketch::TrimPiece::Kind::Ellipse || rp.kind == sketch::TrimPiece::Kind::EllipseArc) {  // S4-SKT-007
            const sketch::SolvedState::EllipseShape sh{rp.rx, rp.ry, rp.angle};
            const auto pl = rp.kind == sketch::TrimPiece::Kind::Ellipse ? ellipsePolyline(rp.center, sh, 128)
                                                                       : ellipseArcPolyline(rp.center, sh, rp.start, rp.end, 96);
            QPolygonF poly;
            for (const auto& q : pl) poly << toScreen(q);
            p.drawPolyline(poly);
        } else {
            const double r = rp.kind == sketch::TrimPiece::Kind::Circle ? rp.r : (rp.start - rp.center).norm();
            double a0 = 0, sw = 2 * PI;
            if (rp.kind == sketch::TrimPiece::Kind::Arc) {
                a0 = std::atan2((rp.start - rp.center).y(), (rp.start - rp.center).x());
                const double a1 = std::atan2((rp.end - rp.center).y(), (rp.end - rp.center).x());
                sw = geom2::ccwSweepPositive(a0, a1);
            }
            QPolygonF poly;
            const int n = 96;
            for (int i = 0; i <= n; ++i) {
                const double a = a0 + sw * i / n;
                poly << toScreen(rp.center + r * Point2(std::cos(a), std::sin(a)));
            }
            p.drawPolyline(poly);
        }
    }
    // The hovered implicit target of «Dimension» / «Constraint» (FX9-SK-006): a silhouette line / extreme point or a sketch
    // axis (body edges and vertices are highlighted by the view).
    if (cursorValid_ && (tool_ == Tool::Dimension || tool_ == Tool::Constraint) && toolHover_.view.kind == PickTarget::Kind::None &&
        toolHover_.hit.id.rfind("src", 0) == 0) {
        const std::string src = toolHover_.hit.id.substr(5);
        p.setPen(QPen(COLOR_HOVER.color, SNAP_MARK_WIDTH_PX));
        if (src == "sketch:x" || src == "sketch:y") {
            const bool x = src == "sketch:x";
            const QPointF o = toScreen(Point2::Zero()), e = toScreen(x ? Point2(1, 0) : Point2(0, 1));
            const QPointF dv = e - o;
            const double len = std::hypot(dv.x(), dv.y());
            if (len > 0) {
                const QPointF u = dv / len * double(view_.width() + view_.height());
                p.drawLine(o - u, o + u);
            }
        } else if (src.rfind("refaxis:", 0) == 0) {  // sketch.md §2.5
            // A reference axis feature: the line of its projection, from the reference geometry of the shown state.
            if (const auto g = model_.session().referenceGeometry(api::StateRef::final())) {
                if (const auto ax = g->axes.find(src.substr(8)); ax != g->axes.end()) {
                    const QPointF a = toScreen(state_->frame.toLocal(ax->second.point));
                    const QPointF b = toScreen(state_->frame.toLocal(Point3(ax->second.point + ax->second.dir)));
                    const QPointF dv = b - a;
                    const double len = std::hypot(dv.x(), dv.y());
                    if (len > 0) {
                        const QPointF u = dv / len * double(view_.width() + view_.height());
                        p.drawLine(a - u, a + u);
                    }
                }
            }
        } else if (const auto* tbs = bodyTargets()) {
            for (const DrawBody& tb : *tbs) {
                for (const auto& ed : tb.mesh.edges)
                    if (ed.name == src) {
                        QPolygonF poly;
                        for (const auto& pl : ed.polylines)
                            for (size_t i = 0; i + 2 < pl.size(); i += 3) poly << toScreen(state_->frame.toLocal(Point3(pl[i], pl[i + 1], pl[i + 2])));
                        p.drawPolyline(poly);
                    }
                for (const auto& v : tb.mesh.vertices)
                    if (v.name == src) p.drawEllipse(toScreen(state_->frame.toLocal(Point3(v.position[0], v.position[1], v.position[2]))), SNAP_MARK_PX, SNAP_MARK_PX);
            }
        }
    }
    if (cursorValid_ && tool_ != Tool::Select && tool_ != Tool::Dimension && tool_ != Tool::Constraint && tool_ != Tool::Trim) {
        const QPointF q = toScreen(hoverSnap_.at);
        // Position snap mark (FX8-SK-006): half-size SNAP_MARK_PX, line SNAP_MARK_WIDTH_PX.
        const double m = SNAP_MARK_PX;
        p.setPen(QPen(COLOR_SNAP.color, SNAP_MARK_WIDTH_PX));
        if (hoverSnap_.kind == Snap::Kind::Point || hoverSnap_.kind == Snap::Kind::Center) p.drawEllipse(q, m, m);
        else if (hoverSnap_.kind == Snap::Kind::Midpoint) p.drawPolygon(QPolygonF{q + QPointF(0, -m), q + QPointF(m, 0), q + QPointF(0, m), q + QPointF(-m, 0)});
        else if (hoverSnap_.kind == Snap::Kind::Curve) p.drawArc(QRectF(q.x() - m, q.y() - m, 2 * m, 2 * m), 0, 180 * 16);
        else if (hoverSnap_.kind == Snap::Kind::Intersection) {
            p.drawLine(q + QPointF(-m, -m), q + QPointF(m, m));
            p.drawLine(q + QPointF(-m, m), q + QPointF(m, -m));
        }
        // The implicit target curve the snap holds is shown — and only it (FX8-SK-002, FX8-SK-010): a silhouette line is
        // not an edge of the body, a projection of an edge off the plane is not where the edge is seen.
        if (const auto* tbs = bodyTargets())
            for (const DrawBody& tb : *tbs)
                for (const auto& ed : tb.mesh.edges)
                    if (ed.name == hoverSnap_.source || ed.name == hoverSnap_.source2) {
                        QPolygonF poly;
                        for (const auto& pl : ed.polylines)
                            for (size_t i = 0; i + 2 < pl.size(); i += 3) poly << toScreen(state_->frame.toLocal(Point3(pl[i], pl[i + 1], pl[i + 2])));
                        p.setPen(QPen(COLOR_SNAP.color, SNAP_MARK_WIDTH_PX - 1));
                        p.drawPolyline(poly);
                    }
        // Alignment lines — dashed across the whole visible area (§6.4).
        QPen al(COLOR_SNAP.color, 1, Qt::DashLine);
        p.setPen(al);
        if (hoverSnap_.alignX) p.drawLine(QPointF(toScreen(*hoverSnap_.alignX).x(), 0), QPointF(toScreen(*hoverSnap_.alignX).x(), view_.height()));
        if (hoverSnap_.alignY) p.drawLine(QPointF(0, toScreen(*hoverSnap_.alignY).y()), QPointF(view_.width(), toScreen(*hoverSnap_.alignY).y()));
        // Segment extension (§6.4 item 3в): dashed from the near end of the segment to the point.
        if (const auto* ln = hoverSnap_.extLine.empty() ? nullptr : data()->findEntity(hoverSnap_.extLine)) {
            const Point2 a = pointPos(ln->p1), b = pointPos(ln->p2);
            const Point2 from = (a - hoverSnap_.at).norm() < (b - hoverSnap_.at).norm() ? a : b;
            p.drawLine(toScreen(from), toScreen(hoverSnap_.at));
        }
    }
}

}  // namespace construe::app
