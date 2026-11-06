// Main window: building the interface and menus (spec/app-ux.md §2, §3).
#include "app/main_window.h"
#include "app/action_tracker.h"
#include "app/session_log.h"
#include "app/weld_panel.h"

#include "app/document_hub.h"
#include "app/file_open_events.h"
#include "app/gl_setup.h"

#include "app/part_tool.h"
#include "api/sketch_ops.h"
#include "app/scene_builder.h"

#include "app/constraint_commands.h"
#include "app/hotkeys_table.h"
#include "app/icons.h"
#include "app/keys.h"
#include "app/messages.h"
#include "app/panel_header.h"
#include "app/theme.h"

#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include "app/app_data.h"
#include "app/portable.h"
#include <QActionGroup>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QTabBar>
#include <QTableWidget>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <numbers>
#include <set>
#include <tuple>

namespace construe::app {

namespace {

// Panel stack (FX7-ASM-002): sized by the current page only. QStackedWidget takes the largest page, so a hidden tall
// panel (the library) would raise the least height of the main window above a 1366×768 screen.
class CurrentPageStack : public QStackedWidget {
public:
    CurrentPageStack() {
        connect(this, &QStackedWidget::currentChanged, this, [this] { updateGeometry(); });
    }
    QSize sizeHint() const override { return currentWidget() ? currentWidget()->sizeHint() : QStackedWidget::sizeHint(); }
    QSize minimumSizeHint() const override {
        return currentWidget() ? currentWidget()->minimumSizeHint() : QStackedWidget::minimumSizeHint();
    }
};

// Ribbon tabs (app-ux §2.3, FX7-UI-002): the tab bar is at least RIBBON_TAB_MIN_H high, so the buttons above the
// ribbon (corner widgets) have ribbon-sized icons and are not clipped by the page.
class RibbonTabBar : public QTabBar {
public:
    QSize tabSizeHint(int i) const override {
        QSize s = QTabBar::tabSizeHint(i);
        s.setHeight(std::max(s.height(), RIBBON_TAB_MIN_H));
        return s;
    }
};

class RibbonTabs : public QTabWidget {
public:
    RibbonTabs() { setTabBar(new RibbonTabBar); }
};

}  // namespace

std::vector<HelpCommand> MainWindow::helpCommands() const {
    std::vector<HelpCommand> out;
    std::set<QString> seen;
    QString group;  // the ribbon page — a group of the help tree (§2.1б, FX8-UI-002)
    auto add = [&](const QString& object, const QString& label, const QString& tip, const QIcon& icon) {
        if (object.isEmpty() || !seen.insert(object).second) return;
        out.push_back({object, label.isEmpty() ? tip : label, tip, icon, group});
    };
    std::vector<std::pair<QWidget*, QString>> pages{{ribbon_->cornerWidget(Qt::TopLeftCorner), QString::fromUtf8(txt("ui.main.aboveTheRibbonLeft"))},
                                                    {ribbon_->cornerWidget(Qt::TopRightCorner), QString::fromUtf8(txt("ui.main.aboveTheRibbonRight"))}};
    for (int i = 0; i < ribbon_->count(); ++i) pages.push_back({ribbon_->widget(i), ribbon_->tabText(i).remove('&')});
    for (const auto& [p, name] : pages) {
        if (!p) continue;
        group = name;
        for (QToolButton* b : p->findChildren<QToolButton*>()) {
            if (!b->objectName().startsWith("ribbon_")) continue;
            add(b->objectName(), b->text(), b->toolTip(), b->icon());
            if (b->menu())
                for (QAction* a : b->menu()->actions())
                    if (!a->isSeparator()) add(a->objectName(), a->text().remove('&'), a->toolTip(), a->icon());
        }
    }
    return out;
}

void MainWindow::buildUi() {
    auto* central = new QWidget;
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    ribbon_ = new RibbonTabs;
    ribbon_->setObjectName("ribbon");
    // Height follows the content (tab bar + page), not fixed: at any screen scale the buttons
    // are not clipped (app-ux §2.3, S1-UX-007).
    ribbon_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    ribbon_->setMinimumHeight(RIBBON_MIN_H);
    v->addWidget(ribbon_);
    auto* split = new QSplitter(Qt::Horizontal);
    split_ = split;
    left_ = new QSplitter(Qt::Vertical);
    tree_ = new FeatureTree(*model_);
    tree_->setMenuHook([this](QMenu& m, const QString& kind, const std::string& id) { extendTreeMenu(m, kind, id); });
    props_ = new CurrentPageStack;
    extrude_ = new ExtrudePanel(*model_);
    props_->addWidget(extrude_);
    revolve_ = new RevolvePanel(*model_);
    props_->addWidget(revolve_);
    coil_ = new CoilPanel(*model_);  // §5.3м, FX7-PT-004
    props_->addWidget(coil_);
    fillet_ = new EdgePanel(*model_, true);
    props_->addWidget(fillet_);
    chamfer_ = new EdgePanel(*model_, false);
    props_->addWidget(chamfer_);
    refPlane_ = new RefGeomPanel(*model_, true);
    props_->addWidget(refPlane_);
    refAxis_ = new RefGeomPanel(*model_, false);
    props_->addWidget(refAxis_);
    pattern_ = new PatternPanel(*model_);
    props_->addWidget(pattern_);
    mirror_ = new PatternPanel(*model_, true);
    props_->addWidget(mirror_);
    shell_ = new ShellPanel(*model_);
    props_->addWidget(shell_);
    draft_ = new DraftPanel(*model_);
    props_->addWidget(draft_);
    sweep_ = new SweepPanel(*model_);
    props_->addWidget(sweep_);
    loft_ = new LoftPanel(*model_);
    props_->addWidget(loft_);
    hole_ = new HolePanel(*model_);
    props_->addWidget(hole_);
    thread_ = new ThreadPanel(*model_);
    props_->addWidget(thread_);
    gear_ = new GearPanel(*model_);
    props_->addWidget(gear_);
    sheetMetal_ = new SheetMetalPanel(*model_);
    props_->addWidget(sheetMetal_);
    sheetWall_ = new SheetWallPanel(*model_);
    props_->addWidget(sheetWall_);
    sheetFlange_ = new SheetFlangePanel(*model_);
    props_->addWidget(sheetFlange_);
    sheetBend_ = new SheetBendPanel(*model_);
    props_->addWidget(sheetBend_);
    sheetCut_ = new SheetCutPanel(*model_);
    props_->addWidget(sheetCut_);
    propertiesPanel_ = new PropertiesPanel(*model_);
    props_->addWidget(propertiesPanel_);
    section_ = new SectionPanel(*model_);
    props_->addWidget(section_);
    annotation_ = new AnnotationPanel;  // §5.12
    annotation_->setFaceLabeler([this](const std::string& f) { return topoCaption(model_->session(), f, false); });
    props_->addWidget(annotation_);
    mate_ = new MatePanel(*model_);
    mate_->setRefLabeler([this](const std::string& r) { return mateRefLabel(r); });
    connect(mate_, &MatePanel::changed, this, [this] { mateChanged(); });           // preview (FX8-ASM-001)
    connect(mate_, &MatePanel::constructionToggled, this, [this](bool) {  // the parts' sketch elements (FX9-ASM-001)
        updateSketchPicks();
        rebuildScene();
    });
    connect(mate_, &MatePanel::nextRequested, this, [this] { acceptMate(true); });  // FX8-ASM-002
    props_->addWidget(mate_);
    weld_ = new WeldPanel(*model_);  // §8а, S5-WELD-001
    weld_->setRefLabeler([this](const std::string& r) { return mateRefLabel(r); });
    props_->addWidget(weld_);
    repair_ = new RepairPanel;  // §5.5в, S7F-FEAT-003
    connect(repair_, &RepairPanel::applyRequested, this, [this] { repairApply(); });
    connect(repair_, &RepairPanel::skipRequested, this, [this] { repairSkip(); });
    connect(repair_, &RepairPanel::doneRequested, this, [this] { repairDone(); });
    connect(repair_, &RepairPanel::revertRequested, this, [this] { repairRevert(); });
    connect(repair_->slotTree(), &SlotTree::changed, this, [this] { syncRepairSelection(); });
    props_->addWidget(repair_);
    instancePattern_ = new InstancePatternPanel(*model_);  // §8а, FX7-ASM-007
    instancePattern_->setLabeler([this](const std::string& r) { return instancePatternLabel(r); });
    instancePattern_->setFeatureLister([this](const std::string& path) { return partPatternFeatures(path); });
    connect(instancePattern_, &InstancePatternPanel::changed, this, [this] { syncInstancePatternSelection(); });
    props_->addWidget(instancePattern_);
    // ✓ / ✗ of the command panel headers — the same actions as the corner (§2.1д, §2.4, S7F-RF-008).
    connect(section_, &SectionPanel::accepted, this, [this] { confirmAccept(); });
    connect(section_, &SectionPanel::cancelled, this, [this] { confirmReject(); });
    connect(annotation_, &AnnotationPanel::accepted, this, [this] { confirmAccept(); });
    connect(annotation_, &AnnotationPanel::cancelled, this, [this] { confirmReject(); });
    connect(mate_, &MatePanel::accepted, this, [this] { confirmAccept(); });
    connect(mate_, &MatePanel::cancelled, this, [this] { confirmReject(); });
    connect(weld_, &WeldPanel::accepted, this, [this] { confirmAccept(); });
    connect(weld_, &WeldPanel::cancelled, this, [this] { confirmReject(); });
    connect(instancePattern_, &InstancePatternPanel::accepted, this, [this] { confirmAccept(); });
    connect(instancePattern_, &InstancePatternPanel::cancelled, this, [this] { confirmReject(); });
    // The «Interference» panel (§8а, S2-ASM-007).
    interferencePanel_ = new QWidget;
    interferencePanel_->setObjectName("interferencePanel");
    {
        auto* col = new QVBoxLayout(interferencePanel_);
        // Header with the close cross (§2.1в, FX8-UI-008): the same as ✓ of the corner — back to the assembly.
        auto* head = new PanelHeader(txt("ui.common.interference"), "interferenceClose");
        connect(head, &PanelHeader::closeRequested, this, [this] {
            if (mode_ == Mode::Interference) confirmAccept();
        });
        col->addWidget(head);
        interferenceList_ = new QListWidget;
        interferenceList_->setObjectName("interferenceList");
        col->addWidget(interferenceList_);
    }
    props_->addWidget(interferencePanel_);
    connect(interferenceList_, &QListWidget::currentRowChanged, this, [this](int row) {
        highlight_.clear();
        if (row >= 0 && row < int(interferences_.size())) highlight_ = {interferences_[size_t(row)].a, interferences_[size_t(row)].b};
        rebuildScene();
    });
    connect(section_, &SectionPanel::changed, this, [this] { sectionChanged(); });
    connect(propertiesPanel_, &PropertiesPanel::accepted, this, [this] { setMode(Mode::Part); });
    connect(tree_, &FeatureTree::propertiesRequested, this, &MainWindow::cmdProperties);
    connect(tree_, &FeatureTree::recognizeRequested, this, &MainWindow::cmdReconstruct);
    connect(propertiesPanel_, &PropertiesPanel::message, this, [this](const QString& t, bool err) { showToast(t, err); });
    panel_ = extrude_;
    // STL export (§10.3) and document settings (§10.4) panels.
    exportPanel_ = new ExportPanel(model_->session());
    connect(exportPanel_, &ExportPanel::accepted, this, &MainWindow::acceptExport);
    connect(exportPanel_, &ExportPanel::cancelled, this, [this] { setMode(Mode::Part); });
    props_->addWidget(exportPanel_);
    importPanel_ = new ImportPanel;
    connect(importPanel_, &ImportPanel::accepted, this, &MainWindow::acceptImport);
    connect(importPanel_, &ImportPanel::cancelled, this, [this] { setMode(Mode::Part); });
    props_->addWidget(importPanel_);
    libraryPanel_ = new LibraryPanel(model_->session());  // S6-LIB-003, S6-LIB-004 (§8г)
    connect(libraryPanel_, &LibraryPanel::accepted, this, &MainWindow::acceptLibrary);
    connect(libraryPanel_, &LibraryPanel::cancelled, this, [this] { setMode(Mode::Part); });
    connect(libraryPanel_, &LibraryPanel::selectionChanged, this, [this] { updateAvailability(); });
    connect(libraryPanel_, &LibraryPanel::seatRequested, this, &MainWindow::acceptSeat);  // S6A-BRG-003
    connect(libraryPanel_, &LibraryPanel::replaceRequested, this, &MainWindow::acceptLibraryReplace);  // FX10-LIB-004
    connect(libraryPanel_, &LibraryPanel::seatFacesChanged, this, &MainWindow::syncLibrarySelection);
    libraryPanel_->setRefLabeler([this](const std::string& r) { return mateRefLabel(r); });
    connect(libraryPanel_, &LibraryPanel::refreshRequested, this, [this] {
        model_->session().refreshLibrary();
        libraryPanel_->reload();
    });
    solutionWizard_ = new SolutionWizard(model_->session());  // S6-SOL-002…008 (§8д)
    connect(solutionWizard_, &SolutionWizard::accepted, this, &MainWindow::acceptSolution);
    connect(solutionWizard_, &SolutionWizard::cancelled, this, [this] { setMode(Mode::Part); });
    connect(solutionWizard_, &SolutionWizard::changed, this, [this] {
        updateAvailability();
        syncSolutionSelection();
        if (mode_ == Mode::Solution) setPrompt(solutionWizard_->hint());  // names the current place (S7F-UI-006)
    });
    connect(solutionWizard_, &SolutionWizard::sameHolesRequested, this, &MainWindow::solutionSameHoles);
    solutionWizard_->setRefLabeler([this](const std::string& r) { return mateRefLabel(r); });
    // The wizard lives in the right panel shown while a connection is created or edited (§8д, FX8-BLT-004); the library
    // of standard parts — there too (§8г, S7F-UI-008); setMode shows the one of the mode.
    solutionDock_ = new QWidget;
    solutionDock_->setObjectName("solutionDock");
    {
        auto* dl = new QVBoxLayout(solutionDock_);
        dl->setContentsMargins(0, 0, 0, 0);
        dl->addWidget(solutionWizard_);
        dl->addWidget(libraryPanel_);
        libraryPanel_->hide();
    }
    solutionDock_->setMinimumWidth(RIGHT_PANEL_W);
    solutionDock_->hide();
    solutionRecorder_ = new SolutionRecorder(model_->session());  // S6-SOL-009 (§8е)
    connect(solutionRecorder_, &SolutionRecorder::accepted, this, &MainWindow::acceptSolutionRecord);
    connect(solutionRecorder_, &SolutionRecorder::cancelled, this, [this] { setMode(Mode::Part); });
    connect(solutionRecorder_, &SolutionRecorder::changed, this, [this] {
        updateAvailability();
        syncSolutionSelection();
    });
    solutionRecorder_->setRefLabeler([this](const std::string& r) { return mateRefLabel(r); });
    props_->addWidget(solutionRecorder_);
    librarySavePanel_ = new LibrarySavePanel(model_->session());  // S6-LIB-006
    connect(librarySavePanel_, &LibrarySavePanel::accepted, this, &MainWindow::acceptLibrarySave);
    connect(librarySavePanel_, &LibrarySavePanel::cancelled, this, [this] { setMode(Mode::Part); });
    props_->addWidget(librarySavePanel_);
    settingsPanel_ = new SettingsPanel;
    connect(settingsPanel_, &SettingsPanel::accepted, this, &MainWindow::acceptSettings);
    connect(settingsPanel_, &SettingsPanel::cancelled, this, [this] { setMode(Mode::Part); });
    props_->addWidget(settingsPanel_);
    left_->addWidget(tree_);
    left_->addWidget(props_);
    props_->hide();
    view_ = new Viewport;
    view_->setAnimationMs(testMode_ ? 0 : ANIM_VIEW_MS);  // §12.1
    variables_ = new VariablesPanel(*model_);
    variables_->setTestMode(testMode_);
    variables_->setFixedWidth(RIGHT_PANEL_W);
    variables_->hide();
    split->addWidget(left_);
    // Center: 3D view or drawing sheet (§8б).
    connect(tree_, &FeatureTree::sheetSelected, this, [this](const QString& id) { selectSheet(id.toStdString()); });
    connect(tree_, &FeatureTree::viewSelected, this, [this](const QString& id) {  // drawing view in the tree (§8б)
        const auto d = model_->session().document().document;
        for (const auto& sh : d->active().sheets)
            if (sh.viewIndex(id.toStdString()) >= 0 && sh.id != currentSheet_) selectSheet(sh.id);
        selectView(id.toStdString());
    });
    connect(tree_, &FeatureTree::viewMenuRequested, this,
            [this](const QString& id, const QPoint& global) { showViewMenu(id.toStdString(), global); });
    center_ = new QStackedWidget;
    center_->addWidget(view_);
    center_->addWidget(buildDrawingPage());
    center_->addWidget(buildSpecPage());  // bill of materials (§8в)
    split->addWidget(center_);
    split->addWidget(variables_);
    split->addWidget(solutionDock_);
    split->setStretchFactor(1, 1);
    split->setSizes({LEFT_PANEL_W, 1000, RIGHT_PANEL_W, RIGHT_PANEL_W});  // a size for each of the 4 panels (S7F-UI-007)
    left_->setMinimumWidth(LEFT_PANEL_MIN_W);
    v->addWidget(split, 1);
    setCentralWidget(central);
    // Application settings (§10.6): applied at startup and on every change; the window is modeless.
    for (const char* k : {"view/mouseProfile", "view/invertWheel", "fonts/textFont", "fonts/directories", "drawing/fontGost", "drawing/fontIso"})
        applySetting(k);
    applyLibraryDirectories();  // library folders (library.md §4)
    libraryPanel_->setStandards({appSettings_->libraryGost(), appSettings_->libraryIso()});  // FX11-STD-002
    connect(appSettings_.get(), &AppSettings::changed, this, &MainWindow::applySetting);
    appSettingsWindow_ = new AppSettingsWindow(*appSettings_, testMode_, this);
    appSettingsWindow_->hide();
    // Problem report window (§10.8): created at startup (hidden) like the settings window.
    reportWindow_ = new ReportWindow(
        *appSettings_,
        {[this] { return grab().toImage(); },
         [this]() -> std::optional<ReportFile> {
             const auto p = model_->session().path();
             const QString name = p ? QFileInfo(QString::fromStdString(*p)).fileName() : QString("document.construe");
             return ReportFile{name, QByteArray::fromStdString(model_->session().documentText())};
         }},
        testMode_, this);
    reportWindow_->hide();
    aboutWindow_ = new AboutWindow(this);  // «About» (§2.1а): created hidden, like the report window
    helpWindow_ = new HelpWindow(this, [this] { return helpCommands(); });  // «Help» (§2.1б, FX7-UI-007)
    aboutWindow_->hide();
    // Update window (§10.10): created hidden; a manual check shows its result in it (FX8-UI-004), installation —
    // installUpdate.
    updateWindow_ = new UpdateWindow(this);
    updateWindow_->hide();
    connect(updateWindow_, &UpdateWindow::installReady, this, &MainWindow::installUpdate);
    connect(updateWindow_, &UpdateWindow::restartRequested, this, [this](const QString& p) { finishUpdate(UpdateAction::RestartAppImage, p); });
    connect(reportWindow_, &ReportWindow::sent, this, [this](const QString& id) { showToast(text("ui.report.sent", {{"id", id}})); });
    connect(reportWindow_, &ReportWindow::savedZip, this, [this](const QString& p) { showToast(text("ui.report.savedZip", {{"path", p}})); });
    // Data and cache folders (§10.5; without installation — §10.5а).
    if (!testMode_) dataDir_ = storageLayout().dataDir;
    if (!dataDir_.isEmpty()) applySetting("fonts/directories");  // the user's fonts folder <data>/fonts (FX10-DRW-004)
    new FileOpenEvents([this](const QString& p) { openFromSystem(p); }, this);  // macOS Finder (§10.1)
    // On-disk evaluator cache (§10.5, core-api §4.1ж): disabled in test mode.
    if (!testMode_) setRegenCacheDirForTest(storageLayout().cacheDir + "/regen");
    buildMenus();
    buildRibbon();
    // User action trace (app-ux §10.8 «След действий», FX14-UI-003): one application event filter; the menus and the
    // ribbon are subscribed now, later widgets and actions — when polished or added.
    if (ActionTracker* t = ActionTracker::install()) t->trackTree(this);
    // Autosave (§10.5).
    autosave_ = new QTimer(this);
    autosave_->setInterval(AUTOSAVE_INTERVAL_MS);
    connect(autosave_, &QTimer::timeout, this, [this] { autosaveNow(); });
    autosave_->start();
    if (!testMode_) QTimer::singleShot(0, this, [this] {
        checkRecovery();
        checkCrashReport();
    });
    // Automatic update check (update.md §4): after the start; in test mode only with an explicit address.
    if (!testMode_ || updateUrlOverridden()) QTimer::singleShot(UPDATE_START_DELAY_MS, this, [this] { autoCheckUpdates(); });

    // Confirmation corner (§2.4).
    confirmOk_ = new QToolButton(view_);
    confirmOk_->setObjectName("confirmOk");
    confirmOk_->setText(QString::fromUtf8("✓"));
    confirmOk_->setFixedSize(CONFIRM_CORNER_BTN_PX, CONFIRM_CORNER_BTN_PX);
    confirmOk_->setStyleSheet(QString("color: white; background: %1; font-weight: bold;").arg(COLOR_OK.color.name()));
    confirmCancel_ = new QToolButton(view_);
    confirmCancel_->setObjectName("confirmCancel");
    confirmCancel_->setText(QString::fromUtf8("✗"));
    confirmCancel_->setFixedSize(CONFIRM_CORNER_BTN_PX, CONFIRM_CORNER_BTN_PX);
    confirmCancel_->setStyleSheet(QString("color: white; background: %1; font-weight: bold;").arg(COLOR_CANCEL.color.name()));
    // «Normal to» — in the confirmation corner next to ✓ (§2.4, S1-UX-007).
    auto* normal = new QToolButton(view_);
    normal->setObjectName("ribbon_normalTo");
    normal->setIcon(commandIcon("normalTo"));
    normal->setIconSize(QSize(RIBBON_ICON_PX, RIBBON_ICON_PX));
    normal->setToolTip(txt("ui.common.normalToCtrl8"));
    normal->setFixedSize(CONFIRM_CORNER_BTN_PX, CONFIRM_CORNER_BTN_PX);
    connect(normal, &QToolButton::clicked, this, [this] { cmdNormalTo(); });
    buttons_["normalTo"] = normal;
    // ✓ / ✗ of the corner — the mode's actions by the table (§2.4, S7F-RF-008).
    connect(confirmOk_, &QToolButton::clicked, this, [this] { confirmAccept(); });
    connect(confirmCancel_, &QToolButton::clicked, this, [this] { confirmReject(); });
    conflictPanel_ = new ConflictPanel(model_->session(), view_);
    connect(conflictPanel_, &ConflictPanel::undoRequested, this, [this] { cmdUndo(); });
    connect(conflictPanel_, &ConflictPanel::error, this, [this](const QString& t) { showToast(t, true); });
    // Notifications over the center (3D view, drawing sheet or bill of materials): on the 3D view they would be hidden
    // with it under a drawing (§9.1, FX8-UI-004).
    toasts_ = new ToastHost(center_);
    connect(center_, &QStackedWidget::currentChanged, toasts_, [this] { toasts_->raise(); });
    // Long work stripe (§9.4, FX8-UI-001) over the top of the central area.
    busy_ = new BusyIndicator(central, [this] { return model_->regenPending() || model_->previewPending() || exportPending_; });
    // Context panel (§7.2): CONTEXT_BAR_DELAY_MS after a click/box selection.
    ctx_ = new ContextBar(this);
    connect(ctx_, &ContextBar::triggered, this, &MainWindow::runContextCommand);
    ctxTimer_.setSingleShot(true);
    ctxTimer_.setInterval(CONTEXT_BAR_DELAY_MS);
    connect(&ctxTimer_, &QTimer::timeout, this, [this] {
        // Commands with a ribbon button are available like their buttons (§7.2, §7.5): Sketch, Extrude, Revolve, Normal to.
        std::vector<QString> cmds;
        for (const QString& c : contextCommands(contextSelection())) {
            const bool ok = c == "sketch"    ? commandEnabled("ribbon_sketch") || commandEnabled("ribbon_asmSketch")
                            : c == "extrude" ? commandEnabled("ribbon_extrude")
                            : c == "revolve" ? commandEnabled("ribbon_revolve")
                            : c == "normalTo" ? commandEnabled("ribbon_normalTo")
                            : c == "pmiFit"   ? commandEnabled("tool_pmiRoughness")
                                              : true;
            if (ok) cmds.push_back(c);
        }
        ctx_->showAt(cmds, ctxPos_);
    });

    // Status bar.
    statusHint_ = new QLabel;
    statusHint_->setObjectName("statusHint");
    statusDof_ = new QLabel;
    statusDof_->setObjectName("statusDof");
    statusUnits_ = new QLabel(txt("ui.main.mm"));
    statusUnits_->setObjectName("statusUnits");
    statusRegen_ = new QLabel;
    statusBar()->addWidget(statusHint_, 1);
    // «Sides» / «Rays» of the polygon and star tools — in the left panel under the tree, where feature parameters are
    // shown, while the tool is active (app-ux §6.3, FX10-SK-003).
    sketchToolPanel_ = new QWidget;
    sketchToolPanel_->setObjectName("sketchToolPanel");
    {
        auto* tl = new QVBoxLayout(sketchToolPanel_);
        sketchToolTitle_ = new QLabel;
        sketchToolTitle_->setObjectName("sketchToolTitle");
        QFont bold = sketchToolTitle_->font();
        bold.setBold(true);
        sketchToolTitle_->setFont(bold);
        tl->addWidget(sketchToolTitle_);
        auto* form = sketchToolForm_ = new QFormLayout;
        polygonSides_ = new QSpinBox;
        polygonSides_->setObjectName("polygonSides");
        polygonSides_->setRange(3, 40);
        polygonSides_->setValue(6);
        connect(polygonSides_, &QSpinBox::valueChanged, this, [this](int n) { sketch_->setPolygonSides(n); });
        form->addRow(txt("ui.primitive.sides"), polygonSides_);
        starRays_ = new QSpinBox;  // star (§6.3, S4-SKT-003)
        starRays_->setObjectName("starRays");
        starRays_->setRange(3, 40);
        starRays_->setValue(5);
        connect(starRays_, &QSpinBox::valueChanged, this, [this](int n) { sketch_->setStarRays(n); });
        form->addRow(txt("ui.primitive.rays"), starRays_);
        tl->addLayout(form);
        tl->addStretch(1);
    }
    props_->addWidget(sketchToolPanel_);
    statusBar()->addPermanentWidget(statusDof_);
    statusBar()->addPermanentWidget(statusUnits_);
    statusBar()->addPermanentWidget(statusRegen_);
    statusBar()->setFixedHeight(STATUSBAR_H);

    sketch_ = std::make_unique<SketchEditor>(*model_, *view_);
    partTool_ = std::make_unique<PartTool>(*this);
    view_->setTool(partTool_.get());
    connect(sketch_.get(), &SketchEditor::statusChanged, this, &MainWindow::updateStatus);
    connect(sketch_.get(), &SketchEditor::escapeBottom, this, &MainWindow::sketchEscapeBottom);
    connect(sketch_.get(), &SketchEditor::hint, this, [this](const QString& t) { setPrompt(t); });
    connect(sketch_.get(), &SketchEditor::toolHintEnded, this, [this](const QString& t) {
        if (statusHint_->text() == t) setPrompt({});
    });
    connect(sketch_.get(), &SketchEditor::error, this, [this](const QString& t) { showToast(t, true); });
    connect(sketch_.get(), &SketchEditor::notice, this, [this](const QString& t, bool undoable) {
        std::vector<ToastAction> acts;
        if (undoable) acts.push_back({text("action.undo"), [this] { cmdUndo(); }});
        showToast(t, false, std::move(acts));
    });
    connect(sketch_.get(), &SketchEditor::madeDriven, this, [this](const QString& t, const QString& cid) {
        showToast(t, false, {{text("action.makeDriving"), [this, cid] {
                                  auto r = model_->session().sketchUpdate(doc::Json::array({{{"id", cid.toStdString()}, {"driving", true}}}));
                                  if (!r) showToast(errorText(r.error()), true);
                              }}});
    });
    connect(sketch_.get(), &SketchEditor::yieldedAuto, this, [this](const QString& t, const QString& cid, const QList<doc::Json>& restore) {
        showToast(t, false, {{text("action.makeDriven"), [this, cid, restore] {
                                  // Reference dimension and restoring removed auto-constraints — one undo step (§6.9).
                                  auto& s = model_->session();
                                  s.beginGroup("undo.group.dimension");
                                  auto r = s.sketchUpdate(doc::Json::array({{{"id", cid.toStdString()}, {"driving", false}}}));
                                  doc::Json items = doc::Json::array();
                                  int k = 1;
                                  for (auto it : restore) {
                                      it["id"] = "t" + std::to_string(k++);
                                      items.push_back(it);
                                  }
                                  if (r && !items.empty()) {
                                      auto a = s.sketchAdd(items);
                                      if (!a) showToast(errorText(a.error()), true);
                                  }
                                  s.endGroup();
                                  if (!r) showToast(errorText(r.error()), true);
                              }}});
    });
    // Command availability (§7.5, FX4-UI-002): recomputed when the view or tree selection changes; the tree menu
    // uses the same rule table.
    connect(view_, &Viewport::selectionChanged, this, &MainWindow::updateAvailability);
    connect(view_, &Viewport::selectionChanged, this, [this] { traceAction(selectionTraceText(view_->selection())); });  // FX14-UI-003
    connect(view_, &Viewport::selectionChanged, this, &MainWindow::syncFaceFeature);  // S7F-TREE-002
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateAvailability);
    tree_->setAvailability([this](const QString& command, ElementKind subject) {
        AvailabilityState st = availabilityState();
        st.subjectKind = subject;
        return commandAvailable(command.toStdString(), st);
    });
    connect(tree_, &FeatureTree::editSketch, this, [this](const QString& id) { editSketch(id.toStdString()); });
    connect(tree_, &FeatureTree::editFeature, this, [this](const QString& id) { editFeature(id.toStdString()); });
    connect(tree_, &FeatureTree::repairRequested, this, [this](const QString& id) { startRepair(id.toStdString()); });
    connect(tree_, &FeatureTree::editSection, this, [this](const QString& id) { editSection(id.toStdString()); });
    connect(tree_, &FeatureTree::editAnnotation, this, [this](const QString& id) { editAnnotation(id.toStdString()); });  // §4.2в
    connect(tree_, &FeatureTree::openInstance, this, [this](const QString& id) { openInstanceFromAssembly(id.toStdString()); });
    connect(tree_, &FeatureTree::editInContext, this, [this](const QString& id) { editInstanceInContext(id.toStdString()); });
    connect(tree_, &FeatureTree::instanceProperties, this, [this](const QString& id) {
        if (mode_ != Mode::Part || !assemblyActive()) return;
        propertiesPanel_->setInstance(id.toStdString());
        setMode(Mode::Properties);
    });
    connect(propertiesPanel_, &PropertiesPanel::openInstance, this, [this](const QString& id) {
        setMode(Mode::Part);
        openInstanceFromAssembly(id.toStdString());
    });
    connect(tree_, &FeatureTree::editMate, this, [this](const QString& id) { editMate(id.toStdString()); });
    connect(tree_, &FeatureTree::editWeld, this, [this](const QString& id) { editWeld(id.toStdString()); });
    connect(tree_, &FeatureTree::editInstancePattern, this, [this](const QString& id) { editInstancePattern(id.toStdString()); });
    connect(tree_, &FeatureTree::drawingPanelRequested, this, [this] { showDrawingPanel(true, true); });  // §8б, FX1-UI-007
    // Changes of linked files (§8а): after saving in any window of the application and on returning to the application.
    connect(&DocumentHub::instance(), &DocumentHub::saved, this, [this] { checkLinks(); });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState st) {
        if (st == Qt::ApplicationActive) checkLinks();
    });
    connect(tree_, &FeatureTree::addAssemblyRequested, this, [this] {
        if (auto r = model_->session().addElement("assembly"); !r) showToast(errorText(r.error()), true);
    });
    connect(tree_, &FeatureTree::sketchOnPlane, this, [this](const QString& id) { createSketchOn(id.toStdString()); });
    connect(tree_, &FeatureTree::message, this, [this](const QString& t, bool err) { showToast(t, err); });
    // Planes visible with bodies: hovered in the tree and pinned with the eye (§3.4, §4.2а).
    connect(tree_, &FeatureTree::planeHovered, this, [this](const QString& id) {
        std::set<std::string> shown = tree_->pinnedPlanes();
        if (!id.isEmpty()) shown.insert(id.toStdString());
        view_->setShownPlanes(shown);
    });
    connect(tree_, &FeatureTree::featureHovered, this, [this](const QString& id) { treeFeatureHovered(id.toStdString()); });
    connect(tree_, &FeatureTree::pinnedPlanesChanged, this, [this] { view_->setShownPlanes(tree_->pinnedPlanes()); });
    connect(tree_, &FeatureTree::baseVisibilityChanged, this, [this] { rebuildScene(); });  // base body (S4-REC-009)
    connect(tree_, &FeatureTree::visibilityChanged, this, [this] {  // eye of a sketch or annotation (§4.2а, FX1-UI-009)
        updateRegions();
        refreshAnnotations();
    });
    connect(tree_, &FeatureTree::convertPartRequested, this, [this](const QString& id, bool toSheet) { cmdConvertPart(id.toStdString(), toSheet); });
    connect(tree_, &FeatureTree::undoableDone, this, [this](const QString& t) {
        showToast(t, false, {{text("action.undo"), [this] { cmdUndo(); }}});
    });
    connect(tree_, &QTreeWidget::itemSelectionChanged, view_, [this] { view_->update(); });  // annotation highlight (§4.2в)
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (mode_ == Mode::Section) {  // section plane from the tree (§5.11)
            for (auto* it : tree_->selectedItems()) {
                const auto t = treeTarget(it->data(0, FeatureTree::KindRole).toString(), it->data(0, FeatureTree::IdRole).toString().toStdString(), false);
                if (t && t->kind == PickTarget::Kind::Plane) pickedInSection(*t);
            }
            return;
        }
        if (mode_ != Mode::Part) return;
        std::set<PickTarget> sel;
        for (auto* it : tree_->selectedItems()) {
            const QString kind = it->data(0, FeatureTree::KindRole).toString();
            const std::string id = it->data(0, FeatureTree::IdRole).toString().toStdString();
            if (kind == "plane") sel.insert({PickTarget::Kind::Plane, {}, id});
            else if (kind == "origin") sel.insert({PickTarget::Kind::Origin, {}, id});
            else if (kind == "axis") sel.insert({PickTarget::Kind::Axis, {}, id});
            else if (kind == "weld") sel.insert({PickTarget::Kind::Weld, {}, id});  // §8а
        }
        view_->setSelection(sel);
    });
    connect(variables_, &VariablesPanel::message, this, [this](const QString& t, bool err) { showToast(t, err); });
    connect(variables_, &VariablesPanel::undoableDone, this, [this](const QString& t) {
        showToast(t, false, {{text("action.undo"), [this] { cmdUndo(); }}});
    });
    for (FeaturePanel* p : {static_cast<FeaturePanel*>(extrude_), static_cast<FeaturePanel*>(revolve_), static_cast<FeaturePanel*>(coil_),
                            static_cast<FeaturePanel*>(fillet_), static_cast<FeaturePanel*>(chamfer_),
                            static_cast<FeaturePanel*>(refPlane_), static_cast<FeaturePanel*>(refAxis_),
                            static_cast<FeaturePanel*>(pattern_), static_cast<FeaturePanel*>(mirror_),
                            static_cast<FeaturePanel*>(shell_), static_cast<FeaturePanel*>(draft_),
                            static_cast<FeaturePanel*>(sweep_), static_cast<FeaturePanel*>(loft_),
                            static_cast<FeaturePanel*>(hole_), static_cast<FeaturePanel*>(thread_), static_cast<FeaturePanel*>(gear_),
                            static_cast<FeaturePanel*>(sheetMetal_), static_cast<FeaturePanel*>(sheetWall_),
                            static_cast<FeaturePanel*>(sheetFlange_), static_cast<FeaturePanel*>(sheetBend_),
                            static_cast<FeaturePanel*>(sheetCut_)}) {
        connect(p, &FeaturePanel::accepted, this, &MainWindow::acceptFeature);
        connect(p, &FeaturePanel::cancelled, this, &MainWindow::cancelFeature);
        connect(p, &FeaturePanel::previewRequested, this, &MainWindow::requestPreview);
    }
    connect(loft_, &FeaturePanel::previewRequested, this, &MainWindow::updateSketchPicks);  // section points (FX10-PT-004)
    // Selection fields of the feature panel are filled by a click in the tree (§5.5, §5.5д).
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it) {
        usedTreePlane_.clear();  // a new choice in the tree (§3.6б)
        // Projection in a sketch: another sketch in the tree is the source (§6.11б).
        if (mode_ == Mode::Sketch && it && sketch_->tool() == SketchEditor::Tool::Pierce &&
            it->data(0, FeatureTree::KindRole).toString() == "feature") {
            const std::string id = it->data(0, FeatureTree::IdRole).toString().toStdString();
            const auto f = model_->session().document().document->active().find(id);
            if (f && f->sketchData() && id != sketch_->feature()) sketch_->pierceSource("pierce:" + id);
            return;
        }
        // A feature panel takes tree nodes into its fields (§5.5д, S7F-RF-011): the «Up to» end of an extrude, the
        // revolve axis, the neutral plane of a draft, pattern and mirror fields, reference geometry «References».
        if (mode_ == Mode::FeatureEdit && it &&
            pickedInTreeFeatureEdit(it->data(0, FeatureTree::KindRole).toString(), it->data(0, FeatureTree::IdRole).toString().toStdString()))
            return;
        // Mate objects from the tree (§8а): origin and reference planes and axes, an assembly sketch — its plane
        // (FX7-ASM-006).
        if (mode_ == Mode::Mate && it) {
            const QString kind = it->data(0, FeatureTree::KindRole).toString();
            const std::string id = it->data(0, FeatureTree::IdRole).toString().toStdString();
            // Construction of a part (FX9-ASM-001): the node ID is "<instance path>/<name>".
            if (kind == "partPlane" || kind == "partAxis" || kind == "partOrigin") {
                const auto slash = id.rfind('/', id.find(':') == std::string::npos ? std::string::npos : id.find(':'));
                const PickTarget::Kind k = kind == "partPlane" ? PickTarget::Kind::Plane : kind == "partAxis" ? PickTarget::Kind::Axis : PickTarget::Kind::Origin;
                pickedInMate({k, id.substr(0, slash), id.substr(slash + 1)});
            }
            if (const auto t = treeTarget(kind, id, true)) pickedInMate(*t);
            return;
        }
        // Instance pattern (§8а, FX7-ASM-007): an instance from the tree — the source; origin and reference planes and
        // axes, an assembly sketch — the direction or axis.
        if (mode_ == Mode::InstancePattern && it) {
            const QString kind = it->data(0, FeatureTree::KindRole).toString();
            const std::string id = it->data(0, FeatureTree::IdRole).toString().toStdString();
            if (kind == "instance") instancePattern_->setSource(id);
            else if (kind != "origin")
                if (const auto t = treeTarget(kind, id, true)) pickedInInstancePattern(*t);
            return;
        }
        // Sketch base set (selection.md §5.2): a plane, an axis or the origin from the tree joins a started set.
        if (mode_ == Mode::PickPlane && it && sketchSelection().hasSet()) {
            const QString kind = it->data(0, FeatureTree::KindRole).toString();
            const std::string id = it->data(0, FeatureTree::IdRole).toString().toStdString();
            if (const auto t = treeTarget(kind, id, true)) pickedInPickPlane(*t, true);
            return;
        }
        if (mode_ != Mode::FeatureEdit || !it) return;
        // A plane in the tree while a panel field waits for a sketch — a sketch on it (§3.6а, FX1-FEAT-001).
        if (it->data(0, FeatureTree::KindRole).toString() == "plane" &&
            startBaseSketch({PickTarget::Kind::Plane, {}, it->data(0, FeatureTree::IdRole).toString().toStdString()}))
            return;
        if (it->data(0, FeatureTree::KindRole).toString() != "feature") return;
        const std::string id = it->data(0, FeatureTree::IdRole).toString().toStdString();
        const auto f = model_->session().document().document->active().find(id);
        if (f && f->type == doc::FeatureType::Plane && startBaseSketch({PickTarget::Kind::Plane, {}, id})) return;
        if (f && f->sketchData() && !panel_->editing()) panel_->setSketch(id);
    });
    connect(view_, &Viewport::cameraChanged, this, [this] {
        placeOverlays();
        // View scale changed (zoom, view area size) — re-tessellation after a pause (§11.6, FX3-PERF-002).
        if (std::abs(view_->camera().mmPerPx() - sceneMmPerPx_) > 1e-12 * std::max(1e-12, sceneMmPerPx_)) retessTimer_.start();
    });
    placeOverlays();
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(txt("ui.common.file"));
    file->addAction(txt("ui.common.new"), QKeySequence("Ctrl+N"), this, &MainWindow::cmdNew)->setObjectName("actionNew");
    file->addAction(txt("ui.main.open"), QKeySequence("Ctrl+O"), this, &MainWindow::cmdOpen)->setObjectName("actionOpen");
    recentMenu_ = file->addMenu(txt("ui.main.recentFiles"));
    recentMenu_->setObjectName("menuRecent");
    connect(recentMenu_, &QMenu::aboutToShow, this, [this] { rebuildRecentMenu(); });
    rebuildRecentMenu();
    file->addAction(txt("ui.common.save"), QKeySequence("Ctrl+S"), this, [this] { cmdSave(); })->setObjectName("actionSave");
    file->addAction(txt("ui.main.saveAs2"), QKeySequence("Ctrl+Shift+S"), this, [this] { cmdSaveAs(); })->setObjectName("actionSaveAs");
    // Library of standard parts (§8г): the active part as a library element; update of the document's copies.
    file->addAction(txt("ui.main.saveAsLibraryItem"), this, [this] { openLibrarySave(); })->setObjectName("actionSaveAsLibrary");
    // Library of standard methods (§8е): recording a solution on the active assembly as an example.
    file->addAction(txt("ui.main.recordSolution"), this, [this] { openSolutionRecorder(); })->setObjectName("actionRecordSolution");
    // Update links to other files (§8а) — immediately, without asking.
    file->addAction(txt("ui.main.updateReferences"), this, [this] {
        notifiedLinks_.clear();
        if (auto r = model_->session().refreshLinks(); !r) showToast(errorText(r.error()), true);
        else model_->noteRecomputeRequested();
    })->setObjectName("actionRefreshLinks");
    file->addAction(txt("ui.main.updateFromLibrary"), this, [this] { cmdUpdateFromLibrary(); })->setObjectName("actionLibraryUpdate");
    QMenu* exp = file->addMenu(txt("ui.common.export"));
    exp->setObjectName("menuExport");
    QAction* stl = exp->addAction("STL…", this, &MainWindow::cmdExportStl);
    stl->setObjectName("actionExportStl");
    QAction* tmf = exp->addAction("3MF…", this, [this] {  // S2-IO-005
        if (mode_ != Mode::Part || exportPending_) return;
        exportThreeMf_ = true;
        exportFlat_ = false;
        setMode(Mode::ExportStl);
    });
    tmf->setObjectName("actionExport3mf");
    // Flat pattern of sheet metal parts for laser cutting (S5-DXF-001, app-ux §10.3).
    exp->addAction(txt("ui.main.flatPatternDxf"), this, [this] {
        if (mode_ != Mode::Part || exportPending_) return;
        exportThreeMf_ = false;
        exportFlat_ = true;
        setMode(Mode::ExportStl);
    })->setObjectName("actionExportFlatDxf");
    // STEP (S2-IO-004, app-ux §10.3 menu table): export of the active object, import of bodies and assemblies.
    QAction* stepOut = exp->addAction("STEP…", this, [this] { cmdExportStep(); });
    stepOut->setObjectName("actionExportStep");
    // FreeCAD (S4A-UI-001, app-ux §10.1): the whole document or the active object; the exchange report after it.
    exp->addAction("FreeCAD (.FCStd)…", this, [this] { cmdExportFcstd(false); })->setObjectName("actionExportFcstd");
    exp->addAction(txt("ui.main.freecadActiveObjectFcstd"), this, [this] { cmdExportFcstd(true); })->setObjectName("actionExportFcstdActive");
    // Drawing sheet (S3-IO-001, S3-IO-002, S3-IO-003, app-ux §8б): only when a drawing is active.
    // PDF of a drawing — the choice of drawings first (FX7-DRW-005); printing — the same choice (FX7-DRW-004).
    exp->addAction("PDF…", this, [this] {
        if (drawingActive() || specificationActive()) showDrawingPick(PickPurpose::Pdf);  // FX8-DRW-005: with specifications
        else cmdExportSheet(io::SheetFileFormat::Pdf);
    })->setObjectName("actionExportPdf");
    exp->addAction(commandIcon("print"), txt("ui.main.print"), this, [this] { cmdPrint(); })->setObjectName("actionPrint");
    exp->addAction("SVG…", this, [this] { cmdExportSheet(io::SheetFileFormat::Svg); })->setObjectName("actionExportSvg");
    exp->addAction("DXF…", this, [this] {
        if (drawingActive()) showDrawingPick(PickPurpose::Dxf);  // drawings by sheets (FX11-DXF-001, FX11-CUT-006)
        else cmdExportSheet(io::SheetFileFormat::Dxf);
    })->setObjectName("actionExportDxf");
    QMenu* imp = file->addMenu(txt("ui.common.import"));
    imp->setObjectName("menuImport");
    QAction* stepIn = imp->addAction("STEP…", this, [this] { cmdImportStep(); });
    stepIn->setObjectName("actionImportStep");
    // STL and 3MF meshes (S4-MESH-001, app-ux §10.3 menu table): body from recognized surfaces or a polyhedral one.
    QAction* meshIn = imp->addAction("STL, 3MF…", this, [this] { cmdImportMesh(); });
    meshIn->setObjectName("actionImportMesh");
    // FreeCAD (S4A-UI-001): the file opens as a new document (like «Open»).
    imp->addAction("FreeCAD (.FCStd)…", this, [this] { cmdImportFcstd(); })->setObjectName("actionImportFcstd");
    file->addSeparator();
    QAction* settings = file->addAction(txt("ui.main.documentSettings"), this, &MainWindow::cmdDocumentSettings);
    settings->setObjectName("actionDocumentSettings");
    QAction* appSettings = file->addAction(txt("ui.main.programSettings"), this, [this] { showAppSettings(); });
    appSettings->setObjectName("actionAppSettings");
    file->addSeparator();
    QAction* exitAction = file->addAction(txt("ui.main.exit"), this, &QWidget::close);
    exitAction->setObjectName("actionExit");
    QMenu* edit = menuBar()->addMenu(txt("ui.main.edit"));
    QAction* undo = edit->addAction(txt("ui.common.undo"), this, &MainWindow::cmdUndo);
    undo->setObjectName("actionUndo");
    undo->setShortcuts({QKeySequence("Ctrl+Z")});
    QAction* redo = edit->addAction(txt("ui.common.redo"), this, &MainWindow::cmdRedo);
    redo->setObjectName("actionRedo");
    redo->setShortcuts({QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")});
    edit->addSeparator();
    QAction* rebuild = edit->addAction(txt("ui.common.rebuild"), this, [this] {
        if (model_->session().rebuild()) model_->noteRecomputeRequested();  // the same revision (FX14-QA-007)
    });
    rebuild->setObjectName("actionRebuild");
    rebuild->setShortcut(QKeySequence("Ctrl+Q"));
    // Shape healing and history recovery of an imported body (S4-REC-017, app-ux §8а).
    edit->addAction(txt("ui.common.recognizeDesign"), this, &MainWindow::cmdReconstruct)->setObjectName("actionRecognize");
    connect(edit, &QMenu::aboutToShow, this, &MainWindow::updateUndoState);
    // Standard views and «Normal to» — window actions without a menu (§3.3, §7.3; the «View»
    // menu is removed, FX8-UI-005): the views are in «Isometric ▾», «Normal to» — in the context bar
    // and the confirmation corner; the hotkeys work in any mode of the window.
    const std::pair<const char*, StandardView> views[] = {{txt("ui.common.front"), StandardView::Front}, {txt("ui.common.back"), StandardView::Back},
                                                          {txt("ui.common.left"), StandardView::Left},    {txt("ui.common.right"), StandardView::Right},
                                                          {txt("ui.common.top"), StandardView::Top},    {txt("ui.common.bottom"), StandardView::Bottom},
                                                          {txt("ui.common.isometric"), StandardView::Iso}};
    int k = 1;
    for (const auto& [name, sv] : views) {
        // A standard view fits the model — like «Isometric ▾» (§3.3, FX2-UI-001).
        auto* va = new QAction(QString::fromUtf8(name), this);
        va->setShortcut(QKeySequence(QString("Ctrl+%1").arg(k)));
        va->setObjectName(QString("actionMenuView%1").arg(k++));
        connect(va, &QAction::triggered, this, [this, sv = sv] { standardViewFit(sv); });
        addAction(va);
    }
    auto* normalTo = new QAction(txt("ui.main.normalTo"), this);
    normalTo->setShortcut(QKeySequence("Ctrl+8"));
    normalTo->setObjectName("actionNormalTo");
    connect(normalTo, &QAction::triggered, this, [this] { cmdNormalTo(); });
    addAction(normalTo);
    // Help (§2.1): «About» — a modeless window (§2.1а, S7-LIC-003); hotkeys — a topic of the help.
    QMenu* help = menuBar()->addMenu(txt("ui.common.help"));
    // «Help» (§2.1б, FX7-UI-007): the guide with screenshots and the command reference, a modeless window.
    QAction* helpAction = help->addAction(txt("ui.common.help"), this, [this] { helpWindow_->open(); });
    helpAction->setObjectName("actionHelp");
    QAction* about = help->addAction(txt("ui.main.about"), this, [this] {
        const QString& r = view_->glRenderer();
        aboutWindow_->open(aboutGlText(r, QString::fromLatin1(glAutoSoftwareVersion())));  // FX3-PERF-004: +auto-switch
    });
    about->setObjectName("actionAbout");
    // The hotkey table is a topic of the help tree (§2.1б, FX8-UI-003).
    QAction* keys = help->addAction(txt("ui.main.hotkeys"), this, [this] { helpWindow_->open("hotkeys"); });
    keys->setObjectName("actionHotkeys");
    // Problem report (§10.8, S4-BUG-001): a modeless window.
    help->addAction(txt("ui.main.reportAProblem"), this, [this] { reportWindow_->openManual(dataDir_); })->setObjectName("actionReportProblem");
    // Updating from the program (§10.10, REL-CT-002): the same check as the automatic one, results always reported.
    help->addAction(txt("ui.main.checkForUpdates"), this, [this] { cmdCheckUpdates(); })->setObjectName("actionCheckUpdates");
    // macOS application menu (S8-PKG-003, app-ux §10.1): «О программе», «Настройки программы…» and «Выход» move there
    // by role; no other item is moved by Qt's guess from its text (TextHeuristicRole: «Document settings…» in English).
    for (QAction* a : menuBar()->findChildren<QAction*>()) a->setMenuRole(QAction::NoRole);
    about->setMenuRole(QAction::AboutRole);
    appSettings->setMenuRole(QAction::PreferencesRole);
    exitAction->setMenuRole(QAction::QuitRole);
    // Ctrl+Enter — ✓ of the mode while it is available (§2.4, §7.5, S7F-RF-008).
    auto* ctrlEnter = new QShortcut(QKeySequence("Ctrl+Return"), this);
    connect(ctrlEnter, &QShortcut::activated, this, [this] {
        if (modeConfirm(mode_).accept != ConfirmAction::None && commandEnabled("confirmOk")) confirmAccept();
    });
}

}  // namespace construe::app
