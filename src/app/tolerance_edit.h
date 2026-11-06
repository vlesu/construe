// Tolerance field of a dimension (spec/app-ux.md §6.9, §5.6а; FX13-TOL-001): an editable list — free input of a
// tolerance record (pmi.md §5.4) and a drop-down arrow with the standard fields (pmi.md §4.3а). One component for the
// sketch dimension field (InlineDimensionEdit) and the feature dimension field (DimensionField).
#pragma once

#include <QLineEdit>
#include <QPointer>

class QAction;
class QMenu;

namespace construe::app {

class ToleranceEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit ToleranceEdit(QWidget* parent = nullptr);

    // Open the list of standard fields under the field (the arrow, F4, Alt+Down). Non-blocking.
    void showList();
    QMenu* list() const { return menu_; }  // the open list (nullptr — closed)
    QAction* listAction() const { return arrow_; }

protected:
    void keyPressEvent(QKeyEvent* e) override;

private:
    void pick(const QString& field);
    QAction* arrow_;
    QPointer<QMenu> menu_;
};

}  // namespace construe::app
