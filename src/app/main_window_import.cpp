// Import of STEP, STL, 3MF via the panel (app-ux §10.7, core-api §4.7б/§4.7г/§4.7д; S4-REC-018); FreeCAD .FCStd —
// opening as a new document and export, both with the exchange report (app-ux §10.1, §10.9; S4A-UI-001, S4A-REP-001).
#include "app/main_window.h"
#include "app/session_log.h"
#include "app/long_operation.h"
#include "app/messages.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>

#include <algorithm>

namespace construe::app {

// A file is chosen (open dialog): the import panel with defaults from application settings (§10.6); nothing changes
// until ✓ is pressed in the panel.
void MainWindow::beginImport(const QString& path, bool mesh) {
    importPath_ = path;
    importMesh_ = mesh;
    setMode(Mode::Import);
    importPanel_->reset(QFileInfo(path).fileName(), mesh, appSettings_->healTolerance(), appSettings_->roundTolerance());
}

// Panel ✓: import the file; with "recognize construction" — immediately recover the history of each new body with
// the panel's rounding tolerance (a separate entry in the undo history).
void MainWindow::acceptImport() {
    if (mode_ != Mode::Import) return;
    const QString path = importPath_;
    const bool mesh = importMesh_, recognize = importPanel_->recognize();
    const double heal = importPanel_->healTolerance(), round = importPanel_->roundTolerance();
    setMode(Mode::Part);
    auto& session = model_->session();
    const auto d = session.document().document;
    const QString file = QFileInfo(path).fileName();
    std::vector<std::string> created;
    // Long operations (app-ux §9.3, FX7-UI-006): preparation in a worker thread with «Abort», then the
    // application here; an aborted one leaves the document as it was.
    const QString opTitle = text("ui.longOp.import", {{"file", file}});
    if (mesh) {
        std::optional<Result<api::MeshImportPrepared>> prep;
        if (!LongOperation::run(this, opTitle, [&](const Cancel* c) { prep = session.prepareImportMesh(path.toStdString(), c); }) || !prep)
            return showToast(text("error.API_CANCELLED"));
        const BusyScope busy;  // applying in the interface thread (app-ux §9.4)
        auto r = *prep ? session.applyImportMesh(std::move(**prep)) : Result<api::MeshImportReport>(prep->error());
        if (!r) {
            showToast(errorText(r.error(), d.get()), true);
            return;
        }
        int faces = 0, triangles = 0;
        bool all = true;
        for (const auto& b : r->bodies) {
            faces += b.faces;
            triangles += b.triangles;
            all = all && b.fitted;
            created.push_back(b.element);
        }
        if (all) showToast(text("ui.importMesh.done", {{"file", file}, {"faces", QString::number(faces)}}));
        else showToast(text("ui.importMesh.polyhedral", {{"file", file}, {"triangles", QString::number(triangles)}}), true);
    } else {
        // Shape healing — the panel's tolerance (default from settings; app-ux §10.6, core-api §4.7б; S4-REC-013);
        // with recognition identical products become one object, recognized once (app-ux §10.7; FX7-IO-001); names by
        // the file, a single part without its wrapping assembly (FX10-IO-001, FX10-IO-002).
        api::StepImportReport report;
        std::optional<Result<api::StepImportPrepared>> prep;
        if (!LongOperation::run(this, opTitle, [&](const Cancel* c) { prep = session.prepareImportStep(path.toStdString(), {.healTolerance = heal, .dedupe = recognize, .nameByFile = true, .singlePart = true}, c); }) ||
            !prep)
            return showToast(text("error.API_CANCELLED"));
        const BusyScope busy;  // app-ux §9.4
        auto r = *prep ? session.applyImportStep(std::move(**prep), &report) : Result<std::vector<std::string>>(prep->error());
        if (!r) {
            showToast(errorText(r.error(), d.get()), true);
            return;
        }
        created = *r;
        auto mm = [](double v) { return QString::number(v, 'g', 3); };
        double before = 0, after = 0, unhealed = 0;
        int healed = 0;
        for (const auto& b : report.bodies) {
            if (b.healed) ++healed, before = std::max(before, b.tolerance), after = std::max(after, b.healedTolerance);
            else unhealed = std::max(unhealed, b.tolerance);
        }
        if (unhealed > 0)
            showToast(text(unhealed > heal ? "ui.import.tolerance" : "ui.import.healFailed",
                           {{"file", file}, {"tol", mm(unhealed)}, {"heal", mm(heal)}}), true);
        else if (healed > 0)
            showToast(text("ui.import.healed", {{"file", file}, {"count", QString::number(healed)}, {"before", mm(before)}, {"after", mm(after)}}));
        else
            showToast(text("ui.import.done", {{"file", file}}));
    }
    if (!recognize) return;

    // Construction recognition (core-api §4.7г): every new body; a full result replaces the body, otherwise the body is
    // kept (FX10-IO-001); the imported assembly stays active.
    const std::string root = session.document().document->active().id;
    for (const auto& id : created) {
        const auto cur = session.document().document;
        const int i = cur->elementIndex(id);
        if (i < 0 || !cur->elements[size_t(i)].isBody()) continue;
        auto r = runReconstruct(id, {.roundTolerance = round, .replace = true});
        if (!r) {
            if (r.error().code == "API_CANCELLED") return showToast(errorText(r.error()));
            showToast(errorText(r.error(), session.document().document.get()), true);
            return;
        }
        const auto nd = session.document().document;
        const int pi = nd->elementIndex(r->element);
        const QString name = pi >= 0 ? QString::fromStdString(nd->elements[size_t(pi)].name) : QString();
        if (!r->replaced) {
            showToast(text("ui.reconstruct.kept", {{"name", name}}));
        } else if (!r->assembly.empty()) {
            const int ai = nd->elementIndex(r->assembly);
            const QString asmName = ai >= 0 ? QString::fromStdString(nd->elements[size_t(ai)].name) : name;
            int full = 0, partial = 0, importOnly = 0;
            for (const auto& p : r->parts) (p.level == "full" ? full : p.level == "partial" ? partial : importOnly)++;
            showToast(text("ui.reconstruct.multi", {{"count", QString::number(r->parts.size())}, {"full", QString::number(full)},
                                                    {"partial", QString::number(partial)}, {"importOnly", QString::number(importOnly)},
                                                    {"instances", QString::number(r->instances)}, {"name", asmName}}));
        } else {
            showToast(text(("ui.reconstruct." + r->level).c_str(), {{"name", name}}));
        }
    }
    const auto nd = session.document().document;
    const int ri = nd->elementIndex(root);
    if (ri >= 0 && !nd->elements[size_t(ri)].isBody() && nd->active().id != root) session.setActiveElement(root);
}

// Recognition of a body (core-api §4.7г) as a long operation (app-ux §9.3, FX7-UI-006): API_CANCELLED — aborted.
Result<api::ReconstructReport> MainWindow::runReconstruct(const std::string& body, const api::ReconstructOptions& options) {
    auto& session = model_->session();
    const auto d = session.document().document;
    const int i = d->elementIndex(body);
    const QString name = i >= 0 ? QString::fromStdString(d->elements[size_t(i)].name) : QString::fromStdString(body);
    std::optional<Result<api::ReconstructPrepared>> prep;
    if (!LongOperation::run(this, text("ui.longOp.reconstruct", {{"name", name}}),
                            [&](const Cancel* c) { prep = session.prepareReconstruct(body, options, c); }) ||
        !prep)
        return Error("API_CANCELLED");
    if (!*prep) return prep->error();
    const BusyScope busy;  // app-ux §9.4
    return session.applyReconstruct(std::move(**prep));
}

// «Import → FreeCAD (.FCStd)…»: the file opens as a new document, like «Open» (app-ux §10.1).
void MainWindow::cmdImportFcstd() {
    if (mode_ != Mode::Part) return;
    if (!confirmDiscard()) return;
    QFileDialog::Options opt = testMode_ ? QFileDialog::DontUseNativeDialog : QFileDialog::Options();
    const QString path = QFileDialog::getOpenFileName(this, txt("ui.main.importFreecad"), QString(), "FreeCAD (*.FCStd)", nullptr, opt);
    if (path.isEmpty()) return;
    openFcstdPath(path);
}

// A FreeCAD document → a new unsaved document of ours, then the exchange report (core-api §4.7е).
void MainWindow::openFcstdPath(const QString& path) {
    auto& session = model_->session();
    std::optional<Result<api::FcstdPrepared>> prep;
    if (!LongOperation::run(this, text("ui.longOp.import", {{"file", QFileInfo(path).fileName()}}),
                            [&](const Cancel* c) { prep = session.prepareOpenFcstd(path.toStdString(), c); }) ||
        !prep)
        return showToast(text("error.API_CANCELLED"));
    const BusyScope busy;  // app-ux §9.4
    auto r = *prep ? session.applyOpenFcstd(std::move(**prep)) : Result<api::FcstdReport>(prep->error());
    if (!r) {
        showToast(errorText(r.error(), nullptr, path), true);
        return;
    }
    tree_->resetViewState();
    noteFileUsed(path);
    logSession(LogLevel::Info, "file imported: " + QDir::toNativeSeparators(path));  // i18n-skip: log (FX14-UI-003)
    sketch_->close();
    setMode(Mode::Part);
    view_->fitAll();
    if (!fcstdReport_) fcstdReport_ = new FcstdReportWindow(this);
    fcstdReport_->open(text("ui.fcstdReport.import", {{"file", QFileInfo(path).fileName()}}), r->entries);
}

// «Export → FreeCAD (.FCStd)…»: the whole document or the active object (app-ux §10.1); the report window after it.
void MainWindow::cmdExportFcstd(bool activeOnly) {
    if (mode_ != Mode::Part || exportPending_) return;
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    auto& session = model_->session();
    const std::string active = session.document().document->active().id;
    QString suggested = QString::fromStdString(session.exportFileName(active));
    if (!activeOnly && session.path()) suggested = QFileInfo(QString::fromStdString(*session.path())).completeBaseName();
    QString path = QFileDialog::getSaveFileName(this, txt("ui.main.exportFreecad"), suggested + ".FCStd", "FreeCAD (*.FCStd)", nullptr, opt);
    if (path.isEmpty()) return;
    if (!path.endsWith(".fcstd", Qt::CaseInsensitive)) path += ".FCStd";
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto r = session.exportFcstd(path.toStdString(), activeOnly ? std::optional<std::string>(active) : std::nullopt);
    QApplication::restoreOverrideCursor();
    const auto d = session.document().document;
    if (!r) {
        showToast(errorText(r.error(), d.get(), path), true);
        return;
    }
    const QString file = QFileInfo(path).fileName();
    showToast(text("ui.fcstdExport.done", {{"file", file}}));
    if (!fcstdReport_) fcstdReport_ = new FcstdReportWindow(this);
    fcstdReport_->open(text("ui.fcstdReport.export", {{"file", file}}), r->entries);
}

}  // namespace construe::app
