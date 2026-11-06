// Fillet and chamfer, shell, draft panels (spec/app-ux.md §5.3, §5.3г, §5.3д).
#include "app/feature_panel.h"
#include "app/flip_button.h"

#include <QStandardItemModel>

#include "app/app_model.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QHBoxLayout>
#include <QPushButton>

#include <algorithm>
#include <numbers>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace construe::app {

// ---------------------------------------------------------------- fillet / chamfer (§5.3)

EdgePanel::EdgePanel(AppModel& model, bool fillet, QWidget* parent)
    : FeaturePanel(model, fillet ? txt("ui.common.fillet") : txt("ui.common.chamfer"), parent, Profiles::Hidden), fillet_(fillet) {
    setObjectName(fillet ? "filletPanel" : "chamferPanel");
    edgesLabel_ = addSelectionField("edges", txt("ui.common.edges"), txt("ui.feature.selectEdges"), [this](int i) {
        if (i < int(edges_.size())) edges_.erase(edges_.begin() + i);
    });
    edgesLabel_->setManyFormat(txt("ui.common.edgesN"));
    edgesLabel_->setRequired(true);
    edgeItems_ = edgesLabel_->items();
    if (!fillet) {
        mode_ = new QComboBox;
        mode_->setObjectName("chamferMode");
        mode_->addItem(txt("ui.common.distance"), "distance");
        mode_->addItem(txt("ui.feature.distanceAndAngle"), "distanceAngle");
        mode_->addItem(txt("ui.feature.twoDistances"), "twoDistances");
        form_->addRow(txt("ui.common.type"), mode_);
        connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
            applyMode();
            changed();
        });
    }
    auto* row = new QHBoxLayout;
    size_ = newDimensionField(fillet ? "radius" : "distance");
    size_->setObjectName(fillet ? "radius" : "distance");
    row->addWidget(dimRow(size_), 1);
    form_->addRow(fillet ? txt("ui.common.radius") : txt("ui.common.distance"), row);
    connect(size_, &QLineEdit::textEdited, this, &EdgePanel::changed);
    connect(size_, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
    if (!fillet) {
        auto* row2 = new QHBoxLayout;
        size2_ = newDimensionField("angle");
        size2_->setObjectName("chamferSecond");
        flip_ = new FlipButton("chamferFlip", txt("ui.feature.flipTheDistanceIsMeasured"));
        row2->addWidget(dimRow(size2_), 1);
        row2->addWidget(flip_);
        form_->addRow(txt("ui.common.angle"), row2);
        size2Label_ = form_->labelForField(row2);
        connect(size2_, &QLineEdit::textEdited, this, &EdgePanel::changed);
        connect(flip_, &QToolButton::toggled, this, &EdgePanel::changed);
        applyMode();
    }
}

void EdgePanel::applyMode() {
    if (!mode_) return;
    const QString m = mode_->currentData().toString();
    const bool on = m != "distance";
    form_->setRowVisible(size2Label_, on);
    DimensionField::of(size2_)->setParam(m == "distanceAngle" ? "angle" : "distance2");  // FX12-PMI-007
    if (auto* l = qobject_cast<QLabel*>(size2Label_)) l->setText(m == "distanceAngle" ? txt("ui.common.angle") : txt("ui.feature.secondDistance"));
    // The default of the second value follows the mode: 45° or the first distance (the chamfer stays equal at first).
    const bool angle = m == "distanceAngle";
    if (on && size2_->property("mode").toString() != m) {
        size2_->setText(angle ? QString("45") : size_->text());
        size2_->setProperty("mode", m);
    }
}

void EdgePanel::startNew(const std::optional<std::string>&) {
    editing_.reset();
    title_->setText(baseTitle_);
    const auto d = model_.session().document().document;
    size_->setText(formatNumber(1 / expr::lengthFactor(model_.session().units().length)));  // FILLET_DEFAULT_RADIUS / CHAMFER_DEFAULT_DISTANCE = 1 mm
    if (mode_) {
        size2_->setProperty("mode", QString());
        flip_->setChecked(false);
        mode_->setCurrentIndex(0);
        applyMode();
    }
    banner_->hide();
    changed();
    size_->setFocus();
    size_->selectAll();
}

void EdgePanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || !f->edgeFeature()) return;
    editing_ = featureId;
    edges_ = f->edgeFeature()->edges;
    title_->setText(QString::fromStdString(f->name));
    size_->setText(displayExpr(f->edgeFeature()->size, d->lengthUnit));
    if (mode_) {
        const auto* e = f->edgeFeature();
        const QString m = e->mode.empty() ? QString("distance") : QString::fromStdString(e->mode);
        size2_->setProperty("mode", m);
        mode_->setCurrentIndex(std::max(0, mode_->findData(m)));
        size2_->setText(e->mode.empty() ? QString() : displayExpr(e->size2, e->mode == "distanceAngle" ? d->angleUnit : d->lengthUnit));
        flip_->setChecked(e->flip);
        applyMode();
    }
    banner_->hide();
    changed();
}

void EdgePanel::setEdges(const std::vector<std::string>& edges) {
    edges_ = edges;
    changed();
}

void EdgePanel::toggleEdge(const std::string& name) {
    auto it = std::find(edges_.begin(), edges_.end(), name);
    if (it == edges_.end()) edges_.push_back(name);
    else edges_.erase(it);
    changed();
}

bool EdgePanel::pick(const PickTarget& t) {
    if (t.kind != PickTarget::Kind::Edge) return false;
    toggleEdge(t.name);
    return true;
}

std::vector<PickTarget> EdgePanel::selectedTargets() const {
    std::vector<PickTarget> out;
    for (const auto& e : edges_) out.push_back({PickTarget::Kind::Edge, {}, e});
    return out;
}

void EdgePanel::updateLabels() {
    ok_->setEnabled(canAccept());
    showExprState(size_, expr::LENGTH);
    std::vector<SelectionItem> items;
    for (const auto& e : edges_) items.push_back({topoCaption(model_.session(), e, true), {{PickTarget::Kind::Edge, {}, e}}, {}});
    edgesLabel_->setSelection(std::move(items));
    if (mode_ && mode_->currentData().toString() != "distance") {
        const bool angle = mode_->currentData().toString() == "distanceAngle";
        const auto d = model_.session().document().document;
        showExprState(size2_, angle ? expr::ANGLE : expr::LENGTH);
    }
}

doc::Json EdgePanel::definition() const {
    const auto d = model_.session().document().document;
    doc::Json edges = doc::Json::array();
    for (const auto& e : edges_) edges.push_back(e);
    doc::Json def{{"type", fillet_ ? "fillet" : "chamfer"}, {"edges", edges},
                  {fillet_ ? "radius" : "distance", size_->expression()}};
    if (mode_ && mode_->currentData().toString() != "distance") {
        const std::string m = mode_->currentData().toString().toStdString();
        def["mode"] = m;
        def[m == "distanceAngle" ? "angle" : "distance2"] = size2_->expression();
        if (flip_->isChecked()) def["flip"] = true;
    }
    return def;
}

// ---------------------------------------------------------------- shell (§5.3г, S2-FEAT-004)

ShellPanel::ShellPanel(AppModel& model, QWidget* parent) : FeaturePanel(model, txt("ui.common.shell"), parent, Profiles::Hidden) {
    setObjectName("shellPanel");
    facesLabel_ = addSelectionField("shellFaces", txt("ui.feature.removeFaces"), txt("ui.feature.selectFaces"), [this](int i) {
        if (i < int(faces_.size())) faces_.erase(faces_.begin() + i);
    });
    facesLabel_->setManyFormat(txt("ui.common.facesN"));
    faceItems_ = facesLabel_->items();
    auto* row = new QHBoxLayout;
    thickness_ = newDimensionField("thickness");
    thickness_->setObjectName("shellThickness");
    outward_ = new FlipButton("shellOutward", txt("ui.feature.outward"));
    row->addWidget(dimRow(thickness_), 1);
    row->addWidget(outward_);
    form_->addRow(txt("ui.feature.thickness"), row);
    connect(thickness_, &QLineEdit::textEdited, this, &ShellPanel::changed);
    connect(thickness_, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
    connect(outward_, &QToolButton::toggled, this, &ShellPanel::changed);
}

void ShellPanel::startNew(const std::optional<std::string>&) {
    editing_.reset();
    title_->setText(baseTitle_);
    faces_.clear();
    thickness_->setText(formatNumber(2 / expr::lengthFactor(model_.session().units().length)));  // SHELL_DEFAULT_THICKNESS = 2 mm
    outward_->setChecked(false);
    banner_->hide();
    changed();
    thickness_->setFocus();
    thickness_->selectAll();
}

void ShellPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::Shell) return;
    editing_ = featureId;
    const doc::Json& def = f->pattern()->def;
    faces_.clear();
    for (const auto& x : def["faces"]) faces_.push_back(x);
    title_->setText(QString::fromStdString(f->name));
    thickness_->setText(displayExpr(def["thickness"].get<std::string>(), d->lengthUnit));
    outward_->setChecked(def.value("outward", false));
    banner_->hide();
    changed();
}

void ShellPanel::setFaces(const std::vector<std::string>& faces) {
    faces_ = faces;
    changed();
}

void ShellPanel::toggleFace(const std::string& name) {
    auto it = std::find(faces_.begin(), faces_.end(), name);
    if (it == faces_.end()) faces_.push_back(name);
    else faces_.erase(it);
    changed();
}

bool ShellPanel::pick(const PickTarget& t) {
    if (t.kind != PickTarget::Kind::Face) return false;
    toggleFace(t.name);
    return true;
}

std::vector<PickTarget> ShellPanel::selectedTargets() const {
    std::vector<PickTarget> out;
    for (const auto& f : faces_) out.push_back({PickTarget::Kind::Face, {}, f});
    return out;
}

void ShellPanel::updateLabels() {
    ok_->setEnabled(canAccept());
    std::vector<SelectionItem> items;
    for (const auto& f : faces_) items.push_back({topoCaption(model_.session(), f, false), {{PickTarget::Kind::Face, {}, f}}, {}});
    facesLabel_->setSelection(std::move(items));
    showExprState(thickness_, expr::LENGTH);
}

doc::Json ShellPanel::definition() const {
    const auto d = model_.session().document().document;
    doc::Json def = {{"type", "shell"}, {"faces", faces_}, {"thickness", thickness_->expression()}};
    if (outward_->isChecked()) def["outward"] = true;
    return def;
}

// ---------------------------------------------------------------- draft (§5.3д, S2-FEAT-005)

DraftPanel::DraftPanel(AppModel& model, QWidget* parent) : FeaturePanel(model, txt("ui.common.draft"), parent, Profiles::Hidden) {
    setObjectName("draftPanel");
    neutralField_ = addSelectionField("draftNeutral", txt("ui.feature.neutralPlane"), txt("ui.feature.selectAPlane"), [this](int) { neutral_.reset(); });
    facesField_ = addSelectionField("draftFaces", txt("ui.common.faces"), txt("ui.feature.selectFaces"), [this](int i) {
        if (i < int(faces_.size())) faces_.erase(faces_.begin() + i);
    });
    facesField_->setManyFormat(txt("ui.common.facesN"));
    for (auto* f : {neutralField_, facesField_}) f->setRequired(true), fields_.add(f);
    neutralItems_ = neutralField_->items();
    faceItems_ = facesField_->items();
    // «Select all adjacent faces» (§5.3д, FX10-PT-005).
    adjacent_ = new QPushButton(txt("ui.feature.selectAllAdjacentFaces"));
    adjacent_->setObjectName("draftAdjacent");
    form_->addRow(adjacent_);
    connect(adjacent_, &QPushButton::clicked, this, &DraftPanel::selectAdjacentFaces);
    auto* row = new QHBoxLayout;
    angle_ = newDimensionField("angle");
    angle_->setObjectName("draftAngle");
    reverse_ = new FlipButton("draftReverse", txt("ui.feature.flipPullDirection"));
    row->addWidget(dimRow(angle_), 1);
    row->addWidget(reverse_);
    form_->addRow(txt("ui.common.angle"), row);
    connect(angle_, &QLineEdit::textEdited, this, &DraftPanel::changed);
    connect(angle_, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
    connect(reverse_, &QToolButton::toggled, this, &DraftPanel::changed);
}

void DraftPanel::startNew(const std::optional<std::string>&) {
    editing_.reset();
    title_->setText(baseTitle_);
    neutral_.reset();
    faces_.clear();
    fields_.setActive(neutralField_);
    angle_->setText(formatNumber(3.0 * (std::numbers::pi / 180) / expr::angleFactor(model_.session().units().angle)));  // DRAFT_DEFAULT_ANGLE = 3°
    reverse_->setChecked(false);
    banner_->hide();
    changed();
}

void DraftPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::Draft) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    for (const auto& x : def["faces"]) faces_.push_back(x);
    const doc::Json& p = def["neutral"];
    if (p.contains("face")) neutral_ = PickTarget{PickTarget::Kind::Face, {}, p["face"]};
    else neutral_ = PickTarget{PickTarget::Kind::Plane, {}, p.contains("datum") ? p["datum"].get<std::string>() : p.value("feature", std::string())};
    angle_->setText(displayExpr(def["angle"].get<std::string>(), d->angleUnit));
    reverse_->setChecked(def.value("reverse", false));
    fields_.setActive(facesField_);
    changed();
}

bool DraftPanel::pick(const PickTarget& t) {
    if (t.kind == PickTarget::Kind::Plane || (t.kind == PickTarget::Kind::Face && neutralField_->isActive())) {
        neutral_ = t;
        fields_.setActive(facesField_);
    } else if (t.kind == PickTarget::Kind::Face) {
        auto it = std::find(faces_.begin(), faces_.end(), t.name);
        if (it == faces_.end()) faces_.push_back(t.name);
        else faces_.erase(it);
    } else {
        return false;
    }
    changed();
    return true;
}

void DraftPanel::selectAdjacentFaces() {
    if (!neutral_) return;
    auto r = model_.session().draftAdjacentFaces(definition()["neutral"], faces_, selectionState());
    if (!r) return;
    for (const auto& f : *r)
        if (std::find(faces_.begin(), faces_.end(), f) == faces_.end()) faces_.push_back(f);
    fields_.setActive(facesField_);
    changed();
}

FeaturePanel::PickOutcome DraftPanel::pickTree(const TreePick& p) {
    // An origin or reference plane — the neutral plane; other nodes do not fit (§5.3д).
    if (!p.target || p.target->kind != PickTarget::Kind::Plane) return PickOutcome::Refused;
    pick(*p.target);
    return PickOutcome::Taken;
}

std::vector<PickTarget> DraftPanel::selectedTargets() const {
    std::vector<PickTarget> out;
    if (neutral_) out.push_back(*neutral_);
    for (const auto& f : faces_) out.push_back({PickTarget::Kind::Face, {}, f});
    return out;
}

void DraftPanel::updateLabels() {
    ok_->setEnabled(canAccept());
    std::vector<SelectionItem> neutral;
    if (neutral_)
        neutral.push_back({neutral_->kind == PickTarget::Kind::Face ? topoCaption(model_.session(), neutral_->name, false)
                                                                    : objectCaption(model_.session(), neutral_->name, CaptionStyle::List),
                           {*neutral_}, {}});
    neutralField_->setSelection(std::move(neutral));
    std::vector<SelectionItem> items;
    for (const auto& f : faces_) items.push_back({topoCaption(model_.session(), f, false), {{PickTarget::Kind::Face, {}, f}}, {}});
    facesField_->setSelection(std::move(items));
    adjacent_->setEnabled(neutral_.has_value());
    showExprState(angle_, expr::ANGLE);
}

doc::Json DraftPanel::definition() const {
    const auto d = model_.session().document().document;
    doc::Json neutral = nullptr;
    if (neutral_)
        neutral = neutral_->kind == PickTarget::Kind::Face        ? doc::Json{{"face", neutral_->name}}
                  : neutral_->name.rfind("plane:", 0) == 0 ? doc::Json{{"datum", neutral_->name}}
                                                            : doc::Json{{"feature", neutral_->name}};
    doc::Json def = {{"type", "draft"}, {"faces", faces_}, {"neutral", neutral}, {"angle", angle_->expression()}};
    if (reverse_->isChecked()) def["reverse"] = true;
    return def;
}

}  // namespace construe::app
