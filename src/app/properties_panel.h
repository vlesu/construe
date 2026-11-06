// Part properties panel (spec/app-ux.md §5.10, S2-UX-001, S2-PART-*): name, designation, material, part color and
// face color, mass properties, without modal dialogs; each change is a kernel command (an undo step).
#pragma once

#include "doc/json_text.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

class AppModel;

class PropertiesPanel : public QWidget {
    Q_OBJECT
public:
    explicit PropertiesPanel(AppModel& model, QWidget* parent = nullptr);
    // Fill the fields from the active element and mass properties (after regeneration).
    void refresh();
    // Face selected in the view (empty: none): the «Face color» section.
    void setFace(const std::string& face);
    // Assembly instance (app-ux §8а, S2-UX-001): empty means the properties of the active object.
    void setInstance(const std::string& instance) { instance_ = instance; }
    const std::string& instance() const { return instance_; }
    QLineEdit* nameField() const { return name_; }
    QLineEdit* designationField() const { return designation_; }
    QComboBox* materialBox() const { return material_; }
    QLabel* massLabel() const { return mass_; }

signals:
    void accepted();
    void message(const QString& text, bool error);
    void openInstance(const QString& instance);

private:
    void openColorDialog();  // S7F-UI-002
    void applyMaterialIndex(int index);
    void applyOwnMaterial();
    void setPartColor(const std::optional<std::string>& c);
    void setFaceColor(const std::optional<std::string>& c);
    // Instance variables (§8а, S6-PARAM-004): variant per table, formula per variable.
    void refreshInstanceVariables();
    void applyInstanceVariables();

    AppModel& model_;
    bool loading_ = false;
    std::string face_;
    std::string instance_;
    QWidget* formBox_ = nullptr;   // object properties
    QWidget* instBox_ = nullptr;   // instance properties
    QLineEdit* instName_ = nullptr;
    QLabel *instObject_ = nullptr, *instDesignation_ = nullptr, *instMaterial_ = nullptr, *instMass_ = nullptr;
    QCheckBox *instFixed_ = nullptr, *instHidden_ = nullptr, *instSuppressed_ = nullptr, *instTransparent_ = nullptr;
    QLineEdit* instColorHex_ = nullptr;  // FX12-ASM-003: the instance color (#RRGGBB; empty — by the part)
    void setInstanceColor(const std::optional<std::string>& c);
    void openInstanceColorDialog();
    QPushButton* instOpen_ = nullptr;  // «Open object» / «Replace…» of a library instance (FX10-LIB-004)
    QWidget* instVars_ = nullptr;  // §8а "Instance variables"
    QFormLayout* instVarsForm_ = nullptr;
    QLabel* instQualifier_ = nullptr;
    std::string instVarsKey_;  // structure of the group: rebuilt only when it changes
    std::map<std::string, QComboBox*> variantBoxes_;  // table ID → choice
    std::map<std::string, QLineEdit*> overrideEdits_;  // variable ID → formula
    std::map<std::string, QLabel*> valueLabels_;       // variable ID → value
    QLineEdit* name_;
    QLineEdit* designation_;
    QComboBox* material_;
    QWidget* ownBox_;
    QLineEdit *ownName_, *ownDensity_, *ownE_, *ownNu_, *ownYield_, *ownUltimate_;
    QLineEdit* colorHex_;
    QCheckBox* transparent_;
    QLabel* faceTitle_;
    QWidget* faceRow_;
    QLabel *volume_, *area_, *mass_, *centroid_, *moments_;
    QLabel* missing_ = nullptr;  // assembly: parts without material (S2-ASM-009)
    QFormLayout* form_ = nullptr;
    QWidget* colorRow_ = nullptr;
};

}  // namespace construe::app
