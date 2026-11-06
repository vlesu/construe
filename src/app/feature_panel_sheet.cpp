// Sheet metal feature panels (spec/app-ux.md §5.3к, spec/sheet.md).
#include "app/feature_panel_sheet.h"

#include "app/app_model.h"
#include "app/flip_button.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QHBoxLayout>

#include <algorithm>
#include <numbers>
#include <utility>

namespace construe::app {

namespace {

// Default values of a new feature (§5.3к): in millimetres, shown in document units.
constexpr double SHEET_PANEL_THICKNESS = 2, SHEET_PANEL_RADIUS = 2, SHEET_PANEL_WIDTH = 20, SHEET_PANEL_FLANGE_LENGTH = 20;

QString mmText(AppModel& m, double mm) { return formatNumber(mm / expr::lengthFactor(m.session().units().length)); }

}  // namespace

// ---------------------------------------------------------------- common

bool SheetPanel::pick(const PickTarget& t) {
    if (t.kind != PickTarget::Kind::Region) return false;
    toggleRegion(t.body, t.name);
    return true;
}

ExprField* SheetPanel::exprRow(const QString& label, const char* name, const char* param, QWidget* extra) {
    auto* row = new QHBoxLayout;
    auto* f = *param ? newDimensionField(param) : newNumberField();  // §5.6а, FX12-PMI-007
    f->setObjectName(name);
    row->addWidget(dimRow(f), 1);
    if (extra) row->addWidget(extra);
    form_->addRow(label, row);
    connect(f, &QLineEdit::textEdited, this, &SheetPanel::changed);
    connect(f, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
    return f;
}

QToolButton* SheetPanel::toggle(const char* name, const QString& text, const QString& tip) {
    auto* b = new QToolButton;
    b->setObjectName(name);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    connect(b, &QToolButton::toggled, this, &SheetPanel::changed);
    return b;
}

QToolButton* SheetPanel::flip(const char* name, const QString& tip) {
    auto* b = new FlipButton(name, tip);
    connect(b, &QToolButton::toggled, this, &SheetPanel::changed);
    return b;
}

std::string SheetPanel::lengthUnit() const { return model_.session().document().document->lengthUnit; }
std::string SheetPanel::angleUnit() const { return model_.session().document().document->angleUnit; }

// ---------------------------------------------------------------- sheet parameters (S5-SHEET-001)

SheetMetalPanel::SheetMetalPanel(AppModel& model, QWidget* parent) : SheetPanel(model, txt("ui.common.sheetMetalPart"), parent, Profiles::Hidden) {
    setObjectName("sheetMetalPanel");
    thickness_ = exprRow(txt("ui.feature.thickness"), "sheetThickness", "thickness");
    radius_ = exprRow(txt("ui.feature.bendRadius"), "sheetRadius", "radius");
    k_ = exprRow(txt("ui.feature.kFactor"), "sheetKFactor", "");
    k_->setAllowEmpty(true);
    reliefType_ = new QComboBox;
    reliefType_->setObjectName("sheetReliefType");
    reliefType_->addItem(txt("ui.feature.rectangular"), "rect");
    reliefType_->addItem(txt("ui.feature.rounded"), "round");
    reliefType_->addItem(txt("ui.feature.noRelief"), "none");
    form_->addRow(txt("ui.feature.relief"), reliefType_);
    connect(reliefType_, &QComboBox::currentIndexChanged, this, &SheetMetalPanel::changed);
    reliefWidth_ = exprRow(txt("ui.feature.reliefWidth"), "sheetReliefWidth", "relief.width");
    reliefWidth_->setAllowEmpty(true);
    reliefDepth_ = exprRow(txt("ui.feature.reliefDepth"), "sheetReliefDepth", "relief.depth");
    reliefDepth_->setAllowEmpty(true);
    reliefWidth_->setPlaceholderText(txt("ui.feature.thickness2"));
    reliefDepth_->setPlaceholderText(txt("ui.feature.thickness2"));
}

void SheetMetalPanel::startNew(const std::optional<std::string>&) {
    editing_.reset();
    recognize_ = false;
    thickness_->setPlaceholderText({});
    radius_->setPlaceholderText({});
    title_->setText(baseTitle_);
    banner_->hide();
    thickness_->setText(mmText(model_, SHEET_PANEL_THICKNESS));
    radius_->setText(mmText(model_, SHEET_PANEL_RADIUS));
    k_->setText("0.44");  // SHEET_DEFAULT_K (sheet.md §5)
    reliefType_->setCurrentIndex(0);
    reliefWidth_->clear();
    reliefDepth_->clear();
    changed();
    thickness_->setFocus();
    thickness_->selectAll();
}

void SheetMetalPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::SheetMetal) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    // «Sheet from body» (S5-SHEET-006): thickness and radius are optional — empty means "from the form".
    recognize_ = def.value("recognize", false);
    if (recognize_) thickness_->setPlaceholderText(txt("ui.feature.fromShape")), radius_->setPlaceholderText(txt("ui.feature.fromShape"));
    thickness_->setText(def.contains("thickness") ? displayExpr(def["thickness"].get<std::string>(), d->lengthUnit) : QString());
    radius_->setText(def.contains("radius") ? displayExpr(def["radius"].get<std::string>(), d->lengthUnit) : QString());
    k_->setText(QString::fromStdString(def.value("kFactor", std::string("0.44"))));
    if (def.contains("relief")) {
        const doc::Json& r = def["relief"];
        reliefType_->setCurrentIndex(std::max(0, reliefType_->findData(QString::fromStdString(r["type"].get<std::string>()))));
        if (r.contains("width")) reliefWidth_->setText(displayExpr(r["width"].get<std::string>(), d->lengthUnit));
        if (r.contains("depth")) reliefDepth_->setText(displayExpr(r["depth"].get<std::string>(), d->lengthUnit));
    }
    changed();
}

bool SheetMetalPanel::canAccept() const {
    return recognize_ || (!thickness_->text().trimmed().isEmpty() && !radius_->text().trimmed().isEmpty());
}

void SheetMetalPanel::updateLabels() {
    ok_->setEnabled(canAccept());
    for (auto* f : {thickness_, radius_}) {
        f->setAllowEmpty(recognize_);
        showExprState(f, expr::LENGTH);
    }
    showExprState(k_, expr::Dim{0, 0});
    showExprState(reliefWidth_, expr::LENGTH);
    showExprState(reliefDepth_, expr::LENGTH);
}

doc::Json SheetMetalPanel::definition() const {
    const std::string u = lengthUnit();
    doc::Json def = {{"type", "sheetMetal"}};
    for (auto [f, key] : {std::pair{thickness_, "thickness"}, std::pair{radius_, "radius"}})
        if (!recognize_ || !f->text().trimmed().isEmpty()) def[key] = f->expression();
    if (recognize_) def["recognize"] = true;
    if (!k_->text().trimmed().isEmpty()) def["kFactor"] = k_->expression();
    const std::string type = reliefType_->currentData().toString().toStdString();
    const bool w = !reliefWidth_->text().trimmed().isEmpty(), dp = !reliefDepth_->text().trimmed().isEmpty();
    if (type != "rect" || w || dp) {
        doc::Json r = {{"type", type}};
        if (w) r["width"] = reliefWidth_->expression();
        if (dp) r["depth"] = reliefDepth_->expression();
        def["relief"] = r;
    }
    return def;
}

// ---------------------------------------------------------------- base wall (S5-SHEET-002)

SheetWallPanel::SheetWallPanel(AppModel& model, QWidget* parent) : SheetPanel(model, txt("ui.common.baseWall"), parent) {
    setObjectName("sheetWallPanel");
    reverse_ = flip("sheetWallReverse", txt("ui.feature.flipPlateThicknessProfileWidth"));
    width_ = exprRow(txt("ui.feature.widthOpenProfile"), "sheetWallWidth", "width", reverse_);
    width_->setAllowEmpty(true);
    side_ = new QComboBox;
    side_->setObjectName("sheetWallSide");
    side_->addItem(txt("ui.feature.materialInside"), "inside");
    side_->addItem(txt("ui.feature.materialOutside"), "outside");
    side_->addItem(txt("ui.feature.centered"), "mid");
    form_->addRow(txt("ui.feature.materialSide"), side_);
    connect(side_, &QComboBox::currentIndexChanged, this, &SheetWallPanel::changed);
}

void SheetWallPanel::startNew(const std::optional<std::string>& sketch) {
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    sketch_.clear();
    regions_.reset();
    width_->setText(mmText(model_, SHEET_PANEL_WIDTH));
    side_->setCurrentIndex(0);
    reverse_->setChecked(false);
    if (sketch) setSketch(*sketch);
    changed();
}

void SheetWallPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::SheetWall) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    sketch_ = def["sketch"].get<std::string>();
    if (def["profiles"].is_array()) {
        regions_ = std::vector<std::string>{};
        for (const auto& r : def["profiles"]) regions_->push_back(r);
    }
    if (def.contains("width")) width_->setText(displayExpr(def["width"].get<std::string>(), d->lengthUnit));
    side_->setCurrentIndex(std::max(0, side_->findData(QString::fromStdString(def.value("side", std::string("inside"))))));
    reverse_->setChecked(def.value("reverse", false));
    changed();
}

void SheetWallPanel::updateLabels() {
    FeaturePanel::updateLabels();
    showExprState(width_, expr::LENGTH);
}

doc::Json SheetWallPanel::definition() const {
    doc::Json def = {{"type", "sheetWall"}, {"sketch", sketch_}, {"profiles", profilesJson()}};
    // Width and side matter only for an open profile (sketch without regions, document.md §2.10л).
    bool open = false;
    if (auto st = model_.session().sketchState(sketch_)) open = st->result.profiles.regions.empty();
    if (open) {
        if (!width_->text().trimmed().isEmpty()) def["width"] = width_->expression();
        const std::string side = side_->currentData().toString().toStdString();
        if (side != "inside") def["side"] = side;
    }
    if (reverse_->isChecked()) def["reverse"] = true;
    return def;
}

// ---------------------------------------------------------------- flange (S5-SHEET-003)

SheetFlangePanel::SheetFlangePanel(AppModel& model, QWidget* parent) : SheetPanel(model, txt("ui.common.flange"), parent, Profiles::Hidden) {
    setObjectName("sheetFlangePanel");
    edgesLabel_ = addSelectionField("sheetFlangeEdges", txt("ui.common.edges"), txt("ui.feature.selectEdges"), [this](int i) {
        if (i < int(edges_.size())) edges_.erase(edges_.begin() + i);
    });
    edgesLabel_->setManyFormat(txt("ui.common.edgesN"));
    edgesLabel_->setRequired(true);
    edgeItems_ = edgesLabel_->items();
    length_ = exprRow(txt("ui.feature.length"), "sheetFlangeLength", "length");
    reverse_ = flip("sheetFlangeReverse", txt("ui.feature.flipFlangeToTheOther"));
    angle_ = exprRow(txt("ui.common.angle"), "sheetFlangeAngle", "angle", reverse_);
    radius_ = exprRow(txt("ui.common.radius"), "sheetFlangeRadius", "radius");
    radius_->setAllowEmpty(true);
    radius_->setPlaceholderText(txt("ui.feature.sheetRadius"));
    position_ = new QComboBox;
    position_->setObjectName("sheetFlangePosition");
    position_->addItem(txt("ui.feature.materialInside"), "inside");
    position_->addItem(txt("ui.feature.materialOutside"), "outside");
    position_->addItem(txt("ui.feature.bendLineOnEdge"), "edge");
    form_->addRow(txt("ui.feature.position"), position_);
    connect(position_, &QComboBox::currentIndexChanged, this, &SheetFlangePanel::changed);
    offsetStart_ = exprRow(txt("ui.feature.startOffset"), "sheetFlangeOffsetStart", "offsets.start");
    offsetStart_->setAllowEmpty(true);
    offsetEnd_ = exprRow(txt("ui.feature.endOffset"), "sheetFlangeOffsetEnd", "offsets.end");
    offsetEnd_->setAllowEmpty(true);
    relief_ = new QComboBox;
    relief_->setObjectName("sheetFlangeRelief");
    relief_->addItem(txt("ui.feature.asSheet"), "");
    relief_->addItem(txt("ui.feature.rectangular"), "rect");
    relief_->addItem(txt("ui.feature.rounded"), "round");
    relief_->addItem(txt("ui.feature.noRelief"), "none");
    form_->addRow(txt("ui.feature.relief"), relief_);
    connect(relief_, &QComboBox::currentIndexChanged, this, &SheetFlangePanel::changed);
}

void SheetFlangePanel::startNew(const std::optional<std::string>&) {
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    edges_.clear();
    length_->setText(mmText(model_, SHEET_PANEL_FLANGE_LENGTH));
    angle_->setText(angleUnit() == "rad" ? formatNumber(std::numbers::pi / 2) : QString("90"));
    radius_->clear();
    offsetStart_->clear();
    offsetEnd_->clear();
    position_->setCurrentIndex(0);
    relief_->setCurrentIndex(0);
    reverse_->setChecked(false);
    changed();
}

void SheetFlangePanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::SheetFlange) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    for (const auto& e : def["edges"]) edges_.push_back(e);
    length_->setText(displayExpr(def["length"].get<std::string>(), d->lengthUnit));
    angle_->setText(displayExpr(def["angle"].get<std::string>(), d->angleUnit));
    if (def.contains("radius")) radius_->setText(displayExpr(def["radius"].get<std::string>(), d->lengthUnit));
    if (def.contains("offsets")) {
        offsetStart_->setText(displayExpr(def["offsets"]["start"].get<std::string>(), d->lengthUnit));
        offsetEnd_->setText(displayExpr(def["offsets"]["end"].get<std::string>(), d->lengthUnit));
    }
    position_->setCurrentIndex(std::max(0, position_->findData(QString::fromStdString(def.value("position", std::string("inside"))))));
    relief_->setCurrentIndex(std::max(0, relief_->findData(QString::fromStdString(def.value("relief", std::string())))));
    reverse_->setChecked(def.value("reverse", false));
    changed();
}

bool SheetFlangePanel::canAccept() const {
    return !edges_.empty() && !length_->text().trimmed().isEmpty() && !angle_->text().trimmed().isEmpty();
}

bool SheetFlangePanel::pick(const PickTarget& t) {
    if (t.kind != PickTarget::Kind::Edge) return false;
    auto it = std::find(edges_.begin(), edges_.end(), t.name);
    if (it == edges_.end()) edges_.push_back(t.name);
    else edges_.erase(it);
    changed();
    return true;
}

std::vector<PickTarget> SheetFlangePanel::selectedTargets() const {
    std::vector<PickTarget> out;
    for (const auto& e : edges_) out.push_back({PickTarget::Kind::Edge, {}, e});
    return out;
}

void SheetFlangePanel::setEdges(const std::vector<std::string>& edges) {
    edges_ = edges;
    changed();
}

void SheetFlangePanel::updateLabels() {
    ok_->setEnabled(canAccept());
    std::vector<SelectionItem> items;
    for (const auto& e : edges_) items.push_back({topoCaption(model_.session(), e, true), {{PickTarget::Kind::Edge, {}, e}}, {}});
    edgesLabel_->setSelection(std::move(items));
    showExprState(length_, expr::LENGTH);
    showExprState(angle_, expr::ANGLE);
    for (auto* f : {radius_, offsetStart_, offsetEnd_}) showExprState(f, expr::LENGTH);
}

doc::Json SheetFlangePanel::definition() const {
    const std::string u = lengthUnit();
    doc::Json def = {{"type", "sheetFlange"}, {"edges", edges_}, {"length", length_->expression()},
                     {"angle", angle_->expression()}};
    if (!radius_->text().trimmed().isEmpty()) def["radius"] = radius_->expression();
    const std::string pos = position_->currentData().toString().toStdString();
    if (pos != "inside") def["position"] = pos;
    if (reverse_->isChecked()) def["reverse"] = true;
    const bool s = !offsetStart_->text().trimmed().isEmpty(), e = !offsetEnd_->text().trimmed().isEmpty();
    if (s || e)
        def["offsets"] = {{"start", s ? offsetStart_->expression() : std::string("0")},
                          {"end", e ? offsetEnd_->expression() : std::string("0")}};
    const std::string relief = relief_->currentData().toString().toStdString();
    if (!relief.empty()) def["relief"] = relief;
    return def;
}

// ---------------------------------------------------------------- sketch bend (S5-SHEET-004)

SheetBendPanel::SheetBendPanel(AppModel& model, QWidget* parent) : SheetPanel(model, txt("ui.common.bendOnLine"), parent) {
    setObjectName("sheetBendPanel");
    form_->labelForField(profiles_)->setProperty("text", txt("ui.feature.bendLine"));
    reverse_ = flip("sheetBendReverse", txt("ui.feature.flipBendAgainstTheSketch"));
    angle_ = exprRow(txt("ui.common.angle"), "sheetBendAngle", "angle", reverse_);
    radius_ = exprRow(txt("ui.common.radius"), "sheetBendRadius", "radius");
    radius_->setAllowEmpty(true);
    radius_->setPlaceholderText(txt("ui.feature.sheetRadius"));
    fixedRight_ = toggle("sheetBendFixedRight", txt("ui.feature.rightSideFixed"), txt("ui.feature.fixedSideToTheRight"));
    form_->addRow(QString(), fixedRight_);
}

void SheetBendPanel::startNew(const std::optional<std::string>& sketch) {
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    sketch_.clear();
    regions_.reset();
    angle_->setText(angleUnit() == "rad" ? formatNumber(std::numbers::pi / 2) : QString("90"));
    radius_->clear();
    fixedRight_->setChecked(false);
    reverse_->setChecked(false);
    if (sketch) setSketch(*sketch);
    changed();
}

void SheetBendPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::SheetBend) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    sketch_ = def["sketch"].get<std::string>();
    angle_->setText(displayExpr(def["angle"].get<std::string>(), d->angleUnit));
    if (def.contains("radius")) radius_->setText(displayExpr(def["radius"].get<std::string>(), d->lengthUnit));
    fixedRight_->setChecked(def.value("fixed", std::string("left")) == "right");
    reverse_->setChecked(def.value("reverse", false));
    changed();
}

void SheetBendPanel::updateLabels() {
    FeaturePanel::updateLabels();
    if (!sketch_.empty()) {
        const auto f = model_.session().document().document->active().find(sketch_);
        auto items = profileLabels_;  // the bend line is a whole sketch: the header names the sketch
        profiles_->setSelection(std::move(items), QString::fromStdString(f ? f->name : sketch_));
    }
    showExprState(angle_, expr::ANGLE);
    showExprState(radius_, expr::LENGTH);
}

doc::Json SheetBendPanel::definition() const {
    doc::Json def = {{"type", "sheetBend"}, {"sketch", sketch_}, {"angle", angle_->expression()}};
    if (!radius_->text().trimmed().isEmpty()) def["radius"] = radius_->expression();
    if (fixedRight_->isChecked()) def["fixed"] = "right";
    if (reverse_->isChecked()) def["reverse"] = true;
    return def;
}

// ---------------------------------------------------------------- cut (S5-SHEET-005)

SheetCutPanel::SheetCutPanel(AppModel& model, QWidget* parent) : SheetPanel(model, txt("ui.common.sheetCut"), parent) {
    setObjectName("sheetCutPanel");
}

void SheetCutPanel::startNew(const std::optional<std::string>& sketch) {
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    sketch_.clear();
    regions_.reset();
    if (sketch) setSketch(*sketch);
    changed();
}

void SheetCutPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::SheetCut) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    sketch_ = def["sketch"].get<std::string>();
    if (def["profiles"].is_array()) {
        regions_ = std::vector<std::string>{};
        for (const auto& r : def["profiles"]) regions_->push_back(r);
    }
    changed();
}

doc::Json SheetCutPanel::definition() const { return {{"type", "sheetCut"}, {"sketch", sketch_}, {"profiles", profilesJson()}}; }

}  // namespace construe::app
