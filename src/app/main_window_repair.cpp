// Main window: the repair wizard of lost references (spec/app-ux.md §5.5в, regen.md §8а, S7F-FEAT-003).
#include "app/main_window.h"

#include "app/messages.h"
#include "app/theme.h"

#include <algorithm>

namespace construe::app {

namespace {

// Features with lost references in history order (lostReferences is in history order).
std::vector<std::string> lostFeatures(const std::vector<api::LostReference>& all) {
    std::vector<std::string> out;
    for (const auto& l : all)
        if (std::find(out.begin(), out.end(), l.feature) == out.end()) out.push_back(l.feature);
    return out;
}

bool sameReferences(const std::vector<api::LostReference>& a, const std::vector<api::LostReference>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].feature != b[i].feature || a[i].ref != b[i].ref) return false;
    return true;
}

}  // namespace

std::set<std::string> MainWindow::updateLostReferences() {
    std::set<std::string> fresh;
    auto& s = model_->session();
    const auto pr = model_->result();
    if (!pr) return fresh;
    const kernel::TessParams tp{SceneBuilder::chordFor(view_->camera().mmPerPx(), 0), VIEW_ANGLE_TOL};
    auto lost = s.lostReferences(tp);
    lostAll_ = lost ? std::move(*lost) : std::vector<api::LostReference>{};
    // Seen pairs are kept per element: coming back to an assembly does not make its known losses fresh again. A result of
    // an older revision (e.g. of the element left just now) does not change them.
    const std::string el = s.document().document->active().id;
    const std::string prefix = el + "\n";
    if (pr->revision == s.revision()) {
        std::erase_if(lostSeen_, [&](const auto& p) { return p.first.compare(0, prefix.size(), prefix) == 0; });
        for (const auto& l : lostAll_)
            if (lostSeen_.insert({prefix + l.feature, l.ref}).second) fresh.insert(l.feature);
    }
    // The undo point of the element: its last applied result of the current revision without lost references (§5.5в);
    // a deletion in a part that breaks an assembly is undone from the assembly too.
    if ((lostAll_.empty() && pr->revision == s.revision()) || !s.undoLabel()) repairUndoPoints_[el] = s.undoDepth();
    // Errors of lost references are never notified as errors (the wizard or the red mark instead), new or known.
    std::set<std::string> silent;
    for (const auto& l : lostAll_) silent.insert(l.feature);
    if (mode_ == Mode::Repair) {
        repairNext();
        return silent;
    }
    // After a user command (there is an undo step), not after opening a document.
    if (!fresh.empty() && s.undoLabel()) {
        if (mode_ == Mode::Part) startRepair();
        else
            showToast(text("ui.repair.toast", {{"count", QString::number(fresh.size())}}), false,
                      {{text("ui.repair.start"), [this] { startRepair(); }}});
    }
    return silent;
}

void MainWindow::startRepair(const std::string& feature) {
    if (mode_ != Mode::Part || lostAll_.empty()) return;
    const auto order = lostFeatures(lostAll_);
    if (!feature.empty() && std::find(order.begin(), order.end(), feature) == order.end()) return;  // nothing lost there
    repairSkipped_.clear();
    if (!feature.empty())  // the wizard starts at this feature: the ones before it are passed this round
        for (const auto& f : order) {
            if (f == feature) break;
            repairSkipped_.insert(f);
        }
    setMode(Mode::Repair);
    repairNext();
    rebuildScene();
}

void MainWindow::repairNext() {
    if (mode_ != Mode::Repair) return;
    const auto order = lostFeatures(lostAll_);
    std::string cur = repair_->feature();
    if (std::find(order.begin(), order.end(), cur) == order.end() || repairSkipped_.count(cur)) {
        cur.clear();
        for (const auto& f : order)
            if (!repairSkipped_.count(f)) {
                cur = f;
                break;
            }
    }
    if (cur.empty()) {  // nothing left: the wizard closes (§5.5в)
        setMode(Mode::Part);
        showToast(order.empty() ? text("ui.repair.done") : text("ui.repair.left", {{"count", QString::number(order.size())}}));
        return;
    }
    std::vector<api::LostReference> mine;
    for (const auto& l : lostAll_)
        if (l.feature == cur) mine.push_back(l);
    if (cur == repair_->feature() && sameReferences(mine, repair_->lost())) return;  // keep the picks
    // Name of the object (§5.5в): a feature, a mate or a weld of the assembly.
    const auto d = model_->session().document().document;
    std::string name = cur;
    if (const auto f = d->active().find(cur)) name = f->name;
    for (const auto& m : d->active().mates)
        if (m.id == cur) name = m.name;
    for (const auto& w : d->active().welds)
        if (w.value("id", std::string()) == cur) name = w.value("name", cur);
    const int index = int(std::find(order.begin(), order.end(), cur) - order.begin()) + 1;
    repairTargets_.clear();
    repair_->show(cur, QString::fromStdString(name), index, int(order.size()), mine);
    syncRepairSelection();
    setPrompt(repair_->slotTree()->places().hint());
}

void MainWindow::pickedInRepair(const PickTarget& t) {
    std::string kind;
    QString label;
    for (const auto& b : view_->bodies()) {  // «Edge N» / «Face N»: the position of the name in the shown body's mesh
        if (b.mesh.bodyId != t.body) continue;
        if (t.kind == PickTarget::Kind::Edge)
            for (size_t i = 0; i < b.mesh.edges.size(); ++i)
                if (b.mesh.edges[i].name == t.name) label = text("ui.repair.kind.edge") + " " + QString::number(i + 1);
        if (t.kind == PickTarget::Kind::Face)
            for (size_t i = 0; i < b.mesh.faceRanges.size(); ++i)
                if (b.mesh.faceRanges[i].name == t.name) label = text("ui.repair.kind.face") + " " + QString::number(i + 1);
        if (t.kind == PickTarget::Kind::Vertex)
            for (size_t i = 0; i < b.mesh.vertices.size(); ++i)
                if (b.mesh.vertices[i].name == t.name) label = text("ui.repair.kind.vertex") + " " + QString::number(i + 1);
    }
    if (t.kind == PickTarget::Kind::Face) kind = "face";
    else if (t.kind == PickTarget::Kind::Edge) kind = "edge";
    else if (t.kind == PickTarget::Kind::Vertex) kind = "vertex";
    if (kind.empty() || t.name.empty()) return;
    // In an assembly the reference is "<instance path>/<name>" (assembly.md §3, as the weld panel, §8а).
    std::string ref = t.name;
    if (model_->session().document().document->active().isAssembly()) {
        const auto slash = t.body.rfind('/');
        if (slash == std::string::npos) return;
        ref = t.body.substr(0, slash) + "/" + t.name;
    }
    if (label.isEmpty()) label = QString::fromStdString(t.name);
    repairTargets_[ref] = t;
    repair_->picked(ref, kind, SelectionItem{label, {t}, {}});
    setPrompt(repair_->slotTree()->places().hint());
}

void MainWindow::syncRepairSelection() {
    if (mode_ != Mode::Repair) return;
    std::set<PickTarget> sel;
    const SlotSet& set = repair_->slotTree()->places();
    for (int i = 0; i < set.count(); ++i)
        for (const auto& o : set.objects(i))
            if (auto it = repairTargets_.find(o.ref); it != repairTargets_.end()) sel.insert(it->second);
    view_->setSelection(sel);
}

void MainWindow::repairApply() {
    if (mode_ != Mode::Repair || !repair_->complete()) return;
    // One undo step per feature; the next feature — after the regeneration (updateLostReferences → repairNext).
    if (auto r = model_->session().rebindReferences(repair_->feature(), repair_->replacements()); !r)
        showToast(errorText(r.error()), true);
}

void MainWindow::repairSkip() {
    if (mode_ != Mode::Repair) return;
    repairSkipped_.insert(repair_->feature());
    repairNext();
    rebuildScene();
}

void MainWindow::repairDone() {
    if (mode_ != Mode::Repair) return;
    setMode(Mode::Part);
    rebuildScene();
}

void MainWindow::repairRevert() {
    if (mode_ != Mode::Repair) return;
    setMode(Mode::Part);
    auto& s = model_->session();
    // Undo down to the point before the references were lost: the deletion or edit and the wizard's rebinds (§5.5в).
    const auto point = repairUndoPoints_.find(s.document().document->active().id);
    const size_t to = point != repairUndoPoints_.end() ? point->second : s.undoDepth();
    while (s.undoDepth() > to)
        if (!s.undo()) break;
    showToast(text("ui.repair.reverted"));
    rebuildScene();
}

std::vector<DrawBody> MainWindow::repairGhosts() const {
    std::vector<DrawBody> out;
    int i = 0;
    for (const auto& l : repair_->lost()) {
        ++i;
        if (!l.ghost) continue;
        DrawBody b;
        b.mesh = *l.ghost;
        b.mesh.bodyId = "ghost:" + std::to_string(i);
        b.key = "ghost/" + std::to_string(model_->session().revision()) + "/" + l.feature + "/" + l.ref;
        b.color = COLOR_ERROR.color;
        b.alpha = float(PREVIEW_ALPHA);
        b.edgeColor = COLOR_ERROR.color;
        b.pickable = false;
        out.push_back(std::move(b));
    }
    return out;
}

}  // namespace construe::app
