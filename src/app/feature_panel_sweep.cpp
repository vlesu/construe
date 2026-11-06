// Sweep and loft panels (spec/app-ux.md §5.3е, §5.3ж).
#include "app/feature_panel.h"

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

// ---------------------------------------------------------------- sweep (§5.3е, S2-FEAT-006)

SweepPanel::SweepPanel(AppModel& model, QWidget* parent) : FeaturePanel(model, txt("ui.common.sweep"), parent, Profiles::Hidden) {
    setObjectName("sweepPanel");
    profileField_ = addSelectionField("sweepProfile", txt("ui.common.profile"), txt("ui.feature.selectASketch"), [this](int i) { removeProfileItem(i); });
    pathField_ = addSelectionField("sweepPath", txt("ui.feature.path"), txt("ui.feature.selectASketchOrEdges"), [this](int i) {
        if (!pathSketch_.empty()) pathSketch_.clear();
        else if (i < int(pathEdges_.size())) pathEdges_.erase(pathEdges_.begin() + i);
    });
    profileField_->setManyFormat(txt("ui.feature.profilesN"));
    pathField_->setManyFormat(txt("ui.common.edgesN"));
    for (auto* f : {profileField_, pathField_}) f->setRequired(true), fields_.add(f);
    sweepProfileItems_ = profileField_->items();
    pathItems_ = pathField_->items();
    form_->addRow(txt("ui.feature.operation"), buildOperationRow());
}

void SweepPanel::startNew(const std::optional<std::string>& sketch) {
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    sketch_.clear();
    regions_.reset();
    pathSketch_.clear();
    pathEdges_.clear();
    setOperation("add");
    fields_.setActive(profileField_);
    if (sketch) setSketch(*sketch);
    changed();
}

void SweepPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::Sweep) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    sketch_ = def["sketch"];
    if (def["profiles"].is_array()) {
        regions_ = std::vector<std::string>{};
        for (const auto& r : def["profiles"]) regions_->push_back(r);
    }
    if (def["path"].contains("sketch")) pathSketch_ = def["path"]["sketch"];
    else
        for (const auto& e : def["path"]["edges"]) pathEdges_.push_back(e);
    setOperation(def["operation"]);
    fields_.setActive(pathField_);
    changed();
}

void SweepPanel::setSketch(const std::string& sketchId) {
    if (pathField_->isActive() && !sketch_.empty()) {
        pathSketch_ = sketchId;
        pathEdges_.clear();
        changed();
        return;
    }
    FeaturePanel::setSketch(sketchId);
    fields_.setActive(pathField_);
}

void SweepPanel::toggleEdge(const std::string& name) {
    pathSketch_.clear();
    auto it = std::find(pathEdges_.begin(), pathEdges_.end(), name);
    if (it == pathEdges_.end()) pathEdges_.push_back(name);
    else pathEdges_.erase(it);
    fields_.setActive(pathField_);
    changed();
}

bool SweepPanel::pick(const PickTarget& t) {
    if (t.kind == PickTarget::Kind::Region) {
        toggleRegion(t.body, t.name);
        return true;
    }
    if (t.kind != PickTarget::Kind::Edge) return false;
    toggleEdge(t.name);
    return true;
}

std::vector<PickTarget> SweepPanel::selectedTargets() const {
    std::vector<PickTarget> out;
    for (const auto& e : pathEdges_) out.push_back({PickTarget::Kind::Edge, {}, e});
    return out;
}

bool SweepPanel::wantsBaseSketch() const {
    if (editing_) return false;
    if (pathField_->isActive() && !sketch_.empty()) return pathSketch_.empty() && pathEdges_.empty();
    return sketch_.empty();
}

bool SweepPanel::canAccept() const { return FeaturePanel::canAccept() && (!pathSketch_.empty() || !pathEdges_.empty()); }

void SweepPanel::updateLabels() {
    FeaturePanel::updateLabels();
    auto profile = profileLabels_;
    profileField_->setSelection(std::move(profile));
    std::vector<SelectionItem> path;
    if (!pathSketch_.empty()) path.push_back({objectCaption(model_.session(), pathSketch_, CaptionStyle::List), {}, pathSketch_});
    for (const auto& e : pathEdges_) path.push_back({topoCaption(model_.session(), e, true), {{PickTarget::Kind::Edge, {}, e}}, {}});
    pathField_->setSelection(std::move(path));
    ok_->setEnabled(canAccept());
}

doc::Json SweepPanel::definition() const {
    doc::Json path = pathSketch_.empty() ? doc::Json{{"edges", pathEdges_}} : doc::Json{{"sketch", pathSketch_}};
    return {{"type", "sweep"}, {"sketch", sketch_}, {"profiles", profilesJson()}, {"path", path}, {"operation", operation()}};
}

// ---------------------------------------------------------------- loft (§5.3ж, S2-FEAT-007)

LoftPanel::LoftPanel(AppModel& model, QWidget* parent) : FeaturePanel(model, txt("ui.common.loft"), parent, Profiles::Hidden) {
    setObjectName("loftPanel");
    sectionsLabel_ = addSelectionField("loftSections", txt("ui.feature.sections"), txt("ui.feature.selectSketchesInTheTree"), [this](int i) {
        if (i < int(sections_.size())) sections_.erase(sections_.begin() + i);
    });
    sectionsLabel_->setManyFormat(txt("ui.feature.sectionsN"));
    sectionsLabel_->setRequired(true);
    sectionItems_ = sectionsLabel_->items();
    // Start points of the sections (FX10-PT-004): the row's cross clears the start of its section.
    startsLabel_ = addSelectionField("loftStarts", txt("ui.feature.startPoints"), txt("ui.feature.clickSectionPoints"), [this](int i) {
        int k = 0;
        for (auto& sec : sections_)
            if (!sec.start.empty() && k++ == i) sec.start.clear();
    });
    form_->addRow(txt("ui.feature.operation"), buildOperationRow());
}

void LoftPanel::startNew(const std::optional<std::string>&) {
    editing_.reset();
    title_->setText(baseTitle_);
    banner_->hide();
    sections_.clear();
    setOperation("add");
    changed();
}

void LoftPanel::startEdit(const std::string& featureId) {
    const auto d = model_.session().document().document;
    const auto f = d->active().find(featureId);
    if (!f || f->type != doc::FeatureType::Loft) return;
    startNew(std::nullopt);
    editing_ = featureId;
    title_->setText(QString::fromStdString(f->name));
    const doc::Json& def = f->pattern()->def;
    for (const auto& s : def["sections"]) sections_.push_back({s["sketch"], s.value("region", std::string()), s.value("start", std::string())});
    setOperation(def["operation"]);
    changed();
}

void LoftPanel::setSketch(const std::string& sketchId) {
    auto it = std::find_if(sections_.begin(), sections_.end(), [&](const Section& s) { return s.sketch == sketchId; });
    if (it != sections_.end()) sections_.erase(it);
    else sections_.push_back({sketchId, {}});
    changed();
}

bool LoftPanel::pick(const PickTarget& t) {
    if (t.kind == PickTarget::Kind::SketchEntity) return setStart(t.body, t.name);
    if (t.kind != PickTarget::Kind::Region) return false;
    setRegion(t.body, t.name);
    return true;
}

void LoftPanel::setRegion(const std::string& sketchId, const std::string& regionId) {
    auto it = std::find_if(sections_.begin(), sections_.end(), [&](const Section& s) { return s.sketch == sketchId; });
    if (it == sections_.end()) sections_.push_back({sketchId, regionId});
    else it->region = regionId;
    changed();
}

bool LoftPanel::setStart(const std::string& sketchId, const std::string& pointId) {
    auto it = std::find_if(sections_.begin(), sections_.end(), [&](const Section& s) { return s.sketch == sketchId; });
    if (it == sections_.end()) return false;
    const auto d = model_.session().document().document;
    const auto f = d->active().find(sketchId);
    const auto* e = f && f->sketchData() ? f->sketchData()->findEntity(pointId) : nullptr;
    if (!e || e->type != sketch::EntityType::Point) return false;
    it->start = it->start == pointId ? std::string() : pointId;
    changed();
    return true;
}

void LoftPanel::updateLabels() {
    ok_->setEnabled(canAccept());
    const auto d = model_.session().document().document;
    std::vector<SelectionItem> items;
    for (const auto& s : sections_) {
        const auto f = d->active().find(s.sketch);
        const QString name = QString::fromStdString(f ? f->name : s.sketch) + (s.region.empty() ? QString() : QString(txt("ui.feature.region")));
        if (s.region.empty()) items.push_back({name, {}, s.sketch});
        else items.push_back({name, {{PickTarget::Kind::Region, s.sketch, s.region}}, {}});
    }
    sectionsLabel_->setSelection(std::move(items));
    std::vector<SelectionItem> starts;
    for (const auto& s : sections_)
        if (!s.start.empty()) {
            const auto f = d->active().find(s.sketch);
            starts.push_back({QString::fromStdString((f ? f->name : s.sketch) + ": " + s.start), {{PickTarget::Kind::SketchEntity, s.sketch, s.start}}, {}});
        }
    startsLabel_->setSelection(std::move(starts));
}

doc::Json LoftPanel::definition() const {
    doc::Json secs = doc::Json::array();
    for (const auto& s : sections_) {
        doc::Json j = {{"sketch", s.sketch}};
        if (!s.region.empty()) j["region"] = s.region;
        if (!s.start.empty()) j["start"] = s.start;
        secs.push_back(j);
    }
    return {{"type", "loft"}, {"sections", secs}, {"operation", operation()}};
}

}  // namespace construe::app
