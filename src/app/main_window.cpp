// Main window (spec/app-ux.md §2, §3, §4, §10).
#include "app/main_window.h"
#include "app/session_log.h"
#include "app/ui_clock.h"

#include "app/button_tips.h"
#include "app/document_hub.h"
#include "app/expr_field.h"
#include "app/foreground.h"

#include "app/part_tool.h"
#include "app/selection_field.h"
#include "api/sketch_ops.h"
#include "app/scene_builder.h"

#include "app/constraint_commands.h"
#include "app/hotkeys_table.h"
#include "app/icons.h"
#include "app/keys.h"
#include "app/messages.h"
#include "app/retranslate.h"
#include "app/theme.h"
#include "app/window_placement.h"

#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include "app/app_data.h"
#include "app/portable.h"
#include <QActionGroup>
#include <QCloseEvent>
#include <QDir>
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
#include <QTableWidget>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <iterator>
#include <numbers>
#include <tuple>

namespace construe::app {

// ---------------------------------------------------------------- window

MainWindow::MainWindow(bool testMode, QWidget* parent) : QMainWindow(parent), testMode_(testMode) {
    setWindowIcon(appIcon());  // window and taskbar icon (S4-APP-001)
    // Application settings (§10.6): QSettings — the registry, without installation an INI file (§10.5а); in test mode —
    // in memory only (§12.1).
    appSettings_ = std::make_unique<AppSettings>(testMode_ ? nullptr : openSettings(storageLayout()));
    setLanguage(settingLanguage());  // interface language before any widget (§9.5)
    // The model after the language: the first document and the name templates are in it (FX14-UI-004).
    model_ = std::make_unique<AppModel>();
    buildUi();
    ButtonTips::install();  // button tooltips (§2.1, FX9-UI-001)
    connect(model_.get(), &AppModel::documentChanged, this, &MainWindow::onDocumentChanged);
    connect(sketch_.get(), &SketchEditor::selectionPicked, this,
            [this](const QPointF& p) { scheduleContextBar(view_->mapTo(this, p.toPoint()), PickSource::View); });
    connect(tree_, &FeatureTree::selectionPicked, this,
            [this](const QPoint& p) { scheduleContextBar(tree_->viewport()->mapTo(this, p), PickSource::Tree); });
    connect(model_.get(), &AppModel::regenerated, this, &MainWindow::onRegenerated);
    connect(model_.get(), &AppModel::previewReady, this, [this](const QString& id, const api::PreviewResult& r) {
        if (id != previewId_) return;
        previewValid_ = r.status != regen::FeatureStatus::Error;
        QString msg;
        for (const auto& m : r.messages) msg += errorText(m.e, model_->session().document().document.get()) + "\n";
        panel_->setPreviewMessage(msg.trimmed(), r.status == regen::FeatureStatus::Error);
        rebuildScene();
    });
    connect(model_.get(), &AppModel::exportFinished, this, [this](const QString&, bool ok, const QString& msg) {
        exportPending_ = false;
        statusRegen_->clear();
        logSession(ok ? LogLevel::Info : LogLevel::Error,
                   QString(ok ? "export done: " : "export failed: ") + QDir::toNativeSeparators(property("exportPath").toString()));  // i18n-skip: log
        if (ok) showToast(text("ui.export.done", {{"file", QFileInfo(property("exportPath").toString()).fileName()}}));
        else showToast(msg, true);
        updateExportActions();
    });
    retessTimer_.setSingleShot(true);
    retessTimer_.setInterval(VIEW_RETESS_DEBOUNCE_MS);
    connect(&retessTimer_, &QTimer::timeout, this, [this] {
        // Meshes are rebuilt only if some shown mesh is coarser than allowed (§11.6, FX3-PERF-002).
        if (SceneBuilder::needsRetessellation(view_->bodies(), view_->camera().mmPerPx())) rebuildScene();
    });
    regenIndicator_.setSingleShot(true);
    regenIndicator_.setInterval(REGEN_INDICATOR_DELAY_MS);
    connect(&regenIndicator_, &QTimer::timeout, this, [this] {
        if (model_->regenPending()) statusRegen_->setText(QString::fromUtf8("⟳"));
    });
    if (testMode_) {
        explodeAnimMs_ = 0;  // animations are instant in test mode (§12.1)
        setFixedSize(TEST_WINDOW_W, TEST_WINDOW_H);
    } else {
        // Inside the available area of the screen (FX14-UI-002): the window has no screen of its own yet — the primary.
        const QScreen* screen = QGuiApplication::primaryScreen();
        placeOnScreen(screen ? screen->availableGeometry() : QRect(0, 0, WINDOW_DEFAULT_W, WINDOW_DEFAULT_H));
    }
    qApp->installEventFilter(this);
    // Hover and centering of the selection lists (§5.5а, S7F-UI-001); the previous window's hooks come back when this
    // one goes (a temporary window of a test).
    prevSelectionView_ = SelectionItems::view();
    SelectionItems::setView(this);
    onDocumentChanged();
    view_->fitAll();
}

MainWindow::~MainWindow() {
    if (SelectionItems::view() == this) SelectionItems::setView(prevSelectionView_);
    qApp->removeEventFilter(this);
    view_->setTool(nullptr);
    // The sheet text tool filters focus events of the window's widgets and reads the model: it goes before the model,
    // otherwise closing the widgets in the base destructor reaches a destroyed model (focus-out of the text field).
    delete texts_;
    texts_ = nullptr;
}

void MainWindow::placeOnScreen(const QRect& available) {
    // The real frame once the window is shown, the allowance before that (FX14-UI-002).
    QMargins frame(WINDOW_FRAME_ALLOWANCE_SIDE, WINDOW_FRAME_ALLOWANCE_TOP, WINDOW_FRAME_ALLOWANCE_SIDE, WINDOW_FRAME_ALLOWANCE_SIDE);
    if (isVisible() && frameGeometry().size() != geometry().size()) {
        const QRect f = frameGeometry(), g = geometry();
        frame = QMargins(g.left() - f.left(), g.top() - f.top(), f.right() - g.right(), f.bottom() - g.bottom());
    }
    const WindowPlacement p =
        initialWindowPlacement(available, QSize(WINDOW_DEFAULT_W, WINDOW_DEFAULT_H), QSize(WINDOW_MIN_W, WINDOW_MIN_H), frame);
    setMinimumSize(p.minimum);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    resize(p.size);
    move(p.pos);
    if (p.maximized) setWindowState(windowState() | Qt::WindowMaximized);
}

void MainWindow::setDisplayStyle(DisplayStyle s) {
    view_->setDisplayStyle(s);
    const QString n = styleName(s);
    for (QAction* a : styleActions_->actions())
        if (a->objectName() == "actionStyle" + n.left(1).toUpper() + n.mid(1)) a->setChecked(true);
    if (auto* b = buttons_["displayStyle"]) b->setIcon(commandIcon("style_" + n));
    appSettings_->set("view/displayStyle", n);
}

QAction* MainWindow::toolVariant(const QString& name) const {
    auto it = toolVariants_.find(name);
    return it != toolVariants_.end() ? it->second : nullptr;
}

QToolButton* MainWindow::ribbonButton(const QString& name) const {
    auto it = buttons_.find(name);
    return it != buttons_.end() ? it->second : nullptr;
}

// ---------------------------------------------------------------- modes

void MainWindow::setMode(Mode m) {
    // Trace of mode changes (app-ux §10.8, FX14-UI-003); names — as in the Mode enum.
    static const char* const modeNames[] = {"Part", "PickPlane", "FeatureEdit", "Sketch", "ExportStl", "Import", "Settings", "Properties", "Section", "Mate",
                                            "Weld", "Interference", "Annotation", "Library", "LibrarySave", "Solution", "SolutionRecord", "InstancePattern", "Repair"};
    static_assert(std::size(modeNames) == std::size(ALL_MODES));
    if (m != mode_) traceAction(QString("mode %1").arg(modeNames[int(m)]));  // i18n-skip: log
    if (mode_ == Mode::Mate && m != Mode::Mate) leftMate();  // §8а: series and highlight of mate objects
    // Leaving the wizard ends the editing of a connection (FX7-ASM-004): a recomputation of the same revision.
    if (mode_ == Mode::Solution && m != Mode::Solution && model_->session().connectionEditing()) {
        model_->session().setConnectionEditing(std::nullopt);
        model_->noteRecomputeRequested();
    }
    if (mode_ == Mode::Weld && m != Mode::Weld) leftWeld();  // §8а: highlight of weld edges
    if (mode_ == Mode::Repair && m != Mode::Repair) {  // §5.5в: the wizard's feature and picks
        repair_->clear();
        repairTargets_.clear();
        view_->setSelection({});
    }
    if (mode_ == Mode::InstancePattern && m != Mode::InstancePattern) leftInstancePattern();  // §8а: highlight
    if (mode_ == Mode::Annotation && m != Mode::Annotation) drawingRest_ = false;  // FX12-DRW-007: ✗ as well as ✓
    if (mode_ == Mode::FeatureEdit && m != Mode::FeatureEdit && panel_) panel_->clearEditing();  // app-ux §5
    if (mode_ != m) {  // the selection set lives within its mode (selection.md §2.4, FX7-SEL-001)
        if (sketchSel_) sketchSel_->clear();
        selField_ = nullptr;
    }
    mode_ = m;
    if (m != Mode::Part && tree_) tree_->setFaceFeature({});  // S7F-TREE-002: part mode only
    updateSketchPicks();
    updateSelectionUi();
    ctxTimer_.stop();
    ctx_->hide();
    const ModeLayout l = modeLayout(m, dynamic_cast<RevolvePanel*>(panel_) != nullptr);
    view_->setAxesVisible(l.axesVisible);  // origin axes — while the revolve panel with the "Axis" field is open (§3.4)
    ribbon_->setCurrentIndex(l.ribbonTab);
    // Button availability — updateAvailability (§7.5) from updateElementKindUi below.
    // A sheet mark on a drawing: the annotation panel is in the right panel, not under the tree (FX12-DRW-001).
    const bool sheetAnnotation = m == Mode::Annotation && drawingActive();
    hostSheetAnnotation(sheetAnnotation);
    props_->setVisible(l.propsVisible && !sheetAnnotation);
    if (m == Mode::Settings) {
        const auto d = model_->session().document().document;
        settingsPanel_->load(d->lengthUnit, d->angleUnit);  // current document units
        props_->setCurrentWidget(settingsPanel_);
    }
    if (m == Mode::FeatureEdit) props_->setCurrentWidget(panel_);
    if (m == Mode::Properties) {
        props_->setCurrentWidget(propertiesPanel_);
        propertiesPanel_->setFace(std::string());
        propertiesPanel_->refresh();
    }
    if (m == Mode::Section) props_->setCurrentWidget(section_);
    if (m == Mode::Annotation && !sheetAnnotation) props_->setCurrentWidget(annotation_);
    if (m == Mode::Mate) props_->setCurrentWidget(mate_);
    tree_->setPartConstructionShown(m == Mode::Mate);  // FX9-ASM-001, FX10-ASM-001: the whole mate mode
    if (m == Mode::Weld) props_->setCurrentWidget(weld_);
    if (m == Mode::Repair) props_->setCurrentWidget(repair_);  // §5.5в
    if (m == Mode::InstancePattern) props_->setCurrentWidget(instancePattern_);
    if (m == Mode::Interference) props_->setCurrentWidget(interferencePanel_);
    if (m != Mode::Interference && !highlight_.empty()) highlight_.clear();
    if (m == Mode::Import) props_->setCurrentWidget(importPanel_);
    if (m == Mode::LibrarySave) props_->setCurrentWidget(librarySavePanel_);
    // The right panel (FX8-BLT-004, S7F-UI-008): the connection wizard or the library of standard parts.
    solutionWizard_->setVisible(m != Mode::Library);
    libraryPanel_->setVisible(m == Mode::Library);
    solutionDock_->setVisible(l.rightPanel);
    if (l.rightPanel) openSidePanel(solutionDock_, RIGHT_PANEL_W);
    if (l.propsVisible && !sheetAnnotation) openSidePanel(left_, LEFT_PANEL_MIN_W);  // the properties panel opens under the tree
    if (m == Mode::SolutionRecord) props_->setCurrentWidget(solutionRecorder_);
    if (m == Mode::ExportStl) {
        props_->setCurrentWidget(exportPanel_);
        exportPanel_->setPrinterClearance(appSettings_->printClearance());  // S6-PRINT-001: from the settings on open
        exportPanel_->reset(exportFlat_ ? ExportPanel::Kind::FlatDxf : exportThreeMf_ ? ExportPanel::Kind::ThreeMf : ExportPanel::Kind::Stl);
    }
    if (props_->isVisible()) {
        const int h = left_->height();
        left_->setSizes({int(h * (1 - PROPS_HEIGHT_FRACTION)), int(h * PROPS_HEIGHT_FRACTION)});
    }
    tree_->setCommandsEnabled(l.treeCommands);
    tree_->setReorderEnabled(m == Mode::Part);
    view_->setTool(l.sketchTool ? static_cast<ViewportTool*>(sketch_.get()) : partTool_.get());
    view_->setPlanesVisible(l.planesVisible);
    view_->setHover({});
    if (!l.keepSelection) view_->setSelection({});
    setPrompt(waitHint());  // S7F-UI-006: every mode waiting for a pick says what to pick
    placeOverlays();
    updateStatus();
    updateUndoState();
    updateElementKindUi();
    if (drawingActive()) updateDrawingPanels();  // FX12-DRW-001: no drawing panel beside properties or settings
    rebuildScene();
}

void MainWindow::syncModeWithContext() {
    const auto ctx = model_->session().editContext();
    if (ctx.sketch && (mode_ != Mode::Sketch || sketch_->feature() != *ctx.sketch)) {
        sketch_->open(*ctx.sketch);
        setMode(Mode::Sketch);
    } else if (!ctx.sketch && mode_ == Mode::Sketch) {
        sketch_->close();
        basePanel_ = nullptr;  // sketch closed not by exit — the command started on a base is cancelled (§3.6а item 3)
        baseSketch_.clear();
        setMode(Mode::Part);
    }
}

void MainWindow::viewSelectionPicked(const QPoint& atInView) { scheduleContextBar(view_->mapTo(this, atInView), PickSource::View); }

void MainWindow::scheduleContextBar(const QPoint& atInWindow, PickSource source) {
    ctx_->hide();
    ctxPos_ = atInWindow;
    ctxSource_ = source;
    ctxTimer_.start();
}

ContextSelection MainWindow::contextSelection() const {
    ContextSelection s;
    if (mode_ == Mode::Sketch) {
        const auto& sel = sketch_->selection();
        const auto st = sketch_->state();
        const auto docPtr = model_->session().document().document;
        const auto f = docPtr->active().find(sketch_->feature());
        const auto* d = f ? f->sketchData() : nullptr;
        if (sel.empty() || !d || !st) return s;
        if (sel.size() == 1) {
            if (const auto* c = d->findConstraint(*sel.begin())) {
                s.kind = sketch::isDimensionType(c->type) ? ContextSelection::Kind::SketchDimension : ContextSelection::Kind::SketchConstraint;
                s.diametral = sketch_->diametralApplicable(c->id);
                return s;
            }
            // A weak (gray) dimension clicked in «Select» — the same dimension bar (FX9-SK-002).
            if (sketch_->isWeak(*sel.begin()) && sketch_->dimensionConstraint(*sel.begin())) {
                s.kind = ContextSelection::Kind::SketchDimension;
                s.weak = true;
                s.diametral = sketch_->diametralApplicable(*sel.begin());
                return s;
            }
        }
        sketch::SketchData sd = *d;
        sd.projections = st->projections;
        s.kind = ContextSelection::Kind::SketchEntities;
        s.constraints = api::sketchops::applicableConstraints(sd, std::vector<std::string>(sel.begin(), sel.end()));
        return s;
    }
    if (mode_ != Mode::Part) return s;
    if (ctxSource_ == PickSource::Tree) {
        if (const auto id = tree_->selectedFeature()) {
            const auto f = model_->session().document().document->active().find(*id);
            s.kind = f && f->sketchData() ? ContextSelection::Kind::Sketch : ContextSelection::Kind::Feature;
        } else if (tree_->selectedPlane()) {
            s.kind = ContextSelection::Kind::Plane;
        }
        return s;
    }
    const auto& sel = view_->selection();
    if (sel.size() == 1 && sel.begin()->kind == PickTarget::Kind::Plane) s.kind = ContextSelection::Kind::Plane;
    // One cylindrical face — its diameter tolerance (FX12-PMI-004; the «Size tolerance» ribbon button is gone).
    if (sel.size() == 1 && sel.begin()->kind == PickTarget::Kind::Face && !drawingActive()) {
        // In an assembly — the instance face reference «<path>/<face>» (FX12-PMI-003).
        std::string ref = sel.begin()->name;
        if (assemblyActive()) {
            const auto slash = sel.begin()->body.rfind('/');
            ref = slash == std::string::npos ? std::string() : sel.begin()->body.substr(0, slash) + "/" + ref;
        }
        const auto ti = model_->session().topologyInfo(ref, api::StateRef::final());
        if (ti && ti->surface && ti->surface->kind == kernel::SurfaceInfo::Kind::Cylinder) s.kind = ContextSelection::Kind::CylinderFace;
    }
    return s;
}

void MainWindow::runContextCommand(const QString& c) {
    ctx_->hide();
    if (c == "sketch") cmdSketch();
    else if (c == "normalTo") cmdNormalTo();
    else if (c == "extrude") cmdExtrude();
    else if (c == "revolve") cmdRevolve();
    else if (c == "pmiFit") cmdAnnotation("fit");  // preselected cylindrical face (FX12-PMI-004)
    else if (c == "editSketch") {
        if (const auto id = tree_->selectedFeature()) editSketch(*id);
    } else if (c == "editFeature") {
        if (const auto id = tree_->selectedFeature()) editFeature(*id);
    } else if (c == "suppress") tree_->toggleSuppressSelected();
    else if (c == "rename") tree_->renameSelected();
    else if (c == "delete") {
        if (mode_ == Mode::Sketch) sketch_->deleteSelection();
        else tree_->deleteSelectedFeatures();
    } else if (c.startsWith("constraint_")) sketch_->applyConstraint(c.mid(int(sizeof("constraint_")) - 1).toStdString());
    else if (c == "dimension") sketch_->startDimensionFromSelection();
    else if (c == "construction") sketch_->toggleConstruction();
    else if (c == "editValue" || c == "toggleDriven" || c == "toggleDiametral") {
        const auto& sel = sketch_->selection();
        if (sel.size() != 1) return;
        if (c == "editValue") sketch_->editDimension(*sel.begin());
        else if (c == "toggleDiametral") sketch_->toggleDiametral(*sel.begin());
        else sketch_->toggleDriven(*sel.begin());
    }
}

void MainWindow::onDocumentChanged() {
    ctx_->hide();
    // Another active element ends editing in context (S7F-ASM-005).
    if (!contextPart_.empty() && model_->session().document().document->activeElement != contextPart_) contextPart_.clear(), contextAssembly_.clear();
    updateUndoState();
    updateElementKindUi();
    syncModeWithContext();
    if (mode_ == Mode::Sketch) sketch_->refresh();
    tree_->rebuild(mode_ == Mode::FeatureEdit ? panel_->editing() : std::nullopt);
    variables_->rebuild();
    updateTitle();
    updateStatus();
    updateUnitsStatus();
    regenIndicator_.start();
    // A new revision cancelled the preview (core-api §4.5) — request it again over the current document.
    if (mode_ == Mode::FeatureEdit && panel_ && panel_->canAccept()) {
        previewValid_ = false;
        requestPreview();
    }
    refreshAnnotations();
}

void MainWindow::logRegenerationMessages() {
    const auto pr = model_->result();
    if (!pr) return;
    std::set<QString> now;
    for (const auto& [id, fr] : pr->featureStatus)
        for (const auto& m : fr.messages)
            now.insert(QString("rebuild %1: %2 %3 — %4")  // i18n-skip: log
                           .arg(m.error ? "error" : "warning", QString::fromStdString(id), QString::fromStdString(m.e.code),
                                errorText(m.e, model_->session().document().document.get())));
    for (const QString& line : now)
        if (!loggedRegenMessages_.count(line)) logSession(line.startsWith("rebuild error") ? LogLevel::Error : LogLevel::Warning, line);
    loggedRegenMessages_ = std::move(now);
}

void MainWindow::onRegenerated() {
    const BusyScope busy;  // applying the result in the interface thread (§9.4)
    logRegenerationMessages();  // FX14-UI-003: new rebuild errors and warnings go to the session log
    if (auto pr = model_->result(); pr && pr->revision >= committedRevision_) committedMotions_.clear();  // S7F-ASM-001
    statusRegen_->clear();
    tree_->rebuild(mode_ == Mode::FeatureEdit ? panel_->editing() : std::nullopt);
    variables_->rebuild();
    if (mode_ == Mode::Sketch) sketch_->refresh();
    if (mode_ == Mode::Properties) propertiesPanel_->refresh();  // mass properties after an edit (§5.10)
    if (drawingActive()) refreshDrawing();  // models and view projections have been computed (§8б, core-api §5.4)
    if (specificationActive()) refreshSpecification();  // the fastener rows of the assembly's connections (FX14-SPEC-001)
    checkMateSeries();  // mate series: the inserted part is fully defined — exit (§8а, FX1-ASM-002)
    if (mode_ == Mode::Mate && pullInstance_.empty()) mateChanged();  // the preview over the new placements (FX9-ASM-003)
    if (explodePending_) startExplodeAnimation();  // FX8-ASM-006
    refreshAnnotations();
    updateAvailability();  // availability depends on the regeneration result: whether there is a body (§7.5)
    // New lost references — the repair wizard instead of an error notification (§5.5в, S7F-FEAT-003).
    const std::set<std::string> silent = updateLostReferences();
    // New feature errors — notification with the feature name (§9.1, S1-UX-002).
    if (auto pr = model_->result()) {
        const auto d = model_->session().document().document;
        std::map<std::string, bool> nowError;
        for (const auto& [id, fr] : pr->featureStatus) {
            const bool err = fr.status == regen::FeatureStatus::Error;
            nowError[id] = err;
            if (err && !featureErrors_.count(id) && !fr.messages.empty() && !silent.count(id)) {
                const auto f = d->active().find(id);
                // The «Edit» action opens the feature panel (§5, OK on preview error).
                std::vector<ToastAction> acts;
                if (f && !f->sketchData()) acts.push_back({text("action.edit"), [this, id = id] { editFeature(id); }});
                showToast(text("ui.feature.newError", {{"feature", QString::fromStdString(f ? f->name : id)},
                                                       {"message", errorText(fr.messages.front().e, d.get())}}),
                          true, std::move(acts));
            }
        }
        featureErrors_.clear();
        for (const auto& [id, err] : nowError)
            if (err) featureErrors_.insert(id);
    }
    rebuildScene();
}

void MainWindow::updateTitle() {
    const auto info = model_->session().document();
    const auto p = model_->session().path();
    const QString name = p ? QFileInfo(QString::fromStdString(*p)).fileName() : QString(txt("ui.main.untitled"));
    setWindowTitle("construe — " + name + (info.modified ? "*" : ""));
}

QString MainWindow::dofText() const { return statusDof_->text(); }

void MainWindow::updateStatus() {
    syncSketchToolButtons();  // the current tool's button is checked (§2.3, FX1-UI-003)
    if (QAction* a = toolVariant("showConstraints")) a->setChecked(sketch_->constraintsShown());  // FX7-SK-014
    // The «Sides» / «Rays» fields — in the left panel while the polygon / star tool is active (app-ux §6.3, FX10-SK-003).
    if (mode_ == Mode::Sketch) {
        const bool polygon = sketch_->tool() == SketchEditor::Tool::Polygon, star = sketch_->tool() == SketchEditor::Tool::Star;
        if (polygon || star) {
            sketchToolTitle_->setText(txt(polygon ? "ui.primitive.nGon" : "ui.common.star"));
            sketchToolForm_->setRowVisible(polygonSides_, polygon);
            sketchToolForm_->setRowVisible(starRays_, star);
            props_->setCurrentWidget(sketchToolPanel_);
            if (!props_->isVisible()) {
                props_->show();
                openSidePanel(left_, LEFT_PANEL_MIN_W);
                const int h = left_->height();
                left_->setSizes({int(h * (1 - PROPS_HEIGHT_FRACTION)), int(h * PROPS_HEIGHT_FRACTION)});
            }
        } else if (props_->isVisible() && props_->currentWidget() == sketchToolPanel_) {
            props_->hide();
        }
    }
    if (mode_ == Mode::Sketch) {
        statusDof_->setText(sketch_->dofText());
        statusDof_->setStyleSheet(QString("color: %1;").arg(sketch_->dofColor().name()));
    } else {
        statusDof_->clear();
    }
    updateConflictPanel();
    // «Unlink» depends on the sketch selection (§7.5, S4-SKT-005).
    if (QAction* a = toolVariant("sketchUnlink")) a->setEnabled(commandAvailable("tool_sketchUnlink", availabilityState()));
}

void MainWindow::dofClicked() {
    const auto st = mode_ == Mode::Sketch ? sketch_->state() : std::nullopt;
    if (!st) return;
    switch (st->result.status) {
        case sketch::SketchStatus::Conflict:
        case sketch::SketchStatus::OverConstrainedRedundant:
            // Open (redundant) or expand the collapsed panel (§6.10, §6.12 item 4).
            conflictPanel_->openFromIndicator(st, sketch_->feature());
            break;
        case sketch::SketchStatus::NotConverged:
        case sketch::SketchStatus::Error:
            if (!st->result.messages.empty()) showToast(errorText(st->result.messages.front()), true);
            break;
        default: break;
    }
}

void MainWindow::updateConflictPanel() {
    conflictPanel_->refresh(mode_ == Mode::Sketch ? sketch_->state() : std::nullopt, sketch_->feature());
}

void MainWindow::placeOverlays() {
    const ModeLayout l = modeLayout(mode_, dynamic_cast<RevolvePanel*>(panel_) != nullptr);
    confirmOk_->setVisible(l.confirmOk);
    confirmCancel_->setVisible(l.confirmCancel);
    QToolButton* normal = buttons_["normalTo"];
    normal->setVisible(mode_ == Mode::Sketch);
    const int x = view_->width() - CONFIRM_CORNER_MARGIN_PX - CONFIRM_CORNER_BTN_PX;
    if (!l.confirmCancel) confirmOk_->move(x, CONFIRM_CORNER_MARGIN_PX);
    else {
        confirmCancel_->move(x, CONFIRM_CORNER_MARGIN_PX);
        confirmOk_->move(x - CONFIRM_CORNER_BTN_PX - 4, CONFIRM_CORNER_MARGIN_PX);
    }
    normal->move(confirmOk_->x() - CONFIRM_CORNER_BTN_PX - 4, CONFIRM_CORNER_MARGIN_PX);
    toasts_->relayout();
    if (busy_) busy_->relayout();
}

void MainWindow::standardViewFit(StandardView v) {
    // Standard view and fit the visible geometry — in a single transition (§2.4, §3.3; FX1-UI-008, FX2-UI-001).
    Camera c = view_->targetCamera();
    c.setStandard(v);
    view_->animateTo(view_->fitted(c));
}

void MainWindow::openSidePanel(QWidget* panel, int minWidth) {
    if (!split_ || !panel) return;
    const int i = split_->indexOf(panel), from = split_->indexOf(center_);
    if (i < 0 || from < 0 || split_->width() <= 0) return;
    const QList<int> cur = split_->sizes();
    const std::vector<int> want = openPanelSizes(std::vector<int>(cur.begin(), cur.end()), size_t(i), minWidth, size_t(from));
    if (QList<int>(want.begin(), want.end()) != cur) split_->setSizes(QList<int>(want.begin(), want.end()));
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    placeOverlays();
}

void MainWindow::showToast(const QString& t, bool error, std::vector<ToastAction> actions) {
    // A conditional message closes once it lost its sense (S7F-UI-003, app-ux §9.1 «Removal»).
    toasts_->post(t, error, std::move(actions), toastLapseOf(t), toastState());
}

ToastState MainWindow::toastState() const {
    ToastState s;
    auto& ses = model_->session();
    s.sketchOpen = ses.editContext().sketch.has_value();
    if (const auto d = ses.document().document) s.active = d->active().id;
    s.revision = ses.revision();
    s.mode = int(mode_);
    s.hasPath = ses.path().has_value();
    return s;
}

QStringList MainWindow::toasts() const { return toasts_->texts(); }
QStringList MainWindow::toastHistory() const { return toasts_->history(); }

// ---------------------------------------------------------------- scene

SceneRequest MainWindow::sceneRequest() const {
    SceneRequest r;
    if (mode_ == Mode::Sketch) {
        r.mode = SceneRequest::Mode::Sketch;
        r.sketch = sketch_->feature();
    } else if (mode_ == Mode::FeatureEdit) {
        r.mode = SceneRequest::Mode::FeatureEdit;
        r.editing = panel_->editing();
        r.operation = panel_->operation();
        if (previewValid_ && !previewId_.isEmpty()) r.preview = previewId_.toStdString();
    } else if (mode_ == Mode::Repair && !repair_->feature().empty() &&
               !model_->session().document().document->active().isAssembly()) {  // the state before the feature (§5.5в)
        r.mode = SceneRequest::Mode::FeatureEdit;
        r.editing = repair_->feature();
    }
    r.profilesPanel = mode_ == Mode::FeatureEdit && panel_ && panel_->picksRegions();
    if (r.profilesPanel) r.panelSketch = panel_->sketch();
    r.mmPerPx = view_->camera().mmPerPx();
    if (auto pr = model_->result()) r.revision = pr->revision;
    r.highlight = highlight_;
    r.partPlanes = mode_ == Mode::Mate;  // FX10-ASM-001
    if (!dragInstance_.empty()) {
        r.dragMotions = dragSolved_;
        if (!r.dragMotions.count(dragInstance_)) r.dragMotions[dragInstance_] = dragMotion_;
    } else if (!pullInstance_.empty() || !pullGroup0_.empty()) {
        r.dragMotions = dragSolved_;  // free-degree manipulator (FX8-ASM-003), group manipulator (FX13-ASM-002)
    } else if (!committedMotions_.empty()) {
        r.dragMotions = committedMotions_;  // released placements until their regeneration is applied (S7F-ASM-001)
    } else if (mode_ == Mode::Mate && !matePreview_.empty()) {
        r.dragMotions = matePreview_;  // mate preview (FX8-ASM-001)
    } else if (!explodeShown_.empty()) {
        r.dragMotions = explodeShown_;  // exploded view transition frame (FX8-ASM-006)
    }
    // Fasteners following their attachments in the exploded view (assembly.md §7д.5, FX10-ASM-004) move with them.
    if (!r.dragMotions.empty())
        if (const auto a = model_->session().assembly(); a && a->exploded)
            for (const auto& [f, att] : a->explodedFollow) {
                const auto it = r.dragMotions.find(att);
                const auto* ae = a->find(att);
                const auto* fe = a->find(f);
                if (r.dragMotions.count(f) || it == r.dragMotions.end() || !ae || !fe) continue;
                r.dragMotions[f] = it->second * regen::Motion3{ae->world.R.transpose(), -(ae->world.R.transpose() * ae->world.t)} * fe->world;
            }
    // Sections are active in every mode (S7F-VIEW-001): only the tree eye turns them on and off.
    if (mode_ == Mode::Section) {
        const api::Json def = section_->definition();
        r.sections = SceneBuilder::viewSections(model_->session().sections(), section_->editing(), &def);
    } else {
        r.sections = SceneBuilder::viewSections(model_->session().sections(), std::nullopt, nullptr);
    }
    r.shownSketches = tree_->shownSketches();  // tree eye (§3.4, FX1-UI-009)
    if (!hoverSketch_.empty() && std::find(r.shownSketches->begin(), r.shownSketches->end(), hoverSketch_) == r.shownSketches->end())
        r.shownSketches->push_back(hoverSketch_);  // the sketch hovered in the tree (FX7-PT-011)
    r.shownBases = tree_->shownBases();        // base body eye (§3.4, S4-REC-009)
    r.unfolded = unfolded_ && mode_ == Mode::Part;  // flat pattern display (§7.4, S5-FLAT-001)
    return r;
}

void MainWindow::rebuildScene() {
    const BusyScope busy;  // §9.4
    auto pr = model_->result();
    if (!pr) return;
    sceneMmPerPx_ = view_->camera().mmPerPx();
    updateSketchPicks();  // solved positions of the sketch elements may have changed (FX7-SEL-001)
    // The request is gathered here (widgets); the build runs in a worker thread while the stripe runs (§9.4, FX13-UI-001).
    auto request = sceneRequest();
    auto scene = busyRun([&] { return SceneBuilder(model_->session()).build(request); });
    view_->setPlaneHalf(scene.planeHalf);
    view_->setReferenceGeometry(std::move(scene.refPlanes), std::move(scene.refAxes),
                                mode_ == Mode::FeatureEdit && (dynamic_cast<RefGeomPanel*>(panel_) != nullptr || dynamic_cast<PatternPanel*>(panel_) != nullptr));
    view_->setNestedReference(std::move(scene.nested));  // FX9-ASM-008
    // Non-triangulated faces of shown bodies — a warning, at most once per revision (§9.1).
    if (scene.failedFaces > 0 && partialTessWarnedRevision_ != pr->revision) {
        partialTessWarnedRevision_ = pr->revision;
        showToast(errorText(Error("KERNEL_TESSELLATION_PARTIAL").with("actual", double(scene.failedFaces))));
    }
    if (mode_ == Mode::Repair)  // ghosts of the lost references over the bodies (§5.5в)
        for (auto& g : repairGhosts()) scene.bodies.push_back(std::move(g));
    view_->setBodies(std::move(scene.bodies));
    faceWeldLines_ = std::move(scene.weldLines);
    updateWeldLines();
    sceneRevision_ = pr->revision;
    updateRegions();
}

void MainWindow::syncFaceFeature() {
    // Part mode: the first selected face → its base name → the feature that created it (S7F-TREE-002).
    std::string feature;
    if (mode_ == Mode::Part)
        if (auto pr = model_->result(); pr && pr->final) {
            const auto d = model_->session().document().document;  // held: active() refers into it
            const auto& el = d->active();
            for (const auto& t : view_->selection()) {
                if (t.kind != PickTarget::Kind::Face) continue;
                for (const auto& fc : pr->final->names.faces)
                    if (fc.full == t.name && pr->final->bodies.at(size_t(fc.body)).id == t.body) {
                        feature = faceCreator(el, fc.base);
                        break;
                    }
                if (!feature.empty()) break;
            }
        }
    tree_->setFaceFeature(feature);
}

void MainWindow::treeFeatureHovered(const std::string& id) {
    // Part mode only: the hovered feature's faces and edges (stable names with the feature's base) are drawn as hovered,
    // a hovered sketch is shown (§4.4, FX7-PT-011).
    std::set<PickTarget> targets;
    std::string sketch;
    if (mode_ == Mode::Part && !id.empty()) {
        const auto f = model_->session().document().document->active().find(id);
        if (f && f->sketchData()) sketch = id;
        else if (f) targets = featureGeometry(id);
    }
    view_->setTreeHover(targets);
    if (sketch != hoverSketch_) {
        hoverSketch_ = sketch;
        updateRegions();
    }
}

std::set<PickTarget> MainWindow::featureGeometry(const std::string& id) const {
    std::set<PickTarget> targets;
    auto pr = model_->result();
    if (!pr || !pr->final || id.empty()) return targets;
    const std::string key = id + ".";
    auto own = [&](const std::string& full) {
        for (size_t p = full.find(key); p != std::string::npos; p = full.find(key, p + 1))
            if (p == 0 || !std::isalnum(static_cast<unsigned char>(full[p - 1]))) return true;
        return false;
    };
    const auto& names = pr->final->names;
    for (const auto& fc : names.faces)
        if (own(fc.base)) targets.insert({PickTarget::Kind::Face, pr->final->bodies.at(size_t(fc.body)).id, fc.full});
    for (const auto& e : names.edges) {
        const auto& a = names.faces.at(size_t(e.f1));
        const auto& b = names.faces.at(size_t(e.f2));
        if (own(a.base) && own(b.base)) targets.insert({PickTarget::Kind::Edge, pr->final->bodies.at(size_t(e.body)).id, e.full});
    }
    return targets;
}

void MainWindow::updateRegions() {
    const SceneRequest req = sceneRequest();
    view_->setRegions(SceneBuilder(model_->session()).regions(req), req.profilesPanel);
    if (req.profilesPanel) {
        // Regions chosen in the «Profiles» field are highlighted as selected (§5.5).
        std::set<PickTarget> sel;
        for (const auto& t : view_->selection())
            if (t.kind == PickTarget::Kind::Edge) sel.insert(t);
        if (panel_->regions())
            for (const auto& rid : *panel_->regions()) sel.insert({PickTarget::Kind::Region, panel_->sketch(), rid});
        view_->setSelection(sel);
    }
}

// ---------------------------------------------------------------- commands

bool MainWindow::confirmDiscard() {
    if (!model_->session().document().modified) return true;
    const auto p = model_->session().path();
    const QString name = p ? QFileInfo(QString::fromStdString(*p)).fileName() : QString(txt("ui.main.untitled"));
    QMessageBox box(QMessageBox::Question, "construe", text("ui.file.unsaved.question", {{"file", name}}),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.button(QMessageBox::Save)->setText(txt("ui.common.save"));
    box.button(QMessageBox::Discard)->setText(txt("ui.main.donTSave"));
    box.button(QMessageBox::Cancel)->setText(txt("ui.common.cancel"));
    box.setDefaultButton(QMessageBox::Save);
    const int r = box.exec();
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return cmdSave();
    return true;
}

void MainWindow::cmdNew() {
    if (mode_ == Mode::Repair) return;  // the repair wizard (§5.5в)
    if (mode_ == Mode::FeatureEdit || mode_ == Mode::ExportStl || mode_ == Mode::Import || mode_ == Mode::Settings || mode_ == Mode::Section || mode_ == Mode::Annotation || mode_ == Mode::Mate || mode_ == Mode::Weld || mode_ == Mode::Interference || mode_ == Mode::Library || mode_ == Mode::LibrarySave || mode_ == Mode::Solution || mode_ == Mode::SolutionRecord || mode_ == Mode::InstancePattern) return;
    if (!confirmDiscard()) return;
    sketch_->close();
    setMode(Mode::Part);
    model_->session().newDocument();
    tree_->resetViewState();
    dropRecovery();
    Camera c = view_->camera();
    c.setStandard(StandardView::Iso);
    view_->setCamera(c);
    view_->fitAll();
}

void MainWindow::cmdOpen() {
    if (mode_ == Mode::Repair) return;  // the repair wizard (§5.5в)
    if (mode_ == Mode::FeatureEdit || mode_ == Mode::ExportStl || mode_ == Mode::Import || mode_ == Mode::Settings || mode_ == Mode::Section || mode_ == Mode::Annotation || mode_ == Mode::Mate || mode_ == Mode::Weld || mode_ == Mode::Interference || mode_ == Mode::Library || mode_ == Mode::LibrarySave || mode_ == Mode::Solution || mode_ == Mode::SolutionRecord || mode_ == Mode::InstancePattern) return;
    if (!confirmDiscard()) return;
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    const QString path = QFileDialog::getOpenFileName(this, txt("ui.common.open"), {}, txt("ui.main.documentsConstrueFcstdConstrueDocument"),
                                                      nullptr, opt);
    if (path.isEmpty()) return;
    openPath(path);
}

void MainWindow::openPath(const QString& path) {
    if (path.endsWith(".fcstd", Qt::CaseInsensitive)) return openFcstdPath(path);  // FreeCAD document (S4A-UI-001)
    auto s = model_->session().openDocument(path.toStdString());
    if (!s) {
        showToast(errorText(s.error(), nullptr, path), true);
        if (!dataDir_.isEmpty() && s.error().code == "IO_READ") saveRecent(dataDir_, removeRecent(loadRecent(dataDir_), path));
        return;
    }
    tree_->resetViewState();
    noteFileUsed(path);
    logSession(LogLevel::Info, "file opened: " + QDir::toNativeSeparators(path));  // i18n-skip: log
    sketch_->close();
    setMode(Mode::Part);
    view_->fitAll();
}

void MainWindow::openFromCommandLine(const QStringList& arguments) {
    const QString path = commandLineDocument(arguments);
    if (!path.isEmpty()) openPath(path);
}

void MainWindow::openFromSystem(const QString& path) {
    if (!testMode_) bringToFront(this);
    if (mode_ == Mode::FeatureEdit || mode_ == Mode::ExportStl || mode_ == Mode::Import || mode_ == Mode::Settings || mode_ == Mode::Section || mode_ == Mode::Annotation || mode_ == Mode::Mate || mode_ == Mode::Weld || mode_ == Mode::Interference || mode_ == Mode::Library || mode_ == Mode::LibrarySave || mode_ == Mode::Solution || mode_ == Mode::SolutionRecord || mode_ == Mode::InstancePattern) return;
    if (!confirmDiscard()) return;
    openPath(path);
}

void MainWindow::noteFileUsed(const QString& path) {
    dropRecovery();
    if (dataDir_.isEmpty()) return;
    saveRecent(dataDir_, addRecent(loadRecent(dataDir_), path));
}

void MainWindow::dropRecovery() {
    if (dataDir_.isEmpty()) return;
    removeRecovery(dataDir_, QCoreApplication::applicationPid());
    autosavedRevision_ = 0;
}

void MainWindow::rebuildRecentMenu() {
    recentMenu_->clear();
    const QStringList list = dataDir_.isEmpty() ? QStringList() : loadRecent(dataDir_);
    if (list.isEmpty()) {
        QAction* none = recentMenu_->addAction(text("ui.recent.empty"));
        none->setObjectName("recentEmpty");  // stub, not a command
        none->setEnabled(false);
        none->setMenuRole(QAction::NoRole);
        return;
    }
    for (int i = 0; i < list.size(); ++i) {
        const QString p = list[i];
        QAction* a = recentMenu_->addAction(QFileInfo(p).fileName(), this, [this, p] {
            if (mode_ == Mode::FeatureEdit || mode_ == Mode::ExportStl || mode_ == Mode::Import || mode_ == Mode::Settings || mode_ == Mode::Section || mode_ == Mode::Annotation || mode_ == Mode::Mate || mode_ == Mode::Weld || mode_ == Mode::Interference || mode_ == Mode::Library || mode_ == Mode::LibrarySave || mode_ == Mode::Solution || mode_ == Mode::SolutionRecord || mode_ == Mode::InstancePattern) return;
            if (!confirmDiscard()) return;
            openPath(p);
        });
        a->setObjectName(QString("recent_%1").arg(i));
        a->setMenuRole(QAction::NoRole);  // a file name is never an application-menu item on macOS (§10.1)
        a->setToolTip(p);
        if (ribbon_) a->setEnabled(commandEnabled(a->objectName()));  // §7.5: menu is rebuilt when opened
    }
}

void MainWindow::autosaveNow() {
    if (dataDir_.isEmpty()) return;
    const auto info = model_->session().document();
    if (!info.modified) return;
    if (info.revision == autosavedRevision_) return;
    const auto path = model_->session().path();
    const std::optional<QString> original = path ? std::optional<QString>(QString::fromStdString(*path)) : std::nullopt;
    if (writeRecovery(dataDir_, QCoreApplication::applicationPid(), QByteArray::fromStdString(model_->session().documentText()), original,
                      QDateTime::currentDateTime()))
        autosavedRevision_ = info.revision;
}

void MainWindow::checkCrashReport() {
    if (auto m = readCrashMarker(dataDir_)) reportWindow_->openCrash(dataDir_, *m);
}

void MainWindow::autoCheckUpdates() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!updateCheckDue(appSettings_->value(UPDATE_LAST_CHECK_KEY).toString(), now)) return;
    if (updateWindow_->check(false)) appSettings_->set(UPDATE_LAST_CHECK_KEY, updateCheckStamp(now));
}

void MainWindow::cmdCheckUpdates() {
    if (updateWindow_->check(true)) appSettings_->set(UPDATE_LAST_CHECK_KEY, updateCheckStamp(QDateTime::currentDateTimeUtc()));
}

// update.md §5 «Installation»: an installer (setup) is launched, a disk image (dmg) is opened.
void MainWindow::installUpdate(const QString& path) {
    finishUpdate(installAction(packageFormat()).value_or(UpdateAction::RunInstaller), path);
}

void MainWindow::finishUpdate(UpdateAction action, const QString& path) {
    if (!confirmDiscard()) {
        updateWindow_->installNotStarted(text("ui.update.installCancelled"));
        return;
    }
    const QString shown = QDir::toNativeSeparators(path);
    switch (runUpdateAction(action, path)) {
        case LaunchResult::Unsupported: updateWindow_->installNotStarted(text("ui.update.windowsOnly", {{"path", shown}})); return;
        case LaunchResult::Failed:
            updateWindow_->installNotStarted(text(action == UpdateAction::OpenDiskImage ? "ui.update.openFailed" : "ui.update.launchFailed", {{"path", shown}}));
            return;
        case LaunchResult::Launched: break;
    }
    if (testMode_) {  // the test window stays open (§12.1)
        updateExitRequested_ = true;
        return;
    }
    closeConfirmed_ = true;
    if (!close()) closeConfirmed_ = false;
}

void MainWindow::checkRecovery() {
    if (dataDir_.isEmpty()) return;
    const auto r = findRecovery(dataDir_, QCoreApplication::applicationPid());
    if (!r) return;
    const QString file = r->original ? QFileInfo(*r->original).fileName() : text("ui.recovery.newDocument");
    const QString dir = dataDir_;
    const Recovery rec = *r;
    showToast(text("ui.recovery.found", {{"file", file}, {"time", rec.savedAt.toString("dd.MM.yyyy HH:mm")}}), true,
              {{txt("ui.main.restore"), [this, dir, rec] {
                    if (!confirmDiscard()) return;
                    const std::optional<std::string> orig = rec.original ? std::optional<std::string>(rec.original->toStdString()) : std::nullopt;
                    auto s = model_->session().openRecovery(rec.copyPath.toStdString(), orig);
                    if (!s) {
                        showToast(errorText(s.error(), nullptr, rec.copyPath), true);
                        return;
                    }
                    removeRecovery(dir, rec.pid);
                    tree_->resetViewState();
                    sketch_->close();
                    setMode(Mode::Part);
                    view_->fitAll();
                    updateTitle();
                }},
               {txt("ui.common.delete"), [dir, rec] { removeRecovery(dir, rec.pid); }}});
}

bool MainWindow::cmdSave() {
    if (!model_->session().path()) return cmdSaveAs();
    auto s = model_->session().saveDocument();
    if (!s) {
        showToast(errorText(s.error()) + " " + text("ui.file.notSaved"), true);
        return false;
    }
    noteFileUsed(QString::fromStdString(*model_->session().path()));
    logSession(LogLevel::Info, "file saved: " + QDir::toNativeSeparators(QString::fromStdString(*model_->session().path())));  // i18n-skip: log
    updateTitle();
    emit DocumentHub::instance().saved(QString::fromStdString(*model_->session().path()));
    updateAvailability();  // the document got a path — «From file…» (§7.5)
    return true;
}

bool MainWindow::cmdSaveAs() {
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    QString path = QFileDialog::getSaveFileName(this, txt("ui.main.saveAs"), {}, txt("ui.common.construeDocumentConstrue"), nullptr, opt);
    if (path.isEmpty()) return false;
    if (!path.endsWith(".construe")) path += ".construe";
    auto s = model_->session().saveDocument(path.toStdString());
    if (!s) {
        showToast(errorText(s.error(), nullptr, path) + " " + text("ui.file.notSaved"), true);
        return false;
    }
    noteFileUsed(path);
    logSession(LogLevel::Info, "file saved: " + QDir::toNativeSeparators(path));  // i18n-skip: log
    updateTitle();
    emit DocumentHub::instance().saved(path);
    updateAvailability();  // the document got a path — «From file…» (§7.5)
    return true;
}

void MainWindow::updateUndoState() {
    // §7.4: «Undo: <action>» / «Redo: <action>». Availability (empty
    // history, modes without undo) — the §7.5 table (updateAvailability): menu, quick icons and keys share one state.
    const auto u = model_->session().undoLabel(), r = model_->session().redoLabel();
    const QString ut = u ? txt("ui.main.undo") + text(u->key) : QString(txt("ui.common.undo")), rt = r ? txt("ui.main.redo") + text(r->key) : QString(txt("ui.common.redo"));
    if (auto* a = findChild<QAction*>("actionUndo")) a->setText(ut);
    if (auto* a = findChild<QAction*>("actionRedo")) a->setText(rt);
    if (auto* b = ribbonButton("undo")) b->setToolTip(ut + " (Ctrl+Z)");
    if (auto* b = ribbonButton("redo")) b->setToolTip(rt + " (Ctrl+Y)");
}

// ---------------------------------------------------------------- command availability (§7.5, FX4-UI-002)

static ElementKind elementKindOf(const doc::Element& e) {
    if (e.isAssembly()) return ElementKind::Assembly;
    if (e.isDrawing()) return ElementKind::Drawing;
    if (e.isSpecification()) return ElementKind::Specification;
    if (e.isBody()) return ElementKind::Body;
    return ElementKind::Part;
}

AvailabilityState MainWindow::availabilityState() const {
    AvailabilityState s;
    s.mode = mode_;
    auto& session = model_->session();
    const auto docPtr = session.document().document;  // hold the document: active() refers into it
    const doc::Element& a = docPtr->active();
    s.kind = elementKindOf(a);
    s.subjectKind = s.kind;
    s.view3d = s.kind != ElementKind::Drawing && s.kind != ElementKind::Specification;
    const auto pr = model_->result();
    if (s.kind == ElementKind::Part || s.kind == ElementKind::Body) s.hasBody = pr && pr->final && !pr->final->bodies.empty();
    if (s.kind == ElementKind::Part && pr && pr->final && pr->final->sheet) {  // sheet metal part (S5-SHEET-*)
        s.sheetParams = true;
        s.sheetBody = !pr->final->sheet->body.empty();
    }
    // FX7-SH-002; an explicit kind (S5-SHEET-006) wins over the history.
    s.sheetPart = s.kind == ElementKind::Part && (a.sheetMetal ? *a.sheetMetal : (a.isSheetPart() || s.sheetParams));
    // Features before the rollback marker: any unsuppressed feature with a tool (pattern/mirror source, regen.md §4.7).
    if (a.rollback.kind != doc::Rollback::Kind::Start)
        for (const auto& f : a.features) {
            using T = doc::FeatureType;
            if (!f->suppressed && api::Session::isToolFeature(f->type) && f->type != T::Pattern && f->type != T::Mirror) s.hasPatternable = true;
            if (a.rollback.kind == doc::Rollback::Kind::After && f->id == a.rollback.feature) break;
        }
    s.instances = int(a.instances.size());
    for (const auto& in : a.instances) s.shownInstances += !in.hidden && !in.suppressed;
    if (s.kind == ElementKind::Drawing) {
        const int si = a.sheetIndex(currentSheet_);
        s.hasSheetView = si >= 0 && !a.sheets[size_t(si)].views.empty();
    }
    s.hasPath = session.path().has_value();
    s.hasLinks = !session.linkedFiles().empty();
    s.hasLibraryParts = !session.document().document->libraryParts.empty();
    s.canUndo = session.undoLabel().has_value();
    s.canRedo = session.redoLabel().has_value();
    s.planeSelected = tree_ && tree_->selectedPlane().has_value();
    if (view_)
        for (const auto& t : view_->selection()) s.planeSelected = s.planeSelected || t.kind == PickTarget::Kind::Plane;
    if (mode_ == Mode::Sketch && sketch_) {
        const int idx = a.indexOf(sketch_->feature());
        const auto f = a.find(sketch_->feature());
        if (const auto* d = f ? f->sketchData() : nullptr)
            for (const auto& e : d->entities) s.sketchHasEntities = s.sketchHasEntities || e.type != sketch::EntityType::Projected;
        if (const auto st = sketch_->state()) s.sketchWeak = int(st->weak.size());
        s.sketchGroupSelected = sketch_->selectionHasGroupMember();
        for (int i = 0; i < idx; ++i)
            if (a.features[size_t(i)]->sketchData() && !a.features[size_t(i)]->suppressed) s.otherSketch = true;
        // Sketch input state — after the previous feature; in an assembly and part in context — instances and context.
        if (s.kind == ElementKind::Assembly) s.bodyTopology = s.instances > 0;
        else if (a.context) s.bodyTopology = true;
        else if (pr && idx > 0) {
            const auto it = pr->after.find(a.features[size_t(idx - 1)]->id);
            s.bodyTopology = it != pr->after.end() && it->second && !it->second->bodies.empty();
        }
    }
    s.exportPending = exportPending_;
    if (mode_ == Mode::FeatureEdit && panel_) {
        auto* ok = panel_->findChild<QPushButton*>("okButton");
        s.confirmAcceptable = ok ? ok->isEnabled() : panel_->canAccept();
    } else if (mode_ == Mode::Import) {
        s.confirmAcceptable = true;
    } else if (mode_ == Mode::Library && libraryPanel_) {
        s.confirmAcceptable = libraryPanel_->canAccept();
    } else if (mode_ == Mode::Solution && solutionWizard_) {
        s.confirmAcceptable = solutionWizard_->canAccept();
    } else if (mode_ == Mode::SolutionRecord && solutionRecorder_) {
        s.confirmAcceptable = solutionRecorder_->canSave();
    } else if (mode_ == Mode::ExportStl && exportPanel_) {
        auto* ok = exportPanel_->findChild<QPushButton*>("exportOk");
        s.confirmAcceptable = !ok || ok->isEnabled();
    }
    return s;
}

void MainWindow::updateAvailability() {
    if (!ribbon_ || !confirmOk_) return;  // the window is still being built
    if (toasts_) toasts_->recheck(toastState());  // messages that lost their sense (S7F-UI-003)
    const AvailabilityState s = availabilityState();
    // The flat pattern display ends when the part has no sheet body (§7.4, S5-FLAT-001).
    if (unfolded_ && !s.sheetBody && s.mode == Mode::Part) {
        unfolded_ = false;
        if (QToolButton* b = ribbonButton("sheetUnfold")) b->setChecked(false);
    }
    // «Part» tab: the plain or the sheet ribbon by the active part kind (app-ux §2.3, FX8-SH-001).
    if (sheetRibbon(s) != partRibbonSheet_) arrangePartRibbon(sheetRibbon(s));
    // Ribbon, above-ribbon and view-corner buttons; menu items (main menu and button menus) — by the §7.5 rule table.
    for (QToolButton* b : findChildren<QToolButton*>()) {
        const QString n = b->objectName();
        if (n.startsWith("ribbon_") || n == "confirmOk" || n == "confirmCancel") b->setEnabled(commandAvailable(n.toStdString(), s));
    }
    for (QAction* a : findChildren<QAction*>()) {
        const QString n = a->objectName();
        if (n.isEmpty()) continue;
        if (const CommandRule* r = commandRule(n.toStdString())) a->setEnabled(r->available(s));
    }
    // The sheet button of the «Part» tab by the part kind (FX12-SH-001, app-ux §2.3): an ordinary part — "Convert to sheet
    // metal part…", available only when the conversion can succeed (sheetConvertCheck of the last regeneration, checked
    // once per result revision; otherwise unavailable, the tooltip — the reason); a sheet part — "Sheet parameters…".
    if (QToolButton* b = ribbonButton("sheetMetal")) {
        if (s.sheetPart || s.kind != ElementKind::Part) {
            b->setText(txt("ui.ribbon.sheetParams"));
            b->setToolTip(b->text() + QString::fromUtf8(" — ") + txt("ui.ribbon.sheetParamsTip"));  // starts with the label (S1-UX-001)
        } else {
            b->setText(txt("ui.ribbon.convertToSheet"));
            const auto pr = model_->result();
            const qulonglong rev = pr ? qulonglong(pr->revision) + 1 : 0;  // 0 — no result
            if (b->property("sheetCheckRev").toULongLong() != rev || !b->property("sheetCheckRev").isValid()) {
                const auto st = model_->session().sheetConvertCheck();
                const auto held = model_->session().document().document;
                b->setProperty("sheetCheckRev", rev);
                b->setProperty("sheetCheckOk", st.ok());
                b->setProperty("sheetCheckWhy", st.ok() ? QString() : errorText(st.error(), held.get()));
            }
            const bool ok = b->property("sheetCheckOk").toBool();
            b->setEnabled(b->isEnabled() && ok);
            b->setToolTip(b->text() + QString::fromUtf8(" — ") +
                          (ok ? QString(txt("ui.ribbon.convertToSheetTip")) : b->property("sheetCheckWhy").toString()));
        }
    }
    // «From file…» without a document path — a hint why it is unavailable (§7.5).
    for (const char* n : {"insertFromFile", "baseViewFromFile", "threeViewsFromFile"})
        for (QAction* a : findChildren<QAction*>(n)) a->setToolTip(s.hasPath ? a->text() : text("ui.fromFile.needPath"));
}

void MainWindow::cmdUndo() {
    if (mode_ == Mode::Repair) return;  // the repair wizard (§5.5в)
    if (mode_ == Mode::FeatureEdit || mode_ == Mode::ExportStl || mode_ == Mode::Import || mode_ == Mode::Settings || mode_ == Mode::Section || mode_ == Mode::Annotation || mode_ == Mode::Mate || mode_ == Mode::Weld || mode_ == Mode::Interference || mode_ == Mode::Library || mode_ == Mode::LibrarySave || mode_ == Mode::Solution || mode_ == Mode::SolutionRecord || mode_ == Mode::InstancePattern) return;
    if (mode_ == Mode::Sketch && sketch_->inlineEditOpen()) return;
    if (mode_ == Mode::Sketch) sketch_->dropPending();
    auto s = model_->session().undo();
    if (!s) setPrompt(errorText(s.error()));
}

void MainWindow::cmdRedo() {
    if (mode_ == Mode::Repair) return;  // the repair wizard (§5.5в)
    if (mode_ == Mode::FeatureEdit || mode_ == Mode::ExportStl || mode_ == Mode::Import || mode_ == Mode::Settings || mode_ == Mode::Section || mode_ == Mode::Annotation || mode_ == Mode::Mate || mode_ == Mode::Weld || mode_ == Mode::Interference || mode_ == Mode::Library || mode_ == Mode::LibrarySave || mode_ == Mode::Solution || mode_ == Mode::SolutionRecord || mode_ == Mode::InstancePattern) return;
    auto s = model_->session().redo();
    if (!s) setPrompt(errorText(s.error()));
}

void MainWindow::createSketchOn(const std::string& planeId) {
    if (mode_ != Mode::Part && mode_ != Mode::PickPlane) return;
    // Origin plane — by ID plane:*, reference plane — by feature ID (S2-REF-005).
    const doc::Json plane = planeId.rfind("plane:", 0) == 0 ? doc::Json{{"datum", planeId}} : doc::Json{{"feature", planeId}};
    auto r = model_->session().createSketch(plane);
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    usedTreePlane_ = planeId;  // the base is used (§3.6б)
    editSketch(*r);
    newSketch_ = *r;
    newSketchPlane_ = planeId, newSketchFace_.reset();
}

void MainWindow::cmdSketch() {
    if (mode_ != Mode::Part) return;
    std::optional<std::string> plane = tree_->selectedPlane();
    if (!plane)  // «Plane» feature selected in the tree (S2-REF-005)
        if (auto f = tree_->selectedFeature())
            if (auto ff = model_->session().document().document->active().find(*f); ff && ff->type == doc::FeatureType::Plane) plane = *f;
    std::optional<std::string> face;
    for (const auto& s : view_->selection()) {
        if (!plane && s.kind == PickTarget::Kind::Plane) plane = s.name;
        if (s.kind == PickTarget::Kind::Face) face = s.name;
    }
    // A sketch selected in the tree (no plane, no face) — edit it (§3.6б, S7F-FEAT-002).
    if (!plane && !face)
        if (auto f = tree_->selectedFeature())
            if (auto ff = model_->session().document().document->active().find(*f); ff && ff->sketchData()) {
                const std::string id = *f;
                editSketch(id);
                return;
            }
    if (face && !plane) {
        const auto sel = view_->selection();  // copy: creating a sketch clears the selection
        for (const auto& s : sel)
            if (s.kind == PickTarget::Kind::Face) {
                createSketchOnFace(s);
                break;
            }
        return;
    }
    if (plane) createSketchOn(*plane);
    else setMode(Mode::PickPlane);
}

bool MainWindow::createSketchOnFace(const PickTarget& t) {
    if (mode_ != Mode::Part && mode_ != Mode::PickPlane) return false;
    const std::string face = assemblyPick(t).name;  // face of an assembly instance — the assembly's name (S2-ASM-012)
    auto info = model_->session().topologyInfo(face, api::StateRef::final());
    if (!info || !info->surface || info->surface->kind != kernel::SurfaceInfo::Kind::Plane) {
        setPrompt(text("ui.status.pickPlane"));
        return false;
    }
    auto r = model_->session().createSketch(doc::Json{{"face", face}});
    if (!r) {
        showToast(errorText(r.error()), true);
        return false;
    }
    view_->setSelection({});
    if (auto pl = tree_->selectedPlane()) usedTreePlane_ = *pl;  // the base is used (§3.6б)
    editSketch(*r);
    newSketch_ = *r;
    newSketchPlane_.reset(), newSketchFace_ = t;
    return true;
}

void MainWindow::editSketch(const std::string& id) {
    if (mode_ == Mode::Repair) return;  // the repair wizard (§5.5в)
    if (mode_ == Mode::FeatureEdit || mode_ == Mode::ExportStl || mode_ == Mode::Import || mode_ == Mode::Settings || mode_ == Mode::Section || mode_ == Mode::Annotation || mode_ == Mode::Mate || mode_ == Mode::Weld || mode_ == Mode::Interference || mode_ == Mode::Library || mode_ == Mode::LibrarySave || mode_ == Mode::Solution || mode_ == Mode::SolutionRecord || mode_ == Mode::InstancePattern) return;
    if (!model_->session().editContext().sketch || *model_->session().editContext().sketch != id) {
        auto s = model_->session().enterSketch(id);
        if (!s) {
            showToast(errorText(s.error()), true);
            return;
        }
    }
    // Bodies shown before entering — the whole part, fitted together with the sketch (§6.1, FX10-IO-003).
    QVector3D partMin, partMax;
    const bool hasPart = view_->sceneBounds(partMin, partMax);
    sketch_->open(id);
    setMode(Mode::Sketch);
    tree_->rebuild(id);
    // Camera normal to the sketch plane, fitting the sketch and the part (§6.1, FX1-UI-010, FX10-IO-003); empty sketch —
    // the center is projected onto the plane, the scale is kept.
    if (auto st = sketch_->state()) {
        Camera c = view_->targetCamera();
        const auto& f = st->frame;
        c.setNormalTo(QVector3D(float(f.normal.x()), float(f.normal.y()), float(f.normal.z())),
                      QVector3D(float(f.y.x()), float(f.y.y()), float(f.y.z())));
        if (const auto box = sketch_->localBounds()) {
            QVector3D mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
            for (const Point2& q : {box->first, Point2(box->second.x(), box->first.y()), box->second, Point2(box->first.x(), box->second.y())}) {
                const Point3 w = f.toWorld(q);
                const QVector3D v(float(w.x()), float(w.y()), float(w.z()));
                mn = QVector3D(std::min(mn.x(), v.x()), std::min(mn.y(), v.y()), std::min(mn.z(), v.z()));
                mx = QVector3D(std::max(mx.x(), v.x()), std::max(mx.y(), v.y()), std::max(mx.z(), v.z()));
            }
            if (hasPart) {
                mn = QVector3D(std::min(mn.x(), partMin.x()), std::min(mn.y(), partMin.y()), std::min(mn.z(), partMin.z()));
                mx = QVector3D(std::max(mx.x(), partMax.x()), std::max(mx.y(), partMax.y()), std::max(mx.z(), partMax.z()));
            }
            c.fit(mn, mx);
        } else {
            const Point3 cc(c.center.x(), c.center.y(), c.center.z());
            const Point2 lc = f.toLocal(cc);
            const Point3 pc = f.toWorld(lc);
            c.center = QVector3D(float(pc.x()), float(pc.y()), float(pc.z()));
        }
        view_->animateTo(c);
    }
}

void MainWindow::cmdExitSketch() {
    if (mode_ != Mode::Sketch) return;
    const std::string closed = sketch_->feature();
    sketch_->escape();
    model_->session().exitSketch();
    sketch_->close();
    // A new sketch left empty is deleted from the tree (FX7-SK-017); undo brings it back.
    if (closed == newSketch_) {
        const auto f = model_->session().document().document->active().find(closed);
        const auto* d = f ? f->sketchData() : nullptr;
        if (d && d->entities.empty() && d->constraints.empty() && d->groups.empty()) model_->session().deleteFeature(closed);
    }
    newSketch_.clear();
    setMode(Mode::Part);
    tree_->rebuild();
    resumeAfterBaseSketch(closed);  // the command started by clicking a base continues (§3.6а)
}

void MainWindow::cmdCancelSketch() {
    // All changes of the sketch since it was opened are cancelled in one undo step; a new sketch left empty is deleted
    // in the same step and its plane or face stays selected (app-ux §6.1, S7F-SK-004).
    if (mode_ != Mode::Sketch) return;
    const std::string closed = sketch_->feature();
    const bool isNew = closed == newSketch_;
    sketch_->escape();
    auto& s = model_->session();
    s.beginGroup("undo.group.sketchCancel");
    if (auto r = s.sketchCancel(); !r) showToast(errorText(r.error()), true);
    bool deleted = false;
    if (isNew) {
        const auto f = s.document().document->active().find(closed);
        const auto* d = f ? f->sketchData() : nullptr;
        if (d && d->entities.empty() && d->constraints.empty() && d->groups.empty()) deleted = s.deleteFeature(closed).ok();
    }
    s.endGroup();
    if (s.editContext().sketch) s.exitSketch();
    sketch_->close();
    newSketch_.clear();
    setMode(Mode::Part);
    tree_->rebuild();
    resumeAfterBaseSketch(closed);  // as on exit: a deleted sketch does not resume the command (§3.6а)
    if (deleted) {
        if (newSketchPlane_)
            if (auto* it = tree_->itemById(*newSketchPlane_)) tree_->setCurrentItem(it);
        if (newSketchFace_) view_->setSelection({*newSketchFace_});
    }
    newSketchPlane_.reset(), newSketchFace_.reset();
}

void MainWindow::cmdExtrude() { startFeature(extrude_); }
void MainWindow::cmdRevolve() { startFeature(revolve_); }
void MainWindow::cmdCoil() { startFeature(coil_); }  // §5.3м, FX7-PT-004

void MainWindow::cmdEdgeFeature(bool fillet) {
    if (mode_ != Mode::Part) return;
    // Preselection: edges selected in the view (§3.6).
    std::vector<std::string> edges;
    for (const auto& t : view_->selection())
        if (t.kind == PickTarget::Kind::Edge) edges.push_back(t.name);
    EdgePanel* p = fillet ? fillet_ : chamfer_;
    previewId_.clear();
    previewValid_ = false;
    panel_ = p;
    setMode(Mode::FeatureEdit);
    p->beginNew(std::nullopt);
    p->setEdges(edges);
    syncRefSelection();
}

void MainWindow::cmdConvertPart(const std::string& id, bool toSheet) {
    QString name;
    {
        const auto held = model_->session().document().document;
        if (const int i = held->elementIndex(id); i >= 0) name = QString::fromStdString(held->elements[size_t(i)].name);
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto r = model_->session().convertPart(id, toSheet ? "sheet" : "part");
    QApplication::restoreOverrideCursor();
    if (!r) {
        const auto held = model_->session().document().document;
        showToast(errorText(r.error(), held.get()), true);
        // The place of an invalid form (sheet.md §4.9): the face in the current state of the part.
        if (const auto* face = r.error().stringParam("name"))
            if (auto pr = model_->result(); pr && pr->final)
                for (const auto& fe : pr->final->names.faces)
                    if (fe.full == *face) view_->setSelection({{PickTarget::Kind::Face, pr->final->bodies.at(size_t(fe.body)).id, *face}});
        return;
    }
    showToast(text(toSheet ? "ui.convert.toSheet" : "ui.convert.toPart", {{"name", name}}), false,
              {{text("action.undo"), [this] { cmdUndo(); }}});
}

// ---------------------------------------------------------------- click on a base (§3.6а, FX1-FEAT-001)

bool MainWindow::startBaseSketch(const PickTarget& t) {
    if (mode_ != Mode::FeatureEdit || !panel_->wantsBaseSketch()) return false;
    if (t.kind != PickTarget::Kind::Plane && t.kind != PickTarget::Kind::Face) return false;
    doc::Json plane;
    if (t.kind == PickTarget::Kind::Plane) {
        // Origin plane — by ID plane:*, reference plane — by feature ID (S2-REF-005).
        plane = t.name.rfind("plane:", 0) == 0 ? doc::Json{{"datum", t.name}} : doc::Json{{"feature", t.name}};
    } else {
        const std::string face = assemblyPick(t).name;
        auto info = model_->session().topologyInfo(face, api::StateRef::final());
        if (!info || !info->surface || info->surface->kind != kernel::SurfaceInfo::Kind::Plane) {
            setPrompt(text("ui.status.needSketchOrBase"));
            return true;
        }
        plane = doc::Json{{"face", face}};
    }
    // The panel is hidden with its values kept; after exiting the sketch the command continues (§3.6а).
    FeaturePanel* panel = panel_;
    model_->session().cancelPreview();
    model_->notePreviewCancelled();
    previewId_.clear();
    previewValid_ = false;
    setMode(Mode::Part);
    auto r = model_->session().createSketch(plane);
    if (!r) {
        showToast(errorText(r.error()), true);
        panel_ = panel;
        setMode(Mode::FeatureEdit);
        return true;
    }
    basePanel_ = panel;
    baseSketch_ = *r;
    newSketch_ = *r;
    newSketchPlane_.reset(), newSketchFace_.reset();
    view_->setSelection({});
    if (auto pl = tree_->selectedPlane()) usedTreePlane_ = *pl;  // the base is used (§3.6б)
    editSketch(*r);
    return true;
}

void MainWindow::resumeAfterBaseSketch(const std::string& closedSketch) {
    if (!basePanel_) return;
    FeaturePanel* panel = basePanel_;
    const std::string sk = baseSketch_;
    basePanel_ = nullptr;
    baseSketch_.clear();
    if (sk != closedSketch || !model_->session().document().document->active().find(sk)) return;
    panel_ = panel;
    previewId_.clear();
    previewValid_ = false;
    setMode(Mode::FeatureEdit);
    panel_->setSketch(sk);
    updateRegions();
    syncRefSelection();
}

// The assembly hole (§8а, FX8-ASM-007): a click on a planar instance face puts a point where the hole will be — the
// first one makes a sketch on that face (the face is taken from the click), the next ones add points to it; the panel
// stays, the preview follows.
bool MainWindow::holePointClick(const PickTarget& t) {
    if (mode_ != Mode::FeatureEdit || panel_ != hole_ || panel_->editing() || !assemblyActive()) return false;
    if (t.kind != PickTarget::Kind::Face || t.body.empty()) return false;
    const std::string face = assemblyPick(t).name;
    auto info = model_->session().topologyInfo(face, api::StateRef::final());
    if (!info || !info->surface || info->surface->kind != kernel::SurfaceInfo::Kind::Plane) {
        setPrompt(text("ui.status.needSketchOrBase"));
        return true;
    }
    const auto at = view_->pointUnder(selAt_);
    if (!at) return true;
    model_->session().cancelPreview();
    model_->notePreviewCancelled();
    previewId_.clear();
    previewValid_ = false;
    const std::string current = hole_->sketch();
    auto r = model_->session().placeFacePoint(current.empty() ? std::nullopt : std::optional<std::string>(current), face,
                                              {at->x(), at->y(), at->z()});
    if (!r) {
        showToast(errorText(r.error()), true);
        return true;
    }
    hole_->setSketch(*r);
    updateRegions();
    return true;
}

void MainWindow::pickedInFeatureEdit(const PickTarget& t) {
    if (holePointClick(t)) return;  // the assembly hole: a point on the clicked face (FX8-ASM-007)
    // Click on a plane or a planar face while the field waits for a sketch — a sketch on it (§3.6а).
    if (startBaseSketch(t)) return;
    // Everything else — to the panel (§5.5д, S7F-RF-011): it decides which field takes the object; a refusal names what
    // the current field needs.
    const bool taken = t.kind != PickTarget::Kind::None && panel_->pick(assemblyPick(t));
    if (!taken) setPrompt(panel_->needed());
    syncRefSelection();
    if (panel_->picksRegions()) updateRegions();  // regions of the «Profiles» field (§5.5)
    setActiveSelection(panel_->lastField());
    if (taken && !(selField_ && selField_->pending())) setPrompt(waitHint());  // the next place the panel waits for
}

std::optional<PickTarget> MainWindow::treeTarget(const QString& kind, const std::string& id, bool featureAsPlane) const {
    if (kind == "plane") return PickTarget{PickTarget::Kind::Plane, {}, id};
    if (kind == "axis") return PickTarget{PickTarget::Kind::Axis, {}, id};
    if (kind == "origin") return PickTarget{PickTarget::Kind::Origin, {}, "origin"};
    if (kind != "feature") return std::nullopt;
    const auto f = model_->session().document().document->active().find(id);
    if (!f) return std::nullopt;
    if (f->type == doc::FeatureType::Axis) return PickTarget{PickTarget::Kind::Axis, {}, id};
    if (f->type == doc::FeatureType::Plane || featureAsPlane) return PickTarget{PickTarget::Kind::Plane, {}, id};
    return std::nullopt;
}

bool MainWindow::pickedInTreeFeatureEdit(const QString& kind, const std::string& id) {
    FeaturePanel::TreePick p{treeTarget(kind, id, false), kind == "feature" ? id : std::string()};
    const auto outcome = panel_->pickTree(p);
    if (outcome == FeaturePanel::PickOutcome::NotTaken) return false;
    if (outcome == FeaturePanel::PickOutcome::Refused) setPrompt(panel_->neededInTree());
    syncRefSelection();
    if (panel_->lastField()) setActiveSelection(panel_->lastField());
    if (outcome == FeaturePanel::PickOutcome::Taken && !(selField_ && selField_->pending())) setPrompt(waitHint());
    return true;
}

void MainWindow::setPrompt(const QString& t) {
    if (statusHint_) statusHint_->setText(t);
}

QString MainWindow::waitHint() const {
    switch (mode_) {
        case Mode::PickPlane: return text("ui.status.pickPlane");
        case Mode::Sketch: return sketch_ ? SketchEditor::toolHint(sketch_->tool()) : QString();
        case Mode::Section: return text("ui.wait.section");
        case Mode::Mate: return text("ui.wait.mate");
        case Mode::Weld: return text("ui.wait.weld");
        case Mode::Annotation: return text("ui.wait.annotation");
        case Mode::Solution: return solutionWizard_ ? solutionWizard_->hint() : QString();
        case Mode::SolutionRecord: return text("ui.wait.record");
        case Mode::Properties: return assemblyActive() ? QString() : text("ui.wait.faceColor");  // no face colors in an assembly (FX12-ASM-003)
        case Mode::InstancePattern: return text("ui.wait.instancePattern");
        case Mode::Library: {
            const QString h = libraryPanel_ ? libraryPanel_->seatHint() : QString();
            return h.isEmpty() ? text("ui.wait.library") : h;
        }
        case Mode::FeatureEdit: break;
        default: return {};
    }
    // A feature panel: the active selection field, else what the panel waits for (§5.5д).
    if (selField_ && !selField_->hint().isEmpty()) return selField_->hint();
    return panel_ ? panel_->prompt() : text("ui.wait.feature");
}

void MainWindow::syncRefSelection() {
    // The picks of the panel are the view selection (§5.5д); a panel without picks leaves the selection alone.
    if (mode_ != Mode::FeatureEdit || !panel_ || !panel_->showsPicks()) return;
    std::set<PickTarget> sel;
    for (const PickTarget& t : panel_->selectedTargets()) sel.insert(withBody(t));
    view_->setSelection(sel);
}

PickTarget MainWindow::withBody(PickTarget t) const {
    // Body of a face, edge, vertex — from the name table (view selection compares body and name).
    if (auto pr = model_->result(); pr && pr->final && t.body.empty()) {
        const auto& nt = pr->final->names;
        if (t.kind == PickTarget::Kind::Face)
            for (const auto& e : nt.faces) if (e.full == t.name) t.body = pr->final->bodies.at(e.body).id;
        if (t.kind == PickTarget::Kind::Edge)
            for (const auto& e : nt.edges) if (e.full == t.name) t.body = pr->final->bodies.at(e.body).id;
        if (t.kind == PickTarget::Kind::Vertex)
            for (const auto& e : nt.vertices) if (e.full == t.name) t.body = pr->final->bodies.at(e.body).id;
    }
    return t;
}

void MainWindow::cmdRefGeom(bool plane, const std::string& mode) {
    if (mode_ != Mode::Part) return;
    const std::set<PickTarget> pre = view_->selection();  // preselection (§5.8)
    RefGeomPanel* p = plane ? refPlane_ : refAxis_;
    // Plane first: the panel fills its reference fields in order (the reference plane, then the object).
    std::vector<PickTarget> picks;
    for (const auto& t : pre) picks.push_back(assemblyPick(t));
    std::stable_partition(picks.begin(), picks.end(),
                          [](const PickTarget& t) { return t.kind == PickTarget::Kind::Plane || t.kind == PickTarget::Kind::Face; });
    // «Offset» with a plane and an object through which it passes (a point, a parallel plane or face) preselected —
    // «Offset up to object» at once (app-ux §5.3а, FX9-PT-001): by the selection rules (selection.md §3.1, П2, П5, П10).
    // Other sets of two or three objects (S7F-ASM-006): the plane through them by the first candidate of the same rules
    // (П4 three points, П6 line and point, П7 two lines, П5 — perpendicular through a line…).
    std::string m = mode;
    std::optional<api::Json> through;
    if (plane && mode == "offset" && (picks.size() == 2 || picks.size() == 3)) {
        std::vector<std::string> refs;
        for (const auto& t : picks) refs.push_back(pickRef(t));
        const auto r = model_->session().selectionCandidates("plane", refs, api::StateRef::final());
        if (r && r->status == "candidates" && !r->candidates.empty()) {
            const auto& c = r->candidates.front();
            const bool to = !c.aux.empty() && c.aux.front().value("mode", std::string()) == "offset" && c.aux.front().contains("to");
            const bool parallel = c.rule == "П3";  // i18n-skip: rule ID of the data; two parallel planes / faces: offset from the first up to the second
            if (picks.size() == 2 && (to || parallel)) m = "offsetTo";
            else if (!c.aux.empty() && c.aux.front().contains("mode")) through = c.aux.front();
        }
    }
    previewId_.clear();
    previewValid_ = false;
    panel_ = p;
    setMode(Mode::FeatureEdit);
    if (through) p->startNewFrom(*through);
    if (through && !p->canAccept()) through.reset();  // a reference the panel cannot hold (a sketch point) — as before
    if (!through) {
        p->startNewMode(m);
        for (const auto& t : picks) p->pick(t);
        // A single preselected face (S7F-ASM-006): the plane through it — offset 0; a base or reference plane keeps the
        // default offset.
        if (plane && m == "offset" && picks.size() == 1 && picks.front().kind == PickTarget::Kind::Face) p->setOffsetText("0");
    }
    if (auto pl = tree_->selectedPlane(); pl && pre.empty()) p->pick({PickTarget::Kind::Plane, {}, *pl});
    syncRefSelection();
}

void MainWindow::cmdProperties() {
    if (mode_ != Mode::Part) return;
    propertiesPanel_->setInstance({});
    setMode(Mode::Properties);
}

void MainWindow::faceClickedInProperties(const PickTarget& t) {
    if (t.kind == PickTarget::Kind::Face) view_->setSelection({t});
    else view_->setSelection({});
    propertiesPanel_->setFace(t.kind == PickTarget::Kind::Face ? t.name : std::string());
}

// ---------------------------------------------------------------- keys and closing

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    // Context panel (§7.2): opacity by cursor distance; Esc closes it first (§6.13).
    if (ctx_ && ctx_->isVisible()) {
        if (e->type() == QEvent::MouseMove) ctx_->updateFade(mapFromGlobal(static_cast<QMouseEvent*>(e)->globalPosition().toPoint()));
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && isActiveWindow()) {
            ctx_->hide();
            // The bar of a just placed dimension (S7F-SK-002) is shown by itself — not a ladder step: Esc goes on.
            if (!(mode_ == Mode::Sketch && sketch_->dimensionAutoSelected())) return true;
        }
    }
    if (o == statusDof_ && e->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton) {
        dofClicked();
        return true;
    }
    if (o == statusUnits_ && e->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton) {
        if (commandEnabled("statusUnits")) cmdDocumentSettings();  // like "Document settings…" (§7.5)
        return true;
    }
    // The OK of the feature or export panel became (un)available — the view-corner ✓ mirrors it (§7.5).
    if (e->type() == QEvent::EnabledChange && confirmOk_ && (o->objectName() == "okButton" || o->objectName() == "exportOk"))
        confirmOk_->setEnabled(commandEnabled("confirmOk"));
    if (e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        QWidget* focus = QApplication::focusWidget();
        bool inText = focus && (focus->inherits("QLineEdit") || focus->inherits("QAbstractSpinBox"));
        // Esc in an unchanged expression field goes up the ladder (§5.6, §6.13); a changed one reverts its text itself.
        if (auto* x = qobject_cast<ExprField*>(focus); x && k->key() == Qt::Key_Escape && x->escapeReverts() && !x->changedSinceFocus())
            inText = false;
        // Modal and popup windows (menus, drop-down lists) handle keys themselves.
        if (!isActiveWindow() || QApplication::activeModalWidget() || QApplication::activePopupWidget())
            return QMainWindow::eventFilter(o, e);
        const Qt::KeyboardModifiers m = k->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
        // A candidate waiting for «Yes»: Enter / Tab / Shift+Tab with the focus in the view (selection.md §5.3, К4, К5).
        if (o == view_ && selectionKey(k)) return true;
        // Esc: the ladder (§6.13); input fields handle Esc themselves; dragging in the tree is cancelled by the tree (§4.3).
        if (k->key() == Qt::Key_Escape && o == tree_ && tree_->dragging()) return false;
        if (k->key() == Qt::Key_Escape && !inText && o == focus) {
            if (mode_ == Mode::Sketch && focus == view_) return false;  // the sketcher handles it itself
            escape();
            return true;
        }
        if (!inText && o == focus && m == Qt::ShiftModifier && mode_ == Mode::Part) {
            // A key acts like its button: only when the button is available (§7.3, §7.5).
            auto enabled = [this](const char* n) {
                QToolButton* b = ribbonButton(n);
                return b && b->isEnabled();
            };
            if (physicalKey(k) == Qt::Key_S) {
                if (enabled("sketch") || enabled("asmSketch")) cmdSketch();
                return true;
            }
            if (physicalKey(k) == Qt::Key_E) {
                if (enabled("extrude")) cmdExtrude();
                return true;
            }
            if (physicalKey(k) == Qt::Key_W) {
                if (enabled("revolve")) cmdRevolve();
                return true;
            }
            if (physicalKey(k) == Qt::Key_F) {
                if (enabled("fillet")) cmdEdgeFeature(true);
                return true;
            }
            if (physicalKey(k) == Qt::Key_C) {
                if (enabled("chamfer")) cmdEdgeFeature(false);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!testMode_ && !closeConfirmed_ && !confirmDiscard()) {
        e->ignore();
        return;
    }
    dropRecovery();
    e->accept();
}

bool MainWindow::isIdle() const {
    if (model_->regenPending() || model_->previewPending() || exportPending_) return false;
    if (panel_->previewTimerActive()) return false;
    if (sceneRevision_ != model_->session().revision()) return false;
    if (view_->isVisible() && !view_->frameUpToDate()) return false;  // no 3D view is drawn under a drawing sheet (§8б)
    if (view_->isAnimating()) return false;
    if (ctxTimer_.isActive()) return false;  // §12.1: CONTEXT_BAR_DELAY_MS
    if (solutionWizard_ && solutionWizard_->checkPending()) return false;  // variants checked in the background (FX8-BLT-001)
    if (retessTimer_.isActive()) return false;  // §11.6: VIEW_RETESS_DEBOUNCE_MS
    if (ExprField::anyCheckPending()) return false;  // §5.6: EXPR_CHECK_DEBOUNCE_MS
    return true;
}

void MainWindow::resetForTest() {
    // Dialogs, menus and popups of the previous test (FX14-QA-008, §12.1): every shown top-level window but this one is
    // closed — a dialog (a question in its own event loop included) is rejected, others are hidden (no close events).
    closeOtherTopLevelWindows(this);
    restoreTestGeometry();  // FX14-QA-011: the window and side panels of the previous test
    explodeAnimMs_ = 0;  // a failed FX8-ASM-006 test must not leave its animation and clock to the next one
    explodeClock_ = {};
    featureErrors_.clear();
    lostAll_.clear();  // §5.5в
    lostSeen_.clear();
    repairUndoPoints_.clear();
    lostDimsDeleted_.clear();
    basePanel_ = nullptr;
    baseSketch_.clear();
    sketch_->close();
    model_->session().cancelPreview();
    model_->notePreviewCancelled();
    previewId_.clear();
    previewValid_ = false;
    setMode(Mode::Part);
    setRegenCacheDirForTest({});  // on-disk cache is off in test mode (§10.5); the cache test enables it itself
    model_->session().newDocument();
    tree_->resetViewState();
    variables_->hide();
    libraryPanel_->clearSearch();  // the catalog search query stays between openings of the panel
    toasts_->clear();
    helpWindow_->hide();
    model_->setResultDelayForTest(0);  // §9.4
    UiClock::setManualForTest(false);  // a failed test must not leave the manual test clock (§12.1, FX14-CI-002)
    model_->setResultHoldForTest(false);
    showDrawingPanel(true);  // drawing properties panel — visible by default (§8б)
    // Drawing toggles and panels (FX7-DRW-003…006): blue dimensions off, no text placement, no choice panel.
    if (auto* b = buttons_["showUndimensioned"]) b->setChecked(false);
    if (dims_) dims_->setShowUndimensioned(false);
    if (texts_) texts_->escape(), texts_->setPlacing(false);
    if (drawingPickPanel_) drawingPickPanel_->hide();
    testPrintTarget_.clear();
    sketch_->setConstraintsShown(true);
    for (const auto& showFirst : groupDefaults_) showFirst();  // group buttons: the first variant (FX14-QA-002)
    // Application settings — defaults (§10.6, §12.1): mouse profile, wheel, style, fonts.
    appSettings_->resetToDefaults();
    appSettingsWindow_->hide();
    reportWindow_->hide();  // problem report (§10.8)
    updateWindow_->close();  // update window (§10.10): an unfinished download is cancelled
    updateWindow_->setDownloadDirForTest({});
    updateWindow_->setSaveDirForTest({});
    updateExitRequested_ = false;
    setDisplayStyle(DisplayStyle::ShadedEdges);  // display style (§3.4а)
    view_->setSelection({});
    tree_->clearSelection();
    // Tree hover of the previous test (FX7-PT-011, S1-VIEW-006): the feature's geometry by stable names would match
    // bodies of the next document with the same feature ids; hovered planes would stay shown.
    treeFeatureHovered({});
    view_->setShownPlanes(tree_->pinnedPlanes());
    view_->setAnimationMs(0);  // the test may have enabled transitions (§12.1)
    // The scene of the empty document (FX14-QA-005): the new document's scene arrives with its regeneration, after this
    // fit — without this the camera was fitted to the bodies and planes of the previous test (a sketch of the next test
    // opened at that test's zoom, its points off screen).
    view_->setBodies({});
    view_->setPlaneHalf(PLANE_MIN_HALF_MM);
    Camera c = view_->camera();
    c.setStandard(StandardView::Iso);
    view_->setCamera(c);
    view_->fitAll();
}

QList<int> MainWindow::sidePanelSizesForTest() const { return split_ ? split_->sizes() : QList<int>{}; }

void MainWindow::restoreTestGeometry() {
    // Window state, position and size of the test window (FX14-QA-011, app-ux §12.1): a test may have maximized the window
    // (FX14-UI-002), moved it or resized it; the window of every test is the one the harness fitted at start.
    if (windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized)) setWindowState(Qt::WindowNoState);
    if (testGeometry_.isValid()) {
        if (size() != testGeometry_.size()) setFixedSize(testGeometry_.size());
        // testGeometry_ is the client geometry (geometry()), while move() and pos() are the frame position: the window is
        // moved by the client offset, otherwise the title bar of a framed window (macOS: 32 px) shifts it down.
        if (geometry().topLeft() != testGeometry_.topLeft()) move(pos() + (testGeometry_.topLeft() - geometry().topLeft()));
    }
    if (layout()) layout()->activate();  // the splitter gets the width of the restored window before its sizes are set
    // Side panels — the start widths (main_window_ui.cpp): the tree LEFT_PANEL_W, the view the rest, the right panels
    // (hidden) RIGHT_PANEL_W for their next opening. Before this a panel narrowed by a test (S7F-UI-007: the tree dragged
    // to zero, opened again at LEFT_PANEL_MIN_W) stayed narrow, and the fit of the 3D view (e2e) narrowed the window by
    // the difference: 1200 px instead of 1280 for all later tests of the shard, a narrow field in FX14-UI-001.
    if (!split_ || split_->count() != 4) return;
    const QList<int> cur = split_->sizes();
    int total = 0;
    for (int i = 0; i < cur.size(); ++i)
        if (split_->widget(i)->isVisibleTo(this)) total += cur[i];
    QList<int> want{LEFT_PANEL_W, 0, RIGHT_PANEL_W, RIGHT_PANEL_W};
    int others = 0;
    for (int i = 0; i < want.size(); ++i)
        if (i != 1 && split_->widget(i)->isVisibleTo(this)) others += want[i];
    want[1] = std::max(0, total - others);
    if (want != cur) split_->setSizes(want);
}

std::optional<std::pair<QPointF, QPointF>> MainWindow::manipulatorScreen() const {
    if (const auto pl = sectionManipulatorPlane()) {  // section offset (§5.11): from the plane center toward the cut-off side
        BBox3 all;
        all.setEmpty();
        if (auto infos = model_->session().bodies(api::StateRef::final()))
            for (const auto& b : *infos) all.extend(b.bbox);
        const Point3 base = pl->toWorld(pl->toLocal(all.isEmpty() ? Point3::Zero() : Point3(all.center())));
        const Camera& cam = view_->camera();
        const Point3 tip = base + pl->normal * (MANIP_ARROW_PX * cam.mmPerPx());
        auto Q = [](const Point3& p) { return QVector3D(float(p.x()), float(p.y()), float(p.z())); };
        const QPointF a = cam.project(Q(base)), b = cam.project(Q(tip));
        if (std::hypot(b.x() - a.x(), b.y() - a.y()) < 6) return std::nullopt;
        return std::make_pair(a, b);
    }
    if (mode_ != Mode::FeatureEdit || panel_ != extrude_ || !extrude_->canAccept()) return std::nullopt;
    if (extrude_->definition()["end"]["type"].get<std::string>().rfind("throughAll", 0) == 0) return std::nullopt;  // no manipulator (§5.1)
    const auto depth = extrude_->depthMm();
    if (!depth) return std::nullopt;
    auto st = model_->session().sketchState(extrude_->sketch());
    if (!st) return std::nullopt;
    Point2 c = Point2::Zero();
    int n = 0;
    for (const auto& r : st->result.profiles.regions)
        if (r.depth % 2 == 0) c += r.interiorPoint, ++n;
    if (!n) return std::nullopt;
    c /= n;
    const auto& f = st->frame;
    Vec3 dir = f.normal * (extrude_->reversed() ? -1.0 : 1.0);
    const double off = extrude_->symmetric() ? *depth / 2 : *depth;
    const Point3 base = f.toWorld(c) + dir * off;
    const Camera& cam = view_->camera();
    const Point3 tip = base + dir * (MANIP_ARROW_PX * cam.mmPerPx());
    auto Q = [](const Point3& p) { return QVector3D(float(p.x()), float(p.y()), float(p.z())); };
    const QPointF a = cam.project(Q(base)), b = cam.project(Q(tip));
    if (std::hypot(b.x() - a.x(), b.y() - a.y()) < 6) return std::nullopt;  // manipulator axis along the line of sight
    return std::make_pair(a, b);
}

bool MainWindow::onlyUiTimersPendingForTest() const {
    if (model_->regenPending() || model_->previewPending() || exportPending_) return false;
    if (sceneRevision_ != model_->session().revision()) return false;
    if (view_->isVisible() && !view_->frameUpToDate()) return false;
    if (view_->isAnimating()) return false;
    if (solutionWizard_ && solutionWizard_->checkPending()) return false;
    if (QApplication::activeModalWidget()) return false;
    return panel_->previewTimerActive() || ctxTimer_.isActive() || retessTimer_.isActive() || ExprField::anyCheckPending();
}

bool MainWindow::fastForwardUiTimerForTest() {
    std::vector<QTimer*> timers = ExprField::pendingCheckTimersForTest();
    for (QTimer* t : {panel_->previewTimerForTest(), &ctxTimer_, &retessTimer_})
        if (t->isActive()) timers.push_back(t);
    QTimer* first = nullptr;
    for (QTimer* t : timers)
        if (!first || t->remainingTime() < first->remainingTime()) first = t;
    if (!first) return false;
    // The timer's own expiry path: QTimer::timerEvent stops a single-shot timer and emits timeout.
    QTimerEvent ev(first->id());
    QCoreApplication::sendEvent(first, &ev);
    return true;
}

QString MainWindow::idleReport() const {
    QStringList r;
    if (model_->regenPending()) r << QString("regenPending(rev=%1, applied=%2)").arg(model_->session().revision()).arg(model_->result() ? model_->result()->revision : 0);
    if (model_->previewPending()) r << "previewPending";
    if (exportPending_) r << "exportPending";
    if (panel_->previewTimerActive()) r << "previewTimer";
    if (sceneRevision_ != model_->session().revision()) r << QString("sceneRevision=%1").arg(sceneRevision_);
    if (view_->isVisible() && !view_->frameUpToDate()) r << QString("frameNotPainted(frame=%1, visible=%2)").arg(view_->frameNumber()).arg(view_->isVisible());
    if (view_->isAnimating()) r << "cameraAnimating";
    if (ctxTimer_.isActive()) r << "contextBarTimer";
    if (retessTimer_.isActive()) r << "retessTimer";
    if (ExprField::anyCheckPending()) r << "exprCheckTimer";
    return r.join(", ");
}

}  // namespace construe::app
