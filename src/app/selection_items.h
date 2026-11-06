// Unified multi-object selection field (spec/app-ux.md §5.5а, S7F-UI-001): the full list of the selected objects, a ✕
// at each, hovering a row highlights its object in the 3D view, clicking a row centers the view on it. The only
// implementation of such a list in the application: every panel that picks several objects uses it (also the places
// tree, slot_tree.h).
#pragma once

#include "app/viewport_types.h"

#include <QString>
#include <QStringList>
#include <QWidget>

#include <string>
#include <vector>

namespace construe::app {

// One selected object of the list: its caption and the view objects it stands for (a face, an edge, an axis, a
// region…; body may be empty — the view resolves it by the stable name) and/or a reference the view resolves: a
// feature ID (a sketch, a pattern source — its geometry, like hovering it in the tree) or an assembly reference
// "<path>/<name>" (a face of an instance, a mate object).
struct SelectionItem {
    QString label;
    std::vector<PickTarget> targets;
    std::string ref;
};

// What the list asks of the 3D view (implemented by the main window). Not set (unit tests of panels) — nothing happens.
class SelectionView {
public:
    virtual ~SelectionView() = default;
    // Hover highlight of the item's objects; nullptr — the hover ends.
    virtual void hoverSelectionItem(const SelectionItem* item) = 0;
    // The view is centered on the item's objects so that they take SELECTION_FRAME_FRACTION of the view.
    virtual void centerOnSelectionItem(const SelectionItem& item) = 0;
};

// Rows "<name>Item<i>" with the caption "<name>ItemLabel<i>" and the button "<name>Remove<i>" (✕ removes the item,
// FX7-PT-009); the widget "<name>Items" is hidden while the list is empty.
class SelectionItems : public QWidget {
    Q_OBJECT
public:
    explicit SelectionItems(const QString& name, QWidget* parent = nullptr);
    ~SelectionItems() override;
    void setItems(std::vector<SelectionItem> items);
    // Captions only: items without view objects (a list of names, e.g. a test panel).
    void setItems(const QStringList& labels);
    int count() const { return int(rows_.size()); }
    const std::vector<SelectionItem>& items() const { return items_; }
    // The hovered row, -1 — none.
    int hovered() const { return hovered_; }

    // The view hooks of all lists (the main window sets itself, resets on destruction).
    static void setView(SelectionView* v);
    static SelectionView* view();

signals:
    void removed(int index);
    void hoveredChanged(int index);  // -1 — the hover ended
    void activated(int index);       // a click on the row (not on ✕): the view is centered on the object

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void setHovered(int i);

    QString name_;
    std::vector<QWidget*> rows_;
    std::vector<SelectionItem> items_;
    int hovered_ = -1;
};

}  // namespace construe::app
