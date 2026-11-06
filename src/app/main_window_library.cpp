// Main window: library of standard parts (spec/app-ux.md §8г, §10.6; library.md; S6-LIB-002…006) — library folders
// from the settings, the catalog panel and insertion, saving the active part as a library element, update of copies.
#include "app/main_window.h"

#include "app/messages.h"

#include <QDir>
#include <QFileInfo>

namespace construe::app {

QString MainWindow::userLibraryDirectory() const {
    const QString set = appSettings_->value("library/userDirectory").toString();
    if (!set.isEmpty()) return set;
    return dataDir_.isEmpty() ? QString() : dataDir_ + "/library";
}

void MainWindow::applyLibraryDirectories() {
    // Priority (library.md §4): the user folder, the shared folders in list order, the system folders.
    std::vector<std::string> dirs;
    const QString user = userLibraryDirectory();
    if (!user.isEmpty() && QFileInfo(user).isDir()) dirs.push_back(QDir::cleanPath(user).toStdString());
    for (const QString& d : appSettings_->value("library/directories").toStringList()) dirs.push_back(QDir::cleanPath(d).toStdString());
    for (auto& d : api::Session::defaultLibraryDirectories()) dirs.push_back(d);
    model_->session().setLibraryLanguage(language().toStdString());  // the dictionary of the interface language (library.md §17)
    model_->session().setLibraryDirectories(dirs);
    if (mode_ == Mode::Library) libraryPanel_->reload();
}

void MainWindow::openLibrary() {
    if (mode_ != Mode::Part) return;
    setMode(Mode::Library);
    libraryPanel_->reset(assemblyActive());
    updateAvailability();
}

void MainWindow::openLibraryReplace(const std::string& instance) {
    if (mode_ != Mode::Part && mode_ != Mode::Properties) return;
    auto& s = model_->session();
    const doc::LibraryPart* lp = s.instanceLibraryPart(instance);
    if (!lp || !assemblyActive()) return;
    std::string size;
    const auto held = s.document().document;
    for (const auto& in : held->active().instances)
        if (in.id == instance)
            for (const auto& r : in.variantRows)
                if (r.table == lp->sizesTable) size = r.row;
    if (size.empty())
        if (const auto* t = lp->document->variantTable(lp->sizesTable)) size = t->active;
    const std::string package = lp->package;
    setMode(Mode::Library);
    libraryPanel_->startReplace(instance, package, size, s.libraryAttachmentPlaces(instance));
    syncLibrarySelection();
    updateAvailability();
}

void MainWindow::acceptLibraryReplace() {
    if (mode_ != Mode::Library || libraryPanel_->replacing().empty() || !libraryPanel_->canAccept()) return;
    auto r = model_->session().replaceLibraryPart(libraryPanel_->replacing(), libraryPanel_->packageId(), libraryPanel_->size(),
                                                  libraryPanel_->targets());
    if (!r) {
        libraryPanel_->setSeatMessage(errorText(r.error()));
        return;
    }
    setMode(Mode::Part);
}

void MainWindow::acceptLibrary() {
    if (mode_ == Mode::Library && !libraryPanel_->replacing().empty()) {  // ✓ in the replacement mode
        acceptLibraryReplace();
        return;
    }
    if (mode_ != Mode::Library || !assemblyActive() || !libraryPanel_->canAccept()) return;
    const bool nonEmpty = !model_->session().document().document->active().instances.empty();
    auto r = model_->session().insertLibraryPart(libraryPanel_->packageId(), libraryPanel_->size());
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    setMode(Mode::Part);
    if (nonEmpty) startMateSeries(*r);  // as any insertion (FX1-ASM-002)
}

void MainWindow::acceptSeat() {
    if (mode_ != Mode::Library || !assemblyActive() || !libraryPanel_->canSeat()) return;
    // Insertion by attachments (library.md §6.6, FX10-LIB-003); a bearing — the seating on a shaft as its special case.
    auto r = model_->session().attachLibraryPart(libraryPanel_->packageId(), libraryPanel_->size(), libraryPanel_->targets());
    if (!r) {
        libraryPanel_->setSeatMessage(errorText(r.error()));
        return;
    }
    setMode(Mode::Part);  // no mate series: the attachments set the place
}

void MainWindow::pickedInLibrary(const PickTarget& t) {
    const bool vertex = t.kind == PickTarget::Kind::Vertex;
    if (mode_ != Mode::Library || !assemblyActive() || (t.kind != PickTarget::Kind::Face && !vertex) || t.body.empty()) return;
    const auto slash = t.body.rfind('/');
    if (slash == std::string::npos) return;
    const std::string ref = t.body.substr(0, slash) + "/" + t.name;  // assembly name of the face or vertex (assembly.md §3)
    mateTargets_[ref] = t;
    const auto si = vertex ? std::nullopt : model_->session().assemblyFaceSurface(ref);
    libraryPanel_->picked(ref, si ? std::optional(si->kind) : std::nullopt, si ? 2 * si->radius : 0, vertex);
    setPrompt(libraryPanel_->seatHint());  // names the current place (§5.5б)
}

void MainWindow::syncLibrarySelection() {
    if (mode_ != Mode::Library) return;
    std::set<PickTarget> sel;
    for (const auto& r : libraryPanel_->chosenFaces())
        if (auto t = pickTargetOfRef(r)) sel.insert(*t);
    view_->setSelection(sel);
}

void MainWindow::openLibrarySave() {
    if (mode_ != Mode::Part || model_->session().document().document->active().kind != "part") return;
    setMode(Mode::LibrarySave);
    librarySavePanel_->reset();
}

void MainWindow::acceptLibrarySave() {
    if (mode_ != Mode::LibrarySave) return;
    const QString dir = userLibraryDirectory();
    if (dir.isEmpty()) {
        librarySavePanel_->setMessage(text("ui.library.noUserFolder"));
        return;
    }
    QDir().mkpath(dir);
    api::Session::LibrarySaveDef def = librarySavePanel_->definition();
    def.parent = QDir::cleanPath(dir).toStdString();
    auto r = model_->session().saveLibraryPart(def);
    if (!r) {
        librarySavePanel_->setMessage(errorText(r.error()));
        return;
    }
    applyLibraryDirectories();  // the user folder may have just appeared
    setMode(Mode::Part);
    showToast(text("ui.library.saved", {{"title", QString::fromStdString(def.description.title)}}));
}

void MainWindow::cmdUpdateFromLibrary() {
    if (mode_ != Mode::Part) return;
    auto r = model_->session().updateFromLibrary();
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    QStringList parts;
    if (!r->updated.empty()) parts << text("ui.library.updated", {{"count", QString::number(r->updated.size())}});
    if (!r->missing.empty()) {
        QStringList ids;
        for (const auto& m : r->missing) ids << QString::fromStdString(m);
        parts << text("ui.library.missing", {{"packages", ids.join(", ")}});
    }
    showToast(parts.isEmpty() ? text("ui.library.upToDate") : parts.join(" "), !r->missing.empty() && r->updated.empty());
}

}  // namespace construe::app
