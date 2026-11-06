// Main window: instance patterns and the exploded view of an assembly (spec/app-ux.md §8а; FX7-ASM-005, FX7-ASM-007).
#include "app/main_window.h"

#include <QDateTime>

#include <algorithm>

#include "app/messages.h"

namespace construe::app {

namespace {

// Top-level instance of a picked body «i2/body:f2» (nested «i3/i1/body:f2» → «i3»); empty — not an instance of the
// assembly document (a pattern child or a connection fastener).
std::string topInstanceOf(const PickTarget& t, const doc::Element& el) {
    if (t.body.empty()) return {};
    const std::string top = t.body.substr(0, t.body.find('/'));
    for (const auto& in : el.instances)
        if (in.id == top) return top;
    return {};
}

// Instance path of a picked body: «i3/i1/body:f2» → «i3/i1».
std::string instancePathOf(const PickTarget& t) {
    const auto slash = t.body.rfind('/');
    return slash == std::string::npos ? std::string() : t.body.substr(0, slash);
}

}  // namespace

void MainWindow::cmdInstancePattern() {
    if (mode_ != Mode::Part || !assemblyActive()) return;
    const auto d = model_->session().document().document;  // hold the document: fields of the temporary snapshot
    api::Json def = {{"mode", "linear"}};
    // Preselection: an instance in the tree or the body of a part selected in the view — the source.
    if (const auto in = tree_->selectedInstance()) def["source"] = *in;
    else
        for (const auto& t : view_->selection())
            if (const std::string top = topInstanceOf(t, d->active()); !top.empty()) {
                def["source"] = top;
                break;
            }
    instancePatternTargets_.clear();
    instancePattern_->start(std::nullopt, def);
    setMode(Mode::InstancePattern);
    syncInstancePatternSelection();
}

void MainWindow::editInstancePattern(const std::string& id) {
    if (mode_ != Mode::Part) return;
    const auto d = model_->session().document().document;  // hold the document: the record lives in the snapshot
    const int pi = d->active().patternIndex(id);
    if (pi < 0) return;
    instancePatternTargets_.clear();
    instancePattern_->start(id, d->active().patterns[size_t(pi)].def);
    setMode(Mode::InstancePattern);
    syncInstancePatternSelection();
}

// A click in the view (§8а): the active field takes it — the source (a part of the assembly), the direction or axis
// (as a mate object), in the «By part pattern» mode — the part instance with the pattern.
void MainWindow::pickedInInstancePattern(const PickTarget& t) {
    const auto d = model_->session().document().document;
    if (instancePattern_->activeField() == InstancePatternPanel::Field::Source) {
        const std::string top = topInstanceOf(t, d->active());
        if (top.empty()) return;
        instancePattern_->setSource(top);
        return;
    }
    if (instancePattern_->mode() == "feature") {
        const std::string path = instancePathOf(t);
        if (path.empty() || topInstanceOf(t, d->active()).empty()) return;
        instancePattern_->setRef(path);
        if (instancePattern_->definition()["feature"]["feature"].get<std::string>().empty()) showToast(text("ui.instancePattern.noPartPattern"));
        return;
    }
    const std::string r = mateRefOf(t, d->active());
    if (r.empty()) return;
    instancePatternTargets_[r] = t;
    instancePattern_->setRef(r);
}

void MainWindow::acceptInstancePattern() {
    if (!instancePattern_->ready()) {
        showToast(text("ui.instancePattern.incomplete"), true);
        return;
    }
    auto& s = model_->session();
    Status r = okStatus();
    api::Json def = instancePattern_->definition();
    if (instancePattern_->editing()) {
        for (const char* k : {"directions", "axis", "count", "angle", "reverse", "feature"})  // fields of other modes are removed
            if (!def.contains(k)) def[k] = nullptr;
        r = s.updateInstancePattern(*instancePattern_->editing(), def);
    } else if (auto a = s.addInstancePattern(def); !a) {
        r = a.error();
    }
    if (!r) {
        showToast(errorText(r.error(), s.document().document.get()), true);
        return;
    }
    setMode(Mode::Part);
}

// Field caption: an instance — its name; a reference — as in the mate panel.
QString MainWindow::instancePatternLabel(const std::string& ref) const {
    const auto d = model_->session().document().document;
    if (ref.find('/') == std::string::npos)
        for (const auto& in : d->active().instances)
            if (in.id == ref) return QString::fromStdString(in.name);
    if (instancePattern_->mode() == "feature" && ref == instancePattern_->ref()) {  // the part instance path
        if (const auto a = model_->session().assembly())
            if (const auto* e = a->find(ref)) return QString::fromStdString(e->name);
        return QString::fromStdString(ref);
    }
    return mateRefLabel(ref);
}

// Pattern features of the part of an assembly instance (this document's parts), for the «Part pattern» list.
std::vector<std::pair<std::string, QString>> MainWindow::partPatternFeatures(const std::string& instancePath) const {
    std::vector<std::pair<std::string, QString>> out;
    const auto a = model_->session().assembly();
    const auto* e = a ? a->find(instancePath) : nullptr;
    if (!e || !e->file.empty()) return out;  // parts of other files — not listed (app-ux §8а)
    const auto d = model_->session().document().document;
    const int ei = d->elementIndex(e->element);
    if (ei < 0) return out;
    for (const auto& f : d->elements[size_t(ei)].features)
        if (f->type == doc::FeatureType::Pattern) out.push_back({f->id, QString::fromStdString(f->name)});
    return out;
}

// The source instance and the picked reference are highlighted as a selection while the panel is open (§8а).
void MainWindow::syncInstancePatternSelection() {
    if (mode_ != Mode::InstancePattern) return;
    std::set<PickTarget> sel;
    if (instancePattern_->mode() != "feature")
        if (auto t = pickTargetOfRef(instancePattern_->ref())) sel.insert(*t);
    if (auto it = instancePatternTargets_.find(instancePattern_->ref()); it != instancePatternTargets_.end()) sel.insert(it->second);
    view_->setSelection(sel);
}

void MainWindow::leftInstancePattern() {
    instancePatternTargets_.clear();
    view_->setSelection({});
}

// «Exploded view» (FX7-ASM-005): a toggle of the assembly's mode; the state lives in the document.
void MainWindow::cmdExploded() {
    if (mode_ != Mode::Part || !assemblyActive()) {
        syncExplodedButton();
        return;
    }
    const bool on = !model_->session().document().document->active().explodedActive();
    beginExplodeTransition();
    if (auto r = model_->session().setExploded(on); !r) {
        explodePending_ = false;
        showToast(errorText(r.error()), true);
    }
    syncExplodedButton();
}

void MainWindow::cmdExplodeReset() {
    if (mode_ != Mode::Part || !assemblyActive()) return;
    beginExplodeTransition();
    if (auto r = model_->session().setExploded(true, true); !r) {
        explodePending_ = false;
        showToast(errorText(r.error()), true);
    }
    syncExplodedButton();
}

// Smooth exploded view transition (FX8-ASM-006, app-ux §8а): the placements shown now are remembered; after the
// regeneration the view moves the top-level instances to the new ones over explodeAnimMs_.
void MainWindow::beginExplodeTransition() {
    explodeFrom_.clear();
    if (!explodeShown_.empty()) {
        explodeFrom_ = explodeShown_;  // a new command during a transition starts from the current frame
    } else if (const auto a = model_->session().assembly()) {
        for (const auto& e : a->instances)
            if (e.depth == 0 && e.pattern.empty() && e.connection.empty()) explodeFrom_[e.path] = e.world;
    }
    explodeFrames_.clear();
    explodePending_ = true;
}

void MainWindow::startExplodeAnimation() {
    if (model_->regenPending()) return;  // the result of the command is not here yet
    explodePending_ = false;
    explodeTo_.clear();
    const auto a = model_->session().assembly();
    if (!a) return;
    bool moved = false;
    for (const auto& e : a->instances)
        if (e.depth == 0 && e.pattern.empty() && e.connection.empty()) {
            explodeTo_[e.path] = e.world;
            if (auto it = explodeFrom_.find(e.path); it != explodeFrom_.end())
                moved = moved || (it->second.t - e.world.t).norm() > 1e-9 || (it->second.R - e.world.R).norm() > 1e-12;
        }
    if (!moved || explodeAnimMs_ <= 0) {
        explodeShown_.clear();
        if (explodeTimer_) explodeTimer_->stop();
        return;
    }
    if (!explodeTimer_) {
        explodeTimer_ = new QTimer(this);
        explodeTimer_->setInterval(16);
        connect(explodeTimer_, &QTimer::timeout, this, [this] { explodeTick(); });
    }
    explodeStartMs_ = explodeNowMs();
    explodeTick();
    explodeTimer_->start();
}

qint64 MainWindow::explodeNowMs() const { return explodeClock_ ? explodeClock_() : QDateTime::currentMSecsSinceEpoch(); }

void MainWindow::explodeTick() {
    const double t = std::clamp(double(explodeNowMs() - explodeStartMs_) / std::max(1, explodeAnimMs_), 0.0, 1.0);
    if (t >= 1.0) {
        explodeShown_.clear();
        if (explodeTimer_) explodeTimer_->stop();
        rebuildScene();
        return;
    }
    const double k = t * t * (3 - 2 * t);  // smooth start and end
    explodeShown_.clear();
    for (const auto& [id, to] : explodeTo_) {
        const auto it = explodeFrom_.find(id);
        if (it == explodeFrom_.end()) continue;
        const regen::Motion3& from = it->second;
        regen::Motion3 m;
        m.t = from.t + (to.t - from.t) * k;
        m.R = Eigen::Quaterniond(from.R).slerp(k, Eigen::Quaterniond(to.R)).toRotationMatrix();
        explodeShown_[id] = m;
    }
    explodeFrames_.push_back(explodeShown_);
    rebuildScene();
}

void MainWindow::syncExplodedButton() {
    auto* b = ribbonButton("exploded");
    if (!b) return;
    const auto d = model_->session().document().document;
    b->setChecked(d->active().isAssembly() && d->active().explodedActive());
}

}  // namespace construe::app
