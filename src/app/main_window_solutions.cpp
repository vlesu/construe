// Main window: the connection wizard (spec/app-ux.md §8д, solutions.md; S6-SOL-002…008) — a new connection from the
// «Assembly» tab, editing from the tree, faces picked in the view, applying the solution; recording a solution
// (§8е, S6-SOL-009) — the recorder panel and saving the package into the user library folder.
#include "app/main_window.h"

#include "app/messages.h"

#include <QDir>

namespace construe::app {

void MainWindow::cmdSolution() {
    if (mode_ != Mode::Part || !assemblyActive()) return;
    setMode(Mode::Solution);
    solutionWizard_->setDefaultCut(appSettings_->connectionCut());  // S6-SOL-010: from the settings
    solutionWizard_->start(std::nullopt);
    updateAvailability();
}

void MainWindow::openSolutionWizard(const std::string& feature) {
    if (mode_ != Mode::Part || !assemblyActive()) return;
    setMode(Mode::Solution);
    // While editing, the connection is suppressed in the view: its fasteners hidden, parts without its cuts (FX7-ASM-004).
    if (model_->session().setConnectionEditing(feature)) model_->noteRecomputeRequested();  // the same revision
    solutionWizard_->start(feature);
    tree_->rebuild(feature);
    updateAvailability();
}

void MainWindow::pickedInSolution(const PickTarget& t, const std::optional<QVector3D>& at) {
    if (mode_ != Mode::Solution && mode_ != Mode::SolutionRecord) return;
    std::optional<std::array<double, 3>> p;
    if (at) p = std::array<double, 3>{at->x(), at->y(), at->z()};
    if (t.kind == PickTarget::Kind::Face && !t.body.empty()) {
        const auto slash = t.body.rfind('/');
        if (slash == std::string::npos) return;
        const std::string ref = t.body.substr(0, slash) + "/" + t.name;  // assembly name of the face (assembly.md §3)
        mateTargets_[ref] = t;
        if (mode_ == Mode::SolutionRecord) solutionRecorder_->picked(ref, model_->session().assemblyFaceKind(ref), p);
        else solutionWizard_->picked(ref, model_->session().assemblyFaceKind(ref), p);
        return;
    }
    // An axis given otherwise (§8д, S7F-ASM-008): a circular edge, a reference axis, a sketch point or circle, a vertex —
    // the hole on it.
    if (mode_ != Mode::Solution || t.kind == PickTarget::Kind::None) return;
    if (!solutionWizard_->axisPlaceCurrent()) {
        solutionWizard_->setMessage(text("ui.solution.needPlane"));
        return;
    }
    const std::string ref = mateRefOf(t, model_->session().document().document->active());
    if (ref.empty()) return;
    auto faces = model_->session().holeAxisFaces(ref, solutionWizard_->frameFaceRef());
    if (!faces) {
        solutionWizard_->setMessage(errorText(faces.error()));
        return;
    }
    solutionWizard_->pickedAxes(*faces);
}

void MainWindow::solutionSameHoles() {
    if (mode_ != Mode::Solution || solutionWizard_->axes().empty()) return;
    auto faces = model_->session().holeAxisFaces(solutionWizard_->axes().back(), solutionWizard_->frameFaceRef(), true);
    if (!faces) {
        solutionWizard_->setMessage(errorText(faces.error()));
        return;
    }
    solutionWizard_->pickedAxes(*faces, false);
}

void MainWindow::syncSolutionSelection() {
    if (mode_ != Mode::Solution && mode_ != Mode::SolutionRecord) return;
    std::set<PickTarget> sel;
    for (const auto& r : mode_ == Mode::SolutionRecord ? solutionRecorder_->chosenFaces() : solutionWizard_->chosenFaces())
        if (auto t = pickTargetOfRef(r)) sel.insert(*t);
    view_->setSelection(sel);
}

void MainWindow::acceptSolution() {
    if (mode_ != Mode::Solution || !solutionWizard_->canAccept()) return;
    auto& s = model_->session();
    if (const auto& f = solutionWizard_->editing()) {  // all the axes stay in the connection (FX10-WZ-005)
        if (auto r = s.updateSolutionAxes(*f, solutionWizard_->inputs(), solutionWizard_->axes(), solutionWizard_->choices(), solutionWizard_->cut()); !r) {
            solutionWizard_->setMessage(errorText(r.error()));
            return;
        }
    } else if (auto r = s.applySolutionAxes(solutionWizard_->packageId(), solutionWizard_->inputs(),
                                            solutionWizard_->axes().size() > 1 ? solutionWizard_->axes() : std::vector<std::string>{},
                                            solutionWizard_->choices(), solutionWizard_->cut());
               !r) {
        solutionWizard_->setMessage(errorText(r.error()));
        return;
    }
    setMode(Mode::Part);
}

void MainWindow::openSolutionRecorder() {
    if (mode_ != Mode::Part || !assemblyActive()) return;
    setMode(Mode::SolutionRecord);
    solutionRecorder_->reset();
    updateAvailability();
}

void MainWindow::acceptSolutionRecord() {
    if (mode_ != Mode::SolutionRecord || !solutionRecorder_->canSave()) return;
    const QString dir = userLibraryDirectory();
    if (dir.isEmpty()) {
        solutionRecorder_->setMessage(text("ui.library.noUserFolder"));
        return;
    }
    QDir().mkpath(dir);
    auto r = model_->session().saveSolution(QDir::cleanPath(dir).toStdString(), solutionRecorder_->folder().toStdString(),
                                            *solutionRecorder_->definition());
    if (!r) {
        solutionRecorder_->setMessage(errorText(r.error()));
        return;
    }
    applyLibraryDirectories();  // the user folder may have just appeared
    setMode(Mode::Part);
    showToast(text("ui.record.saved", {{"title", solutionRecorder_->title()}}));
}

}  // namespace construe::app
