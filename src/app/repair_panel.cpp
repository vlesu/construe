// Repair wizard of lost references (spec/app-ux.md §5.5в, S7F-FEAT-003).
#include "app/repair_panel.h"
#include "app/panel_header.h"

#include "app/messages.h"
#include "app/theme.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace construe::app {

QString repairSlotTitle(const api::LostReference& l, int index) {
    const QString kind = text("ui.repair.kind." + l.kind);
    return text(l.ghost ? "ui.repair.slot" : "ui.repair.slotNoGhost", {{"kind", kind}, {"index", QString::number(index)}});
}

RepairPanel::RepairPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("repairPanel");
    auto* v = new QVBoxLayout(this);
    // Command panel header with ✓ «Done» and ✗ (app-ux §2.1д, S7F-RF-008).
    auto* head = new PanelHeader(text("ui.repair.title"), PanelHeader::CommandButtons{"okButton", "cancelButton"});
    head->okButton()->setToolTip(txt("ui.repair.done2"));
    head->cancelButton()->setToolTip(text("ui.repair.cancel"));
    v->addWidget(head);
    step_ = new QLabel;
    step_->setObjectName("repairStep");
    step_->setWordWrap(true);
    step_->setTextFormat(Qt::PlainText);
    v->addWidget(step_);
    slots_ = new SlotTree("repair");
    v->addWidget(slots_);
    auto* buttons = new QHBoxLayout;
    apply_ = new QPushButton(text("ui.repair.apply"));
    apply_->setObjectName("repairApply");
    skip_ = new QPushButton(text("ui.repair.skip"));
    skip_->setObjectName("repairSkip");
    buttons->addWidget(apply_);
    buttons->addWidget(skip_);
    buttons->addStretch(1);
    v->addLayout(buttons);
    v->addStretch(1);
    connect(slots_, &SlotTree::changed, this, [this] { sync(); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        if (complete()) emit applyRequested();
    });
    connect(skip_, &QPushButton::clicked, this, &RepairPanel::skipRequested);
    connect(head, &PanelHeader::accepted, this, &RepairPanel::doneRequested);
    connect(head, &PanelHeader::cancelled, this, &RepairPanel::revertRequested);
}

void RepairPanel::show(const std::string& feature, const QString& name, int index, int total,
                       const std::vector<api::LostReference>& lost) {
    feature_ = feature;
    lost_ = lost;
    step_->setText(text("ui.repair.step", {{"feature", name}, {"count", QString::number(lost.size())},
                                           {"index", QString::number(index)}, {"total", QString::number(total)}}));
    std::vector<SlotSpec> specs;
    for (size_t i = 0; i < lost.size(); ++i)
        specs.push_back({std::to_string(i), repairSlotTitle(lost[i], int(i) + 1), lost[i].kind == "unknown" ? "any" : lost[i].kind, 1, 1, {}});
    slots_->setSpecs(std::move(specs));
    sync();
}

SlotSet::Outcome RepairPanel::picked(const std::string& name, const std::string& kind, const SelectionItem& item) {
    return slots_->click(SlotObject{name, {kind}, item});
}

std::vector<api::Rebind> RepairPanel::replacements() const {
    std::vector<api::Rebind> out;
    const SlotSet& set = slots_->places();
    for (int s = 0; s < set.count() && size_t(s) < lost_.size(); ++s) {
        if (set.objects(s).empty()) continue;
        for (const auto& p : lost_[size_t(s)].paths) out.push_back({p, set.objects(s).front().ref});
    }
    return out;
}

void RepairPanel::clear() {
    feature_.clear();
    lost_.clear();
    slots_->setSpecs({});
    sync();
}

void RepairPanel::sync() { apply_->setEnabled(complete()); }

}  // namespace construe::app
