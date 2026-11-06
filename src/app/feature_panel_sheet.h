// Sheet metal feature panels (spec/app-ux.md §5.3к, spec/sheet.md): sheet parameters, base wall, flange, sketch
// bend, cut.
#pragma once

#include "app/feature_panel.h"
#include "app/messages.h"

namespace construe::app {

// Common part of the sheet panels: clicks in the view and the selection shown in it (§5.3к).
class SheetPanel : public FeaturePanel {
    Q_OBJECT
public:
    using FeaturePanel::FeaturePanel;
    // A click in the view during editing; false — the target does not fit (the hint is needed()).
    bool pick(const PickTarget& t) override;
    QString needed() const override { return txt("ui.feature.requiredSketchInTheTree"); }
    QString prompt() const override { return needed(); }

protected:
    // Expression row: field (objectName name), calculated value, optional trailing widget.
    // param — the dimension parameter (app-ux §5.6а, FX12-PMI-007); "" — not a dimension (the K factor).
    ExprField* exprRow(const QString& label, const char* name, const char* param, QWidget* extra = nullptr);
    QToolButton* toggle(const char* name, const QString& text, const QString& tip);
    QToolButton* flip(const char* name, const QString& tip);  // «Flip» ⇅ (FlipButton, app-ux §5.13)
    std::string lengthUnit() const;
    std::string angleUnit() const;
};

// «Sheet metal part»: sheet parameters (S5-SHEET-001).
class SheetMetalPanel : public SheetPanel {
    Q_OBJECT
public:
    explicit SheetMetalPanel(AppModel& model, QWidget* parent = nullptr);
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override;
    bool pick(const PickTarget&) override { return false; }
    QString needed() const override { return txt("ui.feature.setThicknessAndRadius"); }
    ExprField* thicknessField() const { return thickness_; }
    ExprField* radiusField() const { return radius_; }
    ExprField* kFactorField() const { return k_; }

protected:
    void updateLabels() override;

private:
    ExprField *thickness_, *radius_, *k_, *reliefWidth_, *reliefDepth_;
    QComboBox* reliefType_;
    bool recognize_ = false;  // editing a «Sheet from body» feature (S5-SHEET-006)
};

// «Base wall»: closed profile — a plate, open — a bent profile (S5-SHEET-002).
class SheetWallPanel : public SheetPanel {
    Q_OBJECT
public:
    explicit SheetWallPanel(AppModel& model, QWidget* parent = nullptr);
    bool wantsBaseSketch() const override { return !editing_ && sketch_.empty(); }
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool picksRegions() const override { return true; }
    ExprField* widthField() const { return width_; }
    QComboBox* sideBox() const { return side_; }

protected:
    void updateLabels() override;

private:
    ExprField* width_;
    QComboBox* side_;
    QToolButton* reverse_;
};

// «Flange» by edges (S5-SHEET-003).
class SheetFlangePanel : public SheetPanel {
    Q_OBJECT
public:
    explicit SheetFlangePanel(AppModel& model, QWidget* parent = nullptr);
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override;
    bool pick(const PickTarget& t) override;
    bool showsPicks() const override { return true; }
    std::vector<PickTarget> selectedTargets() const override;
    QString needed() const override { return txt("ui.feature.requiredStraightWallEdge"); }
    void setEdges(const std::vector<std::string>& edges);
    const std::vector<std::string>& edges() const { return edges_; }
    ExprField* lengthField() const { return length_; }
    ExprField* angleField() const { return angle_; }
    QComboBox* positionBox() const { return position_; }

protected:
    void updateLabels() override;

private:
    std::vector<std::string> edges_;
    SelectionFieldWidget* edgesLabel_;
    SelectionItems* edgeItems_;
    ExprField *length_, *angle_, *radius_, *offsetStart_, *offsetEnd_;
    QComboBox *position_, *relief_;
    QToolButton* reverse_;
};

// «Bend on line» (S5-SHEET-004).
class SheetBendPanel : public SheetPanel {
    Q_OBJECT
public:
    explicit SheetBendPanel(AppModel& model, QWidget* parent = nullptr);
    bool wantsBaseSketch() const override { return !editing_ && sketch_.empty(); }
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override { return !sketch_.empty(); }
    bool pick(const PickTarget&) override { return false; }
    QString needed() const override { return txt("ui.feature.requiredSketchWithABend"); }
    ExprField* angleField() const { return angle_; }

protected:
    void updateLabels() override;

private:
    ExprField *angle_, *radius_;
    QToolButton *fixedRight_, *reverse_;
};

// «Sheet cut» by a sketch normal to the sheet (S5-SHEET-005).
class SheetCutPanel : public SheetPanel {
    Q_OBJECT
public:
    explicit SheetCutPanel(AppModel& model, QWidget* parent = nullptr);
    bool wantsBaseSketch() const override { return !editing_ && sketch_.empty(); }
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    std::string operation() const override { return "cut"; }
    bool picksRegions() const override { return true; }
};

}  // namespace construe::app
