// Main window: assembly weld seams — panel, edge picking, weld lines in the view (spec/app-ux.md §8а, spec/weld.md).
#include "app/main_window.h"

#include "app/messages.h"

namespace construe::app {

namespace {

// Assembly reference of an instance face or edge picked in the view: body «i2/body:f2», edge «E{…}» → «i2/E{…}» (§8а).
std::string weldRef(const PickTarget& t, PickTarget::Kind kind) {
    if (t.kind != kind || t.body.empty()) return {};
    const auto slash = t.body.rfind('/');
    if (slash == std::string::npos) return {};
    return t.body.substr(0, slash) + "/" + t.name;
}

}  // namespace

void MainWindow::cmdWeld() {
    if (mode_ != Mode::Part || !assemblyActive()) return;
    api::Json w = api::Json::object();
    weldTargets_.clear();
    for (const auto& t : view_->selection())  // preselection — faces (FX8-WLD-001), otherwise edges selected in the view
        for (const auto& [key, kind] : {std::pair{"faces", PickTarget::Kind::Face}, std::pair{"edges", PickTarget::Kind::Edge}})
            if (const std::string r = weldRef(t, kind); !r.empty()) {
                if (!w.contains(key)) w[key] = api::Json::array();
                w[key].push_back(r);
                weldTargets_[r] = t;
            }
    if (w.contains("faces")) w.erase("edges");
    weld_->start(std::nullopt, w);
    setMode(Mode::Weld);
    syncWeldSelection();
}

void MainWindow::editWeld(const std::string& id) {
    if (mode_ != Mode::Part) return;
    const auto d = model_->session().document().document;  // hold the document: the record lives in the snapshot
    const int wi = d->active().weldIndex(id);
    if (wi < 0) return;
    weldTargets_.clear();
    weld_->start(id, d->active().welds[size_t(wi)]);
    setMode(Mode::Weld);
    syncWeldSelection();
}

void MainWindow::pickedInWeld(const PickTarget& t) {
    if (const std::string r = weldRef(t, PickTarget::Kind::Face); !r.empty()) {  // faces of the parts (FX8-WLD-001)
        weldTargets_[r] = t;
        weld_->toggleFace(r);
    } else if (const std::string e = weldRef(t, PickTarget::Kind::Edge); !e.empty()) {
        weldTargets_[e] = t;
        weld_->toggleEdge(e);
    } else {
        return;  // only instance faces and edges (§8а)
    }
    syncWeldSelection();
}

// Chosen weld edges are highlighted as a selection while the panel is open (§8а).
void MainWindow::syncWeldSelection() {
    if (mode_ != Mode::Weld) return;
    std::set<PickTarget> sel;
    for (const auto* refs : {&weld_->faces(), &weld_->edges()})
        for (const auto& r : *refs) {
            if (auto it = weldTargets_.find(r); it != weldTargets_.end()) {
                sel.insert(it->second);
                continue;
            }
            for (const auto& b : view_->bodies()) {  // editing: the instance body with this face or edge name
                const auto slash = b.mesh.bodyId.rfind('/');
                if (slash == std::string::npos) continue;
                const std::string path = b.mesh.bodyId.substr(0, slash + 1);
                if (r.rfind(path, 0) != 0) continue;
                const std::string name = r.substr(path.size());
                for (const auto& e : b.mesh.edges)
                    if (e.name == name) sel.insert(PickTarget{PickTarget::Kind::Edge, b.mesh.bodyId, name});
                for (const auto& f : b.mesh.faceRanges)
                    if (f.name == name) sel.insert(PickTarget{PickTarget::Kind::Face, b.mesh.bodyId, name});
            }
        }
    view_->setSelection(sel);
}

void MainWindow::acceptWeld() {
    if (!weld_->ready()) {
        showToast(text("ui.weld.needFaces"), true);
        return;
    }
    auto& s = model_->session();
    Status r = okStatus();
    api::Json def = weld_->definition();
    if (weld_->editing()) {
        // All fields (app-ux §8а): absent optional fields are removed.
        for (const char* k : {"process", "leg", "side", "allAround", "field", "intermittent", "removeReinforcement", "smooth", "faces", "edges"})
            if (!def.contains(k)) def[k] = nullptr;
        r = s.updateWeld(*weld_->editing(), def);
    } else if (auto a = s.addWeld(def); !a) {
        r = a.error();
    }
    if (!r) {
        showToast(errorText(r.error(), s.document().document.get()), true);
        return;
    }
    setMode(Mode::Part);
}

// Leaving weld mode (from setMode): the edge highlight is cleared.
void MainWindow::leftWeld() {
    weldTargets_.clear();
    view_->setSelection({});
}

void MainWindow::updateWeldLines() {
    const auto d = model_->session().document().document;
    if (!d->active().isAssembly() || mode_ == Mode::Sketch) {
        view_->setWelds({}, false);
        return;
    }
    auto lines = SceneBuilder::weldLines(d->active(), view_->bodies());
    for (const auto& w : faceWeldLines_) lines.push_back(w);  // welds on faces (FX8-WLD-001)
    view_->setWelds(std::move(lines), mode_ == Mode::Part);
}

}  // namespace construe::app
