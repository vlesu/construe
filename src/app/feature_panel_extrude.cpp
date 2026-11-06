// Extrude and revolve panels (spec/app-ux.md §5.1, §5.2).
#include "app/feature_panel.h"
#include "app/flip_button.h"

#include <QStandardItemModel>

#include "app/app_model.h"
#include "app/captions.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QHBoxLayout>

#include <algorithm>
#include <numbers>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace construe::app {

std::optional<PickTarget> selectionBase(const std::set<PickTarget>& view, const std::optional<std::string>& treePlane) {
    for (const auto& t : view)
        if (t.kind == PickTarget::Kind::Plane || t.kind == PickTarget::Kind::Face) return t;
    if (treePlane) return PickTarget{PickTarget::Kind::Plane, {}, *treePlane};
    return std::nullopt;
}

// ---------------------------------------------------------------- extrude (§5.1)

ExtrudePanel::ExtrudePanel(AppModel& model, QWidget* parent) : FeaturePanel(model, txt("ui.common.extrude"), parent) {
    setObjectName("extrudePanel");
    auto* endRow = new QHBoxLayout;
    endType_ = new QComboBox;
    endType_->setObjectName("endType");
    endType_->addItem(txt("ui.feature.blind"), "blind");
    endType_->addItem(txt("ui.common.symmetric"), "symmetric");
    endType_->addItem(txt("ui.feature.throughAll"), "throughAll");  // 1б
    endType_->addItem(txt("ui.feature.throughAllBothSides"), "throughAllBoth");  // cut only (S1-FEAT-006)
    endType_->addItem(txt("ui.feature.upToNext"), "upToNext");  // S7F-FEAT-001
    endType_->addItem(txt("ui.feature.upTo"), "upTo");
    reverse_ = new FlipButton("reverse", txt("ui.feature.flipDirection"));
    endRow->addWidget(endType_, 1);
    endRow->addWidget(reverse_);
    form_->addRow(txt("ui.feature.endType"), endRow);
    auto* depthRow = new QHBoxLayout;
    depth_ = newDimensionField("end.depth");
    depth_->setObjectName("depth");
    depthRow->addWidget(dimRow(depth_), 1);
    form_->addRow(txt("ui.feature.depth"), depthRow);
    depthLabel_ = form_->labelForField(depthRow);
    // «Up to» (§5.1, S7F-FEAT-001): the end object and the offset from it.
    upToLabel_ = addSelectionField("upToRefs", txt("ui.feature.endObject"), txt("ui.feature.selectAFacePlaneEdge"), [this](int i) {
        if (i >= 0 && i < int(upTo_.size())) upTo_.erase(upTo_.begin() + i);
    });
    upToLabel_->setRequired(true);
    upToItems_ = upToLabel_->items();
    upToOffsetRow_ = new QWidget;
    {
        auto* r = new QHBoxLayout(upToOffsetRow_);
        r->setContentsMargins(0, 0, 0, 0);
        upToOffset_ = newDimensionField("end.offset");
        upToOffset_->setObjectName("upToOffset");
        r->addWidget(dimRow(upToOffset_), 1);
    }
    form_->addRow(txt("ui.common.offset"), upToOffsetRow_);
    connect(upToOffset_, &QLineEdit::textEdited, this, &ExtrudePanel::changed);
    connect(upToOffset_, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
    opRow_ = buildOperationRow();
    form_->addRow(txt("ui.feature.operation"), opRow_);
    // Tab extensions of a sheet part (§5.3к, FX7-SH-003): shown for "add" to a depth in a sheet metal part.
    auto tabRow = [this](ExprField*& field, const char* name, const char* label, const char* param) {
        auto* row = new QHBoxLayout;
        field = newDimensionField(param);
        field->setObjectName(name);
        field->setPlaceholderText("0");
        field->setAllowEmpty(true);
        row->addWidget(dimRow(field), 1);
        form_->addRow(label, row);
        connect(field, &QLineEdit::textEdited, this, &ExtrudePanel::changed);
        return row;
    };
    tabLeftRow_ = tabRow(tabLeft_, "tabLeft", txt("ui.feature.tabLeft"), "tab.left");
    tabRightRow_ = tabRow(tabRight_, "tabRight", txt("ui.feature.tabRight"), "tab.right");
    buildTargetsRow();
    connect(endType_, &QComboBox::currentIndexChanged, this, [this] {
        const QString t = endType_->currentData().toString();
        reverse_->setVisible(t != "symmetric" && t != "throughAllBoth");
        // «Through all» and «Up to» have no depth (app-ux §5.1).
        const bool depthUsed = t == "blind" || t == "symmetric";
        dimRow(depth_)->setVisible(depthUsed);
        if (depthLabel_) depthLabel_->setVisible(depthUsed);
        autoOperation(false);
        updateTabRows();
        updateUpToRows();
        changed();
    });
    connect(endType_, &QComboBox::activated, this, [this] { endTouched_ = true; });  // user choice only
    connect(reverse_, &QToolButton::toggled, this, &ExtrudePanel::changed);
    connect(reverse_, &QToolButton::clicked, this, [this] {
        reverseTouched_ = true;
        autoOperation(true);
    });
    connect(ops_, &QButtonGroup::idClicked, this, [this] {
        opTouched_ = true;
        sheetCutDefault();
        autoDirection();
        updateEndTypes();
        updateTitle();
        updateTabRows();
    });
    connect(depth_, &QLineEdit::textEdited, this, &ExtrudePanel::changed);
    connect(depth_, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
}

void ExtrudePanel::startNew(const std::optional<std::string>& sketch) {
    editing_.reset();
    sketch_ = sketch.value_or("");
    regions_.reset();
    title_->setText(baseTitle_);
    const auto d = model_.session().document().document;
    endType_->setCurrentIndex(0);
    reverse_->setChecked(false);
    reverseTouched_ = false;
    opTouched_ = false;
    endTouched_ = false;
    depth_->setText(formatNumber(EXTRUDE_DEFAULT_DEPTH / expr::lengthFactor(model_.session().units().length)));
    tabLeft_->clear();
    tabRight_->clear();
    upTo_.clear();
    upToOffset_->setText("0");
    updateUpToRows();
    defaultOperation();
    applyAssemblyMode({});
    updateEndTypes();
    autoDirection();
    updateTitle();
    updateTabRows();
    banner_->hide();
    changed();
    depth_->setFocus();
    depth_->selectAll();
}

void ExtrudePanel::applyAssemblyMode(const std::vector<std::string>& targets) {
    const bool on = assemblyMode();
    form_->setRowVisible(opRow_, !on);
    if (on) setOperation("cut");
    loadTargets(targets);
}

void ExtrudePanel::updateTitle() {
    if (editing_) return;
    if (assemblyMode()) {  // §8а
        title_->setText(txt("ui.feature.assemblyCut"));
        return;
    }
    title_->setText(operation() == "cut" || operation() == "intersect" ? QString(txt("ui.common.cut")) : baseTitle_);
}

void ExtrudePanel::autoOperation(bool sideChanged) {
    // §5.1 «Automatic operation»: new feature only, until the user toggles the operation.
    if (editing_ || opTouched_ || assemblyMode()) return;
    const QString t = endType_->currentData().toString();
    if (t != "throughAll" && t != "throughAllBoth") return;
    std::string want;
    if (!sideChanged) {
        auto b = model_.session().bodies(api::StateRef::final());
        if (!b || b->empty()) return;
        want = "cut";
    } else {
        if (t != "throughAll" || sketch_.empty()) return;
        auto m = model_.session().extrudeSideMaterial(sketch_, profilesJson(), reverse_->isChecked(), api::StateRef::final());
        if (!m || *m == api::Session::SideMaterial::Partial) return;
        want = *m == api::Session::SideMaterial::Full ? "cut" : "add";
    }
    const std::string cur = operation() == "intersect" ? "cut" : operation() == "new" ? "add" : operation();
    if (cur == want) return;
    setOperation(want);
    sheetCutDefault();
    autoDirection();
    updateEndTypes();
    updateTitle();
    changed();
}

void ExtrudePanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || !f->extrude()) return;
    editing_ = featureId;
    const auto& x = *f->extrude();
    sketch_ = x.sketch;
    loadProfiles(x.profiles);
    title_->setText(QString::fromStdString(f->name));
    upTo_.clear();
    if (x.upTo.is_array())
        for (const auto& r : x.upTo) {
            PickTarget t;
            if (r.contains("face")) t = {PickTarget::Kind::Face, {}, r["face"]};
            else if (r.contains("edge")) t = {PickTarget::Kind::Edge, {}, r["edge"]};
            else if (r.contains("vertex")) t = {PickTarget::Kind::Vertex, {}, r["vertex"]};
            else if (r.contains("feature")) t = {PickTarget::Kind::Plane, {}, r["feature"]};
            else if (r.value("datum", std::string()) == "origin") t = {PickTarget::Kind::Origin, {}, "origin"};
            else t = {PickTarget::Kind::Plane, {}, r.value("datum", std::string())};
            upTo_.push_back({t, r});
        }
    upToOffset_->setText(x.upToOffset.empty() ? QString("0") : displayExpr(x.upToOffset, d->lengthUnit));
    endType_->setCurrentIndex(std::max(0, endType_->findData(QString::fromStdString(x.endType))));
    updateUpToRows();
    updateEndTypes();
    reverse_->setChecked(x.reverse);
    depth_->setText(displayExpr(x.depth, d->lengthUnit));
    tabLeft_->setText(x.tabLeft.empty() ? QString() : displayExpr(x.tabLeft, d->lengthUnit));
    tabRight_->setText(x.tabRight.empty() ? QString() : displayExpr(x.tabRight, d->lengthUnit));
    setOperation(x.operation);
    applyAssemblyMode(x.targets);
    updateTabRows();
    banner_->hide();
    changed();
    depth_->setFocus();
    depth_->selectAll();
}

void ExtrudePanel::updateLabels() {
    FeaturePanel::updateLabels();
    const auto u = model_.session().document().document->lengthUnit;
    showExprState(depth_, expr::LENGTH);
    for (auto* f : {tabLeft_, tabRight_}) showExprState(f, expr::LENGTH);
    showExprState(upToOffset_, expr::LENGTH);
    // The end objects (§5.1): captions of the list style (app-ux §5.5г) — a face or an edge by its number, a vertex by
    // its kind, a plane by its name.
    std::vector<SelectionItem> items;
    for (const auto& [t, j] : upTo_) {
        QString c;
        switch (t.kind) {
            case PickTarget::Kind::Face: c = topoCaption(model_.session(), t.name, false); break;
            case PickTarget::Kind::Edge: c = topoCaption(model_.session(), t.name, true); break;
            case PickTarget::Kind::Vertex: c = txt("ui.feature.vertex"); break;
            default: c = objectCaption(model_.session(), t.name, CaptionStyle::List);
        }
        items.push_back({c, {t}, {}});
    }
    upToLabel_->setSelection(std::move(items));
}

bool ExtrudePanel::upToMode() const { return endType_->currentData().toString() == "upTo"; }

void ExtrudePanel::updateUpToRows() {
    const bool on = upToMode();
    upToLabel_->setFieldVisible(on);
    form_->setRowVisible(upToOffsetRow_, on);
}

bool ExtrudePanel::canAccept() const { return FeaturePanel::canAccept() && (!upToMode() || !upTo_.empty()); }

QString ExtrudePanel::needed() const {
    if (wantsBaseSketch()) return FeaturePanel::needed();
    return txt(upToMode() ? "ui.feature.requiredFacePlaneEdgeOr" : "ui.feature.requiredSketchInTheTree");
}

FeaturePanel::PickOutcome ExtrudePanel::pickTree(const TreePick& p) {
    // The «Up to» end: origin and reference planes, the origin (§5.1, S7F-FEAT-001); the rest — the usual way.
    if (!upToMode() || wantsBaseSketch() || !p.target) return PickOutcome::NotTaken;
    if (p.target->kind != PickTarget::Kind::Plane && p.target->kind != PickTarget::Kind::Origin) return PickOutcome::NotTaken;
    pick(*p.target);
    return PickOutcome::Taken;
}

bool ExtrudePanel::pick(const PickTarget& t) {
    if (t.kind == PickTarget::Kind::Region) {  // «Profiles»: a region is added / removed (§5.5)
        toggleRegion(t.body, t.name);
        return true;
    }
    if (wantsBaseSketch() && t.kind == PickTarget::Kind::Face) return false;  // a non-planar face is not a base (§3.6а)
    if (!upToMode()) return false;
    doc::Json j;
    switch (t.kind) {
        case PickTarget::Kind::Face: j = {{"face", t.name}}; break;
        case PickTarget::Kind::Edge: j = {{"edge", t.name}}; break;
        case PickTarget::Kind::Vertex: j = {{"vertex", t.name}}; break;
        case PickTarget::Kind::Origin: j = {{"datum", "origin"}}; break;
        case PickTarget::Kind::Plane: j = t.name.rfind("plane:", 0) == 0 ? doc::Json{{"datum", t.name}} : doc::Json{{"feature", t.name}}; break;
        default: return false;
    }
    // A repeated click removes the object; a third one replaces the earliest (§5.1).
    for (size_t i = 0; i < upTo_.size(); ++i)
        if (upTo_[i].second == j) {
            upTo_.erase(upTo_.begin() + long(i));
            changed();
            return true;
        }
    if (upTo_.size() >= 2) upTo_.erase(upTo_.begin());
    upTo_.push_back({t, j});
    changed();
    return true;
}

void ExtrudePanel::sheetCutDefault() {
    if (editing_ || endTouched_ || assemblyMode() || operation() != "cut") return;
    if (!model_.session().document().document->active().isSheetPart()) return;
    const int i = endType_->findData("throughAll");
    if (i >= 0 && endType_->currentIndex() != i) endType_->setCurrentIndex(i);
}

bool ExtrudePanel::tabFieldsShown() const {
    if (assemblyMode() || endType_->currentData().toString() != "blind") return false;
    if (operation() != "add" && operation() != "new") return false;
    const auto d = model_.session().document().document;
    return d->active().isSheetPart();
}

void ExtrudePanel::updateTabRows() {
    const bool on = tabFieldsShown();
    form_->setRowVisible(tabLeftRow_, on);
    form_->setRowVisible(tabRightRow_, on);
}

doc::Json ExtrudePanel::definition() const {
    const auto d = model_.session().document().document;
    doc::Json tab = doc::Json::object();  // tab extensions (§5.3к, FX7-SH-003): only non-empty fields
    if (tabFieldsShown())
        for (auto [f, key] : {std::pair{tabLeft_, "left"}, std::pair{tabRight_, "right"}})
            if (!f->text().trimmed().isEmpty()) tab[key] = f->expression();
    const std::string type = endType_->currentData().toString().toStdString();
    doc::Json end{{"type", type}};
    if (type == "blind" || type == "symmetric") end["depth"] = depth_->expression();
    if (type == "upTo") {  // S7F-FEAT-001: the objects; the offset only when not zero
        doc::Json to = doc::Json::array();
        for (const auto& r : upTo_) to.push_back(r.second);
        end["to"] = to;
        const std::string off = upToOffset_->expression();
        if (!off.empty() && off != "0" && off != "0 " + d->lengthUnit) end["offset"] = off;
    }
    doc::Json def{{"type", "extrude"},
                     {"sketch", sketch_},
                     {"profiles", profilesJson()},
                     {"direction", reverse_->isChecked() ? "reverse" : "normal"},
                     {"end", end},
                     {"operation", operation()},
                     {"targets", targets().empty() ? doc::Json("all") : doc::Json(targets())}};
    if (!tab.empty()) def["tab"] = tab;
    return def;
}

std::optional<double> ExtrudePanel::depthMm() const {
    const auto d = model_.session().document().document;
    auto q = model_.session().checkExpression(depth_->expression(), expr::LENGTH);
    if (!q) return std::nullopt;
    return q->value;
}

void ExtrudePanel::autoDirection() {
    if (editing_ || reverseTouched_) return;
    bool onFace = false;
    const auto d = model_.session().document().document;
    if (const auto f = d->active().find(sketch_); f && f->sketchData()) onFace = f->sketchData()->plane.onFace();
    const bool want = onFace && (operation() == "cut" || operation() == "intersect");
    if (reverse_->isChecked() == want) return;
    const QSignalBlocker b(reverse_);
    reverse_->setChecked(want);
    changed();
}

bool ExtrudePanel::symmetric() const { return endType_->currentData().toString() == "symmetric"; }
bool ExtrudePanel::reversed() const { return reverse_->isChecked(); }

void ExtrudePanel::setDepthFromManipulator(double mm, bool flipDirection) {
    const auto u = model_.session().units();
    if (flipDirection) {
        const QSignalBlocker b(reverse_);
        reverse_->setChecked(!reverse_->isChecked());
        reverseTouched_ = true;
    }
    depth_->setText(formatNumber(mm / expr::lengthFactor(u.length)));  // number in document units (§5.6)
    if (flipDirection) autoOperation(true);
    changed();
}

// ---------------------------------------------------------------- revolve (§5.2)

RevolvePanel::RevolvePanel(AppModel& model, QWidget* parent) : FeaturePanel(model, txt("ui.common.revolve"), parent) {
    setObjectName("revolvePanel");
    axis_ = addSelectionField("revolveAxis", txt("ui.common.axis"), txt("ui.feature.selectAnAxis"), [this](int) {
        axisDatum_.clear();
        axisLine_.clear();
    });
    axis_->setRequired(true);
    axisItems_ = axis_->items();
    angleType_ = new QComboBox;
    angleType_->setObjectName("angleType");
    angleType_->addItem(txt("ui.feature.fullTurn"), "full");
    angleType_->addItem(txt("ui.feature.byAngle"), "angle");
    angleType_->addItem(txt("ui.common.symmetric"), "symmetric");
    form_->addRow(txt("ui.feature.angleType"), angleType_);
    auto* angleRow = new QHBoxLayout;
    angle_ = newDimensionField("angle.value");
    angle_->setObjectName("angle");
    angleRow->addWidget(dimRow(angle_), 1);
    form_->addRow(txt("ui.common.angle"), angleRow);
    angleRow_ = angleRow;
    form_->addRow(txt("ui.feature.operation"), buildOperationRow());
    connect(angleType_, &QComboBox::currentIndexChanged, this, [this] {
        const bool full = angleType_->currentData().toString() == "full";
        dimRow(angle_)->setEnabled(!full);
        changed();
    });
    connect(angle_, &QLineEdit::textEdited, this, &RevolvePanel::changed);
    connect(angle_, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
}

void RevolvePanel::defaultAxis() {
    // Exactly one construction line segment in the sketch: use it (§5.2).
    axisDatum_.clear();
    axisLine_.clear();
    const auto d = model_.session().document().document;
    const auto f = sketch_.empty() ? nullptr : d->active().find(sketch_);
    if (!f || !f->sketchData()) return;
    std::vector<std::string> lines;
    for (const auto& e : f->sketchData()->entities)
        if (e.type == sketch::EntityType::Line && e.construction) lines.push_back(e.id);
    if (lines.size() == 1) axisLine_ = lines.front();
}

void RevolvePanel::startNew(const std::optional<std::string>& sketch) {
    editing_.reset();
    sketch_ = sketch.value_or("");
    regions_.reset();
    title_->setText(baseTitle_);
    defaultAxis();
    angleType_->setCurrentIndex(0);
    {
        const auto d = model_.session().document().document;
        angle_->setText(formatNumber(std::numbers::pi / 2 / expr::angleFactor(model_.session().units().angle)));  // REVOLVE_DEFAULT_ANGLE = 90°
    }
    dimRow(angle_)->setEnabled(false);
    defaultOperation();
    banner_->hide();
    changed();
}

void RevolvePanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || !f->revolve()) return;
    editing_ = featureId;
    const auto& r = *f->revolve();
    sketch_ = r.sketch;
    loadProfiles(r.profiles);
    axisDatum_ = r.axisFeature.empty() ? r.axisDatum : r.axisFeature;
    axisLine_ = r.axisSketchLine;
    title_->setText(QString::fromStdString(f->name));
    angleType_->setCurrentIndex(r.angleType == "angle" ? 1 : r.angleType == "symmetric" ? 2 : 0);
    angle_->setText(r.angleValue.empty() ? formatNumber(std::numbers::pi / 2 / expr::angleFactor(model_.session().units().angle))
                                         : displayExpr(r.angleValue, d->angleUnit));
    setOperation(r.operation);
    banner_->hide();
    changed();
}

void RevolvePanel::setSketch(const std::string& sketchId) {
    regions_.reset();
    sketch_ = sketchId;
    if (axisDatum_.empty() && axisLine_.empty()) defaultAxis();
    updateLabels();
    changed();
}

void RevolvePanel::setAxisDatum(const std::string& axis) {
    axisDatum_ = axis;
    axisLine_.clear();
    changed();
}

bool RevolvePanel::pick(const PickTarget& t) {
    if (t.kind == PickTarget::Kind::Region) {
        toggleRegion(t.body, t.name);
        return true;
    }
    if (t.kind != PickTarget::Kind::Axis) return false;
    setAxisDatum(t.name);
    return true;
}

QString RevolvePanel::needed() const { return wantsBaseSketch() ? FeaturePanel::needed() : text("ui.need.profileOrAxis"); }

FeaturePanel::PickOutcome RevolvePanel::pickTree(const TreePick& p) {
    // An origin axis or a reference axis (S2-REF-004) — into «Axis»; a sketch — the usual way.
    if (!p.target || p.target->kind != PickTarget::Kind::Axis) return PickOutcome::NotTaken;
    setAxisDatum(p.target->name);
    return PickOutcome::Taken;
}

void RevolvePanel::setAxisLine(const std::string& lineId) {
    axisLine_ = lineId;
    axisDatum_.clear();
    changed();
}

void RevolvePanel::updateLabels() {
    FeaturePanel::updateLabels();
    // An origin or reference axis (S2-REF-004) or a sketch line: captions of the list style (app-ux §5.5г).
    std::vector<SelectionItem> axis;
    if (!axisDatum_.empty())
        axis.push_back({objectCaption(model_.session(), axisDatum_, CaptionStyle::List), {{PickTarget::Kind::Axis, {}, axisDatum_}}, {}});
    else if (!axisLine_.empty())
        axis.push_back({txt("ui.feature.line") + QString::fromStdString(axisLine_), {{PickTarget::Kind::SketchEntity, sketch_, axisLine_}}, {}});
    axis_->setSelection(std::move(axis));
    showExprState(angle_, expr::ANGLE);
}

doc::Json RevolvePanel::axisJson() const {
    return axisDatum_.empty()                 ? doc::Json{{"sketchLine", axisLine_}}
           : axisDatum_.rfind("axis:", 0) == 0 ? doc::Json{{"datum", axisDatum_}}
                                               : doc::Json{{"feature", axisDatum_}};
}

void RevolvePanel::loadAxis(const doc::Json& a) {
    axisDatum_ = a.contains("datum") ? a["datum"].get<std::string>() : a.value("feature", std::string());
    axisLine_ = a.value("sketchLine", std::string());
}

doc::Json RevolvePanel::definition() const {
    const doc::Json axis = axisJson();
    const std::string type = angleType_->currentData().toString().toStdString();
    doc::Json angle = {{"type", type}};
    if (type != "full") angle["value"] = angle_->expression();
    return doc::Json{{"type", "revolve"}, {"sketch", sketch_}, {"profiles", profilesJson()}, {"axis", axis},
                     {"angle", angle},    {"operation", operation()}, {"targets", "all"}};
}

// ---------------------------------------------------------------- coil (§5.3м, FX7-PT-004)

CoilPanel::CoilPanel(AppModel& model, QWidget* parent) : RevolvePanel(model, parent) {
    setObjectName("coilPanel");
    baseTitle_ = txt("ui.feature.coil");
    title_->setText(baseTitle_);
    // The angle rows of the revolve are not used: pitch, extent, hand and start angle instead.
    form_->setRowVisible(angleType_, false);
    form_->setRowVisible(angleRow_, false);
    auto exprRow = [&](const char* name, ExprField*& field, const char* param) {
        auto* w = new QWidget;
        auto* r = new QHBoxLayout(w);
        r->setContentsMargins(0, 0, 0, 0);
        field = newDimensionField(param);
        field->setObjectName(name);
        r->addWidget(dimRow(field), 1);
        connect(field, &QLineEdit::textEdited, this, &CoilPanel::changed);
        connect(field, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
        return w;
    };
    auto insert = [&](const QString& label, QWidget* w) { form_->insertRow(form_->rowCount() - 1, label, w); };
    insert(txt("ui.common.pitch"), exprRow("coilPitch", pitch_, "pitch"));
    extent_ = new QComboBox;
    extent_->setObjectName("coilExtent");
    extent_->addItem(txt("ui.feature.numberOfTurns"), "turns");
    extent_->addItem(txt("ui.common.height"), "height");
    insert(txt("ui.feature.defineBy"), extent_);
    insert(txt("ui.common.value"), exprRow("coilValue", value_, "height"));
    hand_ = new QComboBox;
    hand_->setObjectName("coilHand");
    hand_->addItem(txt("ui.feature.rightHand"), "right");
    hand_->addItem(txt("ui.feature.leftHand"), "left");
    insert(txt("ui.feature.winding"), hand_);
    insert(txt("ui.feature.startAngle"), exprRow("coilStart", start_, "startAngle"));
    connect(extent_, &QComboBox::currentIndexChanged, this, [this] {
        value_->setText(extent_->currentData().toString() == "turns" ? QString("5")
                                                                     : formatNumber(50 / expr::lengthFactor(model_.session().units().length)));
        changed();
    });
    connect(hand_, &QComboBox::currentIndexChanged, this, &CoilPanel::changed);
}

void CoilPanel::startNew(const std::optional<std::string>& sketch) {
    RevolvePanel::startNew(sketch);
    title_->setText(baseTitle_);
    const double f = expr::lengthFactor(model_.session().units().length);
    pitch_->setText(formatNumber(10 / f));
    {
        const QSignalBlocker b(extent_);
        extent_->setCurrentIndex(0);
    }
    value_->setText("5");
    hand_->setCurrentIndex(0);
    start_->setText("0");
    changed();
}

void CoilPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::Coil) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    sketch_ = def["sketch"];
    if (def["profiles"].is_array()) {
        regions_ = std::vector<std::string>{};
        for (const auto& r : def["profiles"]) regions_->push_back(r);
    } else {
        regions_.reset();
    }
    loadAxis(def["axis"]);
    pitch_->setText(displayExpr(def["pitch"].get<std::string>(), d->lengthUnit));
    {
        const QSignalBlocker b(extent_);
        extent_->setCurrentIndex(def.contains("height") ? 1 : 0);
    }
    value_->setText(def.contains("height") ? displayExpr(def["height"].get<std::string>(), d->lengthUnit)
                                           : QString::fromStdString(def.value("turns", std::string("1"))));
    hand_->setCurrentIndex(def.value("hand", std::string("right")) == "left" ? 1 : 0);
    start_->setText(def.contains("startAngle") ? displayExpr(def["startAngle"].get<std::string>(), d->angleUnit) : QString("0"));
    setOperation(def["operation"]);
    changed();
}

void CoilPanel::updateLabels() {
    RevolvePanel::updateLabels();
    const auto d = model_.session().document().document;
    showExprState(pitch_, expr::LENGTH);
    const bool turns = extent_->currentData().toString() == "turns";
    showExprState(value_, turns ? expr::DIMLESS : expr::LENGTH);
    DimensionField::of(value_)->toleranceEdit()->setVisible(!turns);  // the number of turns is not a dimension (§5.6а)
    showExprState(start_, expr::ANGLE);
}

doc::Json CoilPanel::definition() const {
    const auto d = model_.session().document().document;
    doc::Json def = {{"type", "coil"}, {"sketch", sketch_}, {"profiles", profilesJson()}, {"axis", axisJson()},
                     {"pitch", pitch_->expression()}};
    if (extent_->currentData().toString() == "turns") def["turns"] = value_->expression();
    else def["height"] = value_->expression();
    if (hand_->currentData().toString() == "left") def["hand"] = "left";
    const std::string a = start_->expression();
    if (!a.empty() && a != "0") def["startAngle"] = a;
    def["operation"] = operation();
    return def;
}

}  // namespace construe::app
