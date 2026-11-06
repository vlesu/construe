// Feature tree with a rollback marker (spec/app-ux.md §4).
#pragma once

#include "api/session.h"
#include "app/command_availability.h"
#include "base/error.h"
#include "doc/document.h"

#include <QTreeWidget>

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

class QMenu;

namespace construe::app {

class AppModel;

// The feature that created a face (S7F-TREE-002) by its base name (naming.md §2.3): the FeatureId of a GenBase; of a
// merge — of its member earliest in the element's history; no such feature in el — empty.
std::string faceCreator(const doc::Element& el, const std::string& base);
// Sketches shown inside the feature's tree node (§4.1): the profile sketch, then a sweep's path sketch (FX10-PT-001).
std::vector<std::string> nestedSketches(const doc::Feature& f);
// The status and messages of feature `id` shown in the tree (§4.2): a sketch used as a sweep path may be open — its
// SKETCH_OPEN_CURVES warning is not shown, and a warning status without other warnings becomes ok (FX10-PT-002).
regen::FeatureResult shownFeatureStatus(const doc::Element& el, const std::string& id, const regen::FeatureResult& r);

class FeatureTree : public QTreeWidget {
    Q_OBJECT
public:
    // KindRole: "element", "basis", "origin", "plane", "axis", "feature"; ElementRole: the node's document element ID;
    // RefRole is the sketch of a shared reference (§4.1).
    enum Role { IdRole = Qt::UserRole + 1, KindRole, ElementRole, RefRole };
    static constexpr int STATUS_COLUMN = 1;

    explicit FeatureTree(AppModel& model, QWidget* parent = nullptr);

    void rebuild(const std::optional<std::string>& editing = std::nullopt);
    // Node by visible name (without sketch-state prefixes); the active element is searched first.
    QTreeWidgetItem* itemByName(const QString& name) const;
    // Node of source geometry or of an active-element feature by ID (the first one that is not a shared reference).
    QTreeWidgetItem* itemById(const std::string& id) const;
    QTreeWidgetItem* activeElementItem() const;  // node of the active document element
    std::optional<std::string> selectedFeature() const;
    std::optional<std::string> selectedPlane() const;
    std::optional<std::string> selectedAxis() const;
    std::optional<std::string> selectedElement() const;
    std::optional<std::string> selectedSection() const;  // §4.2б
    std::optional<std::string> selectedAnnotation() const;  // §4.2в
    std::optional<std::string> selectedInstance() const;  // top-level instance of the active assembly (§8а)
    std::optional<std::string> selectedMate() const;      // mate of the active assembly (§8а)
    std::optional<std::string> selectedWeld() const;      // weld seam of the active assembly (§8а, S5-WELD-001)
    std::optional<std::string> selectedInstancePattern() const;  // instance pattern of the active assembly (§8а, FX7-ASM-007)
    QRect rollbackBarRect() const;  // test mode
    bool rollbackBarShown() const { return hasRollbackBar(); }  // the bar is drawn (FX9-TREE-002; test mode)
    // Color of the bar: transparent at the end of the history (S7F-TREE-001), COLOR_ROLLBACK elsewhere.
    QColor rollbackBarColor() const;
    // The feature whose face is selected in the view (S7F-TREE-002): its node is highlighted and scrolled to; empty — none.
    // The tree selection is not changed (it would replace the selection in the view).
    void setFaceFeature(const std::string& id);
    const std::string& faceFeature() const { return faceFeature_; }
    // A drag in the tree is going on (the bar, features, instances, elements): Esc cancels it (§4.3).
    bool dragging() const { return draggingBar_ || draggingRow_ || draggingInstance_ || draggingElement_; }
    // Marker gap after a level-2 node (y in viewport coordinates); empty if there is no such node (test mode).
    std::optional<int> gapYAfter(const QString& featureName) const;
    int gapYStart() const;
    int gapYEnd() const;
    void setCommandsEnabled(bool on) { commandsEnabled_ = on; }
    // Context-menu command availability per table §7.5 (FX4-UI-002): command name (tree_*) and subject element kind.
    using Availability = std::function<bool(const QString& command, ElementKind subject)>;
    void setAvailability(Availability f) { available_ = std::move(f); }
    // Extra context-menu items added by the main window (§8а): kind — node kind ("element", "basis" — also an empty
    // place, "instance"), id — node ID (FX7-ASM-008…010).
    using MenuHook = std::function<void(QMenu& menu, const QString& kind, const std::string& id)>;
    void setMenuHook(MenuHook f) { menuHook_ = std::move(f); }
    // Reordering features by drag and drop is available only in Part mode (§4.3, S2-FEAT-010).
    void setReorderEnabled(bool on) { reorderEnabled_ = on; }
    // Construction of the instances' parts under their nodes (FX9-ASM-001, «Part construction geometry» of the mate panel).
    void setPartConstructionShown(bool on) {
        if (on == partConstruction_) return;
        partConstruction_ = on;
        rebuild();
    }
    // Gap before a level-2 node (y in viewport coordinates); empty if there is no such node (test mode).
    std::optional<int> gapYBefore(const QString& featureName) const;
    // Delete the selected features (Del, context panel): one undo step, notification with «Undo» (§4.4).
    void deleteSelectedFeatures();
    // Suppress/unsuppress the selected feature (§4.5).
    void toggleSuppressSelected();
    // Rename the selected feature or element: an in-place field over the text (F2, context panel, §4.4).
    void renameSelected();
    // New part (§4.5, S2-DOC-001).
    void addPart(bool sheet = false);  // sheet — a sheet metal part (FX7-SH-002)
signals:
    void propertiesRequested();  // «Properties» in the part menu (app-ux §5.10)
    void recognizeRequested();   // «Recognize design» in the body menu (app-ux §8а, FX8-RB-003)
public:
    // Source planes pinned as visible (§4.2а).
    const std::set<std::string>& pinnedPlanes() const { return pinnedPlanes_; }
    // New or opened document: plane pins and node collapsing are reset to defaults.
    void resetViewState();
    // Visibility of sketches and annotations is session state (§3.4, §4.2а; FX1-UI-009).
    std::vector<std::string> shownSketches() const;  // effective visible sketches of the active element, history order
    bool sketchShown(const std::string& id) const;
    void setSketchShown(const std::string& id, bool shown);
    const std::set<std::string>& hiddenAnnotations() const { return hiddenAnnotations_; }
    void setAnnotationHidden(const std::string& id, bool hidden);
    // Base body of a recovered part (§4.2а, S4-REC-009): eye of its «Import» feature, closed by default.
    std::vector<std::string> shownBases() const;
    void setBaseShown(const std::string& id, bool shown);

signals:
    void editFeature(const QString& id);
    void repairRequested(const QString& id);  // «Repair references» (app-ux §4.5, §5.5в)
    void editSection(const QString& id);  // §4.2б
    void editAnnotation(const QString& id);  // §4.2в
    void sheetSelected(const QString& id);  // drawing sheet selected in the tree (§8б)
    void viewSelected(const QString& id);   // drawing view selected in the tree (§8б)
    void viewMenuRequested(const QString& id, const QPoint& globalPos);  // view context menu (§8б)
    void openInstance(const QString& instanceId);  // open the inserted object of an instance (§8а)
    void editInContext(const QString& instanceId); // edit the instance's part in assembly context (S2-ASM-011)
    void instanceProperties(const QString& instanceId);  // instance properties (S2-UX-001)
    void editMate(const QString& id);              // §8а (M16)
    void editWeld(const QString& id);              // §8а (S5-WELD-001)
    void editInstancePattern(const QString& id);   // §8а (FX7-ASM-007)
    void drawingPanelRequested();                  // double-click on a drawing: properties panel (§8б, FX1-UI-007)
    void addAssemblyRequested();
    void convertPartRequested(const QString& id, bool toSheet);  // «Convert to sheet / to regular» (S5-SHEET-006)
    void editSketch(const QString& id);
    void sketchOnPlane(const QString& planeId);
    void axisClicked(const QString& axisId);
    void undoableDone(const QString& text);  // notification with the «Undo» action (§4.4)
    void message(const QString& text, bool error);
    // Element selected by a click (release point, tree coordinates): context panel (§7.2).
    void selectionPicked(const QPoint& at);
    void planeHovered(const QString& planeId);  // empty: the cursor is not over a source plane (§3.4)
    void featureHovered(const QString& featureId);  // empty — no feature row under the cursor (FX7-PT-011)
    void pinnedPlanesChanged();                 // §4.2а
    void visibilityChanged();  // visibility of a sketch or annotation (§4.2а, FX1-UI-009)
    void baseVisibilityChanged();  // eye of the base body (S4-REC-009)

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    // Level-2 nodes of the active element top to bottom and their feature IDs: marker gaps (§4.3).
    std::vector<std::pair<QTreeWidgetItem*, std::string>> rows() const;
    int bottomOf(QTreeWidgetItem* it) const;  // bottom edge of the node including visible children
    int barY() const;
    bool hasRollbackBar() const;  // the active element has history features (FX9-TREE-002)
    // Element nodes beside the dragged one (FX9-TREE-001), the gaps between them and the move command.
    std::vector<QTreeWidgetItem*> elementSiblings() const;
    int elementGapY(const std::vector<QTreeWidgetItem*>& sib, int gap) const;
    int elementGapAt(const std::vector<QTreeWidgetItem*>& sib, int y) const;
    void sendElementMove(const std::vector<QTreeWidgetItem*>& sib, int gap);
    // Drop places of a dragged top-level or group member element node, or of a group node (FX10-TREE-001): the gaps of
    // the top level and inside groups (y), and «onto» a group node (its row: appended to the group).
    struct ElementSlot {
        int y = 0;
        std::string group, before;
        QRect onto;
    };
    QTreeWidgetItem* pressedNode() const;  // node of pressElement_ (an element or a group)
    bool pressNested() const;              // the pressed element is nested in another element's node (§4.1)
    std::vector<ElementSlot> elementSlots() const;
    static int elementSlotAt(const std::vector<ElementSlot>& places, int y);
    void sendElementPlace(const ElementSlot& slot);
    int gapIndexAt(int y) const;
    int gapY(int gap) const;
    void sendRollbackForGap(int gap);
    // The node's feature and its primary nested sketches (§4.3).
    std::vector<std::string> rowBlock(QTreeWidgetItem* row) const;
    void sendMoveForGap(int gap);
    // Top-level instance nodes of the active assembly and the gaps between them (instance reordering, FX9-ASM-011).
    std::vector<QTreeWidgetItem*> instanceRows() const;
    int instanceGapY(int gap) const;
    int instanceGapAt(int y) const;
    std::optional<std::pair<std::string, Status>> instanceDropAt(int gap) const;
    bool activateElementOf(QTreeWidgetItem* it);  // true: the node's element was inactive (the click is consumed)
    void openItem(QTreeWidgetItem* it);
    static QString expandKey(const QTreeWidgetItem* it);

    AppModel& model_;
    bool draggingBar_ = false;
    int dragY_ = 0;
    Availability available_;
    MenuHook menuHook_;
    bool commandsEnabled_ = true;
    std::string faceFeature_;  // S7F-TREE-002
    bool reorderEnabled_ = true;
    QTreeWidgetItem* pressRow_ = nullptr;  // press on a level-2 node: drag candidate
    QPoint pressPos_;
    bool draggingRow_ = false;
    std::string pressInstance_;  // press on a top-level instance node: reordering candidate (its ID)
    bool draggingInstance_ = false;
    std::string pressElement_;  // press on a document element node: reordering candidate (FX9-TREE-001)
    bool draggingElement_ = false;
    bool pressGroup_ = false;  // pressElement_ is a group (FX10-TREE-001)
    bool partConstruction_ = false;
    std::set<QString> expanded_, collapsed_;  // nodes explicitly expanded/collapsed by the user
    std::set<std::string> pinnedPlanes_;
    std::set<std::string> shownBases_;            // open eyes of base bodies (§4.2а)
    std::map<std::string, bool> sketchShown_;     // explicit sketch visibility (§3.4)
    std::set<std::string> hiddenAnnotations_;     // hidden annotations (§4.2а)
};

}  // namespace construe::app
