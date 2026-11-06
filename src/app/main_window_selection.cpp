// MainWindow: the unified selection approach (spec/selection.md §5, FX7-SEL-001) — the selection set of the sketch base
// (PickPlane), the hint with «Yes» / «Other» / «Reset», keys, candidate preview and pickable elements of visible
// sketches.
#include "app/main_window.h"

#include "app/feature_panel.h"
#include "app/feature_tree.h"
#include "app/mate_panel.h"
#include "app/messages.h"
#include "app/selection_bubble.h"
#include "app/selection_field.h"
#include "app/theme.h"

#include <QKeyEvent>
#include <QLabel>

#include <cmath>
#include <numbers>

namespace construe::app {

SelectionBubble* MainWindow::selectionBubble() {
    if (!selBubble_) {
        selBubble_ = new SelectionBubble(view_);
        connect(selBubble_, &SelectionBubble::yes, this, [this] { selectionYes(); });
        connect(selBubble_, &SelectionBubble::next, this, [this] { selectionNext(1); });
        connect(selBubble_, &SelectionBubble::reset, this, [this] { selectionReset(); });
    }
    return selBubble_;
}

SelectionField& MainWindow::sketchSelection() {
    if (!sketchSel_) {
        sketchSel_ = std::make_unique<SelectionField>(model_->session(), "plane");
        sketchSel_->setAlwaysConfirm(true);  // nothing is created before «Yes» (§5.2)
    }
    return *sketchSel_;
}

void MainWindow::setActiveSelection(SelectionField* f) {
    selField_ = f;
    updateSelectionUi();
}

void MainWindow::updateSelectionUi() {
    SelectionBubble* b = selectionBubble();
    const bool live = selField_ && (mode_ == Mode::PickPlane || mode_ == Mode::FeatureEdit);
    if (!live || (!selField_->pending() && !(selField_->hasSet() && !selField_->hasValue() && !selField_->hint().isEmpty()))) {
        b->hide();
        view_->update();
        return;
    }
    const api::StateRef st = mode_ == Mode::FeatureEdit && panel_ ? panel_->selectionState() : api::StateRef::final();
    if (const auto* c = selField_->current()) {
        QString t = candidateLabel(model_->session(), *c, st);
        if (selField_->variantCount() > 1)
            t += " " + text("ui.sel.counter", {{"k", QString::number(selField_->variantIndex() + 1)}, {"n", QString::number(selField_->variantCount())}});
        b->showAt(selAt_, t, true, selField_->variantCount());
        setPrompt(text("ui.sel.pendingStatus", {{"label", t}}));
    } else {
        b->showAt(selAt_, selField_->hint(), false, 0);
        setPrompt(selField_->hint());
    }
    view_->update();
}

const api::SelectionPreview* MainWindow::selectionPreview(bool* pending) const {
    if (!selField_ || (mode_ != Mode::PickPlane && mode_ != Mode::FeatureEdit)) return nullptr;
    if (const auto* c = selField_->current()) {
        if (pending) *pending = true;
        return &c->preview;
    }
    if (pending) *pending = false;
    // The arrow of an accepted direction is shown always (selection.md §3.2).
    return selField_->valuePreview() ? &*selField_->valuePreview() : nullptr;
}

void MainWindow::selectionYes() {
    if (!selField_ || !selField_->pending()) return;
    if (mode_ == Mode::PickPlane && selField_ == sketchSel_.get()) {
        // The plane and the sketch on it — one command (createSketch with aux, §5.2), then the view turns to it.
        const api::SelectionCandidate c = *selField_->current();
        selField_->clear();
        selField_ = nullptr;
        selectionBubble()->hide();
        auto r = model_->session().createSketch(c.value, c.aux);
        if (!r) {
            showToast(errorText(r.error()), true);
            return;
        }
        view_->setSelection({});
        editSketch(*r);
        return;
    }
    selField_->confirm();
    if (mode_ == Mode::FeatureEdit && panel_) panel_->selectionChanged();
    syncRefSelection();
    updateSelectionUi();
}

void MainWindow::selectionNext(int step) {
    if (!selField_ || !selField_->pending()) return;
    selField_->nextVariant(step);
    if (mode_ == Mode::FeatureEdit && panel_) panel_->selectionChanged();
    updateSelectionUi();
}

void MainWindow::selectionReset() {
    if (!selField_) return;
    selField_->reset();
    if (mode_ == Mode::FeatureEdit && panel_) panel_->selectionChanged();
    if (mode_ == Mode::FeatureEdit) syncRefSelection();
    else view_->setSelection({});
    setPrompt(mode_ == Mode::PickPlane ? text("ui.status.pickPlane") : QString());
    updateSelectionUi();
}

bool MainWindow::selectionEscape() {
    // A new step of the Esc ladder before the panel (selection.md К6): a set or a candidate is reset first.
    if (!selField_ || (mode_ != Mode::PickPlane && mode_ != Mode::FeatureEdit)) return false;
    if (!selField_->pending() && !(selField_->hasSet() && (selField_->extensible() || mode_ == Mode::PickPlane))) return false;
    selectionReset();
    return true;
}

bool MainWindow::selectionKey(QKeyEvent* k) {
    if (!selField_ || !selField_->pending() || (mode_ != Mode::PickPlane && mode_ != Mode::FeatureEdit)) return false;
    if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
        selectionYes();
        return true;
    }
    if (k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab) {
        selectionNext(k->key() == Qt::Key_Backtab || (k->modifiers() & Qt::ShiftModifier) ? -1 : 1);
        return true;
    }
    return false;
}

void MainWindow::pickedInPickPlane(const PickTarget& t0, bool ctrl) {
    const PickTarget t = assemblyPick(t0);  // face of an assembly instance — the assembly's name (S2-ASM-012)
    SelectionField& f = sketchSelection();
    if (t.kind == PickTarget::Kind::None) {
        setPrompt(text("ui.status.pickPlane"));
        return;
    }
    // A plane or a planar face without Ctrl and without a set — the sketch at once, as before (§5.2, К1).
    if (!ctrl && !f.hasSet()) {
        if (t.kind == PickTarget::Kind::Plane) {
            createSketchOn(t.name);
            return;
        }
        if (t.kind == PickTarget::Kind::Face) {
            auto info = model_->session().topologyInfo(t.name, api::StateRef::final());
            if (info && info->surface && info->surface->kind == kernel::SurfaceInfo::Kind::Plane) {
                createSketchOnFace(t0);
                return;
            }
        }
    }
    const auto outcome = f.click(pickRef(t), api::StateRef::final());
    if (outcome == SelectionField::Outcome::Rejected) {
        setPrompt(f.hint());
        return;
    }
    sketchPickTargets_[pickRef(t)] = t0;
    std::set<PickTarget> sel;
    for (const auto& r : f.picks())
        if (auto it = sketchPickTargets_.find(r); it != sketchPickTargets_.end()) sel.insert(it->second);
    view_->setSelection(sel);
    setActiveSelection(&f);
}

void MainWindow::noteViewClick(const QPoint& at) { selAt_ = at; }

bool MainWindow::sketchPicksActive() const {
    if (mode_ == Mode::PickPlane || mode_ == Mode::Mate || mode_ == Mode::InstancePattern) return true;
    // The loft panel picks start points of its sections (§5.3ж, FX10-PT-004).
    return mode_ == Mode::FeatureEdit && (dynamic_cast<PatternPanel*>(panel_) != nullptr || dynamic_cast<LoftPanel*>(panel_) != nullptr);
}

void MainWindow::updateSketchPicks() {
    std::vector<DrawSketchPick> picks;
    if (sketchPicksActive()) {
        auto& session = model_->session();
        const auto d = session.document().document;
        std::vector<std::string> sketches = tree_->shownSketches();
        if (auto* loft = mode_ == Mode::FeatureEdit ? dynamic_cast<LoftPanel*>(panel_) : nullptr)  // sections, even hidden ones
            for (const auto& sec : loft->sections())
                if (std::find(sketches.begin(), sketches.end(), sec.sketch) == sketches.end()) sketches.push_back(sec.sketch);
        for (const auto& id : sketches) {
            const auto f = d->active().find(id);
            auto st = session.sketchState(id);
            if (!f || !f->sketchData() || !st) continue;
            const auto& P = st->result.state.points;
            auto W = [&](const Point2& q) {
                const Point3 w = st->frame.toWorld(q);
                return QVector3D(float(w.x()), float(w.y()), float(w.z()));
            };
            for (const auto& e : f->sketchData()->entities) {
                DrawSketchPick p;
                p.sketch = id;
                p.entity = e.id;
                p.construction = e.construction;
                if (e.type == sketch::EntityType::Point && P.count(e.id)) {
                    p.kind = DrawSketchPick::Kind::Point;
                    p.points = {W(P.at(e.id))};
                } else if (e.type == sketch::EntityType::Line && P.count(e.p1) && P.count(e.p2)) {
                    p.kind = DrawSketchPick::Kind::Line;
                    p.points = {W(P.at(e.p1)), W(P.at(e.p2))};
                } else if ((e.type == sketch::EntityType::Circle || e.type == sketch::EntityType::Arc) && P.count(e.center)) {
                    p.kind = DrawSketchPick::Kind::Curve;
                    const Point2 c = P.at(e.center);
                    double r = e.r, a0 = 0, a1 = 2 * std::numbers::pi;
                    if (auto it = st->result.state.radii.find(e.id); it != st->result.state.radii.end()) r = it->second;
                    if (e.type == sketch::EntityType::Arc && P.count(e.start) && P.count(e.end)) {
                        const Point2 s0 = P.at(e.start) - c, s1 = P.at(e.end) - c;
                        r = s0.norm();
                        a0 = std::atan2(s0.y(), s0.x());
                        a1 = std::atan2(s1.y(), s1.x());
                        if (a1 <= a0) a1 += 2 * std::numbers::pi;
                    }
                    for (int i = 0; i <= 64; ++i) {
                        const double a = a0 + (a1 - a0) * i / 64;
                        p.points.push_back(W(c + Point2(r * std::cos(a), r * std::sin(a))));
                    }
                } else {
                    continue;
                }
                picks.push_back(std::move(p));
            }
        }
    }
    // Sketches of the instances' parts in the mate panel with «Part construction geometry» (FX9-ASM-001): points, lines,
    // circles and arcs at the instance placement; the pick's sketch is "<instance path>/<sketch ID>".
    if (mode_ == Mode::Mate && mate_->constructionShown())
        if (const auto a = model_->session().assembly())
            for (const auto& e : a->instances) {
                if (!e.state || e.error || e.hidden || e.suppressed || !e.pattern.empty() || !e.connection.empty()) continue;
                for (const auto& [sid, se] : e.state->sketches) {
                    const auto& P = se.result.state.points;
                    auto W = [&](const Point2& q) {
                        const Point3 w = e.world.apply(se.frame.toWorld(q));
                        return QVector3D(float(w.x()), float(w.y()), float(w.z()));
                    };
                    const std::string sketch = e.path + "/" + sid;
                    if (e.sketchEntities)
                        if (auto es = e.sketchEntities->find(sid); es != e.sketchEntities->end())
                            for (const auto& [eid, v] : es->second) {
                                DrawSketchPick p;
                                p.sketch = sketch;
                                p.entity = eid;
                                p.construction = true;
                                bool ok = true;
                                for (size_t q = 1; q < v.size(); ++q) ok = ok && P.count(v[q]);
                                if (!ok) continue;
                                if (v[0] == "line") {
                                    p.kind = DrawSketchPick::Kind::Line;
                                    p.points = {W(P.at(v[1])), W(P.at(v[2]))};
                                } else {
                                    p.kind = DrawSketchPick::Kind::Curve;
                                    const Point2 c = P.at(v[1]);
                                    const auto rit = se.result.state.radii.find(eid);
                                    const double r = rit != se.result.state.radii.end() ? rit->second : v.size() > 2 ? (P.at(v[2]) - c).norm() : 0;
                                    for (int i = 0; i <= 64; ++i) {
                                        const double ang = 2 * std::numbers::pi * i / 64;
                                        p.points.push_back(W(c + Point2(r * std::cos(ang), r * std::sin(ang))));
                                    }
                                }
                                picks.push_back(std::move(p));
                            }
                    for (const auto& [pid, q] : P) {
                        DrawSketchPick p;
                        p.sketch = sketch;
                        p.entity = pid;
                        p.construction = true;
                        p.kind = DrawSketchPick::Kind::Point;
                        p.points = {W(q)};
                        picks.push_back(std::move(p));
                    }
                }
            }
    // Visible sketches of nested subassemblies in the mate panel (FX9-ASM-014): points, lines, circles and arcs at the
    // carrying entry's placement; the pick's sketch is "<node path>/<sketch ID>"; their planes and axes — the view's.
    view_->setNestedPickable(mode_ == Mode::Mate);
    if (mode_ == Mode::Mate)
        for (const auto& d : model_->session().nestedReferenceGeometry()) {
            if (d.kind != regen::AssemblyDatum::Kind::Sketch) continue;
            const regen::DatumSketch& ds = d.entities;
            auto Q = [](const Point3& w) { return QVector3D(float(w.x()), float(w.y()), float(w.z())); };
            auto pick = [&](const std::string& eid, DrawSketchPick::Kind kind, std::vector<QVector3D> pts) {
                DrawSketchPick p;
                p.sketch = d.node + "/" + d.id;
                p.entity = eid;
                p.kind = kind;
                p.points = std::move(pts);
                picks.push_back(std::move(p));
            };
            for (const auto& [eid, l] : ds.lines) pick(eid, DrawSketchPick::Kind::Line, {Q(l.first), Q(l.second)});
            for (const auto& [eid, c] : ds.circles) {
                std::vector<QVector3D> pts;
                for (int i = 0; i <= 64; ++i) {
                    const double ang = 2 * std::numbers::pi * i / 64;
                    pts.push_back(Q(c.first + c.second * (std::cos(ang) * ds.frame.x + std::sin(ang) * ds.frame.y)));
                }
                pick(eid, DrawSketchPick::Kind::Curve, std::move(pts));
            }
            for (const auto& [pid, q] : ds.points) pick(pid, DrawSketchPick::Kind::Point, {Q(q)});
        }
    view_->setSketchPicks(std::move(picks));
}

// ---------------------------------------------------------------- unified selection field (§5.5а, S7F-UI-001)

std::set<PickTarget> MainWindow::selectionItemTargets(const SelectionItem& item) const {
    std::set<PickTarget> out;
    for (const auto& t : item.targets) out.insert(withBody(t));
    if (!item.ref.empty()) {
        const auto f = model_->session().document().document->active().find(item.ref);
        if (!f) {
            if (auto t = pickTargetOfRef(item.ref)) {
                out.insert(*t);
            } else {
                // An instance (the source of an instance pattern, S7F-RF-009): the faces of its bodies.
                for (const auto& b : view_->bodies())
                    if (b.mesh.bodyId.rfind(item.ref + "/", 0) == 0)
                        for (const auto& fr : b.mesh.faceRanges) out.insert({PickTarget::Kind::Face, b.mesh.bodyId, fr.name});
            }
        } else if (f->sketchData()) {
            // A sketch: its regions and elements (an empty name — all of the sketch, Viewport::targetBounds).
            out.insert({PickTarget::Kind::Region, item.ref, {}});
            out.insert({PickTarget::Kind::SketchEntity, item.ref, {}});
        } else if ((f->type == doc::FeatureType::Axis || f->type == doc::FeatureType::Plane)) {
            out.insert({f->type == doc::FeatureType::Axis ? PickTarget::Kind::Axis : PickTarget::Kind::Plane, {}, item.ref});
        } else {
            const auto g = featureGeometry(item.ref);
            out.insert(g.begin(), g.end());
        }
    }
    return out;
}

void MainWindow::hoverSelectionItem(const SelectionItem* item) {
    std::set<PickTarget> targets;
    std::string sketch;
    if (item) {
        targets = selectionItemTargets(*item);
        // Sketch placeholders select the shown sketch, they are not drawn objects.
        for (auto it = targets.begin(); it != targets.end();)
            if (it->name.empty()) sketch = it->body, it = targets.erase(it);
            else ++it;
        // The regions of a whole sketch are drawn as hovered.
        if (!sketch.empty())
            for (const auto& r : view_->regions())
                if (r.sketch == sketch) targets.insert({PickTarget::Kind::Region, r.sketch, r.regionId});
    }
    view_->setTreeHover(targets, item != nullptr);  // bright orange over the selection (§5.5а, FX10-WZ-002)
    if (sketch != hoverSketch_) {
        hoverSketch_ = sketch;
        updateRegions();
    }
}

void MainWindow::centerOnSelectionItem(const SelectionItem& item) {
    if (!view_->centerOn(selectionItemTargets(item), !testMode_)) setPrompt(txt("ui.main.theObjectIsNotVisible"));
}

}  // namespace construe::app
