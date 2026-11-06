// Feature property panels with preview (spec/app-ux.md §5): extrude and revolve.
#pragma once

#include "api/session.h"
#include "app/captions.h"
#include "app/check_list.h"
#include "app/dimension_field.h"
#include "app/expr_field.h"
#include "app/messages.h"
#include "app/selection_field.h"
#include "app/selection_field_widget.h"
#include "app/selection_items.h"
#include "app/viewport_types.h"
#include "doc/document.h"
#include "expr/expr.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

#include <functional>
#include <optional>
#include <set>
#include <vector>
#include <string>

namespace construe::app {

class AppModel;

// "Selected A, pressed B" (app-ux §3.6б, S7F-FEAT-002): the base of a new sketch in the selection — the first plane or face
// of the view selection, otherwise the plane selected in the tree (an origin plane ID or a «Plane» feature ID).
std::optional<PickTarget> selectionBase(const std::set<PickTarget>& view, const std::optional<std::string>& treePlane);

// Common part of a panel (§5.4, §5.7): header with ✓/✗, message bar, operation, preview.
class FeaturePanel : public QWidget {
    Q_OBJECT
public:
    // The «Profiles» field the base makes (sketch profiles of extrude-like features): Hidden — a panel
    // without profiles (fillet, shell, pattern, …) hides the field and its label (S7F-RF-013).
    enum class Profiles { Shown, Hidden };
    FeaturePanel(AppModel& model, const QString& title, QWidget* parent = nullptr, Profiles profiles = Profiles::Shown);

    virtual void startNew(const std::optional<std::string>& sketch) = 0;
    virtual void startEdit(const std::string& featureId) = 0;
    virtual doc::Json definition() const = 0;
    // Dimension tolerances (app-ux §5.6а, FX12-PMI-007): beginNew/beginEdit — startNew/startEdit with the tolerance
    // fields cleared / loaded from the feature; fullDefinition — definition() with `tolerances` of the dimension fields
    // whose parameter is in the definition (document.md §2.6); tolerancesValid — every tolerance record parses.
    void beginNew(const std::optional<std::string>& sketch);
    void beginEdit(const std::string& featureId);
    doc::Json fullDefinition() const;
    bool tolerancesValid() const;
    QString toleranceError() const;
    const std::vector<DimensionField*>& dimensionFields() const { return dims_; }
    virtual bool canAccept() const { return !sketch_.empty() && (!regions_ || !regions_->empty()); }
    // Unified selection (selection.md §5, FX7-SEL-001): owned features of the definition, a candidate waiting for «Yes»
    // (OK is unavailable meanwhile, §5.3), the field of the last click, and a change of a field from outside (bubble).
    virtual std::vector<doc::Json> aux() const { return {}; }
    virtual bool selectionPending() const { return false; }
    virtual SelectionField* lastField() { return nullptr; }
    void selectionChanged() { changed(); }
    // State in which the panel's selection is parsed: before the edited feature, otherwise at the marker.
    api::StateRef selectionState() const;
    virtual void setSketch(const std::string& sketchId);
    // Clicking a base makes a sketch for the field awaiting one (app-ux §3.6а, FX1-FEAT-001): new feature, empty field.
    virtual bool wantsBaseSketch() const { return false; }
    // Click dispatch (app-ux §5.5д, S7F-RF-011): the main window hands every click in the view to pick(); false — the
    // object does not fit the current field, and the status line shows needed(). prompt() is what the panel waits for
    // now (S7F-UI-006); needed() — the refusal «Required: …», by default the base hint or prompt().
    virtual bool pick(const PickTarget&) { return false; }
    virtual QString needed() const;
    virtual QString prompt() const;
    // A click on a tree node (§5.5д): the node's object (an origin or reference plane or axis, the origin) and the
    // feature ID of a feature node. NotTaken — the usual tree handling (a sketch into «Profiles», a base plane).
    struct TreePick {
        std::optional<PickTarget> target;
        std::string feature;
    };
    enum class PickOutcome { NotTaken, Taken, Refused };
    virtual PickOutcome pickTree(const TreePick&) { return PickOutcome::NotTaken; }
    virtual QString neededInTree() const { return needed(); }
    // The panel's picks shown as the view selection (§5.5д): showsPicks() — the panel owns the view selection.
    virtual bool showsPicks() const { return false; }
    virtual std::vector<PickTarget> selectedTargets() const { return {}; }
    // The «Profiles» field picks regions in the view: regions of sketches are shown (§5.5).
    virtual bool picksRegions() const { return false; }
    // «Profiles» field (§5.5): whole sketch ("all") or a list of regions; click on a region (§5, item 360).
    void toggleRegion(const std::string& sketchId, const std::string& regionId);
    const std::string& sketch() const { return sketch_; }
    // Language switch (§9.5): the stored title the panel shows again on every opening follows the new language.
    void retranslateTitle(const QString& from, const QString& to);
    const std::optional<std::vector<std::string>>& regions() const { return regions_; }

    std::optional<std::string> editing() const { return editing_; }
    // Leaving the panel forgets the edited feature (app-ux §5): the next new feature in this panel must not show the
    // state before the previously edited one — the scene is built on entering the mode, before startNew.
    void clearEditing() { editing_.reset(); }
    bool previewTimerActive() const { return previewTimer_.isActive(); }
    QTimer* previewTimerForTest() { return &previewTimer_; }  // §12.1, FX14-CI-002
    virtual std::string operation() const;
    void setPreviewMessage(const QString& text, bool error);
    // Selection field (§5.5, S7F-RF-009): the form row «label: header» and the list under it; onRemove(i) removes the
    // i-th item (✕, FX7-PT-009), then the panel is updated.
    SelectionFieldWidget* addSelectionField(const QString& name, const QString& label, const QString& prompt, std::function<void(int)> onRemove);
    // Assembly feature targets (§8а, assembly.md §7г.1): the instance list; nothing checked — all intersected instances.
    QListWidget* targetsList() const { return targets_; }
    std::vector<std::string> targets() const;

signals:
    void accepted();
    void cancelled();
    void previewRequested();

protected:
    void changed();
    virtual void updateLabels();
    void setOperation(const std::string& op);
    void defaultOperation();
    // Expression field of the panel (§5.6, S7F-RF-006): checked against the document.
    ExprField* newExprField(std::optional<expr::Dim> dim = std::nullopt);
    // Dimension field (§5.6а, FX12-PMI-007): an expression field wrapped into a DimensionField of parameter param; the
    // form row takes dimRow(field). Every dimension of a feature panel is made so (the panel catalog test).
    ExprField* newDimensionField(const std::string& param, std::optional<expr::Dim> dim = std::nullopt);
    // Wraps another value widget (the hole size list) into a DimensionField.
    DimensionField* wrapDimension(const std::string& param, QWidget* value);
    // The row widget of a value: its DimensionField, otherwise the value itself.
    static QWidget* dimRow(QWidget* value);
    // An expression field that is not a dimension (a count, the K factor, …; §5.6а).
    ExprField* newNumberField(std::optional<expr::Dim> dim = std::nullopt);
    void loadTolerances(const doc::Json& def);
    std::vector<DimensionField*> dims_;
    // Re-check a field with its expected dimension (a typed change waits for EXPR_CHECK_DEBOUNCE_MS).
    static void showExprState(ExprField* field, std::optional<expr::Dim> dim);

    AppModel& model_;
    std::optional<std::string> editing_;
    std::string sketch_;
    std::optional<std::vector<std::string>> regions_;  // nullopt: the whole sketch ("all")
    doc::Json profilesJson() const;
    void loadProfiles(const doc::Profiles& p);
    QLabel* title_;
    QLabel* banner_;
    SelectionFieldWidget* profiles_;
    SelectionItems* profileItems_;  // ✕ at each region or at the whole sketch (FX7-PT-009)
    std::vector<SelectionItem> profileLabels_;  // items of the «Profiles» field (updateLabels)
    void removeProfileItem(int i);
    QFormLayout* form_;
    QButtonGroup* ops_;
    QCheckBox* outside_ = nullptr;  // «Cut outside the contour» (vision item 17)
    // «Operation» row: «Add» / «Cut» and the outside-cut checkbox (app-ux §5.1).
    QLayout* buildOperationRow();
    // «Instances» row of an assembly feature (§8а): built by the panel, shown only in an assembly.
    void buildTargetsRow();
    // Fills the list with the active assembly's top-level instances, checks selected; hides the row outside an assembly.
    void loadTargets(const std::vector<std::string>& selected);
    bool assemblyMode() const;
    CheckList* targets_ = nullptr;
    QPushButton* ok_;
    QPushButton* cancel_;
    QTimer previewTimer_;
    QString baseTitle_;
};

class ExtrudePanel : public FeaturePanel {
    Q_OBJECT
public:
    explicit ExtrudePanel(AppModel& model, QWidget* parent = nullptr);
    bool wantsBaseSketch() const override { return !editing_ && sketch_.empty(); }
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    ExprField* depthField() const { return depth_; }
    // Depth manipulator (§5.8): current depth in mm and writing a new value.
    std::optional<double> depthMm() const;
    bool symmetric() const;
    bool reversed() const;
    void setDepthFromManipulator(double mm, bool flipDirection);
    // «Up to» (§5.1, S7F-FEAT-001): the end object field is shown; pick — a click on a face, plane, edge, vertex or the
    // origin fills it (a repeated click removes it; a third object replaces the earliest); false — not such an object.
    bool upToMode() const;
    bool pick(const PickTarget& t) override;
    QString needed() const override;
    QString prompt() const override { return text("ui.wait.profile"); }
    PickOutcome pickTree(const TreePick& p) override;
    bool picksRegions() const override { return true; }
    bool canAccept() const override;

protected:
    void updateLabels() override;

private:
    QComboBox* endType_;
    ExprField* depth_;
    QWidget* depthLabel_ = nullptr;
    QToolButton* reverse_;
    bool reverseTouched_ = false;  // the user toggled the direction: automatic cut direction no longer applies
    bool opTouched_ = false;       // user toggled the operation: auto operation no longer applies (§5.1, FX1-FEAT-003)
    bool endTouched_ = false;      // the user chose the end type: the sheet cut default no longer applies (FX8-SH-003)
    void sheetCutDefault();        // a new cut in a sheet part — «Through all» (app-ux §5.1, FX8-SH-003)
    // A cut from a sketch on a face goes into the material (§5.1): default direction for the current operation.
    void autoDirection();
    void updateEndTypes();
    // Automatic operation (§5.1, FX1-FEAT-003): sideChanged means a side change, otherwise a choice of end type.
    void autoOperation(bool sideChanged);
    void updateTitle();  // «Extrude» / «Cut» for a new feature
    QLayout* opRow_ = nullptr;
    // Assembly cut (§8а, assembly.md §7г): the operation is always a cut, the row is hidden; targets are shown.
    void applyAssemblyMode(const std::vector<std::string>& targets);
    // Tab extensions of a sheet part (§5.3к, FX7-SH-003).
    ExprField* tabLeft_ = nullptr;
    ExprField* tabRight_ = nullptr;
    QLayout* tabLeftRow_ = nullptr;
    QLayout* tabRightRow_ = nullptr;
    bool tabFieldsShown() const;
    void updateTabRows();
    // «End object» and «Offset» of the «Up to» end (§5.1, S7F-FEAT-001).
    SelectionFieldWidget* upToLabel_ = nullptr;
    SelectionItems* upToItems_ = nullptr;
    std::vector<std::pair<PickTarget, doc::Json>> upTo_;
    ExprField* upToOffset_ = nullptr;
    QWidget* upToOffsetRow_ = nullptr;
    void updateUpToRows();
};

class RevolvePanel : public FeaturePanel {
    Q_OBJECT
public:
    explicit RevolvePanel(AppModel& model, QWidget* parent = nullptr);
    bool wantsBaseSketch() const override { return !editing_ && sketch_.empty(); }
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override { return FeaturePanel::canAccept() && (!axisDatum_.empty() || !axisLine_.empty()); }
    void setSketch(const std::string& sketchId) override;
    void setAxisDatum(const std::string& axis);
    void setAxisLine(const std::string& lineId);
    // A region — into «Profiles», an axis — into «Axis» (§5.2).
    bool pick(const PickTarget& t) override;
    QString needed() const override;
    QString prompt() const override { return text("ui.wait.profile"); }
    PickOutcome pickTree(const TreePick& p) override;
    bool picksRegions() const override { return true; }
    ExprField* angleField() const { return angle_; }

protected:
    void updateLabels() override;
    // Axis reference of the definition (§5.2): sketch line, datum axis or reference axis.
    doc::Json axisJson() const;
    void loadAxis(const doc::Json& axis);
    void defaultAxis();

    std::string axisDatum_, axisLine_;
    QLayout* angleRow_ = nullptr;
    SelectionFieldWidget* axis_;
    SelectionItems* axisItems_;
    QComboBox* angleType_;
    ExprField* angle_;
};

// Coil — spring / custom thread (§5.3м, FX7-PT-004): profile and axis as the revolve; pitch, turns or height, hand, start angle.
class CoilPanel : public RevolvePanel {
    Q_OBJECT
public:
    explicit CoilPanel(AppModel& model, QWidget* parent = nullptr);
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    ExprField* pitchField() const { return pitch_; }
    QComboBox* extentBox() const { return extent_; }
    ExprField* extentField() const { return value_; }
    QComboBox* handBox() const { return hand_; }
    ExprField* startAngleField() const { return start_; }

protected:
    void updateLabels() override;

private:
    ExprField *pitch_, *value_, *start_;
    QComboBox *extent_, *hand_;
};

// Fillet / chamfer (§5.3, 1б): edges are a selection field (click on an edge in the view), size is an expression field.
class EdgePanel : public FeaturePanel {
    Q_OBJECT
public:
    EdgePanel(AppModel& model, bool fillet, QWidget* parent = nullptr);
    void startNew(const std::optional<std::string>& sketch) override;  // sketch is unused
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override { return !edges_.empty(); }
    void setEdges(const std::vector<std::string>& edges);
    void toggleEdge(const std::string& name);
    const std::vector<std::string>& edges() const { return edges_; }
    bool pick(const PickTarget& t) override;
    QString needed() const override { return txt("ui.main.requiredEdge"); }
    QString prompt() const override { return text("ui.wait.edges"); }
    bool showsPicks() const override { return true; }
    std::vector<PickTarget> selectedTargets() const override;
    ExprField* sizeField() const { return size_; }
    bool fillet() const { return fillet_; }

protected:
    void updateLabels() override;

private:
    bool fillet_;
    std::vector<std::string> edges_;
    SelectionFieldWidget* edgesLabel_;
    SelectionItems* edgeItems_;
    ExprField* size_;
    // Chamfer (§5.3, FX7-PT-010): «Distance» / «Distance and angle» / «Two distances», the second value, «Flip».
    QComboBox* mode_ = nullptr;
    ExprField* size2_ = nullptr;
    QWidget* size2Label_ = nullptr;
    QToolButton* flip_ = nullptr;
    void applyMode();

public:
    QComboBox* chamferMode() const { return mode_; }
    ExprField* secondField() const { return size2_; }
    QToolButton* flipButton() const { return flip_; }
};

// Reference plane / axis (§5.3а, S2-REF-*): type (mode), «References» selection field, value.
class RefGeomPanel : public FeaturePanel {
    Q_OBJECT
public:
    RefGeomPanel(AppModel& model, bool plane, QWidget* parent = nullptr);
    bool isPlane() const { return plane_; }
    void startNew(const std::optional<std::string>& sketch) override;  // sketch is unused
    void startNewMode(const std::string& mode);
    // A new feature with the given definition (S7F-ASM-006: the plane through the preselected objects).
    void startNewFrom(const doc::Json& def);
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override;
    // Click on an object in the view (§5.3а): false means the object does not suit the mode.
    bool pick(const PickTarget& t) override;
    QString needed() const override;  // «Required: …» for the hint
    QString prompt() const override { return needed(); }
    PickOutcome pickTree(const TreePick& p) override;
    bool showsPicks() const override { return true; }
    std::vector<PickTarget> selectedTargets() const override;
    std::string mode() const;
    ExprField* valueField() const { return value_; }
    void setOffsetText(const QString& t);  // the offset field as if typed (S7F-ASM-006)

protected:
    void updateLabels() override;

private:
    struct Ref {
        std::string kind;  // plane | line | point | face | edge
        doc::Json json;
        PickTarget target;
        int order = 0;
    };
    std::vector<std::string> slotKinds() const;
    void setModeKeepRefs(const std::string& mode);
    void loadDefinition(const doc::Json& def);
    bool plane_;
    std::vector<std::optional<Ref>> refs_;
    int pickCounter_ = 0;
    QComboBox* mode_;
    SelectionFieldWidget* refsLabel_;
    SelectionItems* refItems_;
    ExprField* value_;
    QWidget* valueLabel_ = nullptr;
    QToolButton* flip_;
    QCheckBox* far_;  // offset plane up to an object: far tangent (FX4-FEAT-001)
    bool toMode() const;  // "up to object" or "at a distance from object" mode
};

// Sweep (§5.3е, S2-FEAT-006): the active field is the profile (sketch/regions) or the path (sketch/edges).
class SweepPanel : public FeaturePanel {
    Q_OBJECT
public:
    explicit SweepPanel(AppModel& model, QWidget* parent = nullptr);
    // The active field (profile or path) is empty (§3.6а).
    bool wantsBaseSketch() const override;
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override;
    // Sketch from the tree goes into the active field (profile → then the path becomes active).
    void setSketch(const std::string& sketchId) override;
    void toggleEdge(const std::string& name);
    const std::vector<std::string>& pathEdges() const { return pathEdges_; }
    // A region — into the profile, an edge — into the path (§5.3е).
    bool pick(const PickTarget& t) override;
    QString needed() const override { return txt("ui.main.requiredSketchInTheTree"); }
    QString prompt() const override { return text("ui.wait.sweep"); }
    bool showsPicks() const override { return true; }
    std::vector<PickTarget> selectedTargets() const override;
    bool picksRegions() const override { return true; }

protected:
    void updateLabels() override;

private:
    std::string pathSketch_;
    std::vector<std::string> pathEdges_;
    SelectionFieldWidget* profileField_;
    SelectionFieldWidget* pathField_;
    SelectionFieldGroup fields_;
    SelectionItems *sweepProfileItems_, *pathItems_;
};

// Thread (§5.3и, S2-FEAT-009): cylindrical face, designation, length.
class ThreadPanel : public FeaturePanel {
    Q_OBJECT
public:
    explicit ThreadPanel(AppModel& model, QWidget* parent = nullptr);
    void startNew(const std::optional<std::string>& sketch) override;  // sketch is unused
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override;
    // Face from the view; list: designations matching the diameter, kernel's proposal selected (§5.3и, FX2-FEAT-001).
    void setFace(const std::string& name, const std::string& body);
    QLabel* infoLabel() const { return info_; }
    const std::string& face() const { return face_; }
    bool pick(const PickTarget& t) override;
    QString needed() const override { return txt("ui.main.requiredCylindricalFace"); }
    QString prompt() const override { return text("ui.wait.cylinder"); }
    bool showsPicks() const override { return true; }
    std::vector<PickTarget> selectedTargets() const override;
    QComboBox* designationBox() const { return designation_; }
    ExprField* lengthField() const { return length_; }
    QCheckBox* fullLength() const { return full_; }
    // Free thread (§5.3и, FX7-PT-003): «Custom thread» — pitch and profile height by hand.
    QCheckBox* customBox() const { return custom_; }
    ExprField* pitchField() const { return pitch_; }
    ExprField* heightField() const { return height_; }

protected:
    void updateLabels() override;

private:
    std::string face_;
    SelectionFieldWidget* faceLabel_;
    SelectionItems* faceItems_;
    QCheckBox* custom_;
    ExprField *pitch_, *height_;
    QWidget *pitchLabel_ = nullptr, *heightLabel_ = nullptr;
    // Nominal diameter of a free thread on the face: the face diameter (external) or the tap drill + 1.0825·P.
    std::optional<double> customDiameter() const;
    QComboBox* designation_;
    QCheckBox* full_;
    ExprField* length_;
    QToolButton* reverse_;
    QLabel* info_;
    std::optional<api::ThreadFit> fit_;      // threadFit response for the face (none: no face selected)
    std::optional<Error> fitError_;          // threadFit error (not a cylinder)
    // Thread given by diameter and pitch expressions (document.md §2.10и, S6-LIB-008): the fields are kept as they are,
    // the list shows the evaluated designation and is not editable.
    std::optional<doc::Json> byExpr_;
    // Query threadFit for face_ and fill the list; prefer is the designation selected if it is in the list.
    void refit(const std::string& prefer = {});
};

// Gear rim (§5.3л, S6-GEAR-002…004): cylindrical face, module, teeth, shift, width, sector, accuracy grade.
class GearPanel : public FeaturePanel {
    Q_OBJECT
public:
    explicit GearPanel(AppModel& model, QWidget* parent = nullptr);
    void startNew(const std::optional<std::string>& sketch) override;  // sketch is unused
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override;
    // Face from the view: diameter and kind by threadFit; a new feature gets the teeth matching the face (§5.3л).
    void setFace(const std::string& name);
    const std::string& face() const { return face_; }
    bool pick(const PickTarget& t) override;
    QString needed() const override { return txt("ui.main.requiredCylindricalFace"); }
    QString prompt() const override { return text("ui.wait.cylinder"); }
    bool showsPicks() const override { return true; }
    std::vector<PickTarget> selectedTargets() const override;
    QLabel* infoLabel() const { return info_; }
    ExprField* moduleField() const { return module_; }
    ExprField* teethField() const { return teeth_; }
    // «Add» / «Cut» (FX7-PT-006).
    bool adds() const { return opAdd_->isChecked(); }
    // Suggested number of teeth for a face of diameter D (§5.3л, FX10-GEAR-001): the nearest z whose tip (cut) or root
    // (add) diameter fits the face — never the one giving the error REGEN_GEAR_DIAMETER / REGEN_GEAR_ROOT_DIAMETER.
    static long suggestedTeeth(double D, double m, double x, bool internal, bool add);
    std::string operation() const override { return adds() ? "add" : "cut"; }

protected:
    void updateLabels() override;

private:
    QAbstractButton *opAdd_, *opCut_;  // options of the operation row (ChoiceRow)
    SelectionItems* faceItems_;
    // The teeth giving da = D (cut) or df = D (add) for the current module and shift (§5.3л).
    void suggestTeeth();
    bool teethTyped_ = false;  // the user typed the number of teeth: module and shift edits keep it (FX10-GEAR-001)
    // Values of the parameter fields (nullopt — a field is invalid).
    std::optional<double> fieldValue(ExprField* f, expr::Dim dim) const;
    // State in which the face is resolved: final for a new rim, the one before the feature when editing.
    api::StateRef inputState() const;
    std::string face_;
    SelectionFieldWidget* faceLabel_;
    ExprField *module_, *teeth_, *shift_, *width_, *sectorStart_, *sectorAngle_;
    QLineEdit* accuracy_;
    QCheckBox *full_, *sector_;
    QToolButton* reverse_;
    QLabel* info_;
    std::optional<double> diameter_;  // face diameter, mm (threadFit)
    bool internal_ = false;
    std::optional<doc::Json> keptSector_;  // sector between faces of the edited feature — kept as is
};

// Hole (§5.3з, S2-FEAT-008): sketch with standalone points, type, size, pitch/series, end condition.
class HolePanel : public FeaturePanel {
    Q_OBJECT
public:
    explicit HolePanel(AppModel& model, QWidget* parent = nullptr);
    bool wantsBaseSketch() const override { return !editing_ && sketch_.empty(); }
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override { return !sketch_.empty(); }
    QComboBox* kindBox() const { return kind_; }
    QComboBox* sizeBox() const { return size_; }
    ExprField* depthField() const { return depth_; }
    // Where an assembly hole cuts (FX8-ASM-007, assembly.md §7г.6): "assembly" or "parts"; a new one — this default.
    void setDefaultCut(const std::string& c) { defaultCut_ = c; }

protected:
    void updateLabels() override;

private:
    void fillPitches();
    QComboBox *kind_, *size_, *pitch_, *fit_, *end_, *cut_;
    std::string defaultCut_ = "assembly";
    ExprField* depth_;
    QToolButton* reverse_;
    QLabel* info_;
    QWidget *pitchLabel_ = nullptr, *fitLabel_ = nullptr, *depthLabel_ = nullptr;
};

// Loft (§5.3ж, S2-FEAT-007): sections are sketches from the tree in selection order, a region by clicking in the view.
class LoftPanel : public FeaturePanel {
    Q_OBJECT
public:
    explicit LoftPanel(AppModel& model, QWidget* parent = nullptr);
    bool wantsBaseSketch() const override { return !editing_; }  // a new section goes to the end of the list (§3.6а)
    void startNew(const std::optional<std::string>& sketch) override;
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override { return sections_.size() >= 2; }
    void setSketch(const std::string& sketchId) override;  // add/remove a section
    void setRegion(const std::string& sketchId, const std::string& regionId);
    bool pick(const PickTarget& t) override;  // a region — the section's region (§5.3ж)
    QString prompt() const override { return text("ui.wait.loft"); }
    bool picksRegions() const override { return true; }
    struct Section {
        std::string sketch;
        std::string region;  // empty: the sketch's only region
        std::string start;   // start point of the section's contour (FX10-PT-004); empty — none
    };
    const std::vector<Section>& sections() const { return sections_; }
    // A point of a section sketch clicked in the view sets / clears that section's start (§5.3ж, FX10-PT-004).
    bool setStart(const std::string& sketchId, const std::string& pointId);

protected:
    void updateLabels() override;

private:
    std::vector<Section> sections_;
    SelectionFieldWidget* sectionsLabel_;
    SelectionItems* sectionItems_;
    SelectionFieldWidget* startsLabel_;
};

// Shell (§5.3г, S2-FEAT-004): faces to remove by clicking in the view, thickness, «Outward».
class ShellPanel : public FeaturePanel {
    Q_OBJECT
public:
    explicit ShellPanel(AppModel& model, QWidget* parent = nullptr);
    void startNew(const std::optional<std::string>& sketch) override;  // sketch is unused
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override { return !faces_.empty(); }
    void setFaces(const std::vector<std::string>& faces);
    void toggleFace(const std::string& name);
    const std::vector<std::string>& faces() const { return faces_; }
    bool pick(const PickTarget& t) override;
    QString needed() const override { return txt("ui.main.requiredFace"); }
    QString prompt() const override { return text("ui.wait.shell"); }
    bool showsPicks() const override { return true; }
    std::vector<PickTarget> selectedTargets() const override;
    ExprField* thicknessField() const { return thickness_; }

protected:
    void updateLabels() override;

private:
    std::vector<std::string> faces_;
    SelectionFieldWidget* facesLabel_;
    SelectionItems* faceItems_;
    ExprField* thickness_;
    QToolButton* outward_;
};

// Draft (§5.3д, S2-FEAT-005): the active selection field is the neutral plane or the faces.
class DraftPanel : public FeaturePanel {
    Q_OBJECT
public:
    explicit DraftPanel(AppModel& model, QWidget* parent = nullptr);
    void startNew(const std::optional<std::string>& sketch) override;  // sketch is unused
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override { return neutral_.has_value() && !faces_.empty(); }
    // Click on an object: false means it does not fit.
    bool pick(const PickTarget& t) override;
    QString needed() const override { return txt("ui.main.requiredPlaneOrFace"); }
    QString prompt() const override { return text("ui.wait.draft"); }
    PickOutcome pickTree(const TreePick& p) override;  // origin and reference planes — the neutral plane
    bool showsPicks() const override { return true; }
    std::vector<PickTarget> selectedTargets() const override;
    void activateFaces() { fields_.setActive(facesField_); }
    ExprField* angleField() const { return angle_; }
    // «Select all adjacent faces» (§5.3д, FX10-PT-005): the faces next to the neutral plane or the chosen faces are added.
    void selectAdjacentFaces();
    const std::vector<std::string>& faces() const { return faces_; }

protected:
    void updateLabels() override;

private:
    std::optional<PickTarget> neutral_;
    std::vector<std::string> faces_;
    SelectionFieldWidget* neutralField_;
    SelectionFieldWidget* facesField_;
    SelectionFieldGroup fields_;
    SelectionItems *neutralItems_, *faceItems_;
    ExprField* angle_;
    QToolButton* reverse_;
    QPushButton* adjacent_;
};

// Pattern (§5.3б, S2-FEAT-002): source features by click in the tree, directions/axis by click in the view or tree.
// With mirror = true: feature mirror (§5.3в, S2-FEAT-003): the same «Features» and a «Plane».
class PatternPanel : public FeaturePanel {
    Q_OBJECT
public:
    explicit PatternPanel(AppModel& model, bool mirror = false, QWidget* parent = nullptr);
    bool isMirror() const { return mirror_; }
    void startNew(const std::optional<std::string>& sketch) override;  // sketch is unused
    void startNewMode(const std::string& mode);
    // A new feature with the given definition (S7F-ASM-006: the plane through the preselected objects).
    void startNewFrom(const doc::Json& def);
    void startEdit(const std::string& featureId) override;
    doc::Json definition() const override;
    bool canAccept() const override;
    std::string operation() const override;  // operation of the first source feature, for the preview
    std::string mode() const;
    // Click on a feature in the tree: true means the feature fits (extrude/revolve) and the list changed.
    bool toggleFeature(const std::string& featureId);
    // Click on an object in the view or a tree axis (selection.md §5.1): false means the object does not fit.
    bool pick(const PickTarget& t) override;
    QString needed() const override;
    QString prompt() const override { return needed(); }
    // Tree (§5.3б, §5.3в): axes, planes, the origin — into the direction / axis / plane, extrudes and other features
    // with a tool — into «Features», a revolve — into the armed direction field (Н6).
    PickOutcome pickTree(const TreePick& p) override;
    QString neededInTree() const override;
    bool showsPicks() const override { return true; }
    std::vector<PickTarget> selectedTargets() const override;
    std::vector<doc::Json> aux() const override;
    bool selectionPending() const override;
    SelectionField* lastField() override { return last_; }
    SelectionField& field(int i) { return i == 0 ? ref1_ : i == 1 ? ref2_ : planeSel_; }
    // A tree click on a revolve goes to the armed direction field (clicked label, Н6) instead of «Features».
    bool directionArmed() const { return armed_ != 0; }
    QString planeLabelText() const { return planeLabel_->text(); }  // the mirror «Plane» field (tests)
    const std::vector<std::string>& features() const { return features_; }
    ExprField* countField(int dir = 0) const { return dir == 0 ? count1_ : count2_; }
    ExprField* spacingField(int dir = 0) const { return dir == 0 ? spacing1_ : spacing2_; }
    ExprField* angleField() const { return angle_; }
    QCheckBox* secondDirection() const { return dir2_; }

protected:
    void updateLabels() override;

private:
    void applyMode();
    QString valueLabel(const doc::Json& ref) const;  // caption of a stored reference (editing)
    void removeItem(SelectionField& f, int i);       // ✕ of an item of a selection field (FX7-PT-009)
    bool mirror_;
    SelectionField planeSel_;  // mirror: symmetry plane (input "plane")
    SelectionFieldWidget* planeLabel_;
    SelectionItems *featureItems_, *planeItems_, *ref1Items_, *ref2Items_;
    QComboBox* mode_;
    SelectionFieldWidget* featuresLabel_;
    std::vector<std::string> features_;
    SelectionField ref1_, ref2_;  // direction 1 / axis ("direction" / "axis"); direction 2
    SelectionField* last_ = nullptr;
    std::map<std::string, PickTarget> pickTargets_;  // pick ref → view object (highlight)
    int armed_ = 0;                                  // 1 / 2 — direction field armed for the tree
    SelectionFieldWidget* ref1Label_;
    SelectionFieldWidget* ref2Label_;
    ExprField *count1_, *spacing1_, *count2_, *spacing2_, *angle_;
    QToolButton *rev1_, *rev2_, *revC_;
    QCheckBox* dir2_;
    std::vector<QWidget*> linearRows_, dir2Rows_, circularRows_;
};

}  // namespace construe::app
