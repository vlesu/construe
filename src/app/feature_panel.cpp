// Feature property panels (spec/app-ux.md §5).
#include "app/feature_panel.h"
#include "app/panel_header.h"

#include <QStandardItemModel>

#include "app/app_model.h"
#include "app/choice_row.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QHBoxLayout>

#include <algorithm>
#include <numbers>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace construe::app {

void FeaturePanel::removeProfileItem(int i) {
    if (regions_ && i < int(regions_->size())) {
        regions_->erase(regions_->begin() + i);
        if (regions_->empty()) sketch_.clear(), regions_.reset();
    } else {
        sketch_.clear();
        regions_.reset();
    }
}

SelectionFieldWidget* FeaturePanel::addSelectionField(const QString& name, const QString& label, const QString& prompt,
                                                     std::function<void(int)> onRemove) {
    auto* field = new SelectionFieldWidget(name, prompt, this);
    field->addTo(form_, label);
    connect(field, &SelectionFieldWidget::removed, this, [this, onRemove](int i) {
        onRemove(i);
        changed();
    });
    return field;
}

// ---------------------------------------------------------------- common part

void FeaturePanel::retranslateTitle(const QString& from, const QString& to) {
    if (const QString t = translatedText(baseTitle_, from, to); !t.isEmpty()) baseTitle_ = t;
}

FeaturePanel::FeaturePanel(AppModel& model, const QString& title, QWidget* parent, Profiles profiles)
    : QWidget(parent), model_(model), baseTitle_(title) {
    auto* v = new QVBoxLayout(this);
    // Command panel header with ✓ and ✗ (app-ux §2.1д, S7F-RF-008).
    auto* head = new PanelHeader(title, PanelHeader::CommandButtons{"okButton", "cancelButton"});
    title_ = head->title();
    ok_ = head->okButton();
    cancel_ = head->cancelButton();
    v->addWidget(head);
    banner_ = new QLabel;
    banner_->setObjectName("previewBanner");
    banner_->setWordWrap(true);
    banner_->hide();
    v->addWidget(banner_);
    form_ = new QFormLayout;
    profiles_ = new SelectionFieldWidget("profiles", txt("ui.feature.selectASketch"), this);
    profiles_->setManyFormat(txt("ui.feature.profilesN"));
    profiles_->addTo(form_, txt("ui.feature.profiles"));
    if (profiles == Profiles::Hidden) {
        profiles_->hide();
        form_->labelForField(profiles_)->hide();
    }
    profileItems_ = profiles_->items();
    connect(profiles_, &SelectionFieldWidget::removed, this, [this](int i) {
        removeProfileItem(i);
        changed();
    });
    v->addLayout(form_);
    ops_ = new QButtonGroup(this);
    v->addStretch(1);

    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(PREVIEW_DEBOUNCE_MS);
    connect(&previewTimer_, &QTimer::timeout, this, &FeaturePanel::previewRequested);
    connect(head, &PanelHeader::accepted, this, [this] { if (canAccept()) emit accepted(); });
    connect(head, &PanelHeader::cancelled, this, &FeaturePanel::cancelled);
    connect(ops_, &QButtonGroup::idClicked, this, &FeaturePanel::changed);
}

std::string FeaturePanel::operation() const {
    auto* b = ops_->checkedButton();
    const std::string op = b ? b->property("choice").toString().toStdString() : "add";
    return op == "cut" && outside_ && outside_->isChecked() ? "intersect" : op;
}

void FeaturePanel::setOperation(const std::string& op) {
    // The UI has two operations (vision item 17): new → «Add», intersect → «Cut» outside.
    const std::string shown = op == "new" ? "add" : op == "intersect" ? "cut" : op;
    for (auto* b : ops_->buttons())
        if (b->property("choice").toString().toStdString() == shown) b->setChecked(true);
    if (outside_) {
        outside_->setChecked(op == "intersect");
        outside_->setEnabled(shown == "cut");
    }
}

void FeaturePanel::defaultOperation() {
    setOperation("add");  // §5.1: material is added; with no bodies, a new body
}

QLayout* FeaturePanel::buildOperationRow() {
    auto* col = new QVBoxLayout;
    col->addWidget(new ChoiceRow({{"add", txt("ui.feature.add"), "op_add"}, {"cut", txt("ui.feature.cut"), "op_cut"}}, this, ops_));
    outside_ = new QCheckBox(txt("ui.feature.cutOutsideTheContour"), this);
    outside_->setObjectName("op_outside");
    outside_->setEnabled(false);
    col->addWidget(outside_);
    connect(ops_, &QButtonGroup::idClicked, this, [this] { outside_->setEnabled(operation() == "cut" || operation() == "intersect"); });
    connect(outside_, &QCheckBox::toggled, this, &FeaturePanel::changed);
    return col;
}

bool FeaturePanel::assemblyMode() const { return model_.session().document().document->active().isAssembly(); }

void FeaturePanel::buildTargetsRow() {
    targets_ = new CheckList("asmTargets", this);
    targets_->setToolTip(txt("ui.feature.checkedInstancesNothingCheckedAll"));
    targets_->setMaximumHeight(TARGETS_LIST_MAX_H);
    form_->addRow(txt("ui.feature.instances"), targets_);
    connect(targets_, &CheckList::changed, this, &FeaturePanel::changed);
    form_->setRowVisible(targets_, false);
}

void FeaturePanel::loadTargets(const std::vector<std::string>& selected) {
    if (!targets_) return;
    targets_->clear();
    const auto d = model_.session().document().document;
    const bool on = d->active().isAssembly();
    form_->setRowVisible(targets_, on);
    if (!on) return;
    std::vector<CheckList::Item> items;
    for (const auto& in : d->active().instances)
        items.push_back({QString::fromStdString(in.id), QString::fromStdString(in.name),
                         std::find(selected.begin(), selected.end(), in.id) != selected.end()});
    targets_->setItems(items);
}

std::vector<std::string> FeaturePanel::targets() const {
    std::vector<std::string> out;
    if (!targets_ || !assemblyMode()) return out;
    return targets_->checkedIds();
}

QString FeaturePanel::needed() const {
    // A field waiting for a sketch takes a base (§3.6а): a non-planar face or another object is refused with this hint.
    return wantsBaseSketch() ? text("ui.status.needSketchOrBase") : prompt();
}

QString FeaturePanel::prompt() const { return text("ui.wait.feature"); }

void FeaturePanel::toggleRegion(const std::string& sketchId, const std::string& regionId) {
    // A region of another sketch, or while "whole sketch" is set: a list of just this region; otherwise add/remove it.
    if (sketchId != sketch_ || !regions_) {
        sketch_ = sketchId;
        regions_ = std::vector<std::string>{regionId};
    } else if (auto it = std::find(regions_->begin(), regions_->end(), regionId); it != regions_->end()) {
        regions_->erase(it);
    } else {
        regions_->push_back(regionId);
    }
    changed();
}

doc::Json FeaturePanel::profilesJson() const {
    if (!regions_) return "all";
    std::vector<std::string> ids = *regions_;
    std::sort(ids.begin(), ids.end());
    return ids;
}

void FeaturePanel::loadProfiles(const doc::Profiles& p) {
    if (p.all) regions_.reset();
    else regions_ = p.regions;
}

void FeaturePanel::setSketch(const std::string& sketchId) {
    regions_.reset();
    sketch_ = sketchId;
    updateLabels();
    changed();
}

void FeaturePanel::changed() {
    updateLabels();
    // OK waits for «Yes» of a candidate (selection.md §5.3, FX7-SEL-001).
    ok_->setToolTip(selectionPending() ? text("ui.sel.okWait") : QString());
    if (canAccept()) previewTimer_.start();
}

api::StateRef FeaturePanel::selectionState() const {
    if (!editing_) return api::StateRef::final();
    const auto d = model_.session().document().document;
    const int i = d->active().indexOf(*editing_);
    if (i <= 0) return api::StateRef::initial();
    return api::StateRef::after(d->active().features[size_t(i - 1)]->id);
}

void FeaturePanel::updateLabels() {
    const auto d = model_.session().document().document;
    std::vector<SelectionItem> items;
    if (!sketch_.empty()) {
        const auto f = d->active().find(sketch_);
        const QString sk = QString::fromStdString(f ? f->name : sketch_);
        if (!regions_) {
            items.push_back({sk + txt("ui.feature.wholeSketch"), {}, sketch_});
        } else {
            // Region number is its ordinal in the sketch's region list ordered bytewise by regionId (§5.5).
            std::vector<std::string> all;
            if (auto st = model_.session().sketchState(sketch_))
                for (const auto& r : st->result.profiles.regions) all.push_back(r.regionId);
            std::sort(all.begin(), all.end());
            for (const auto& id : *regions_) {
                const auto it = std::find(all.begin(), all.end(), id);
                items.push_back({it != all.end() ? QString(txt("ui.feature.regionNN")).arg(int(it - all.begin()) + 1).arg(sk) : QString::fromStdString(id),
                                 {{PickTarget::Kind::Region, sketch_, id}}, {}});
            }
        }
    }
    profileLabels_ = items;
    profiles_->setPrompt(txt(sketch_.empty() ? "ui.feature.selectASketch" : "ui.feature.selectAProfile"));
    profiles_->setSelection(std::move(items));
    if (profiles_->isHidden()) profileItems_->setItems(std::vector<SelectionItem>{});
    ok_->setEnabled(canAccept());
}

ExprField* FeaturePanel::newExprField(std::optional<expr::Dim> dim) {
    auto* f = new ExprField;
    f->setChecker(&model_.session(), dim);
    return f;
}

ExprField* FeaturePanel::newDimensionField(const std::string& param, std::optional<expr::Dim> dim) {
    ExprField* f = newExprField(dim);
    wrapDimension(param, f);
    return f;
}

DimensionField* FeaturePanel::wrapDimension(const std::string& param, QWidget* value) {
    auto* d = new DimensionField(param, value);
    dims_.push_back(d);
    connect(d, &DimensionField::acceptRequested, this, [this] {
        if (canAccept()) emit accepted();
    });
    return d;
}

QWidget* FeaturePanel::dimRow(QWidget* value) {
    if (auto* d = DimensionField::of(value)) return d;
    return value;
}

ExprField* FeaturePanel::newNumberField(std::optional<expr::Dim> dim) {
    ExprField* f = newExprField(dim);
    markNotDimension(f);
    return f;
}

namespace {

// The parameter path is in the definition (document.md §2.6): "a.b.0.c" — object keys and array indices.
bool pathPresent(const doc::Json& def, const std::string& path) {
    const doc::Json* j = &def;
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t dot = path.find('.', pos);
        const std::string key = path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (j->is_object()) {
            if (!j->contains(key)) return false;
            j = &(*j)[key];
        } else if (j->is_array() && !key.empty() && key.find_first_not_of("0123456789") == std::string::npos) {
            const size_t i = std::stoul(key);
            if (i >= j->size()) return false;
            j = &(*j)[i];
        } else {
            return false;
        }
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return true;
}

}  // namespace

void FeaturePanel::loadTolerances(const doc::Json& def) {
    const doc::Json* tols = def.is_object() && def.contains("tolerances") ? &def["tolerances"] : nullptr;
    for (auto* d : dims_) d->setTolerance(tols && tols->contains(d->param()) ? &(*tols)[d->param()] : nullptr);
}

void FeaturePanel::beginNew(const std::optional<std::string>& sketch) {
    loadTolerances(doc::Json::object());
    startNew(sketch);
}

void FeaturePanel::beginEdit(const std::string& featureId) {
    startEdit(featureId);
    auto def = model_.session().featureDefinition(featureId);
    loadTolerances(def ? *def : doc::Json::object());
}

doc::Json FeaturePanel::fullDefinition() const {
    doc::Json def = definition();
    doc::Json tols = doc::Json::object();
    for (const auto* d : dims_) {
        if (d->param().empty() || tols.contains(d->param())) continue;
        // A parameter outside the definition (a hidden row: «Through all» has no depth) keeps no tolerance; the hole
        // diameter comes from the size list (pmi.md §2.2) — stored always.
        if (!pathPresent(def, d->param()) && !d->property("construeStoredAlways").toBool()) continue;
        if (auto t = d->tolerance()) tols[d->param()] = *t;
    }
    if (!tols.empty()) def["tolerances"] = tols;
    return def;
}

bool FeaturePanel::tolerancesValid() const {
    return std::all_of(dims_.begin(), dims_.end(), [](const DimensionField* d) { return d->toleranceValid(); });
}

QString FeaturePanel::toleranceError() const {
    for (const auto* d : dims_)
        if (!d->toleranceValid()) return d->toleranceError();
    return {};
}

void FeaturePanel::showExprState(ExprField* field, std::optional<expr::Dim> dim) {
    field->setDim(dim);
    field->refresh();
}

void FeaturePanel::setPreviewMessage(const QString& text, bool error) {
    if (text.isEmpty()) {
        banner_->hide();
        return;
    }
    banner_->setText(text);
    banner_->setStyleSheet(QString("background: %1; color: %2; padding: 4px;")
                               .arg((error ? COLOR_ERROR_BANNER_BG : COLOR_WARNING_BANNER_BG).color.name(),
                                    (error ? COLOR_ERROR_TEXT : COLOR_DIM).color.name()));
    banner_->show();
}

}  // namespace construe::app
