// Main window: feature commands, document settings, data exchange (spec/app-ux.md §4).
#include "app/main_window.h"
#include "app/sheet_print.h"

#include "app/document_hub.h"

#include "app/part_tool.h"
#include "api/sketch_ops.h"
#include "app/scene_builder.h"

#include "app/constraint_commands.h"
#include "app/hotkeys_table.h"
#include "app/icons.h"
#include "app/keys.h"
#include "app/messages.h"
#include "app/retranslate.h"
#include "app/theme.h"

#include <QLocale>
#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include "app/app_data.h"
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
#include <QTableWidget>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <numbers>
#include <tuple>

namespace construe::app {

void MainWindow::cmdThread() {
    if (mode_ != Mode::Part) return;
    const std::set<PickTarget> pre = view_->selection();  // preselection — the selected face (§5.8)
    previewId_.clear();
    previewValid_ = false;
    panel_ = thread_;
    setMode(Mode::FeatureEdit);
    thread_->beginNew(std::nullopt);
    for (const auto& t : pre)
        if (t.kind == PickTarget::Kind::Face) thread_->setFace(t.name, t.body);
    syncRefSelection();
}

void MainWindow::cmdGear() {
    if (mode_ != Mode::Part) return;
    const std::set<PickTarget> pre = view_->selection();  // preselection — the selected face (§5.3л)
    previewId_.clear();
    previewValid_ = false;
    panel_ = gear_;
    setMode(Mode::FeatureEdit);
    gear_->beginNew(std::nullopt);
    for (const auto& t : pre)
        if (t.kind == PickTarget::Kind::Face) gear_->setFace(t.name);
    syncRefSelection();
}

void MainWindow::cmdHole() {
    if (mode_ != Mode::Part) return;
    // Preselection (§5.3з): the selected sketch; otherwise the sketch above the marker with hole points (holePoints).
    std::optional<std::string> sk;
    const auto d = model_->session().document().document;
    if (auto f = tree_->selectedFeature()) {
        const auto ff = d->active().find(*f);
        if (ff && ff->sketchData()) sk = *f;
    }
    const std::optional<PickTarget> base = sk ? std::nullopt : selectedBase();  // §3.6б, S7F-FEAT-002
    if (!sk && !base) {
        const auto& el = d->active();
        int pos = int(el.features.size());
        if (el.rollback.kind == doc::Rollback::Kind::Start) pos = 0;
        else if (el.rollback.kind == doc::Rollback::Kind::After) pos = el.indexOf(el.rollback.feature) + 1;
        if (pos > 0 && el.features[pos - 1]->sketchData())  // hole points — holePoints query (§5.3з, FX1-FEAT-002)
            if (auto pts = model_->session().holePoints(el.features[pos - 1]->id); pts && !pts->empty()) sk = el.features[pos - 1]->id;
    }
    previewId_.clear();
    previewValid_ = false;
    panel_ = hole_;
    hole_->setDefaultCut(appSettings_->connectionCut());  // the assembly hole: where to cut, as connections (FX8-ASM-007)
    setMode(Mode::FeatureEdit);
    hole_->beginNew(sk);
    if (base) startBaseSketch(*base);
}

void MainWindow::cmdLoft() {
    if (mode_ != Mode::Part) return;
    std::vector<std::string> pre;  // preselection — selected sketches in history order (§5.3ж)
    const auto d = model_->session().document().document;
    for (const auto& f : d->active().features)
        for (auto* it : tree_->selectedItems())
            if (f->sketchData() && it->data(0, FeatureTree::IdRole).toString().toStdString() == f->id) pre.push_back(f->id);
    previewId_.clear();
    previewValid_ = false;
    panel_ = loft_;
    const std::optional<PickTarget> base = pre.empty() ? selectedBase() : std::nullopt;  // §3.6б, S7F-FEAT-002
    setMode(Mode::FeatureEdit);
    loft_->beginNew(std::nullopt);
    for (const auto& id : pre) loft_->setSketch(id);
    updateRegions();
    if (base) startBaseSketch(*base);
}

void MainWindow::cmdDraft() {
    if (mode_ != Mode::Part) return;
    const std::set<PickTarget> pre = view_->selection();  // preselection — faces (§5.8)
    previewId_.clear();
    previewValid_ = false;
    panel_ = draft_;
    setMode(Mode::FeatureEdit);
    draft_->beginNew(std::nullopt);
    if (auto pl = tree_->selectedPlane()) draft_->pick({PickTarget::Kind::Plane, {}, *pl});
    bool anyFace = false;
    for (const auto& t : pre) anyFace = anyFace || t.kind == PickTarget::Kind::Face;
    if (anyFace) draft_->activateFaces();
    for (const auto& t : pre)
        if (t.kind == PickTarget::Kind::Face) draft_->pick(t);
    syncRefSelection();
}

// Sheet parameters (§5.3к, S5-SHEET-001): the part's «Sheet» feature is edited, otherwise a new one.
void MainWindow::cmdSheetMetal() {
    if (mode_ != Mode::Part) return;
    const auto d = model_->session().document().document;  // holds the document: active() refers into it
    // An ordinary part: «Преобразовать в листовую деталь…» — as the tree command (FX12-SH-001).
    if (!availabilityState().sheetPart && d->active().kind == "part") {
        cmdConvertPart(d->active().id, true);
        return;
    }
    for (const auto& f : d->active().features)
        if (f->type == doc::FeatureType::SheetMetal) {
            editFeature(f->id);
            return;
        }
    previewId_.clear();
    previewValid_ = false;
    panel_ = sheetMetal_;
    setMode(Mode::FeatureEdit);
    sheetMetal_->beginNew(std::nullopt);
}

void MainWindow::cmdSheetFlange() {
    if (mode_ != Mode::Part) return;
    std::vector<std::string> edges;  // preselection — selected edges (§5.8)
    for (const auto& t : view_->selection())
        if (t.kind == PickTarget::Kind::Edge) edges.push_back(t.name);
    previewId_.clear();
    previewValid_ = false;
    panel_ = sheetFlange_;
    setMode(Mode::FeatureEdit);
    sheetFlange_->beginNew(std::nullopt);
    sheetFlange_->setEdges(edges);
    syncRefSelection();
}

// «Unfold» / «Fold» (§7.4, S5-FLAT-001): display state, not an undo step.
void MainWindow::cmdSheetUnfold() {
    if (mode_ != Mode::Part) return;
    unfolded_ = !unfolded_;
    if (QToolButton* b = ribbonButton("sheetUnfold")) {
        b->setChecked(unfolded_);
        b->setToolTip(unfolded_ ? txt("ui.main.foldShowTheFoldedPart") : txt("ui.main.unfoldShowTheSheetMetal"));
    }
    rebuildScene();
}

void MainWindow::cmdShell() {
    if (mode_ != Mode::Part) return;
    std::vector<std::string> faces;  // preselection — selected faces (§5.8)
    for (const auto& t : view_->selection())
        if (t.kind == PickTarget::Kind::Face) faces.push_back(t.name);
    previewId_.clear();
    previewValid_ = false;
    panel_ = shell_;
    setMode(Mode::FeatureEdit);
    shell_->beginNew(std::nullopt);
    shell_->setFaces(faces);
    syncRefSelection();
}

void MainWindow::cmdPattern(PatternPanel* panel, const std::string& mode) {
    if (mode_ != Mode::Part) return;
    const std::set<PickTarget> pre = view_->selection();  // preselection (§5.8)
    std::vector<std::string> preFeatures;
    for (auto* it : tree_->selectedItems())
        if (it->data(0, FeatureTree::KindRole).toString() == "feature") preFeatures.push_back(it->data(0, FeatureTree::IdRole).toString().toStdString());
    previewId_.clear();
    previewValid_ = false;
    panel_ = panel;
    setMode(Mode::FeatureEdit);
    panel->startNewMode(mode);
    for (const auto& id : preFeatures) panel->toggleFeature(id);
    for (const auto& t : pre) panel->pick(t);
    if (auto pl = tree_->selectedPlane(); pl && panel->isMirror() && pre.empty()) panel->pick({PickTarget::Kind::Plane, {}, *pl});
    syncRefSelection();
}

std::optional<PickTarget> MainWindow::selectedBase() const {
    std::optional<std::string> plane = tree_->selectedPlane();
    if (!plane)  // a «Plane» feature selected in the tree
        if (auto f = tree_->selectedFeature())
            if (auto ff = model_->session().document().document->active().find(*f); ff && ff->type == doc::FeatureType::Plane) plane = *f;
    if (plane && *plane == usedTreePlane_) plane.reset();  // already used for a sketch (§3.6б)
    // The view mirrors the tree selection: the used plane is not a base there either.
    std::set<PickTarget> view;
    for (const auto& t : view_->selection())
        if (!(t.kind == PickTarget::Kind::Plane && t.name == usedTreePlane_)) view.insert(t);
    return selectionBase(view, plane);
}

void MainWindow::startFeature(FeaturePanel* panel) {
    if (mode_ != Mode::Part) return;
    // Preselection (§5.8): the selected sketch; otherwise the sketch directly above the marker that has no references.
    std::optional<std::string> sk;
    const auto d = model_->session().document().document;
    if (auto f = tree_->selectedFeature()) {
        const auto ff = d->active().find(*f);
        if (ff && ff->sketchData()) sk = *f;
    }
    // A selected plane or face — the base of a new sketch (§3.6б, S7F-FEAT-002); regions and a sketch take precedence.
    std::optional<PickTarget> base = sk ? std::nullopt : selectedBase();
    if (!sk && !base) {
        const auto& el = d->active();
        int pos = int(el.features.size());
        if (el.rollback.kind == doc::Rollback::Kind::Start) pos = 0;
        else if (el.rollback.kind == doc::Rollback::Kind::After) pos = el.indexOf(el.rollback.feature) + 1;
        if (pos > 0 && el.features[pos - 1]->sketchData()) {
            const std::string id = el.features[pos - 1]->id;
            bool used = false;
            for (const auto& f : el.features)
                if ((f->extrude() && f->extrude()->sketch == id) || (f->revolve() && f->revolve()->sketch == id) ||
                    ((f->type == doc::FeatureType::SheetWall || f->type == doc::FeatureType::SheetCut) &&
                     f->pattern()->def.value("sketch", std::string()) == id))
                    used = true;
            auto st = model_->session().sketchState(id);
            bool hasRegions = false;
            if (st)
                for (const auto& r : st->result.profiles.regions)
                    if (r.depth % 2 == 0) hasRegions = true;
            if (!used && hasRegions) sk = id;
        }
    }
    std::vector<std::pair<std::string, std::string>> preRegions;
    for (const auto& t : view_->selection())
        if (t.kind == PickTarget::Kind::Region) preRegions.push_back({t.body, t.name});
    previewId_.clear();
    previewValid_ = false;
    panel_ = panel;
    setMode(Mode::FeatureEdit);
    view_->setSelection({});
    if (!preRegions.empty()) {
        panel->beginNew(preRegions.front().first);
        for (const auto& [skId, rid] : preRegions)
            if (skId == preRegions.front().first) panel->toggleRegion(skId, rid);
    } else {
        panel->beginNew(sk);
    }
    updateRegions();
    if (preRegions.empty() && base) startBaseSketch(*base);  // §3.6а: the panel returns after the sketch is closed
}

void MainWindow::editFeature(const std::string& id) {
    if (mode_ != Mode::Part) return;
    const auto f = model_->session().document().document->active().find(id);
    if (!f || f->sketchData() || f->type == doc::FeatureType::Import) return;  // a base body has no parameters (S4-REC-009)
    if (f->type == doc::FeatureType::Connection) return openSolutionWizard(id);  // the connection wizard (§8д)
    if (f->type == doc::FeatureType::ConnectionCut) {  // owned by a connection: edited through it (§8д, S6-SOL-010)
        if (auto s = model_->session().updateFeature(id, doc::Json::object()); !s) showToast(errorText(s.error()), true);
        return;
    }
    previewId_.clear();
    previewValid_ = false;
    if (f->extrude()) panel_ = extrude_;
    else if (f->revolve()) panel_ = revolve_;
    else if (f->refGeom()) panel_ = f->type == doc::FeatureType::Plane ? refPlane_ : refAxis_;
    else if (f->type == doc::FeatureType::Mirror) panel_ = mirror_;
    else if (f->type == doc::FeatureType::Shell) panel_ = shell_;
    else if (f->type == doc::FeatureType::Draft) panel_ = draft_;
    else if (f->type == doc::FeatureType::Sweep) panel_ = sweep_;
    else if (f->type == doc::FeatureType::Loft) panel_ = loft_;
    else if (f->type == doc::FeatureType::Hole) panel_ = hole_;
    else if (f->type == doc::FeatureType::Thread) panel_ = thread_;
    else if (f->type == doc::FeatureType::Gear) panel_ = gear_;
    else if (f->type == doc::FeatureType::Coil) panel_ = coil_;
    else if (f->type == doc::FeatureType::SheetMetal) panel_ = sheetMetal_;
    else if (f->type == doc::FeatureType::SheetWall) panel_ = sheetWall_;
    else if (f->type == doc::FeatureType::SheetFlange) panel_ = sheetFlange_;
    else if (f->type == doc::FeatureType::SheetBend) panel_ = sheetBend_;
    else if (f->type == doc::FeatureType::SheetCut) panel_ = sheetCut_;
    else if (f->pattern()) panel_ = pattern_;
    else panel_ = f->type == doc::FeatureType::Fillet ? fillet_ : chamfer_;
    setMode(Mode::FeatureEdit);
    panel_->beginEdit(id);
    syncRefSelection();
    tree_->rebuild(id);
}

void MainWindow::requestPreview() {
    if (mode_ != Mode::FeatureEdit || !panel_->canAccept()) return;
    auto r = model_->session().previewFeature(panel_->fullDefinition(), panel_->editing(), panel_->aux());
    if (!r) {
        panel_->setPreviewMessage(errorText(r.error()), true);
        return;
    }
    previewId_ = QString::fromStdString(*r);
    previewValid_ = false;
    model_->notePreviewRequested(previewId_);
}

void MainWindow::acceptFeature() {
    if (mode_ != Mode::FeatureEdit || !panel_->canAccept()) return;
    // An invalid tolerance record keeps the panel open (app-ux §5.6а, FX12-PMI-007).
    if (!panel_->tolerancesValid()) {
        showToast(panel_->toleranceError(), true);
        return;
    }
    auto& s = model_->session();
    Status st = okStatus();
    const auto editing = panel_->editing();
    s.beginGroup(panel_ == revolve_ ? "undo.group.revolve" : panel_ == fillet_ ? "undo.group.fillet"
                 : panel_ == chamfer_ ? "undo.group.chamfer" : panel_ == refPlane_ ? "undo.group.plane"
                 : panel_ == refAxis_ ? "undo.group.axis" : "undo.group.extrude");
    if (editing) {
        const doc::Json def = panel_->fullDefinition();
        const auto oldDef = s.featureDefinition(*editing);
        if (!oldDef || *oldDef != def || !panel_->aux().empty()) st = s.updateFeature(*editing, def, panel_->aux());
    } else {
        auto r = s.addFeature(panel_->fullDefinition(), panel_->aux());  // owned features of the selection (FX7-SEL-001)
        if (!r) st = r.error();
    }
    s.endGroup();
    if (!st) showToast(errorText(st.error(), s.document().document.get()), true);
    s.cancelPreview();
    model_->notePreviewCancelled();
    previewId_.clear();
    previewValid_ = false;
    setMode(Mode::Part);
    tree_->rebuild();
}

void MainWindow::cancelFeature() {
    model_->session().cancelPreview();
    model_->notePreviewCancelled();
    previewId_.clear();
    previewValid_ = false;
    setMode(Mode::Part);
    tree_->rebuild();
}

void MainWindow::showAppSettings(int tab) { appSettingsWindow_->showTab(tab); }

// Applying an application setting (§10.6).
void MainWindow::applySetting(const QString& key) {
    if (key == "view/mouseProfile")
        view_->setMouseProfile(appSettings_->mouseProfile() == "rightButton" ? Viewport::MouseProfile::RightButton
                                                                            : Viewport::MouseProfile::MiddleButton);
    else if (key == "view/invertWheel")
        view_->setInvertWheel(appSettings_->invertWheel());
    else if (key == "view/displayStyle") {
        const auto s = styleFromName(appSettings_->displayStyle().toStdString());
        if (s && styleActions_ && view_->displayStyle() != *s) setDisplayStyle(*s);
    } else if (key == "fonts/textFont")
        api::Session::setPreferredFont(appSettings_->textFont().toStdString());
    else if (key == "drawing/fontGost" || key == "drawing/fontIso") {  // drawing.md §3.2: sheet font by standard
        const auto s = key == "drawing/fontIso" ? font::DrawingStandard::Iso : font::DrawingStandard::Gost;
        api::Session::setPreferredDrawingFont(s, appSettings_->value(key).toString().toStdString());
        if (center_ && model_ && drawingActive()) refreshDrawing();
    } else if (key == "library/userDirectory" || key == "library/directories") {
        applyLibraryDirectories();
    } else if (key == "library/standardGost" || key == "library/standardIso") {  // FX11-STD-002 (library.md §2.7)
        libraryPanel_->setStandards({appSettings_->libraryGost(), appSettings_->libraryIso()});
    } else if (key == "ui/language") {
        applyLanguage();
    } else if (key == "fonts/directories") {
        api::Session::setFontDirectories(fontLibraryDirectories(dataDir_, appSettings_->fontDirectories(), systemFontDirectories()));
    }
}

QString MainWindow::settingLanguage() const {
    // Test mode: «As in the system» is Russian whatever the machine (§12.1) — the e2e tests find texts in Russian.
    const QStringList system = testMode_ ? QStringList{"ru"} : QLocale::system().uiLanguages();
    return resolveLanguage(appSettings_->value("ui/language").toString(), system);
}

// Language switch without a restart (§9.5): labels of the open windows are retranslated, the views built from the
// document are rebuilt; composite texts not recognized as table texts change at their next update.
void MainWindow::applyLanguage() {
    const QString from = language(), to = settingLanguage();
    if (from == to || !setLanguage(to)) return;
    retranslateWidgets(from, to);
    for (FeaturePanel* p : findChildren<FeaturePanel*>()) p->retranslateTitle(from, to);
    // The name of a new drawing follows the language too (drawing.md §2.6): «Чертёж Вал» / "Drawing Вал".
    model_->session().setDrawingNameTemplate(text("ui.drawing.autoName", {{"name", QStringLiteral("{name}")}}).toStdString());
    applyCutLabelFormat(model_->session());  // cut plan labels (drawing.md §2.7)
    applyNameBases();                         // default names of new objects (document.md §2.12, FX14-UI-004)
    // The library texts (library.md §17, FX14-I18N-001): the dictionary of the new language, the open catalog and wizard.
    model_->session().setLibraryLanguage(to.toStdString());
    if (mode_ == Mode::Library) libraryPanel_->retranslate();
    if (mode_ == Mode::Solution) solutionWizard_->retranslate();
    helpWindow_->setLanguage(to);
    onDocumentChanged();
    rebuildScene();
    updateUnitsStatus();
}

void MainWindow::cmdDocumentSettings() {
    if (mode_ != Mode::Part) return;
    setMode(Mode::Settings);
}

void MainWindow::acceptSettings() {
    const auto d = model_->session().document().document;
    const std::string lu = settingsPanel_->lengthUnit();
    const std::string au = settingsPanel_->angleUnit();
    setMode(Mode::Part);
    if (lu == d->lengthUnit && au == d->angleUnit) return;
    auto r = model_->session().setUnits(lu, au);
    if (!r) showToast(errorText(r.error()), true);
}

void MainWindow::updateUnitsStatus() {
    // Document units in the status bar (§2.1): «mm, °»; a click opens the settings (§10.4).
    const auto d = model_->session().document().document;
    statusUnits_->setText(unitsStatusText(d->lengthUnit, d->angleUnit));
}

void MainWindow::cmdNormalTo() {
    std::optional<construe::Plane> pl;
    if (mode_ == Mode::Sketch && sketch_->state()) pl = sketch_->state()->frame;
    else if (auto p = tree_->selectedPlane()) pl = api::Session::datumPlane(*p);
    else
        for (const auto& s : view_->selection())
            if (s.kind == PickTarget::Kind::Plane) pl = api::Session::datumPlane(s.name);
    if (!pl) {
        setPrompt(text("ui.status.pickPlane"));
        return;
    }
    Camera c = view_->targetCamera();
    const QVector3D n(float(pl->normal.x()), float(pl->normal.y()), float(pl->normal.z()));
    if (QVector3D::dotProduct(c.direction, -n) > std::cos(qDegreesToRadians(1.0))) c.setNormalTo(-n, QVector3D(float(pl->y.x()), float(pl->y.y()), float(pl->y.z())));
    else c.setNormalTo(n, QVector3D(float(pl->y.x()), float(pl->y.y()), float(pl->y.z())));
    view_->animateTo(c);
}

void MainWindow::cmdExportStep() {
    if (mode_ != Mode::Part || exportPending_) return;
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    const std::string active = model_->session().document().document->active().id;
    const QString suggested = QString::fromStdString(model_->session().exportFileName(active)) + ".step";
    QString path = QFileDialog::getSaveFileName(this, txt("ui.main.exportStep"), suggested, "STEP (*.step *.stp)", nullptr, opt);
    if (path.isEmpty()) return;
    if (!path.endsWith(".step", Qt::CaseInsensitive) && !path.endsWith(".stp", Qt::CaseInsensitive)) path += ".step";
    auto r = model_->session().exportStep(path.toStdString());
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    setProperty("exportPath", path);
    exportPending_ = true;
    model_->noteExportStarted();
    statusRegen_->setText(text("ui.export.running"));
    updateAvailability();  // §7.5: while an export runs, export items are unavailable
}

void MainWindow::cmdImportStep() {
    if (mode_ != Mode::Part) return;
    QFileDialog::Options opt = testMode_ ? QFileDialog::DontUseNativeDialog : QFileDialog::Options();
    const QString path = QFileDialog::getOpenFileName(this, txt("ui.main.importStep"), QString(), "STEP (*.step *.stp)", nullptr, opt);
    if (path.isEmpty()) return;
    beginImport(path, false);  // import parameters — the panel (app-ux §10.7, S4-REC-018)
}

// Import of an STL or 3MF mesh (app-ux §10.3, core-api §4.7д; S4-MESH-001): after a file is chosen — the import panel.
void MainWindow::cmdImportMesh() {
    if (mode_ != Mode::Part) return;
    QFileDialog::Options opt = testMode_ ? QFileDialog::DontUseNativeDialog : QFileDialog::Options();
    const QString path = QFileDialog::getOpenFileName(this, txt("ui.main.importMesh"), QString(), txt("ui.main.meshesStl3mfStlStl"), nullptr, opt);
    if (path.isEmpty()) return;
    beginImport(path, true);
}

// «Recognize design»: shape healing and history recovery of the active imported
// body (app-ux §8а, core-api §4.7г; S4-REC-002, S4-REC-017).
void MainWindow::cmdReconstruct() {
    if (mode_ != Mode::Part) return;
    const auto d = model_->session().document().document;
    if (!d->active().isBody()) return;
    // Healing tolerance and dimension value rounding — from app settings (app-ux §10.6; S4-REC-013, S4-REC-017); a long
    // operation with «Abort» (§9.3, FX7-UI-006).
    // A full result replaces the body in its assemblies, mates move to the new faces (FX10-IO-008); otherwise the result
    // is added beside the body.
    auto r = runReconstruct(d->active().id, {.roundTolerance = appSettings_->roundTolerance(), .healTolerance = appSettings_->healTolerance(),
                                             .replace = true, .keepPartial = true});
    if (!r) {
        if (r.error().code == "API_CANCELLED") return showToast(errorText(r.error()));
        showToast(errorText(r.error(), d.get()), true);
        return;
    }
    const auto nd = model_->session().document().document;
    const int i = nd->elementIndex(r->element);
    const QString name = i >= 0 ? QString::fromStdString(nd->elements[size_t(i)].name) : QString();
    // Shape healing: healed — "before → after", not healed — shape tolerance and healing tolerance.
    QString healNote;
    if (r->heal) {
        auto mm = [](double v) { return QString::number(v, 'g', 3); };
        healNote = r->heal->healed ? " " + text("ui.reconstruct.healed", {{"before", mm(r->heal->toleranceBefore)}, {"after", mm(r->heal->toleranceAfter)}})
                                   : " " + text("ui.reconstruct.healFailed", {{"tol", mm(r->heal->toleranceBefore)}, {"heal", mm(appSettings_->healTolerance())}});
    }
    // Recognition limited by the work limits (reconstruction.md §3, S4-REC-023): a "limit" note of any part.
    if (std::any_of(r->notes.begin(), r->notes.end(), [](const auto& n) { return n.first == "limit"; }))
        healNote += " " + text("ui.reconstruct.limit");
    // Mates whose references did not move to the new faces (FX10-IO-008): they show the mate error in the assembly.
    if (!r->lostRefs.empty()) healNote += " " + text("ui.reconstruct.matesLost", {{"count", QString::number(r->lostRefs.size())}});
    if (!r->assembly.empty()) {
        // A product of several bodies — a part per group of identical bodies (S4-REC-012, FX7-IO-001) and an assembly
        // of them (S4-REC-014): part count per level, instance count, assembly name.
        const int ai = nd->elementIndex(r->assembly);
        const QString asmName = ai >= 0 ? QString::fromStdString(nd->elements[size_t(ai)].name) : name;
        int full = 0, partial = 0, importOnly = 0;
        for (const auto& p : r->parts) (p.level == "full" ? full : p.level == "partial" ? partial : importOnly)++;
        showToast(text("ui.reconstruct.multi", {{"count", QString::number(r->parts.size())}, {"full", QString::number(full)},
                                                {"partial", QString::number(partial)}, {"importOnly", QString::number(importOnly)},
                                                {"instances", QString::number(r->instances)}, {"name", asmName}}) +
                  healNote);
        return;
    }
    showToast(text(("ui.reconstruct." + r->level).c_str(), {{"name", name}}) + healNote);
}

void MainWindow::cmdExportStl() {
    if (mode_ != Mode::Part || exportPending_) return;
    exportThreeMf_ = false;
    exportFlat_ = false;
    setMode(Mode::ExportStl);
}

void MainWindow::acceptExport() {
    if (exportPanel_->flat()) return acceptFlatExport();
    const auto o = exportPanel_->meshOptions();
    if (!o) return;  // «Custom» field error or nothing selected: OK is unavailable (§10.3)
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    // Default name — the object's "<designation> <name>" (core-api §4.7а exportFileName).
    const QString suggested = QString::fromStdString(model_->session().exportFileName(o->element));
    const bool tmf = o->fileFormat == io::MeshFileFormat::ThreeMf;
    const QString ext = tmf ? ".3mf" : ".stl";
    QString path;
    if (o->perPart) {
        path = QFileDialog::getExistingDirectory(this, txt("ui.main.exportStlFolder"), QString(), opt | QFileDialog::ShowDirsOnly);
        if (path.isEmpty()) return;
    } else {
        path = QFileDialog::getSaveFileName(this, tmf ? txt("ui.common.export3mf") : txt("ui.common.exportStl"), suggested + ext, tmf ? "3MF (*.3mf)" : "STL (*.stl)", nullptr, opt);
        if (path.isEmpty()) return;  // cancelling the dialog returns to the panel
        if (!path.endsWith(ext, Qt::CaseInsensitive)) path += ext;
    }
    auto r = model_->session().exportMeshes(path.toStdString(), *o);
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    setProperty("exportPath", path);
    exportPending_ = true;
    model_->noteExportStarted();
    statusRegen_->setText(text("ui.export.running"));
    setMode(Mode::Part);  // availability of export items (an export runs) — updateAvailability from setMode
}

// Flat pattern DXF (§10.3, S5-DXF-001): file or directory dialog → exportFlatDxf.
void MainWindow::acceptFlatExport() {
    const auto o = exportPanel_->flatOptions();
    if (!o) return;  // nothing selected or the contour check failed: OK is unavailable
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    QString path;
    if (o->perPart) {
        path = QFileDialog::getExistingDirectory(this, txt("ui.main.exportFlatPatternDxfFolder"), QString(), opt | QFileDialog::ShowDirsOnly);
        if (path.isEmpty()) return;
    } else {
        const QString suggested = QString::fromStdString(model_->session().exportFileName(o->element));
        path = QFileDialog::getSaveFileName(this, txt("ui.common.exportFlatPatternDxf"), suggested + ".dxf", "DXF (*.dxf)", nullptr, opt);
        if (path.isEmpty()) return;  // cancelling the dialog returns to the panel
        if (!path.endsWith(".dxf", Qt::CaseInsensitive)) path += ".dxf";
    }
    auto r = model_->session().exportFlatDxf(path.toStdString(), *o);
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    setProperty("exportPath", path);
    exportPending_ = true;
    model_->noteExportStarted();
    statusRegen_->setText(text("ui.export.running"));
    setMode(Mode::Part);
}

void MainWindow::escape() {
    if (ctx_->isVisible() || ctxTimer_.isActive()) {  // Esc ladder: first close the context panel (§6.13)
        ctxTimer_.stop();
        ctx_->hide();
        // The bar of a just placed dimension (S7F-SK-002) is shown by itself — not a ladder step.
        if (!(mode_ == Mode::Sketch && sketch_->dimensionAutoSelected())) return;
    }
    if (selectionEscape()) return;  // then a selection set or a candidate (selection.md К6)
    if (mode_ != Mode::Part) {
        runConfirm(modeConfirm(mode_).escape);  // the mode's Esc by the table (§2.4, S7F-RF-008)
        return;
    }
    if (drawingEscape()) return;  // sheet: cancel view placement or clear the view selection (§8б)
    view_->setSelection({});
    tree_->clearSelection();
}

void MainWindow::sketchEscapeBottom() {
    // The bottom of the Esc ladder (§6.13 item 6, FX10-SK-001): an unchanged sketch closes like ✗ «Undo».
    if (mode_ == Mode::Sketch && !model_->session().sketchChanged()) cmdCancelSketch();
}

void MainWindow::runConfirm(ConfirmAction a) {
    switch (a) {
        case ConfirmAction::None: break;
        case ConfirmAction::ExitSketch: cmdExitSketch(); break;
        case ConfirmAction::CancelSketch: cmdCancelSketch(); break;
        case ConfirmAction::SketchEscape:
            if (!sketch_->escape()) sketchEscapeBottom();
            break;
        case ConfirmAction::AcceptFeature: acceptFeature(); break;
        case ConfirmAction::CancelFeature: cancelFeature(); break;
        case ConfirmAction::AcceptExport: acceptExport(); break;
        case ConfirmAction::AcceptImport: acceptImport(); break;
        case ConfirmAction::AcceptSettings: acceptSettings(); break;
        case ConfirmAction::AcceptSection: acceptSection(); break;
        case ConfirmAction::AcceptAnnotation: acceptAnnotation(); break;
        case ConfirmAction::AcceptMate: acceptMate(); break;
        case ConfirmAction::AcceptWeld: acceptWeld(); break;
        case ConfirmAction::AcceptInstancePattern: acceptInstancePattern(); break;
        case ConfirmAction::AcceptLibrary: acceptLibrary(); break;
        case ConfirmAction::AcceptLibrarySave: acceptLibrarySave(); break;
        case ConfirmAction::AcceptSolution: acceptSolution(); break;
        case ConfirmAction::AcceptSolutionRecord: acceptSolutionRecord(); break;
        case ConfirmAction::RepairDone: repairDone(); break;
        case ConfirmAction::RepairRevert: repairRevert(); break;
        case ConfirmAction::ToPart: setMode(Mode::Part); break;
        case ConfirmAction::ToPartRebuild:
            setMode(Mode::Part);
            rebuildScene();
            break;
    }
}

}  // namespace construe::app
