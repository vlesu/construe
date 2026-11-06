// Gear rim panel (spec/app-ux.md §5.3л, S6-GEAR-002…004).
#include "app/feature_panel.h"
#include "app/choice_row.h"
#include "app/flip_button.h"

#include "app/app_model.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QHBoxLayout>

#include <algorithm>
#include <cmath>

namespace construe::app {

namespace {
QString mm(double v) { return formatNumber(v).replace('.', ','); }
}  // namespace

api::StateRef GearPanel::inputState() const {
    // The edited rim cuts its own face: the face is looked up in the state before the feature (§5.3л).
    if (!editing_) return api::StateRef::final();
    const auto d = model_.session().document().document;
    const int i = d->active().indexOf(*editing_);
    if (i <= 0) return api::StateRef::initial();
    return api::StateRef::after(d->active().features[size_t(i - 1)]->id);
}

GearPanel::GearPanel(AppModel& model, QWidget* parent) : FeaturePanel(model, txt("ui.common.gearRim"), parent, Profiles::Hidden) {
    setObjectName("gearPanel");
    faceLabel_ = addSelectionField("gearFace", txt("ui.common.face"), txt("ui.feature.selectACylindricalFace"), [this](int) {
        face_.clear();
        diameter_.reset();
    });
    faceLabel_->setRequired(true);
    faceItems_ = faceLabel_->items();
    // «Add» / «Cut» (FX7-PT-006): teeth grow from the face or the spaces are cut into it.
    {
        auto* row = new ChoiceRow({{"add", txt("ui.feature.add2"), "gearOpAdd", txt("ui.feature.teethGrowFromTheFace")},
                                   {"cut", txt("ui.feature.cut"), "gearOpCut", txt("ui.feature.toothSpacesAreCutInto")}});
        opAdd_ = row->button("add");
        opCut_ = row->button("cut");
        opCut_->setChecked(true);
        form_->addRow(txt("ui.feature.operation"), row);
        connect(row, &ChoiceRow::chosen, this, [this] {
            if (!editing_) suggestTeeth();
            changed();
        });
    }
    // param — the dimension parameter (§5.6а, FX12-PMI-007); empty — not a dimension: the module and the counts are gear
    // parameters (the rim accuracy is the accuracy grade), not sizes with a tolerance.
    auto field = [this](const char* name, expr::Dim dim, const char* param) {
        auto* e = *param ? newDimensionField(param, dim) : newNumberField(dim);
        e->setObjectName(name);
        connect(e, &QLineEdit::textEdited, this, &GearPanel::changed);
        connect(e, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
        return e;
    };
    module_ = field("gearModule", expr::LENGTH, "");
    auto* mrow = new QHBoxLayout;
    mrow->addWidget(module_, 1);
    form_->addRow(txt("ui.feature.module"), mrow);
    teeth_ = field("gearTeeth", expr::DIMLESS, "");
    form_->addRow(txt("ui.feature.numberOfTeeth"), teeth_);
    shift_ = field("gearShift", expr::DIMLESS, "");
    form_->addRow(txt("ui.feature.profileShift"), shift_);
    // A new rim keeps the tips on the face while the module or the shift changes, until the teeth are typed (FX10-GEAR-001).
    connect(teeth_, &QLineEdit::textEdited, this, [this] { teethTyped_ = true; });
    for (ExprField* f : {module_, shift_})
        connect(f, &QLineEdit::textEdited, this, [this] {
            if (!editing_ && !teethTyped_) {
                suggestTeeth();
                changed();
            }
        });
    full_ = new QCheckBox(txt("ui.feature.fullLength"));
    full_->setObjectName("gearFull");
    form_->addRow(full_);
    width_ = field("gearWidth", expr::LENGTH, "width");
    reverse_ = new FlipButton("gearReverse", txt("ui.feature.fromTheOtherEndOf"));
    auto* wrow = new QHBoxLayout;
    wrow->addWidget(dimRow(width_), 1);
    wrow->addWidget(reverse_);
    form_->addRow(txt("ui.feature.width"), wrow);
    sector_ = new QCheckBox(txt("ui.feature.sector"));
    sector_->setObjectName("gearSector");
    form_->addRow(sector_);
    sectorStart_ = field("gearSectorStart", expr::ANGLE, "sector.start");
    sectorAngle_ = field("gearSectorAngle", expr::ANGLE, "sector.angle");
    auto* srow = new QHBoxLayout;
    srow->addWidget(new QLabel(txt("ui.feature.from")));
    srow->addWidget(dimRow(sectorStart_), 1);
    srow->addWidget(new QLabel(txt("ui.feature.angle")));
    srow->addWidget(dimRow(sectorAngle_), 1);
    form_->addRow(txt("ui.feature.sector"), srow);
    accuracy_ = new QLineEdit;  // a grade text, not an expression
    accuracy_->setObjectName("gearAccuracy");
    connect(accuracy_, &QLineEdit::textEdited, this, &GearPanel::changed);
    connect(accuracy_, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
    accuracy_->setPlaceholderText("7-C");
    form_->addRow(txt("ui.feature.accuracyGrade"), accuracy_);
    info_ = new QLabel;
    info_->setObjectName("gearInfo");
    info_->setWordWrap(true);
    info_->setStyleSheet("color: gray;");
    form_->addRow(info_);
    connect(full_, &QCheckBox::toggled, this, &GearPanel::changed);
    connect(sector_, &QCheckBox::toggled, this, &GearPanel::changed);
    connect(reverse_, &QToolButton::toggled, this, &GearPanel::changed);
}

void GearPanel::startNew(const std::optional<std::string>&) {
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    face_.clear();
    diameter_.reset();
    keptSector_.reset();
    opCut_->setChecked(true);
    teethTyped_ = false;
    module_->setText("2");
    teeth_->setText("20");
    shift_->setText("0");
    full_->setChecked(true);
    width_->setText(formatNumber(10 / expr::lengthFactor(model_.session().units().length)));
    reverse_->setChecked(false);
    sector_->setChecked(false);
    sectorStart_->setText("0");
    sectorAngle_->setText("90");
    accuracy_->clear();
    changed();
}

void GearPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::Gear) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    face_ = def["face"];
    (def.value("operation", std::string("cut")) == "add" ? opAdd_ : opCut_)->setChecked(true);
    if (auto r = model_.session().threadFit(face_, inputState())) diameter_ = r->diameter, internal_ = r->internal;
    module_->setText(displayExpr(def["module"].get<std::string>(), d->lengthUnit));
    teeth_->setText(QString::fromStdString(def["teeth"].get<std::string>()));
    shift_->setText(QString::fromStdString(def.value("shift", std::string("0"))));
    full_->setChecked(!def.contains("width"));
    if (def.contains("width")) width_->setText(displayExpr(def["width"].get<std::string>(), d->lengthUnit));
    reverse_->setChecked(def.value("reverse", false));
    if (def.contains("sector")) {
        if (def["sector"].contains("faces")) {
            keptSector_ = def["sector"];
        } else {
            sector_->setChecked(true);
            sectorStart_->setText(displayExpr(def["sector"]["start"].get<std::string>(), d->angleUnit));
            sectorAngle_->setText(displayExpr(def["sector"]["angle"].get<std::string>(), d->angleUnit));
        }
    }
    accuracy_->setText(QString::fromStdString(def.value("accuracy", std::string())));
    changed();
}

bool GearPanel::pick(const PickTarget& t) {
    if (t.kind != PickTarget::Kind::Face) return false;
    setFace(t.name);
    return true;
}

std::vector<PickTarget> GearPanel::selectedTargets() const {
    if (face_.empty()) return {};
    return {{PickTarget::Kind::Face, {}, face_}};
}

void GearPanel::setFace(const std::string& name) {
    face_ = name;
    diameter_.reset();
    teethTyped_ = false;
    if (auto r = model_.session().threadFit(face_, inputState())) {
        diameter_ = r->diameter;
        internal_ = r->internal;
        if (!editing_) suggestTeeth();
    }
    changed();
}

long GearPanel::suggestedTeeth(double D, double m, double x, bool internal, bool add) {
    // Cut: da = D (external da = m(z + 2 + 2x), internal da = m(z − 2 + 2x)); add: df = D (external df = m(z − 2.5 + 2x),
    // internal df = m(z + 2.5 + 2x)) — gear.md §3. A fractional z is rounded towards the side that fits (regen.md §4.17
    // items 3, 7): external cut da ≤ D, internal cut da ≥ D, external add df ≥ D, internal add df ≤ D.
    const double tol = regen::GEAR_DIAMETER_TOL / m;
    long z = 0;
    if (add) z = internal ? long(std::floor(D / m - 2.5 - 2 * x + tol)) : long(std::ceil(D / m + 2.5 - 2 * x - tol));
    else z = internal ? long(std::ceil(D / m + 2 * (1 - x) - tol)) : long(std::floor(D / m - 2 * (1 + x) + tol));
    return std::max<long>(z, internal ? gear::GEAR_MIN_TEETH_INTERNAL : gear::GEAR_MIN_TEETH);
}

void GearPanel::suggestTeeth() {
    const auto m = fieldValue(module_, expr::LENGTH);
    const auto x = fieldValue(shift_, expr::DIMLESS);
    if (!diameter_ || !m || *m <= 0) return;
    teeth_->setText(QString::number(suggestedTeeth(*diameter_, *m, x.value_or(0), internal_, adds())));
}

std::optional<double> GearPanel::fieldValue(ExprField* f, expr::Dim dim) const {
    const auto d = model_.session().document().document;
    auto q = model_.session().checkExpression(f->expression(), dim);
    if (!q) return std::nullopt;
    return q->value;
}

bool GearPanel::canAccept() const {
    if (face_.empty() || !diameter_) return false;
    for (auto [f, dim] : {std::pair{module_, expr::LENGTH}, {teeth_, expr::DIMLESS}, {shift_, expr::DIMLESS}})
        if (!fieldValue(f, dim)) return false;
    if (!full_->isChecked() && !fieldValue(width_, expr::LENGTH)) return false;
    if (sector_->isChecked() && (!fieldValue(sectorStart_, expr::ANGLE) || !fieldValue(sectorAngle_, expr::ANGLE))) return false;
    return true;
}

void GearPanel::updateLabels() {
    ok_->setEnabled(canAccept());
    faceLabel_->setSelection(face_.empty() ? std::vector<SelectionItem>{}
                                       : std::vector<SelectionItem>{{topoCaption(model_.session(), face_, false), {{PickTarget::Kind::Face, {}, face_}}, {}}});
    const auto d = model_.session().document().document;
    showExprState(module_, expr::LENGTH);
    dimRow(width_)->setEnabled(!full_->isChecked());
    if (!full_->isChecked()) showExprState(width_, expr::LENGTH);
    sector_->setEnabled(!keptSector_);
    dimRow(sectorStart_)->setEnabled(sector_->isChecked() && !keptSector_);
    dimRow(sectorAngle_)->setEnabled(sector_->isChecked() && !keptSector_);
    QString info;
    if (!face_.empty() && !diameter_) {
        info = text("ui.gear.notCylinder");
    } else if (diameter_) {
        const auto m = fieldValue(module_, expr::LENGTH), z = fieldValue(teeth_, expr::DIMLESS), x = fieldValue(shift_, expr::DIMLESS);
        if (m && z && x && std::abs(*z - std::round(*z)) < 1e-9) {
            auto g = api::Session::gearGeometry(*m, int(std::lround(*z)), *x, internal_);
            if (!g) {
                info = errorText(g.error());
            } else {
                info = text(internal_ ? "ui.gear.internal" : "ui.gear.external",
                            {{"face", mm(*diameter_)}, {"d", mm(g->d)}, {"da", mm(g->da)}, {"df", mm(g->df)}});
                const double tol = regen::GEAR_DIAMETER_TOL;  // regen.md §4.17 items 3, 7
                if (adds()) {
                    if (internal_ ? *diameter_ < g->df - tol : *diameter_ > g->df + tol)
                        info += ". " + text("ui.gear.rootMismatch", {{"df", mm(g->df)}, {"face", mm(*diameter_)}});
                } else {
                    const bool smaller = *diameter_ < g->da - tol, larger = *diameter_ > g->da + tol;
                    if (internal_ ? larger : smaller) info += ". " + text("ui.gear.mismatch", {{"da", mm(g->da)}, {"face", mm(*diameter_)}});
                    else if (internal_ ? smaller : larger) info += ". " + text("ui.gear.trimmed", {{"da", mm(g->da)}, {"face", mm(*diameter_)}});
                }
                for (const auto& w : g->warnings) info += " " + errorText(w);
                // Compatibility (FX10-GEAR-003): the same module and basic rack; the centre distance by the pitch circles.
                info += ". " + text(internal_ ? "ui.gear.meshesInternal" : "ui.gear.meshesExternal", {{"m", mm(*m)}, {"d", mm(g->d)}});
            }
        }
    }
    info_->setText(info);
    info_->setVisible(!info.isEmpty());
}

doc::Json GearPanel::definition() const {
    const auto d = model_.session().document().document;
    doc::Json def = {{"type", "gear"}, {"face", face_}, {"module", module_->expression()},
                     {"teeth", teeth_->expression()}};
    const std::string x = shift_->expression();
    if (!x.empty() && x != "0") def["shift"] = x;
    if (!full_->isChecked()) def["width"] = width_->expression();
    if (reverse_->isChecked()) def["reverse"] = true;
    if (adds()) def["operation"] = "add";
    if (keptSector_) def["sector"] = *keptSector_;
    else if (sector_->isChecked())
        def["sector"] = {{"start", sectorStart_->expression()}, {"angle", sectorAngle_->expression()}};
    if (!accuracy_->text().trimmed().isEmpty()) def["accuracy"] = accuracy_->text().trimmed().toStdString();
    return def;
}

}  // namespace construe::app
