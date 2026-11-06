// Repair wizard of lost references (spec/app-ux.md §5.5в, S7F-FEAT-003): the current feature with its lost references as
// places (the places tree, slot_tree.h), «Apply» / «Skip», ✓ — close, ✗ — revert.
#pragma once

#include "api/session.h"
#include "app/slot_tree.h"

#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>

#include <string>
#include <vector>

namespace construe::app {

class RepairPanel : public QWidget {
    Q_OBJECT
public:
    explicit RepairPanel(QWidget* parent = nullptr);
    // Show the feature: its name, position among the features with lost references, its lost references (places).
    void show(const std::string& feature, const QString& name, int index, int total, const std::vector<api::LostReference>& lost);
    void clear();  // no feature (the wizard is closed)
    const std::string& feature() const { return feature_; }
    const std::vector<api::LostReference>& lost() const { return lost_; }
    SlotTree* slotTree() const { return slots_; }
    // A click in the view: a face, an edge or a vertex of the shown state (stable name) into the current place.
    SlotSet::Outcome picked(const std::string& name, const std::string& kind, const SelectionItem& item);
    // Replacements of the filled places: every path of a lost reference — with the picked name.
    std::vector<api::Rebind> replacements() const;
    bool complete() const { return slots_->places().complete(); }

signals:
    void applyRequested();
    void skipRequested();
    void doneRequested();
    void revertRequested();

private:
    void sync();
    std::string feature_;
    std::vector<api::LostReference> lost_;
    QLabel* step_ = nullptr;
    SlotTree* slots_ = nullptr;
    QPushButton *apply_ = nullptr, *skip_ = nullptr;
};

// Caption of a lost reference's place (§5.5в): «Edge 1 — shown in red».
QString repairSlotTitle(const api::LostReference& l, int index);

}  // namespace construe::app
