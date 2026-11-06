// Main window (spec/app-ux.md §2): modes, ribbon, tree, panels, 3D view, files.
#pragma once

#include "app/app_model.h"
#include "app/app_settings.h"
#include "app/app_settings_window.h"
#include "app/about_window.h"
#include "app/busy_indicator.h"
#include "app/help_window.h"
#include "app/fcstd_report.h"
#include "app/report_window.h"
#include "app/update_window.h"
#include "app/conflict_panel.h"
#include "app/context_bar.h"
#include "app/export_panel.h"
#include "app/import_panel.h"
#include "app/library_panel.h"
#include "app/solution_recorder.h"
#include "app/solution_wizard.h"
#include "app/mode_layout.h"
#include "app/command_availability.h"
#include "app/settings_panel.h"
#include "app/toast_host.h"
#include "app/feature_panel.h"
#include "app/feature_panel_sheet.h"
#include "app/properties_panel.h"
#include "app/section_panel.h"
#include "app/pmi_panel.h"
#include "app/pmi_overlay.h"
#include "app/instance_pattern_panel.h"
#include "app/mate_panel.h"
#include "app/mate_logic.h"
#include "app/weld_panel.h"
#include "app/repair_panel.h"
#include "app/feature_tree.h"
#include "app/sketch_editor.h"
#include "app/variables_panel.h"
#include "app/scene_builder.h"
#include "app/selection_bubble.h"
#include "app/viewport.h"
#include "app/drawing_view.h"
#include "drawing/specification.h"
#include "app/drawing_dims.h"
#include "app/drawing_marks.h"
#include "app/drawing_constructions.h"
#include "app/drawing_texts.h"

#include <QActionGroup>
#include <QComboBox>
#include <QListWidget>
#include <QMenu>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>

#include <functional>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QRadioButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>

#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>

class QFormLayout;
class QHBoxLayout;
class QVBoxLayout;

namespace construe::app {

// Mate reference from a view pick object (assembly.md §7.1; main_window_assembly.cpp).
std::string mateRefOf(const PickTarget& t, const doc::Element& el);

class PartTool;
class TitleBlockForm;
class MeasurePanel;  // FX10-MEAS-001

class MainWindow : public QMainWindow, public SelectionView {
    Q_OBJECT
public:
    explicit MainWindow(bool testMode = false, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Test mode and e2e (§12.1).
    bool isIdle() const;
    QString idleReport() const;
    // Test mode (§12.1, fast-forward of the UI timers, FX14-CI-002): `idle` is not reached only because of the UI timers of
    // condition 6 (preview, expression check, retessellation, context bar debounce) — nothing else goes on.
    bool onlyUiTimersPendingForTest() const;
    // Fires the UI timer of condition 6 due first at once (as if its time had passed); false — none is active.
    bool fastForwardUiTimerForTest();
    // Depth manipulator (§5.8): screen-space base and tip of the arrow, or nullopt.
    std::optional<std::pair<QPointF, QPointF>> manipulatorScreen() const;
    void resetForTest();  // test mode: new doc, Part mode, default view  // which idle conditions are unmet (e2e failure)
    // The geometry of the test window (FX14-QA-011, app-ux §12.1): set by the e2e harness once the shared window is shown
    // and fitted; resetForTest puts the window back to it (position, size, normal state) and the side panels to their
    // start widths, so a test that resized the window or narrowed a panel does not change the window of the next test.
    void setTestGeometryForTest(const QRect& geometry) { testGeometry_ = geometry; }
    QRect testGeometryForTest() const { return testGeometry_; }
    QList<int> sidePanelSizesForTest() const;  // widths of tree | view | variables | right panel (app-ux §2.2)
    // Size and position inside the available screen area (FX14-UI-002, app-ux §2.1): at start-up; a test passes its area.
    void placeOnScreen(const QRect& available);
    // Opens the document named on the command line, if any (app-ux §10.1; main.cpp, file association of the installer).
    void openFromCommandLine(const QStringList& arguments);
    // Opens a document the system asks for while the program runs (app-ux §10.1; macOS Finder — QFileOpenEvent): as a
    // recent file — the question about unsaved changes first; ignored while a modal mode is active.
    void openFromSystem(const QString& path);
    AppModel& model() { return *model_; }
    Viewport* viewport() const { return view_; }
    // Drawing (§8б): sheet window, current sheet, sheet properties row.
    DrawingView* drawingView() const { return drawingView_; }
    bool drawingActive() const;
    const std::string& currentSheet() const { return currentSheet_; }
    void selectSheet(const std::string& sheetId);
    const std::string& selectedView() const;  // selected drawing view (§8б); empty — none
    // Bill of materials (§8в): BOM sheet window; the current sheet is currentSheet() («page<N>»).
    DrawingView* specView() const { return specView_; }
    bool specificationActive() const;
    DimensionTool* dimensionTool() const { return dims_; }  // dimensions on the sheet (§8б, S3-DRW-006)
    SheetTextTool* sheetTextTool() const { return texts_; }  // texts on the sheet (§8б, FX7-DRW-006)
    SheetMarkTool* sheetMarkTool() const { return marks_; }  // sheet marks (§8б, FX9-DRW-008)
    ConstructionTool* constructionTool() const { return cons_; }  // constructions on the sheet (§8б, FX14-DRW-006)
    // Printing in the test mode (§8б, FX7-DRW-004): the print dialog is not shown, the pages go to this PDF file.
    void setTestPrintTarget(const QString& pdfPath) { testPrintTarget_ = pdfPath; }
    FeatureTree* tree() const { return tree_; }
    SketchEditor* sketchEditor() const { return sketch_.get(); }
    ExtrudePanel* extrudePanel() const { return extrude_; }
    RevolvePanel* revolvePanel() const { return revolve_; }
    CoilPanel* coilPanel() const { return coil_; }
    const std::string& hoveredSketch() const { return hoverSketch_; }  // sketch shown by hovering its tree row (FX7-PT-011)
    EdgePanel* filletPanel() const { return fillet_; }
    EdgePanel* chamferPanel() const { return chamfer_; }
    RefGeomPanel* refPlanePanel() const { return refPlane_; }
    PatternPanel* patternPanel() const { return pattern_; }
    PatternPanel* mirrorPanel() const { return mirror_; }
    ShellPanel* shellPanel() const { return shell_; }
    DraftPanel* draftPanel() const { return draft_; }
    SweepPanel* sweepPanel() const { return sweep_; }
    LoftPanel* loftPanel() const { return loft_; }
    HolePanel* holePanel() const { return hole_; }
    ThreadPanel* threadPanel() const { return thread_; }
    GearPanel* gearPanel() const { return gear_; }
    RefGeomPanel* refAxisPanel() const { return refAxis_; }
    // Sheet metal panels (§5.3к, S5-SHEET-*) and the flat pattern display state (§7.4, S5-FLAT-001).
    SheetMetalPanel* sheetMetalPanel() const { return sheetMetal_; }
    SheetWallPanel* sheetWallPanel() const { return sheetWall_; }
    SheetFlangePanel* sheetFlangePanel() const { return sheetFlange_; }
    SheetBendPanel* sheetBendPanel() const { return sheetBend_; }
    SheetCutPanel* sheetCutPanel() const { return sheetCut_; }
    bool unfolded() const { return unfolded_; }
    FeaturePanel* activePanel() const { return panel_; }
    VariablesPanel* variablesPanel() const { return variables_; }
    QToolButton* ribbonButton(const QString& name) const;
    // Tool variant in a ribbon group-button menu (Rectangle ▾ etc., S1-SK-020): action tool_<name>.
    QAction* toolVariant(const QString& name) const;
    QToolButton* confirmOkButton() const { return confirmOk_; }
    ExportPanel* exportPanel() const { return exportPanel_; }
    ImportPanel* importPanel() const { return importPanel_; }  // S4-REC-018
    LibraryPanel* libraryPanel() const { return libraryPanel_; }              // S6-LIB-003 (§8г)
    LibrarySavePanel* librarySavePanel() const { return librarySavePanel_; }  // S6-LIB-006
    SolutionWizard* solutionWizard() const { return solutionWizard_; }        // S6-SOL-002 (§8д)
    SolutionRecorder* solutionRecorder() const { return solutionRecorder_; }  // S6-SOL-009 (§8е)
    SettingsPanel* settingsPanel() const { return settingsPanel_; }
    // Application settings (§10.6, S3-SET-001): values and the modeless window.
    AppSettings& appSettings() { return *appSettings_; }
    AppSettingsWindow* appSettingsWindow() const { return appSettingsWindow_; }
    void showAppSettings(int tab = -1);
    ConflictPanel* conflictPanel() const { return conflictPanel_; }
    Mode mode() const { return mode_; }
    QString dofText() const;
    QStringList toasts() const;
    QStringList toastHistory() const;  // every notification since the last reset (test mode)
    bool testMode() const { return testMode_; }
    // Application data directory (§10.5); in test mode set by the test; empty — recent files and autosave are off.
    void setDataDirForTest(const QString& dir) { dataDir_ = dir; }
    // On-disk cache directory of evaluators (§10.5); empty — disabled. Outside tests it is set when the window starts.
    void setRegenCacheDirForTest(const QString& dir) { model_->session().setRegenCacheDir(dir.toStdString()); }
    void autosaveNow();     // autosave timer step (§10.5)
    void checkRecovery();   // look for another process's copy and notify (§10.5)
    void checkCrashReport();  // a crash marker in the data directory — the crash report window (§10.8)
    ReportWindow* reportWindow() const { return reportWindow_; }
    AboutWindow* aboutWindow() const { return aboutWindow_; }
    HelpWindow* helpWindow() const { return helpWindow_; }
    // Long work indicator (§9.4, FX8-UI-001): the stripe and how many times it has appeared.
    QWidget* busyBar() const { return busy_->bar(); }
    int busyShownCount() const { return busy_->shownCount(); }
    int busyIdlePolls() const { return busy_->idlePolls(); }  // polls that saw no work (§9.4, tests)
    // Ribbon commands for the help (§2.1б, FX7-UI-007): every ribbon button and the items of its menu, in ribbon order.
    std::vector<HelpCommand> helpCommands() const;
    FcstdReportWindow* fcstdReportWindow() const { return fcstdReport_; }  // exchange report (§10.9, S4A-REP-001)
    // Updating from the program (§10.10, update.md, REL-CT-002): the automatic check (when due) and the manual one.
    void autoCheckUpdates();
    void cmdCheckUpdates();
    UpdateWindow* updateWindow() const { return updateWindow_; }
    // Test mode: the installer was launched and the program would have exited (update.md §5).
    bool updateExitRequested() const { return updateExitRequested_; }

    // UI commands (used by buttons, keys, menus).
    void cmdNew();
    void cmdOpen();
    bool cmdSave();
    bool cmdSaveAs();
    void cmdUndo();
    // Undo/redo captions (menu, icons) from the history (§7.4); availability — updateAvailability.
    void updateUndoState();
    // Semantic command availability (app-ux §7.5, FX4-UI-002): document, mode and selection state, and applying the
    // rule table to all ribbon buttons, view-corner buttons and menu items.
    AvailabilityState availabilityState() const;
    void updateAvailability();
    bool commandEnabled(const QString& objectName) const { return commandAvailable(objectName.toStdString(), availabilityState()); }
    void cmdRedo();
    void cmdSketch();
    void cmdExtrude();
    void cmdRevolve();
    void cmdEdgeFeature(bool fillet);
    // A click in the view while a feature panel is open (app-ux §5.5д, S7F-RF-011): the base and the assembly hole first,
    // everything else — the panel's pick(); a refusal puts its needed() into the status line.
    void pickedInFeatureEdit(const PickTarget& t);
    // A click on a tree node while a feature panel is open (§5.5д): false — the panel does not take it.
    bool pickedInTreeFeatureEdit(const QString& kind, const std::string& id);
    // The object of a tree node (§5.5д): plane, axis, origin, a reference plane or axis feature; featureAsPlane — any
    // other feature (a sketch) stands for its plane (mates, instance pattern, sketch base set).
    std::optional<PickTarget> treeTarget(const QString& kind, const std::string& id, bool featureAsPlane) const;
    // The only writer of the status line hint (S7F-RF-011, S7F-UI-006).
    void setPrompt(const QString& t);
    // Click on a base in the feature panel (app-ux §3.6а, FX1-FEAT-001): true — click handled (sketch created or hint).
    bool startBaseSketch(const PickTarget& t);
    // A base selected when a command is pressed (§3.6б, S7F-FEAT-002): a plane or a face in the view, a plane in the tree.
    std::optional<PickTarget> selectedBase() const;
    // A click on an instance face in the assembly hole panel (§8а, FX8-ASM-007): a point of the hole there.
    bool holePointClick(const PickTarget& t);
    void faceClickedInProperties(const PickTarget& t);  // §5.10
    void setDisplayStyle(DisplayStyle s);               // §3.4а
    PropertiesPanel* propertiesPanel() const { return propertiesPanel_; }
    void cmdProperties();
    // «Convert to sheet metal / regular» (app-ux §5.3к, S5-SHEET-006): message; the place of an invalid form is selected.
    void cmdConvertPart(const std::string& id, bool toSheet);
    // Sections (§5.11, S2-VIEW-002).
    void cmdSection();
    void cmdMeasure();  // the «Measure» panel on / off (app-ux §3.8, FX10-MEAS-001)
    // Assemblies (§8а, assembly.md).
    bool assemblyActive() const;
    void editPartInContext(const std::string& partId);        // S2-ASM-011
    void exitPartContext();                                   // S7F-ASM-005: back to the context assembly
    const std::string& contextPart() const { return contextPart_; }
    void editInstanceInContext(const std::string& instanceId);
    PickTarget assemblyPick(PickTarget t) const;
    bool instanceDragBegin(const std::string& instance, const QPoint& press = {});
    void instanceDragUpdate(const QPoint& delta, bool rotate);
    void instanceDragEnd();
    // Free-degree manipulator (FX8-ASM-003, §8а): the selected underdefined instance and the screen point of its icon.
    std::optional<std::string> freeManipInstance() const;
    std::string matedInstance() const;
    // A click in the view (FX9-ASM-010): a second click on a part of the already selected subassembly chooses the
    // manipulator of that part (`before` — the selection before the click).
    void noteInstanceClick(const std::set<PickTarget>& before, const PickTarget& t, bool add);
    bool movableInstance(const std::string& id) const;
    std::optional<QPointF> freeManipulatorScreen() const;
    bool freeManipBegin(const QPoint& press);
    void freeManipUpdate(const QPoint& pos);
    void freeManipEnd();
    void freeManipCancel();  // a click without a drag: the pull is dropped, the placements stay (FX9-ASM-003)
    // Group of instances (FX13-ASM-002, §8а): the top-level instances the group manipulator moves (two or more, empty —
    // none) and the selection box over the assembly view (`window` — drawn left to right; `add` — Ctrl/Shift).
    std::vector<std::string> freeManipGroup() const;
    void boxSelectInstances(const QRectF& box, bool window, bool add, const QPoint& release);
    std::optional<QRectF> selectionBox() const;  // the box being drawn in the view (tests, FX13-ASM-002)
    void openInstanceFromAssembly(const std::string& instanceId);
    // Mates (§8а, M16).
    void cmdMate();
    void editMate(const std::string& id);
    void acceptMate(bool next = false);  // next — «OK / Next» (FX8-ASM-002)
    void pickedInMate(const PickTarget& t);
    // Mate preview (FX8-ASM-001): temporary placements of the top-level instances while the panel is open.
    const std::map<std::string, regen::Motion3>& matePreview() const { return matePreview_; }
    MatePanel* matePanel() const { return mate_; }
    // Mate series of the inserted instance (FX1-ASM-002): empty — no series.
    const std::string& mateSeries() const { return mateSeries_; }
    // Weld seams (§8а, S5-WELD-001…002).
    void cmdWeld();
    void editWeld(const std::string& id);
    void acceptWeld();
    void pickedInWeld(const PickTarget& t);
    WeldPanel* weldPanel() const { return weld_; }
    // Repair wizard of lost references (§5.5в, S7F-FEAT-003): from the feature (empty — the first one in history).
    void startRepair(const std::string& feature = {});
    // Dimensions of the sheet with lost model references — deleted with a message (§5.5в).
    void deleteLostDimensions(const std::string& sheet);
    void pickedInRepair(const PickTarget& t);
    RepairPanel* repairPanel() const { return repair_; }
    // Lost references of the last applied result (core-api lostReferences), history order.
    const std::vector<api::LostReference>& lostReferences() const { return lostAll_; }
    // Instance patterns (§8а, FX7-ASM-007) and the exploded view (§8а, FX7-ASM-005).
    void cmdInstancePattern();
    void editInstancePattern(const std::string& id);
    void acceptInstancePattern();
    void pickedInInstancePattern(const PickTarget& t);
    InstancePatternPanel* instancePatternPanel() const { return instancePattern_; }
    void cmdExploded();
    void cmdExplodeReset();
    // Exploded view transition (FX8-ASM-006, §8а): duration (0 in test mode) and the frames shown so far (for tests).
    void setExplodeAnimationMs(int ms) { explodeAnimMs_ = ms; }
    // Clock of the transition, ms (tests; empty — the wall clock): e2e advances it by a fixed step per reading, so the
    // number of intermediate frames does not depend on how busy the machine is.
    void setExplodeClockForTest(std::function<qint64()> clock) { explodeClock_ = std::move(clock); }
    const std::vector<std::map<std::string, regen::Motion3>>& explodeFrames() const { return explodeFrames_; }
    bool explodeAnimating() const { return explodePending_ || !explodeShown_.empty(); }
    // Assembly analysis (§8а, M17).
    void cmdInterference();
    QListWidget* interferenceList() const { return interferenceList_; }
    // Objects from other files (§8а, assembly.md §5а).
    void insertFromFile();
    // Copying objects between projects, «Make local copy», «Replace with…» (§8а, assembly.md §5б; FX7-ASM-008…010;
    // main_window_xref.cpp).
    void copyElementToClipboard(const std::string& elementId);
    void pasteElementFromClipboard();
    void localizeInstance(const std::string& instanceId);
    void localizeView(const std::string& viewId);
    void chooseReplacement(const std::string& subjectId, bool view);  // object menu at the cursor
    void extendTreeMenu(QMenu& menu, const QString& kind, const std::string& id);
    // Library of standard parts (app-ux §8г, main_window_library.cpp; S6-LIB-002…006).
    void openLibrary();        // the catalog panel (Mode::Library)
    void acceptLibrary();      // insertion of the chosen element and size
    void acceptSeat();         // insertion by the attachments of the chosen element (§8г, FX10-LIB-003)
    void openLibraryReplace(const std::string& instance);  // the library panel replacing an instance (FX10-LIB-004)
    void acceptLibraryReplace();
    void pickedInLibrary(const PickTarget& t);  // journal / shoulder for the seating
    void syncLibrarySelection();                // the chosen seating faces are highlighted in the view
    void openLibrarySave();    // «Save as library item» (Mode::LibrarySave)
    void acceptLibrarySave();
    void cmdUpdateFromLibrary();
    void applyLibraryDirectories();  // library folders of the settings → session (library.md §4)
    QString userLibraryDirectory() const;
    // Connection wizard (app-ux §8д, main_window_solutions.cpp; S6-SOL-002…008).
    void cmdSolution();                                      // a new connection (Mode::Solution)
    void openSolutionWizard(const std::string& feature);     // editing a connection
    void acceptSolution();
    void openSolutionRecorder();  // Mode::SolutionRecord (§8е)
    void acceptSolutionRecord();
    void syncSolutionSelection();                            // the chosen input faces are highlighted in the view
    // What the program waits to be picked in the current mode and panel (spec/app-ux.md §2.7, S7F-UI-006): the status
    // line shows it on entering the mode; empty — the mode waits for no pick.
    QString waitHint() const;
    void pickedInSolution(const PickTarget& t, const std::optional<QVector3D>& at);
    void solutionSameHoles();  // «All identical holes of the part» of the wizard (S7F-ASM-008)
    void checkLinks();  // notification «Update links from: …»
    static MainWindow* windowFor(const QString& path);  // window with file path open (or nullptr)
    void openElementInThisWindow(const std::string& elementId);
    void editSection(const std::string& id);
    void acceptSection();
    void pickedInSection(const PickTarget& t);
    SectionPanel* sectionPanel() const { return section_; }
    MeasurePanel* measurePanel() const { return measure_; }  // FX10-MEAS-001; nullptr — not opened yet
    std::optional<Plane> sectionManipulatorPlane() const;  // section panel plane for the offset manipulator
    // Technical requirements (§5.12, §3.4, S3-PMI-001…003; main_window_pmi.cpp).
    void cmdAnnotation(const std::string& type);
    void cmdRestRoughness();  // roughness of the remaining surfaces (app-ux §5.12, FX5-PMI-001)
    void editAnnotation(const std::string& id);
    // Dragging an annotation in the 3D view (FX12-PMI-002): byAnchor — the anchor slides on its face (the label keeps its
    // distance along the face normal), otherwise the label moves (the anchor — the projection onto the face). Commit with the
    // grabbed point (label or anchor) at view point at: one undo step; false — not committed (no face under the cursor, another face).
    bool annotationDragEnd(const std::string& id, bool byAnchor, const QPoint& at);
    void acceptAnnotation();
    // at — click point on the face (world, from depth buffer): anchor point of the annotation (§5.12, FX1-FEAT-005).
    void pickedInAnnotation(const PickTarget& t, const std::optional<QVector3D>& at = std::nullopt);
    AnnotationPanel* annotationPanel() const { return annotation_; }
    AnnotationOverlay& annotationOverlay() { return overlay_; }
    // Annotations in the view (§3.4): whether shown in the current mode; the one selected in the tree is highlighted.
    bool annotationsShown() const;
    std::string selectedAnnotation() const;
    void cmdExitSketch();
    void cmdCancelSketch();  // ✗ «Cancel» in a sketch (app-ux §6.1, S7F-SK-004)
    void sketchEscapeBottom();  // Esc at the bottom of the sketch ladder (app-ux §6.13 item 6, FX10-SK-001)
    void cmdExportStl();
    void acceptFlatExport();
    void cmdExportStep();  // S2-IO-004
    void cmdImportStep();
    void cmdImportMesh();  // S4-MESH-001, app-ux §10.3
    void cmdImportFcstd();                  // S4A-UI-001, app-ux §10.1
    void cmdExportFcstd(bool activeOnly);   // S4A-UI-001: the whole document or the active object
    void openFcstdPath(const QString& path);
    Result<api::ReconstructReport> runReconstruct(const std::string& body, const api::ReconstructOptions& options);  // FX7-UI-006
    void cmdReconstruct();  // S4-REC-002, app-ux §8а
    void beginImport(const QString& path, bool mesh);  // import panel after a file is chosen (S4-REC-018, app-ux §10.7)
    void acceptImport();
    bool exportThreeMf_ = false;  // export panel was opened by the «3MF…» item (§10.3)
    bool exportFlat_ = false;     // export panel was opened by the «Flat pattern DXF…» item (§10.3, S5-DXF-001)
    // Normal to (§3.3): in a sketch with no selection — to the sketch plane; otherwise to the selected plane.
    void cmdNormalTo();
    void standardViewFit(StandardView v);  // standard view and fit: Ctrl+1…7, "View" menu, "Isometric ▾" (§3.3, FX2-UI-001)
    void editSketch(const std::string& id);
    void editFeature(const std::string& id);
    void createSketchOn(const std::string& planeId);
    // Sketch on a planar body face (§6.1, FX1-SKI-003): non-planar — hint, false.
    bool createSketchOnFace(const PickTarget& face);
    void escape();
    // Runs an action of the "mode → action" table (app-ux §2.4, S7F-RF-008): the corner, command panel headers,
    // Ctrl+Enter and Esc all go through it.
    void runConfirm(ConfirmAction a);
    void confirmAccept() { runConfirm(modeConfirm(mode_).accept); }
    void confirmReject() { runConfirm(modeConfirm(mode_).cancel); }
    // Unified selection (selection.md §5, FX7-SEL-001; main_window_selection.cpp): the set of the sketch base, the
    // active field with its hint «Yes» / «Other» / «Reset», keys, candidate preview, pickable sketch elements.
    SelectionBubble* selectionBubble();
    SelectionField& sketchSelection();
    const SelectionField* activeSelection() const { return selField_; }
    void setActiveSelection(SelectionField* f);
    void updateSelectionUi();
    const api::SelectionPreview* selectionPreview(bool* pending = nullptr) const;
    void selectionYes();
    void selectionNext(int step);
    void selectionReset();
    bool selectionEscape();             // Esc ladder step: true — a set or a candidate was reset
    bool selectionKey(QKeyEvent* k);    // Enter / Tab / Shift+Tab in the view; true — handled
    void pickedInPickPlane(const PickTarget& t, bool ctrl);
    void noteViewClick(const QPoint& at);
    bool sketchPicksActive() const;
    void updateSketchPicks();
    // Notification (§9.1): action buttons close the notification and perform the action.
    using ToastAction = app::ToastAction;
    void showToast(const QString& text, bool error = false, std::vector<ToastAction> actions = {});
    ToastState toastState() const;  // for the conditions of messages (S7F-UI-003)
    // Context panel (§7.2): selection in the view changed by a click (release point, view coordinates).
    void viewSelectionPicked(const QPoint& atInView);
    ContextBar* contextBar() const { return ctx_; }

protected:
    void closeEvent(QCloseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void buildUi();
    void buildMenus();
    void buildRibbon();
    void arrangePartRibbon(bool sheet);
    void setMode(Mode m);
    void onDocumentChanged();
    void onRegenerated();
    void logRegenerationMessages();  // new rebuild errors and warnings → the session log (FX14-UI-003)
    std::set<QString> loggedRegenMessages_;
    void rebuildScene();
    // Scene description for SceneBuilder according to the window's current mode.
    SceneRequest sceneRequest() const;
    // Regions of visible sketches and of the feature panel's sketch (§3.4, §5.5); view selection = panel profiles.
    void updateRegions();
    void updateTitle();
    void updateStatus();
    // Tool buttons of the «Sketch» tab and their checked state (§2.3, FX1-UI-003).
    static const std::vector<QString>& sketchToolButtonNames();
    void syncSketchToolButtons();
    void updateConflictPanel();
    void dofClicked();
    void placeOverlays();
    bool confirmDiscard();
    void requestPreview();
    void startFeature(FeaturePanel* panel);
    void acceptFeature();
    void cancelFeature();
    void acceptExport();
    // Document settings (app-ux §10.4): length and angle units.
    void cmdDocumentSettings();
    void acceptSettings();
    void updateUnitsStatus();
    void applySetting(const QString& key);  // §10.6: apply an application setting
    // Interface language (§9.5, S8-I18N-001): by the «ui/language» setting; switching retranslates the open windows.
    QString settingLanguage() const;
    void applyLanguage();
    void syncModeWithContext();
    enum class PickSource { View, Tree };
    void scheduleContextBar(const QPoint& atInWindow, PickSource source);
    ContextSelection contextSelection() const;
    void runContextCommand(const QString& command);

    bool testMode_;
    QString dataDir_;
    QTimer* autosave_ = nullptr;
    uint64_t autosavedRevision_ = 0;
    QMenu* recentMenu_ = nullptr;
    void rebuildRecentMenu();
    void openPath(const QString& path);
    void noteFileUsed(const QString& path);  // recent files + removal of the autosave copy
    void dropRecovery();
    uint64_t partialTessWarnedRevision_ = 0;  // §9.1: warning about non-triangulated faces — once per revision
    std::unique_ptr<AppModel> model_;
    Viewport* view_ = nullptr;
    FeatureTree* tree_ = nullptr;
    ExtrudePanel* extrude_ = nullptr;
    RevolvePanel* revolve_ = nullptr;
    CoilPanel* coil_ = nullptr;  // §5.3м, FX7-PT-004
    EdgePanel* fillet_ = nullptr;
    EdgePanel* chamfer_ = nullptr;
    RefGeomPanel* refPlane_ = nullptr;
    RefGeomPanel* refAxis_ = nullptr;
    PatternPanel* pattern_ = nullptr;
    PatternPanel* mirror_ = nullptr;
    ShellPanel* shell_ = nullptr;
    DraftPanel* draft_ = nullptr;
    SweepPanel* sweep_ = nullptr;
    LoftPanel* loft_ = nullptr;
    HolePanel* hole_ = nullptr;
    ThreadPanel* thread_ = nullptr;
    GearPanel* gear_ = nullptr;
    SheetMetalPanel* sheetMetal_ = nullptr;
    SheetWallPanel* sheetWall_ = nullptr;
    SheetFlangePanel* sheetFlange_ = nullptr;
    SheetBendPanel* sheetBend_ = nullptr;
    SheetCutPanel* sheetCut_ = nullptr;
    bool unfolded_ = false;  // «Unfold»: the part is shown as its flat pattern (§7.4, S5-FLAT-001)
    PropertiesPanel* propertiesPanel_ = nullptr;
    SectionPanel* section_ = nullptr;
    MeasurePanel* measure_ = nullptr;  // FX10-MEAS-001
    AnnotationPanel* annotation_ = nullptr;
    AnnotationOverlay overlay_;
    void refreshAnnotations();  // annotations(final) cache for display (§3.4)
    std::string dragInstance_;            // instance being dragged (§8а)
    regen::Motion3 dragStart_, dragMotion_;
    std::map<std::string, regen::Motion3> dragSolved_;  // solved positions while dragging (M16)
    std::string manipNested_;  // the nested part chosen for the manipulator by a second click (FX9-ASM-010)
    std::string pullInstance_;  // free-degree manipulator drag (FX8-ASM-003): instance, grab point (instance frame), its world start
    Vec3 pullGrab_ = Vec3::Zero(), pullStart_ = Vec3::Zero();
    QPoint pullPress_;
    // What the manipulator press grabbed (S7F-ASM-003): the centre — a free pull; an arrow — a pull along it; the ring — a
    // turn about the view axis through the centre.
    enum class PullGrip { Free, Arrow, Ring };
    PullGrip pullGrip_ = PullGrip::Free;
    QPointF pullAxis_;          // the arrow's screen direction (unit)
    regen::Motion3 pullWorld0_;  // the instance's world motion at the press
    ScreenTurn pullTurn_;         // the ring turn accumulated over the drag (FX12-ASM-001)
    double pullTurnApplied_ = 0;  // the part of it already applied to the previewed placement
    std::map<std::string, regen::Motion3> pullGroup0_;  // group manipulator drag (FX13-ASM-002): placements at the press
    Vec3 pullGroupCentre_ = Vec3::Zero();               // the group centre at the press (world)
    bool groupManipBegin(const QPoint& press);
    void groupManipUpdate(const QPoint& pos);
    // Placements committed by the last drag (S7F-ASM-001): shown until the regeneration of that revision is applied, so
    // the view never falls back to the old placements between the release and the new result.
    std::string contextPart_, contextAssembly_;  // the part open «in context» and its assembly (S7F-ASM-005)
    std::map<std::string, regen::Motion3> committedMotions_;
    uint64_t committedRevision_ = 0;
    MatePanel* mate_ = nullptr;
    std::map<std::string, regen::Motion3> matePreview_;  // FX8-ASM-001
    void mateChanged();
    void commitInstancePlacements(const std::map<std::string, regen::Motion3>& moved);  // one moveInstances (§8а)
    // Exploded view transition (FX8-ASM-006): positions before the command, the running animation.
    std::map<std::string, regen::Motion3> explodeFrom_, explodeTo_, explodeShown_;
    bool explodePending_ = false;
    int explodeAnimMs_ = 400;
    QTimer* explodeTimer_ = nullptr;
    qint64 explodeStartMs_ = 0;
    std::function<qint64()> explodeClock_;
    qint64 explodeNowMs() const;
    std::vector<std::map<std::string, regen::Motion3>> explodeFrames_;
    void beginExplodeTransition();
    void explodeTick();
    void startExplodeAnimation();
    // Mates (§8а, FX1-ASM-001…002): view pick objects by reference (highlight), series after insertion.
    std::map<std::string, PickTarget> mateTargets_;
    std::string mateSeries_;
    QString mateRefLabel(const std::string& ref) const;
    std::optional<PickTarget> pickTargetOfRef(const std::string& ref) const;
    void syncMateSelection();
    void startMateSeries(const std::string& instance);
    void checkMateSeries();
    void leftMate();
    InstancePatternPanel* instancePattern_ = nullptr;
    std::map<std::string, PickTarget> instancePatternTargets_;  // picked pattern references (highlight)
    QString instancePatternLabel(const std::string& ref) const;
    std::vector<std::pair<std::string, QString>> partPatternFeatures(const std::string& instancePath) const;
    void syncInstancePatternSelection();
    void leftInstancePattern();
    void syncExplodedButton();
    WeldPanel* weld_ = nullptr;
    RepairPanel* repair_ = nullptr;
    std::vector<api::LostReference> lostAll_;                 // §5.5в: lost references of the applied result
    std::set<std::pair<std::string, std::string>> lostSeen_;  // (element + "\n" + owner, reference): last result of each element
    std::map<std::string, size_t> repairUndoPoints_;          // element → undo depth before its references were lost
    std::set<std::string> lostDimsDeleted_;                   // "<sheet>/<dimension>" deleted as lost once (§5.5в)
    std::set<std::string> repairSkipped_;
    std::map<std::string, PickTarget> repairTargets_;         // picked replacements (highlight)
    std::set<std::string> updateLostReferences();             // lost references → owners to keep silent about
    void repairNext();
    void repairApply();
    void repairSkip();
    void repairDone();
    void repairRevert();
    void syncRepairSelection();
    std::vector<DrawBody> repairGhosts() const;
    std::map<std::string, PickTarget> weldTargets_;  // weld faces and edges picked in the view (highlight)
    std::vector<DrawWeld> faceWeldLines_;            // lines of welds on faces from the last scene (FX8-WLD-001)
    void syncWeldSelection();
    void leftWeld();
    void updateWeldLines();  // weld lines of the active assembly in the view (§8а, S5-WELD-002)
    QWidget* interferencePanel_ = nullptr;
    QListWidget* interferenceList_ = nullptr;
    std::vector<api::Session::Interference> interferences_;
    std::set<std::string> highlight_;
    Point3 dragCenter_ = Point3::Zero();
    QMenu* insertMenu_ = nullptr;
    QStringList notifiedLinks_;  // file set of the last change notification
    void updateElementKindUi();
    void rebuildInsertMenu();
    // Drawing (§8б, main_window_drawing.cpp).
    QStackedWidget* center_ = nullptr;
    QWidget* drawingPage_ = nullptr;
    DrawingView* drawingView_ = nullptr;
    QComboBox* sheetFormat_ = nullptr;
    QComboBox* sheetOrientation_ = nullptr;
    QComboBox* sheetScale_ = nullptr;
    QComboBox* drawingStandard_ = nullptr;    // drawing standard (§8б, S3-DRW-008)
    QComboBox* drawingProjection_ = nullptr;  // ISO drawing projection method
    void applyDrawingField(const char* field, const QString& value);  // updateDrawing; empty — null
    std::string currentSheet_;
    QWidget* buildDrawingPage();
    void refreshDrawing();
    void applySheetField(const char* field, const QString& value);
    // Title block (§8б, S3-DRW-002) and sheet export (S3-IO-001, S3-IO-002).
    QWidget* titleBlockPanel_ = nullptr;
    QLabel* titleBlockHeader_ = nullptr;
    QLabel* drawingPanelTitle_ = nullptr;   // «Drawing and sheet» / «View properties» (S7F-DRW-004)
    TitleBlockForm* titleBlockForm_ = nullptr;  // title block fields, hidden with the view settings (S7F-DRW-004)
    // One panel at a time (FX12-DRW-001): the right panel shows the drawing, or the selected view, dimension, text or
    // sheet mark, or the annotation panel of a sheet mark being placed or edited — never two of them.
    enum class DrawingPanelContent { Drawing, View, Dimension, Text, Mark, MarkEdit, Construction };
    DrawingPanelContent drawingPanelContent() const;
    void updateDrawingPanels();          // visibility of the right panel and of its blocks by the content
    void hostSheetAnnotation(bool on);   // the annotation panel moves into the right panel of the drawing and back
    QWidget* drawingPanelHead_ = nullptr;  // the panel header with the close cross
    QWidget* markBar_ = nullptr;           // the selected sheet mark (FX12-DRW-001)
    QLabel* markInfo_ = nullptr;
    QPushButton* markEditBtn_ = nullptr;
    QPushButton* markDeleteBtn_ = nullptr;
    QVBoxLayout* sheetAnnotationSlot_ = nullptr;  // where the annotation panel is hosted on a drawing
    void refreshMarkBar();
    QWidget* buildTitleBlockPanel();
    void refreshTitleBlockPanel();
    void applyTitleBlockField(const std::string& key, const QString& text);
    void cmdExportSheet(io::SheetFileFormat format);
    void updateExportActions();
    // Views (§8б): placement modes, selected view, view properties row, context menu.
    // Section and detail view (§8б, S3-DRW-005): parent choice, two points (line, or center and radius), location.
    enum class DrawingTool {
        None, PlaceBase, PickParent, PlaceProjected,
        PickSectionParent, SectionStart, SectionEnd, PlaceSection,
        PickDetailParent, DetailCenter, DetailRadius, PlaceDetail,
        PlaceThree,  // location of the main view of three projections (FX1-DRW-001)
        PickMarkFace,  // the line of a face for a mark set in the drawing (FX9-DRW-009, FX11-DRW-001)
        PickBalloonFace  // the line of a part for an item number (FX14-DRW-002)
    };
    DrawingTool drawingTool_ = DrawingTool::None;
    std::string placeModel_;   // PlaceBase: model element
    std::string placeFile_;    // PlaceBase, PlaceThree: model file relative to the doc (empty — this doc, FX3-DRW-005)
    // "From file…" in "View ▾" / "3 projections ▾" (FX3-DRW-005): file and object — then placement mode tool.
    void placeViewFromFile(DrawingTool tool);
    std::string placeParent_;  // PlaceProjected, section, detail view: parent view
    Point2 toolPt1_{0, 0}, toolPt2_{0, 0};  // section: line ends; detail: center and radius point (sheet mm)
    void cmdSectionView(bool detail);
    void sectionToolClick(const Point2& at);
    // Snapping of the section line to parent elements (FX9-DRW-002, app-ux §8б): the element under the cursor is
    // highlighted before the click; the click takes the same element.
    struct SectionSnap {
        std::string kind;  // vertex | center | mid | edge | axis
        std::string name;
        Point2 at{0, 0};   // sheet point (axis — the cursor projected onto it)
        Point2 a{0, 0}, b{0, 0};  // edge, axis: the segment on the sheet
        double param = 0.5;       // edge: share from a
    };
    std::optional<SectionSnap> sectionSnapAt(const Point2& at);
public:
    // Kind of the parent element highlighted while entering a section line (FX9-DRW-002); empty — none. For tests.
    std::string sectionHoverKind() const { return sectionHover_ ? sectionHover_->kind : std::string(); }
private:
    std::optional<SectionSnap> sectionHover_, sectionSnap1_, sectionSnap2_;
    std::string sectionEntsView_;  // view of the cached entities
    std::vector<drawing::ViewEntity> sectionEnts_;
    void updateToolOverlay(const Point2& cursor);
    QMenu* baseViewMenu_ = nullptr;
    QMenu* threeViewsMenu_ = nullptr;  // «3 projections ▾» (FX1-DRW-001)
    void cmdModelDimensions();
    // A mark set in the drawing (FX9-DRW-009, FX11-DRW-001, app-ux §8б): the ribbon button — a click on the line of a face
    // on a view — the annotation panel; accepted — addMark. editSheetMark — the panel of the annotation mark under a sheet
    // point (double click) with «Set in drawing»; false — no such mark.
    void cmdDrawingMark(const std::string& type);
    void startDrawingMark(const std::string& view, const std::string& face);
    bool editSheetMark(const Point2& at);
    bool editSheetMarkId(const std::string& id);  // the annotation panel of the sheet mark (in the right panel, FX12-DRW-001)
    bool editSelectedSheetMark();                 // «Edit» of the mark block
    void cmdDrawingRestRoughness();               // the rest roughness set in the drawing (FX12-DRW-007)
    // Projected views depending on a view (recursively, FX12-DRW-003): they move with it.
    std::vector<std::string> projectedDescendants(const doc::Sheet& sh, const std::string& viewId) const;
    bool drawingRest_ = false;                    // the annotation panel edits it
    std::string markType_;  // roughness | geometric | datum of the ribbon button
    std::string markView_;  // the panel serves a sheet mark: its view; empty — a part annotation
    std::string markEdit_;  // the edited sheet mark; empty — a new one
    bool markLocal_ = false;  // the edited mark was set in the drawing
    std::vector<std::string> drawingDatumLetters(const std::string& view) const;
    void cmdModelMarks();  // «Place all additional information» (FX9-DRW-008)         // «Insert all model dimensions» (FX1-DRW-003, FX9-DRW-005)
    QWidget* viewBar_ = nullptr;
    // Title block and drawing properties panel + "Title block" button (§8б); drawingSettings — the drawing settings
    // (the selected view is released: the panel shows either the drawing or the view, S7F-DRW-004).
    void showDrawingPanel(bool on, bool drawingSettings = false);
    QWidget* drawingProps_ = nullptr;  // "Drawing and sheet" block of the title block panel (FX1-UI-007)
    QComboBox* viewOrientation_ = nullptr;
    QComboBox* viewScale_ = nullptr;
    void rebuildBaseViewMenu();
    void cmdProjectedView();
    void setDrawingTool(DrawingTool t);
    void sheetClicked(const Point2& at);
    void selectView(const std::string& viewId);
    void viewDragged(const std::string& viewId, const Point2& delta);
    void showViewMenu(const std::string& viewId, const QPoint& globalPos);
    void applyViewField(const std::string& viewId, const api::Json& def);
    // Dimensions (§8б, S3-DRW-006, drawing_dims.cpp).
    DimensionTool* dims_ = nullptr;
    // Texts (§8б, FX7-DRW-006, drawing_texts.cpp) and the tool group over the sheet window.
    SheetTextTool* texts_ = nullptr;
    SheetMarkTool* marks_ = nullptr;  // sheet marks (§8б, FX9-DRW-008)
    ConstructionTool* cons_ = nullptr;  // constructions (§8б, FX14-DRW-006)
    std::unique_ptr<DrawingView::ToolGroup> sheetTools_;
    // View properties (§8б, FX7-DRW-007, FX7-DRW-008): model, projection plane, display, sheet metal state.
    QComboBox* viewSource_ = nullptr;
    QComboBox* viewPlane_ = nullptr;
    QComboBox* viewDisplay_ = nullptr;
    QComboBox* viewFlat_ = nullptr;
    QComboBox* viewExploded_ = nullptr;  // FX7-ASM-005
    QWidget* viewSections_ = nullptr;    // «Sections»: a check box per model section (S7F-DRW-005)
    void refreshViewExtras(const doc::Document& d, const doc::DrawingView& v);
    // Choice of drawings for PDF and printing (§8б, FX7-DRW-004, FX7-DRW-005): a panel beside the sheet window.
    enum class PickPurpose { Pdf, Print, Dxf };  // Dxf — by sheets (FX11-DXF-001)
    PickPurpose pickPurpose_ = PickPurpose::Pdf;
    QFrame* drawingPickPanel_ = nullptr;
    QHBoxLayout* drawingRow_ = nullptr;  // sheet window row of the drawing page (the pick panel lives there or on the spec page)
    QLabel* drawingPickTitle_ = nullptr;
    CheckList* drawingPickList_ = nullptr;
    QPushButton* drawingPickOk_ = nullptr;
    QWidget* buildDrawingPickPanel();
    void showDrawingPick(PickPurpose purpose);
    // Hides the pick panel (×, «Cancel», OK); the title block panel hidden by opening it comes back (FX8-DRW-004).
    void closeDrawingPick();
    bool titleBlockBeforePick_ = false;
    void acceptDrawingPick();
    std::vector<std::string> pickedDrawings() const;
    void cmdPrint();
    void printDrawings(const std::vector<std::string>& drawings);
    QString testPrintTarget_;
    bool drawingEscape();  // Esc on the sheet: cancel the mode or clear the view selection; false — nothing to cancel
    // Bill of materials (§8в, main_window_spec.cpp): page with the sheet window and a panel of rows and title block.
    QWidget* specPage_ = nullptr;
    DrawingView* specView_ = nullptr;
    QLabel* specHeader_ = nullptr;
    QLabel* specErrors_ = nullptr;
    QTableWidget* specRows_ = nullptr;
    QComboBox* specSection_ = nullptr;
    QToolButton* specDeleteRow_ = nullptr;
    TitleBlockForm* specTitleForm_ = nullptr;
    std::vector<drawing::SpecItem> specItems_;  // rows of the specRows_ table in order
    bool specFilling_ = false;                  // filling the table — not a user edit
    QWidget* buildSpecPage();
    void refreshSpecification();
    // Item numbers and the parts list on an assembly drawing (FX14-DRW-002, FX14-DRW-003; app-ux §8б).
    void cmdDrawingBalloon();
    void cmdPartsList();
    void specCellEdited(int row, int column, const QString& text);
    void applySpecTitleField(const std::string& key, const QString& text);
    void specAddRow();
    void specDeleteRow();
    void sectionChanged();
    QActionGroup* styleActions_ = nullptr;
    void cmdThread();  // §5.3и
    void cmdGear();    // §5.3л
    void cmdCoil();    // §5.3м, FX7-PT-004
    void treeFeatureHovered(const std::string& id);  // §4.4, FX7-PT-011
    // Faces and edges of a feature (stable names with the feature's base) in the final state (§4.4, FX7-PT-011).
    std::set<PickTarget> featureGeometry(const std::string& id) const;
    // A face, edge or vertex without body gets it from the name table of the final state (view objects compare both).
    PickTarget withBody(PickTarget t) const;
    // The unified selection field (§5.5а, S7F-UI-001): hover highlight and centering of a selected object.
    void hoverSelectionItem(const SelectionItem* item) override;
    void centerOnSelectionItem(const SelectionItem& item) override;
    std::set<PickTarget> selectionItemTargets(const SelectionItem& item) const;
    SelectionView* prevSelectionView_ = nullptr;  // the hooks before this window (restored on destruction)
    void syncFaceFeature();  // the tree highlights the feature of the face selected in the view (S7F-TREE-002)
    std::string hoverSketch_;                         // sketch hovered in the tree — shown (FX7-PT-011)
    void cmdHole();  // §5.3з
    void cmdLoft();  // §5.3ж
    void cmdDraft();  // §5.3д
    void cmdShell();  // §5.3г
    void cmdSheetMetal();   // §5.3к: sheet parameters — new or edit the existing one
    void cmdSheetFlange();  // §5.3к
    void cmdSheetUnfold();  // §7.4: unfold / fold the display
    void cmdPattern(PatternPanel* panel, const std::string& mode);  // §5.3б, §5.3в
    void cmdRefGeom(bool plane, const std::string& mode);  // §5.3а
    void syncRefSelection();
    FeaturePanel* panel_ = nullptr;  // active feature panel
    // Command waiting for exit from the sketch created by clicking a base (§3.6а): panel and sketch.
    FeaturePanel* basePanel_ = nullptr;
    std::string baseSketch_;
    // A tree plane a sketch was created on (§3.6б): not a base for the next command until the next click in the tree.
    std::string usedTreePlane_;
    std::string newSketch_;  // sketch created by the last «Sketch» command: left empty — deleted on exit (FX7-SK-017)
    // Base of newSketch_ (a plane in the tree or a face in the view): selected again when the new sketch is cancelled
    // (S7F-SK-004).
    std::optional<std::string> newSketchPlane_;
    std::optional<PickTarget> newSketchFace_;
    void resumeAfterBaseSketch(const std::string& closedSketch);
    VariablesPanel* variables_ = nullptr;
    ExportPanel* exportPanel_ = nullptr;
    ImportPanel* importPanel_ = nullptr;  // S4-REC-018
    LibraryPanel* libraryPanel_ = nullptr;          // S6-LIB-003, S6-LIB-004
    LibrarySavePanel* librarySavePanel_ = nullptr;  // S6-LIB-006
    SolutionWizard* solutionWizard_ = nullptr;      // S6-SOL-002…008
    QWidget* solutionDock_ = nullptr;               // the right panel of the wizard (FX8-BLT-004)
    SolutionRecorder* solutionRecorder_ = nullptr;  // S6-SOL-009
    QString importPath_;                  // file chosen for import; the import panel is open (Mode::Import)
    bool importMesh_ = false;
    SettingsPanel* settingsPanel_ = nullptr;
    std::unique_ptr<AppSettings> appSettings_;
    AppSettingsWindow* appSettingsWindow_ = nullptr;
    ReportWindow* reportWindow_ = nullptr;  // problem report (§10.8)
    AboutWindow* aboutWindow_ = nullptr;    // «About» (§2.1а)
    HelpWindow* helpWindow_ = nullptr;      // «Help» (§2.1б)
    UpdateWindow* updateWindow_ = nullptr;  // updating from the program (§10.10)
    bool updateExitRequested_ = false;      // test mode: exit after the installer launch (update.md §5)
    bool closeConfirmed_ = false;           // the unsaved-changes question was already answered (update.md §5)
    void installUpdate(const QString& path);
    // The unsaved-changes question, the external action and exit without asking again (update.md §5).
    void finishUpdate(UpdateAction action, const QString& path);
    FcstdReportWindow* fcstdReport_ = nullptr;  // FreeCAD exchange report (§10.9)
    QStackedWidget* props_ = nullptr;
    QSplitter* left_ = nullptr;
    QSplitter* split_ = nullptr;  // tree | view | variables | right panel (app-ux §2.2)
    QRect testGeometry_;  // test mode: the geometry resetForTest restores (FX14-QA-011); empty — not set
    void restoreTestGeometry();
    // The program opens a side panel of split_ (S7F-UI-007): the panel gets a working width if it is narrower.
    void openSidePanel(QWidget* panel, int minWidth);
    QTabWidget* ribbon_ = nullptr;
    QToolButton* confirmOk_ = nullptr;
    QToolButton* confirmCancel_ = nullptr;
    ToastHost* toasts_ = nullptr;
    BusyIndicator* busy_ = nullptr;  // long work stripe (§9.4)
    ConflictPanel* conflictPanel_ = nullptr;
    ContextBar* ctx_ = nullptr;
    QTimer ctxTimer_;
    // Re-tessellation after the view scale changes (app-ux §11.6, FX3-PERF-002): VIEW_RETESS_DEBOUNCE_MS after the
    // last change; sceneMmPerPx_ — scale of the last scene build.
    QTimer retessTimer_;
    double sceneMmPerPx_ = 0;
    QPoint ctxPos_;
    PickSource ctxSource_ = PickSource::View;
    QLabel* statusHint_ = nullptr;
    SelectionBubble* selBubble_ = nullptr;          // FX7-SEL-001
    SelectionField* selField_ = nullptr;            // the field of the last selection click
    std::unique_ptr<SelectionField> sketchSel_;     // the sketch base set (PickPlane)
    std::map<std::string, PickTarget> sketchPickTargets_;
    QPoint selAt_;
    QLabel* statusDof_ = nullptr;
    QLabel* statusUnits_ = nullptr;
    QLabel* statusRegen_ = nullptr;
    std::map<QString, QToolButton*> buttons_;
    QHBoxLayout* partRibbon_ = nullptr;  // «Part» tab layout (arrangePartRibbon)
    bool partRibbonSheet_ = false;       // the sheet ribbon is laid out (FX8-SH-001)
    std::map<QString, QAction*> toolVariants_;
    std::vector<std::function<void()>> groupDefaults_;  // show the first variant of every group button (resetForTest, FX14-QA-002)
    QSpinBox* polygonSides_ = nullptr;
    QSpinBox* starRays_ = nullptr;  // star «Rays» (app-ux §6.3, S4-SKT-003)
    QWidget* sketchToolPanel_ = nullptr;  // «Sides» / «Rays» under the tree while the tool is active (FX10-SK-003)
    QLabel* sketchToolTitle_ = nullptr;
    QFormLayout* sketchToolForm_ = nullptr;
    std::unique_ptr<SketchEditor> sketch_;
    std::unique_ptr<PartTool> partTool_;
    Mode mode_ = Mode::Part;
    QString previewId_;
    bool previewValid_ = false;
    uint64_t sceneRevision_ = 0;
    QString sceneKey_;
    QTimer regenIndicator_;
    bool exportPending_ = false;
    std::set<std::string> featureErrors_;  // features with an error in the last applied result
};

}  // namespace construe::app
