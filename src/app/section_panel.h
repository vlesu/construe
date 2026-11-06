// Section panel (spec/app-ux.md §5.11, S2-VIEW-002, S2-VIEW-005): type (by plane / by sketch), plane, offset,
// sketch, "Flip"; section status. Definition — SectionDef (core-api.md §4.1б); ✓/✗ — the window's confirmation corner.
#pragma once

#include "app/expr_field.h"
#include "app/check_list.h"
#include "doc/json_text.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QAbstractButton>
#include <QToolButton>
#include <QWidget>

#include <optional>
#include <string>

namespace construe::app {

class AppModel;
class SelectionFieldWidget;

class SectionPanel : public QWidget {
    Q_OBJECT
public:
    explicit SectionPanel(AppModel& model, QWidget* parent = nullptr);
    // Start: editing — editing a section (its definition is def), otherwise a new one with default def.
    void start(const std::optional<std::string>& editing, const doc::Json& def);
    const std::optional<std::string>& editing() const { return editing_; }
    doc::Json definition() const;
    bool planeKind() const { return kindPlane_->isChecked(); }
    // Plane (PlaneRef) from the selection in the view or in the tree; the field shows its caption (app-ux §5.5г).
    void setPlane(const doc::Json& ref);
    std::optional<double> offsetMm() const;
    void setOffsetFromManipulator(double mm);
    void setStatus(const QString& error);  // empty — no error

signals:
    void accepted();   // ✓ of the header or Enter in a field (§2.1д, S7F-RF-008)
    void cancelled();  // ✗ of the header
    void changed();

private:
    void syncKind();
    void syncPlane();  // the plane field: the caption and the view object of plane_
    AppModel& model_;
    std::optional<std::string> editing_;
    doc::Json plane_;
    QAbstractButton *kindPlane_, *kindSketch_;  // options of the type row (ChoiceRow)
    QWidget *planeRow_, *sketchRow_;
    SelectionFieldWidget* planeField_;
    ExprField* offset_;
    QComboBox* sketch_;
    QToolButton* flip_;
    QLabel* excludeLabel_;
    CheckList* exclude_;  // assembly: instances the section does not cut (sections.md §7а)
    bool assembly_ = false;
    QLabel* status_;
};

}  // namespace construe::app
