// Pattern and mirror panel (spec/app-ux.md §5.3б).
#include "app/feature_panel.h"
#include "app/flip_button.h"

#include <QStandardItemModel>

#include "app/app_model.h"
#include "app/messages.h"
#include "app/theme.h"

#include <QHBoxLayout>

#include <algorithm>
#include <numbers>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace construe::app {

// ---------------------------------------------------------------- pattern (§5.3б, S2-FEAT-002)

namespace {
constexpr int PATTERN_DEFAULT_COUNT_LINEAR = 3;
constexpr int PATTERN_DEFAULT_COUNT_CIRCULAR = 6;
constexpr double PATTERN_DEFAULT_SPACING_MM = 20;

// Operation of a source feature (for the preview, §5.3б): a gear cuts by default; a pattern or mirror — its first source.
std::string sourceOperation(const doc::Document& d, const doc::Feature& f, int depth = 0) {
    if (f.extrude()) return f.extrude()->operation;
    if (f.revolve()) return f.revolve()->operation;
    if (f.type == doc::FeatureType::Hole) return "cut";
    if (!f.pattern()) return "add";
    const doc::Json& def = f.pattern()->def;
    if (f.type == doc::FeatureType::Gear) return def.value("operation", std::string("cut"));
    if ((f.type == doc::FeatureType::Pattern || f.type == doc::FeatureType::Mirror) && depth < 16 && def.contains("features"))
        for (const auto& id : def["features"])
            if (const auto s = d.active().find(id.get<std::string>())) return sourceOperation(d, *s, depth + 1);
    return def.value("operation", std::string("add"));
}

// View object of a stored reference (highlight while editing): PlaneRef / DirRef / AxisRef.
std::optional<PickTarget> refTarget(const doc::Json& r) {
    if (!r.is_object()) return std::nullopt;
    if (r.contains("normal")) return refTarget(r["normal"]);
    if (r.contains("edge")) return PickTarget{PickTarget::Kind::Edge, {}, r["edge"]};
    if (r.contains("face")) return PickTarget{PickTarget::Kind::Face, {}, r["face"]};
    if (r.contains("datum")) {
        const std::string d = r["datum"];
        return PickTarget{d.rfind("plane:", 0) == 0 ? PickTarget::Kind::Plane : PickTarget::Kind::Axis, {}, d};
    }
    if (r.contains("sketchLine"))
        return PickTarget{PickTarget::Kind::SketchEntity, r["sketchLine"].value("sketch", std::string()), r["sketchLine"].value("entity", std::string())};
    if (r.contains("feature") && r["feature"].is_string()) return PickTarget{PickTarget::Kind::Axis, {}, r["feature"]};
    return std::nullopt;
}

// {"aux": i} → {"aux": i + offset} (the owned features of the second field follow those of the first).
doc::Json shiftAux(doc::Json j, int offset) {
    if (j.is_object() && j.size() == 1 && j.contains("aux") && j["aux"].is_number_integer()) return {{"aux", j["aux"].get<int>() + offset}};
    if (j.is_object() || j.is_array())
        for (auto& x : j) x = shiftAux(x, offset);
    return j;
}
}  // namespace

PatternPanel::PatternPanel(AppModel& model, bool mirror, QWidget* parent)
    : FeaturePanel(model, mirror ? txt("ui.common.mirror") : txt("ui.common.pattern"), parent, Profiles::Hidden), mirror_(mirror), planeSel_(model.session(), "plane"),
      ref1_(model.session(), "direction"), ref2_(model.session(), "direction") {
    setObjectName(mirror ? "mirrorPanel" : "patternPanel");
    mode_ = new QComboBox;
    mode_->setObjectName("patternMode");
    mode_->addItem(txt("ui.common.linear"), "linear");
    mode_->addItem(txt("ui.common.circular"), "circular");
    form_->addRow(txt("ui.common.type"), mode_);
    if (mirror) mode_->hide(), form_->labelForField(mode_)->hide();
    featuresLabel_ = addSelectionField(mirror ? "mirrorFeatures" : "patternFeatures", txt("ui.feature.features"),
                                       txt("ui.feature.selectFeaturesInTheTree"), [this](int i) {
                                           if (i < int(features_.size())) features_.erase(features_.begin() + i);
                                       });
    featuresLabel_->setManyFormat(txt("ui.feature.featuresN"));
    featureItems_ = featuresLabel_->items();
    planeLabel_ = addSelectionField("mirrorPlane", txt("ui.common.plane"), txt("ui.feature.select"), [this](int i) { removeItem(planeSel_, i); });
    planeItems_ = planeLabel_->items();
    if (!mirror) planeLabel_->hide(), form_->labelForField(planeLabel_)->hide();
    // param — the dimension parameter (§5.6а, FX12-PMI-007); empty — the instance count, not a dimension.
    auto exprRow = [&](const char* name, ExprField*& field, QToolButton** rev, const char* param) {
        auto* w = new QWidget;
        auto* row = new QHBoxLayout(w);
        row->setContentsMargins(0, 0, 0, 0);
        field = *param ? newDimensionField(param) : newNumberField();
        field->setObjectName(name);
        row->addWidget(dimRow(field), 1);
        if (rev) {
            *rev = new FlipButton(QString(name) + "Reverse");
            row->addWidget(*rev);
            connect(*rev, &QToolButton::toggled, this, &PatternPanel::changed);
        }
        connect(field, &QLineEdit::textEdited, this, &PatternPanel::changed);
        connect(field, &QLineEdit::returnPressed, this, [this] { if (canAccept()) emit accepted(); });
        return w;
    };
    auto add = [&](const QString& label, QWidget* w, std::vector<QWidget*>& group) {
        form_->addRow(label, w);
        group.push_back(w);
        group.push_back(form_->labelForField(w));
    };
    // The direction fields: the one the next click goes to is active (§5.5); a click on a field arms it (Н6).
    ref1Label_ = new SelectionFieldWidget("patternRef1", txt("ui.feature.select"), this);
    add(txt("ui.common.direction1"), ref1Label_, linearRows_);
    auto itemsAfter = [&](SelectionFieldWidget* f) {
        int row = -1;
        QFormLayout::ItemRole role;
        form_->getWidgetPosition(f, &row, &role);
        form_->insertRow(row + 1, f->items());
    };
    itemsAfter(ref1Label_);
    connect(ref1Label_, &SelectionFieldWidget::removed, this, [this](int i) { removeItem(ref1_, i), changed(); });
    ref1Items_ = ref1Label_->items();
    add(txt("ui.common.count"), exprRow("patternCount1", count1_, nullptr, ""), linearRows_);
    add(txt("ui.common.spacing"), exprRow("patternSpacing1", spacing1_, &rev1_, "directions.0.spacing"), linearRows_);
    dir2_ = new QCheckBox(txt("ui.common.direction2"));
    dir2_->setObjectName("patternDir2");
    form_->addRow(dir2_);
    linearRows_.push_back(dir2_);
    ref2Label_ = new SelectionFieldWidget("patternRef2", txt("ui.feature.select"), this);
    add(txt("ui.common.direction2"), ref2Label_, dir2Rows_);
    itemsAfter(ref2Label_);
    connect(ref2Label_, &SelectionFieldWidget::removed, this, [this](int i) { removeItem(ref2_, i), changed(); });
    ref2Items_ = ref2Label_->items();
    for (auto [field, n] : {std::pair{ref1Label_, 1}, std::pair{ref2Label_, 2}}) {
        field->setCursor(Qt::PointingHandCursor);
        connect(field, &SelectionFieldWidget::activated, this, [this, n] {
            armed_ = n;
            updateLabels();
        });
    }
    add(txt("ui.common.count"), exprRow("patternCount2", count2_, nullptr, ""), dir2Rows_);
    add(txt("ui.common.spacing"), exprRow("patternSpacing2", spacing2_, &rev2_, "directions.1.spacing"), dir2Rows_);
    // Circular: the axis is the same selection field as «Direction 1» (ref1_), own value fields.
    add(txt("ui.common.angle"), exprRow("patternAngle", angle_, &revC_, "angle"), circularRows_);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        // Type change: count values reset to the new mode's defaults; a direction that is also an axis (not a normal,
        // nothing built) is kept, the input of the field follows the mode (selection.md §3.2, §3.3).
        count1_->setText(QString::number(mode() == "linear" ? PATTERN_DEFAULT_COUNT_LINEAR : PATTERN_DEFAULT_COUNT_CIRCULAR));
        const bool keep = ref1_.hasValue() && ref1_.aux().empty() && !ref1_.value().contains("normal");
        const doc::Json v = keep ? ref1_.value() : doc::Json();
        const QString l = ref1_.label();
        ref1_.setInput(mode() == "linear" ? "direction" : "axis");
        if (keep) ref1_.setValue(v, l);
        applyMode();
    });
    connect(dir2_, &QCheckBox::toggled, this, [this] { applyMode(); });
}

std::string PatternPanel::mode() const { return mode_->currentData().toString().toStdString(); }

void PatternPanel::applyMode() {
    if (mirror_) {  // only «Features» and «Plane»
        for (auto* group : {&linearRows_, &dir2Rows_, &circularRows_})
            for (auto* w : *group)
                if (w) w->setVisible(false);
        for (QWidget* w : {static_cast<QWidget*>(ref1Label_), count1_->parentWidget(), dimRow(spacing1_)->parentWidget()}) {
            w->setVisible(false);
            if (auto* l = form_->labelForField(w)) l->setVisible(false);
        }
        changed();
        return;
    }
    const bool lin = mode() == "linear";
    for (auto* w : linearRows_)
        if (w) w->setVisible(lin);
    // «Direction 1» and «Count» are shared: circular has «Axis» and count.
    ref1Label_->setVisible(true);
    form_->labelForField(ref1Label_)->setVisible(true);
    if (auto* l = qobject_cast<QLabel*>(form_->labelForField(ref1Label_))) l->setText(lin ? txt("ui.common.direction1") : txt("ui.common.axis"));
    count1_->parentWidget()->setVisible(true);
    form_->labelForField(count1_->parentWidget())->setVisible(true);
    for (auto* w : dir2Rows_)
        if (w) w->setVisible(lin && dir2_->isChecked());
    for (auto* w : circularRows_)
        if (w) w->setVisible(!lin);
    dimRow(spacing1_)->parentWidget()->setVisible(lin);
    form_->labelForField(dimRow(spacing1_)->parentWidget())->setVisible(lin);
    changed();
}

void PatternPanel::startNew(const std::optional<std::string>&) { startNewMode("linear"); }

void PatternPanel::startNewMode(const std::string& m) {
    loadTolerances(doc::Json::object());  // a new feature — no tolerances (FX12-PMI-007)
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    features_.clear();
    ref1_.setInput(m == "linear" ? "direction" : "axis");
    ref2_.clear();
    planeSel_.clear();
    pickTargets_.clear();
    last_ = nullptr;
    armed_ = 0;
    const auto units = model_.session().units();
    const QString spacing = formatNumber(PATTERN_DEFAULT_SPACING_MM / expr::lengthFactor(units.length));
    {
        const QSignalBlocker b(mode_), b2(dir2_);
        mode_->setCurrentIndex(std::max(0, mode_->findData(QString::fromStdString(m))));
        dir2_->setChecked(false);
    }
    count1_->setText(QString::number(m == "linear" ? PATTERN_DEFAULT_COUNT_LINEAR : PATTERN_DEFAULT_COUNT_CIRCULAR));
    count2_->setText("2");
    spacing1_->setText(spacing);
    spacing2_->setText(spacing);
    angle_->setText(formatNumber(360.0 * (std::numbers::pi / 180) / expr::angleFactor(units.angle)));
    rev1_->setChecked(false);
    rev2_->setChecked(false);
    revC_->setChecked(false);
    applyMode();
}

void PatternPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || !f->pattern()) return;
    const doc::Json& def = f->pattern()->def;
    startNewMode(mirror_ ? std::string("linear") : def["mode"].get<std::string>());
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    for (const auto& x : def["features"]) features_.push_back(x);
    if (mirror_) {
        planeSel_.setValue(def["plane"], valueLabel(def["plane"]));
    } else if (def["mode"] == "linear") {
        const auto& dirs = def["directions"];
        ref1_.setValue(dirs[0]["ref"], valueLabel(dirs[0]["ref"]));
        count1_->setText(QString::fromStdString(dirs[0]["count"].get<std::string>()));
        spacing1_->setText(displayExpr(dirs[0]["spacing"].get<std::string>(), d->lengthUnit));
        rev1_->setChecked(dirs[0].value("reverse", false));
        if (dirs.size() > 1) {
            const QSignalBlocker b(dir2_);
            dir2_->setChecked(true);
            ref2_.setValue(dirs[1]["ref"], valueLabel(dirs[1]["ref"]));
            count2_->setText(QString::fromStdString(dirs[1]["count"].get<std::string>()));
            spacing2_->setText(displayExpr(dirs[1]["spacing"].get<std::string>(), d->lengthUnit));
            rev2_->setChecked(dirs[1].value("reverse", false));
        }
    } else {
        ref1_.setValue(def["axis"], valueLabel(def["axis"]));
        count1_->setText(QString::fromStdString(def["count"].get<std::string>()));
        angle_->setText(displayExpr(def["angle"].get<std::string>(), d->angleUnit));
        revC_->setChecked(def.value("reverse", false));
    }
    applyMode();
}

bool PatternPanel::toggleFeature(const std::string& featureId) {
    const auto f = model_.session().document().document->active().find(featureId);
    // Any feature with a tool, including a gear rim, a pattern and a mirror (FX7-PT-008); not the feature being edited.
    const bool tool = f && api::Session::isToolFeature(f->type) && (!editing_ || *editing_ != featureId);
    if (!tool) return false;
    auto it = std::find(features_.begin(), features_.end(), featureId);
    if (it != features_.end()) features_.erase(it);
    else features_.push_back(featureId);
    changed();
    return true;
}

FeaturePanel::PickOutcome PatternPanel::pickTree(const TreePick& p) {
    bool ok = false;
    if (p.target) ok = pick(*p.target);
    else if (!p.feature.empty()) {
        const auto f = model_.session().document().document->active().find(p.feature);
        if (f && f->revolve() && directionArmed()) ok = pick({PickTarget::Kind::Axis, {}, p.feature});  // axis of the revolve (Н6)
        else ok = toggleFeature(p.feature);
    }
    return ok ? PickOutcome::Taken : PickOutcome::Refused;
}

QString PatternPanel::neededInTree() const {
    // «Required: an extrude or a revolve in the tree, <what the field needs>».
    QString h = needed();
    const QString required = QString::fromUtf8(txt("ui.main.required"));
    if (h.startsWith(required)) h = h.mid(required.size());
    return txt("ui.main.requiredExtrudeOrRevolveIn") + h;
}

// ✕ of item i of a selection field (FX7-PT-009, selection.md К13): a pick of the set is removed like a repeated click,
// the accepted value clears the field.
void PatternPanel::removeItem(SelectionField& f, int i) {
    if (f.hasSet() && i < int(f.picks().size())) {
        const std::string ref = f.picks()[size_t(i)];
        f.click(ref, selectionState());
    } else {
        f.clear();
    }
}

QString PatternPanel::valueLabel(const doc::Json& r) const {
    if (!r.is_object()) return {};
    const auto st = selectionState();
    auto& s = model_.session();
    if (r.contains("normal")) return text("ui.sel.normal", {{"a", valueLabel(r["normal"])}});
    if (r.contains("datum")) return objectCaption(s, r["datum"], CaptionStyle::Candidate, st);
    if (r.contains("sketchLine")) return objectCaption(s, r["sketchLine"].value("sketch", std::string()) + ":" + r["sketchLine"].value("entity", std::string()), CaptionStyle::Candidate, st);
    for (const char* k : {"edge", "face", "feature", "revolveAxis"})
        if (r.contains(k) && r[k].is_string()) return objectCaption(s, r[k], CaptionStyle::Candidate, st);
    return {};
}

bool PatternPanel::pick(const PickTarget& t) {
    const std::string ref = pickRef(t);
    if (ref.empty()) {
        last_ = nullptr;
        return false;
    }
    const bool lin = mode() == "linear";
    const bool two = lin && dir2_->isChecked();
    auto has = [&](const SelectionField& f) { return std::find(f.picks().begin(), f.picks().end(), ref) != f.picks().end(); };
    // Routing (selection.md §5.1 item 6, К14): the field whose set contains the element; the armed one; the first
    // while it is empty or its set can be continued; then the second.
    SelectionField* f = &ref1_;
    if (mirror_) f = &planeSel_;
    else if (two && has(ref2_)) f = &ref2_;
    else if (two && !has(ref1_) && armed_ == 2) f = &ref2_;
    else if (two && !has(ref1_) && armed_ != 1 && ref1_.hasValue() && !(ref1_.hasSet() && ref1_.extensible())) f = &ref2_;
    pickTargets_[ref] = t;
    last_ = f;
    const auto outcome = f->click(ref, selectionState());
    if (outcome == SelectionField::Outcome::Rejected) return false;
    if (outcome == SelectionField::Outcome::Accepted || outcome == SelectionField::Outcome::Removed) armed_ = 0;
    changed();
    return true;
}

QString PatternPanel::needed() const {
    if (last_ && !last_->hint().isEmpty()) return last_->hint();
    return text(mirror_ ? "ui.sel.need.plane" : mode() == "linear" ? "ui.sel.need.direction" : "ui.sel.need.axis");
}

std::vector<PickTarget> PatternPanel::selectedTargets() const {
    std::vector<PickTarget> out;
    auto add = [&](const SelectionField& f) {
        for (const auto& r : f.picks())
            if (auto it = pickTargets_.find(r); it != pickTargets_.end()) out.push_back(it->second);
        if (f.picks().empty() && f.hasValue())
            if (auto t = refTarget(f.value()); t && f.aux().empty()) out.push_back(*t);
    };
    if (mirror_) {
        add(planeSel_);
        return out;
    }
    add(ref1_);
    if (mode() == "linear" && dir2_->isChecked()) add(ref2_);
    return out;
}

std::vector<doc::Json> PatternPanel::aux() const {
    if (mirror_) return planeSel_.aux();
    std::vector<doc::Json> out = ref1_.aux();
    if (mode() == "linear" && dir2_->isChecked()) out.insert(out.end(), ref2_.aux().begin(), ref2_.aux().end());
    return out;
}

bool PatternPanel::selectionPending() const {
    if (mirror_) return planeSel_.pending();
    return ref1_.pending() || (mode() == "linear" && dir2_->isChecked() && ref2_.pending());
}

bool PatternPanel::canAccept() const {
    if (selectionPending()) return false;  // a candidate waits for «Yes» (selection.md §5.3)
    if (mirror_) return !features_.empty() && planeSel_.hasValue();
    if (features_.empty() || !ref1_.hasValue()) return false;
    return mode() != "linear" || !dir2_->isChecked() || ref2_.hasValue();
}

std::string PatternPanel::operation() const {
    const auto d = model_.session().document().document;
    int best = -1;
    std::string op = "add";
    for (const auto& id : features_) {
        const int i = d->active().indexOf(id);
        const auto f = d->active().find(id);
        if (!f || (best >= 0 && i > best)) continue;
        best = i;
        op = sourceOperation(*d, *f);
    }
    return op == "new" ? "add" : op;
}

void PatternPanel::updateLabels() {
    ok_->setEnabled(canAccept());
    const auto d = model_.session().document().document;
    std::vector<SelectionItem> featureItems;
    for (const auto& id : features_)
        if (const auto f = d->active().find(id)) featureItems.push_back({QString::fromStdString(f->name), {}, id});
    // A field shows the accepted value, the candidate waiting for «Yes» or the hint of its set (selection.md §5.3).
    auto refText = [this](const SelectionField& f) {
        QString t = f.hasValue() ? f.label() : QString(txt("ui.feature.select"));
        if (const auto* c = f.current()) t = candidateLabel(model_.session(), *c, selectionState()) + txt("ui.feature.waitsForYes");
        else if (f.hasSet() && !f.hint().isEmpty() && !f.hasValue()) t = f.hint();
        return t;
    };
    // The direction field the next click goes to is active (the routing of pick without an own set, §5.5).
    const bool two = !mirror_ && mode() == "linear" && dir2_->isChecked();
    const bool toRef2 = two && (armed_ == 2 || (armed_ != 1 && ref1_.hasValue() && !(ref1_.hasSet() && ref1_.extensible())));
    ref1Label_->setActive(!toRef2);
    ref2Label_->setActive(toRef2);
    // ✕ at each item (FX7-PT-009): features, and the accepted values of the visible selection fields.
    featuresLabel_->setSelection(std::move(featureItems));
    // A field with a set lists its picks (✕ removes the pick, selection.md К13); otherwise the accepted value (✕ clears).
    auto refItem = [this](const SelectionField& f, QLabel* field) {
        std::vector<SelectionItem> out;
        if (field->isHidden()) return out;
        if (f.hasSet())
            for (const auto& r : f.picks()) {
                SelectionItem it{objectCaption(model_.session(), r, CaptionStyle::Candidate, selectionState()), {}, {}};
                if (auto t = pickTargets_.find(r); t != pickTargets_.end()) it.targets.push_back(t->second);
                else if (auto t2 = pickTargetOf(r)) it.targets.push_back(*t2);
                out.push_back(std::move(it));
            }
        else if (f.hasValue()) {
            SelectionItem it{f.label(), {}, {}};
            if (auto t = refTarget(f.value())) it.targets.push_back(*t);
            out.push_back(std::move(it));
        }
        return out;
    };
    ref1Label_->setSelection(refItem(ref1_, ref1Label_), refText(ref1_));
    ref2Label_->setSelection(refItem(ref2_, ref2Label_), refText(ref2_));
    planeLabel_->setSelection(refItem(planeSel_, planeLabel_), refText(planeSel_));
    showExprState(count1_, expr::DIMLESS);
    showExprState(count2_, expr::DIMLESS);
    showExprState(spacing1_, expr::LENGTH);
    showExprState(spacing2_, expr::LENGTH);
    showExprState(angle_, expr::ANGLE);
}

doc::Json PatternPanel::definition() const {
    const auto d = model_.session().document().document;
    if (mirror_) return {{"type", "mirror"}, {"features", features_}, {"plane", planeSel_.hasValue() ? planeSel_.value() : doc::Json(nullptr)}};
    doc::Json def = {{"type", "pattern"}, {"mode", mode()}, {"features", features_}};
    auto count = [](ExprField* f) {
        const std::string t = f->expression();
        return t.empty() ? std::string("1") : t;
    };
    if (mode() == "linear") {
        auto dir = [&](const SelectionField& r, int auxOffset, ExprField* c, ExprField* s, QToolButton* rev) {
            doc::Json j = {{"ref", r.hasValue() ? shiftAux(r.value(), auxOffset) : doc::Json(nullptr)}, {"count", count(c)},
                           {"spacing", s->expression()}};
            if (rev->isChecked()) j["reverse"] = true;
            return j;
        };
        def["directions"] = doc::Json::array({dir(ref1_, 0, count1_, spacing1_, rev1_)});
        if (dir2_->isChecked()) def["directions"].push_back(dir(ref2_, int(ref1_.aux().size()), count2_, spacing2_, rev2_));
    } else {
        def["axis"] = ref1_.hasValue() ? ref1_.value() : doc::Json(nullptr);
        def["count"] = count(count1_);
        def["angle"] = angle_->expression();
        if (revC_->isChecked()) def["reverse"] = true;
    }
    return def;
}

// «Through all both sides» is cut-only (app-ux §5.1, S1-FEAT-006): with «Add»
// the item is disabled and, if selected, replaced with «Through all».
void ExtrudePanel::updateEndTypes() {
    const bool cut = operation() == "cut" || operation() == "intersect";
    if (auto* m = qobject_cast<QStandardItemModel*>(endType_->model()))
        if (QStandardItem* it = m->item(3)) it->setEnabled(cut);
    if (!cut && endType_->currentIndex() == 3) endType_->setCurrentIndex(2);
}

}  // namespace construe::app
