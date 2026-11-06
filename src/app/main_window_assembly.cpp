// Main window: assemblies — insertion, context, dragging, analysis, mates, sections (spec/app-ux.md, spec/assembly.md).
#include "app/main_window.h"

#include "app/captions.h"
#include "app/document_hub.h"

#include "app/part_tool.h"
#include "api/sketch_ops.h"
#include "app/scene_builder.h"

#include "app/constraint_commands.h"
#include "app/hotkeys_table.h"
#include "app/icons.h"
#include "app/keys.h"
#include "app/messages.h"
#include "app/theme.h"
#include "app/mate_logic.h"
#include "app/measure_panel.h"

#include <algorithm>

#include <QApplication>
#include <QSettings>
#include <QSignalBlocker>
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

// ---------------------------------------------------------------- assemblies (§8а)

bool MainWindow::assemblyActive() const { return model_->session().document().document->active().isAssembly(); }

// Assembly view pick object in Part mode (body «i2/body:f2», face «f2.cap(end)») → name in the virtual state of the
// assembly «i2/f2.cap(end)» (assembly.md §7б). In a sketch and the feature panel the view shows the virtual state itself —
// the names are already full.
PickTarget MainWindow::assemblyPick(PickTarget t) const {
    if (!assemblyActive() || t.body.empty()) return t;
    if (t.kind != PickTarget::Kind::Face && t.kind != PickTarget::Kind::Edge && t.kind != PickTarget::Kind::Vertex) return t;
    const auto slash = t.body.rfind('/');
    if (slash == std::string::npos) return t;
    const std::string path = t.body.substr(0, slash);
    if (t.name.rfind(path + "/", 0) != 0) t.name = path + "/" + t.name;
    return t;
}

// The «Assembly» tab and unavailability of part commands while an assembly is active.
void MainWindow::updateElementKindUi() {
    const bool asmActive = assemblyActive();
    const bool drwActive = drawingActive();  // drawing (§8б): no features and no 3D view
    if (asmActive) rebuildInsertMenu();
    syncExplodedButton();  // the «Exploded view» toggle follows the document (FX7-ASM-005)
    refreshDrawing();
    updateAvailability();  // availability of all buttons and menu items — the §7.5 table (FX4-UI-002)
    // Tab by element kind: assembly — "Assembly" (2), drawing — "Drawing" (3); leaving them — to "Part".
    const int special = asmActive && mode_ == Mode::Part ? 2 : drwActive && mode_ == Mode::Part ? 3 : -1;
    const int cur = ribbon_->currentIndex();
    const int want = special >= 0 ? special : (cur == 2 || cur == 3 ? 0 : cur);
    if (cur != want) ribbon_->setCurrentIndex(want);
}

void MainWindow::rebuildInsertMenu() {  // "Insert ▾" (§8а): objects that can be inserted into the active assembly
    // Also rebuilt from a handler of this menu's item: actions are deleted deferred.
    for (QAction* old : insertMenu_->actions()) {
        insertMenu_->removeAction(old);
        old->deleteLater();
    }
    const auto d = model_->session().document().document;
    const std::string active = d->active().id;
    std::function<bool(const std::string&, int)> contains = [&](const std::string& el, int depth) {
        if (el == active || depth > 64) return true;
        const int i = d->elementIndex(el);
        if (i < 0) return false;
        for (const auto& in : d->elements[size_t(i)].instances)
            if (contains(in.refElement, depth + 1)) return true;
        return false;
    };
    int k = 0;
    for (const auto& e : d->elements) {
        ++k;
        // Only parts, imported bodies and assemblies are components (§8а, FX13-ASM-001): no drawings, no specifications.
        if (contains(e.id, 0) || !(e.kind == "part" || e.isBody() || e.isAssembly())) continue;
        QAction* a = insertMenu_->addAction(commandIcon(e.isAssembly() ? "assembly" : "part"), QString::fromStdString(e.name));
        a->setObjectName(QString("insert_%1").arg(k));
        connect(a, &QAction::triggered, this, [this, id = e.id] {
            const bool nonEmpty = !model_->session().document().document->active().instances.empty();
            auto r = model_->session().insertInstance(api::Json{{"element", id}});
            if (!r) showToast(errorText(r.error()), true);
            else if (nonEmpty) startMateSeries(*r);  // FX1-ASM-002
        });
    }
    insertMenu_->addSeparator();
    QAction* ic = insertMenu_->addAction(commandIcon("part"), txt("ui.assembly.newPartInContext"));  // S2-ASM-011
    ic->setObjectName("insertInContext");
    connect(ic, &QAction::triggered, this, [this] {
        auto r = model_->session().insertPartInContext();
        if (!r) {
            showToast(errorText(r.error()), true);
            return;
        }
        editPartInContext((*r)["element"].get<std::string>());
    });
    QAction* ff = insertMenu_->addAction(txt("ui.common.fromFile"));
    ff->setObjectName("insertFromFile");
    connect(ff, &QAction::triggered, this, [this] { insertFromFile(); });
    QAction* fl = insertMenu_->addAction(commandIcon("part"), txt("ui.assembly.fromLibrary"));  // S6-LIB-003, S6-LIB-004 (§8г)
    fl->setObjectName("insertFromLibrary");
    connect(fl, &QAction::triggered, this, [this] { openLibrary(); });
    insertMenu_->setToolTipsVisible(true);  // hint for the unavailable «From file…» (§7.5)
    updateAvailability();
}

// Open the instance's object (§8а): own — make it active; from another file — open the file in a window.
void MainWindow::openInstanceFromAssembly(const std::string& instanceId) {
    const auto a = model_->session().assembly();
    const auto* e = a ? a->find(instanceId) : nullptr;
    if (!e || e->error) return;
    if (doc::isLibraryRef(e->file)) {  // a library instance: the window replacing its element, size or places (FX10-LIB-004)
        if (e->path.find('/') == std::string::npos) openLibraryReplace(instanceId);
        else showToast(text("ui.library.noOpen", {{"name", QString::fromStdString(e->name)}}));
        return;
    }
    if (e->file.empty()) {
        const std::string elementId = e->element;
        openElementInThisWindow(elementId);
        return;
    }
    const QString path = QString::fromStdString(e->file);
    MainWindow* w = windowFor(path);
    if (!w) {
        w = new MainWindow(testMode_);
        w->setAttribute(Qt::WA_DeleteOnClose);
        w->openPath(path);
        w->show();
    }
    w->model().session().setActiveElement(e->element);
    w->raise();
    w->activateWindow();
}

MainWindow* MainWindow::windowFor(const QString& path) {
    const QString want = QFileInfo(path).absoluteFilePath();
    for (QWidget* t : QApplication::topLevelWidgets())
        if (auto* w = qobject_cast<MainWindow*>(t))
            if (auto p = w->model().session().path(); p && QFileInfo(QString::fromStdString(*p)).absoluteFilePath() == want) return w;
    return nullptr;
}

void MainWindow::insertFromFile() {
    if (!model_->session().path()) {
        showToast(errorText(Error("API_NO_PATH")), true);
        return;
    }
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    const QString path = QFileDialog::getOpenFileName(this, txt("ui.assembly.insertFromFile"), {}, txt("ui.common.construeDocumentConstrue"), nullptr, opt);
    if (path.isEmpty()) return;
    auto d = model_->session().fileElements(path.toStdString());
    if (!d) {
        showToast(errorText(d.error(), nullptr, path), true);
        return;
    }
    // Only parts, bodies and assemblies of the file (FX13-ASM-001); one object — at once; several — an object menu at the
    // cursor.
    d->erase(std::remove_if(d->begin(), d->end(), [](const auto& e) { return e.kind != "part" && e.kind != "body" && e.kind != "assembly"; }),
             d->end());
    if (d->empty()) {
        showToast(errorText(Error("API_NOT_COMPONENT")), true);
        return;
    }
    std::string element;
    if (d->size() == 1) {
        element = d->front().id;
    } else {
        QMenu menu(this);
        menu.setObjectName("insertFileMenu");
        for (const auto& e : *d) {
            QAction* a = menu.addAction(commandIcon(e.kind == "assembly" ? "assembly" : "part"), QString::fromStdString(e.name));
            a->setData(QString::fromStdString(e.id));
        }
        QAction* chosen = menu.exec(QCursor::pos());
        if (!chosen) return;
        element = chosen->data().toString().toStdString();
    }
    const bool nonEmpty = !model_->session().document().document->active().instances.empty();
    auto r = model_->session().insertInstance(api::Json{{"file", path.toStdString()}, {"element", element}});
    if (!r) showToast(errorText(r.error()), true);
    else if (nonEmpty) startMateSeries(*r);  // FX1-ASM-002
}

void MainWindow::checkLinks() {
    const auto changed = model_->session().linkChanges();
    QStringList names;
    for (const auto& p : changed) names << QFileInfo(QString::fromStdString(p)).fileName();
    if (names.isEmpty() || names == notifiedLinks_) {
        if (names.isEmpty()) notifiedLinks_.clear();
        return;
    }
    notifiedLinks_ = names;
    showToast(text("ui.links.changed", {{"files", names.join(", ")}}), false, {{text("action.refreshLinks"), [this] {
                  notifiedLinks_.clear();
                  if (auto r = model_->session().refreshLinks(); !r) showToast(errorText(r.error()), true);
                  else model_->noteRecomputeRequested();
              }}});
}

// Editing a part in the context of the active assembly (app-ux §8а, S2-ASM-011): activate the part, notify with return.
void MainWindow::editPartInContext(const std::string& partId) {
    const auto d = model_->session().document().document;
    const std::string from = d->active().id;
    const QString asmName = QString::fromStdString(d->active().name);
    const int i = d->elementIndex(partId);
    if (i < 0) return;
    const QString name = QString::fromStdString(d->elements[size_t(i)].name);
    if (auto r = model_->session().setActiveElement(partId); !r) {
        showToast(errorText(r.error()), true);
        return;
    }
    contextPart_ = partId;
    contextAssembly_ = from;
    showToast(text("ui.context.editing", {{"name", name}, {"assembly", asmName}}), false,
              {{text("action.backToAssembly"), [this, from] { model_->session().setActiveElement(from); }}});
}

void MainWindow::exitPartContext() {
    const std::string to = contextAssembly_;
    contextPart_.clear();
    contextAssembly_.clear();
    if (to.empty()) return;
    if (auto r = model_->session().setActiveElement(to); !r) showToast(errorText(r.error()), true);
}

void MainWindow::editInstanceInContext(const std::string& instanceId) {
    const auto d = model_->session().document().document;
    const std::string asmId = d->active().id;
    std::string part;
    for (const auto& in : d->active().instances)
        if (in.id == instanceId) part = in.refElement;
    if (part.empty()) return;
    if (auto r = model_->session().setPartContext(part, api::Json{{"assembly", asmId}, {"instance", instanceId}}); !r) {
        showToast(errorText(r.error(), d.get()), true);
        return;
    }
    editPartInContext(part);
}

void MainWindow::openElementInThisWindow(const std::string& elementId) {
    const auto d = model_->session().document().document;
    const std::string from = d->active().id;
    const int i = d->elementIndex(elementId);
    if (i < 0) return;
    const QString name = QString::fromStdString(d->elements[size_t(i)].name);
    if (auto r = model_->session().setActiveElement(elementId); !r) {
        showToast(errorText(r.error()), true);
        return;
    }
    showToast(text("ui.assembly.back", {{"name", name}}), false, {{text("action.backToAssembly"), [this, from] { model_->session().setActiveElement(from); }}});
}

bool MainWindow::instanceDragBegin(const std::string& instance, const QPoint& press) {
    const auto a = model_->session().assembly();
    const auto* e = a ? a->find(instance) : nullptr;
    if (!e) return false;
    // A derived connection fastener is fixed except on the exploded view (assembly.md §7д.5, FX10-ASM-004).
    if (e->fixed && !(a->exploded && !e->connection.empty())) {
        showToast(text("ui.instance.fixed"));
        return false;
    }
    dragInstance_ = instance;
    dragStart_ = dragMotion_ = e->world;
    // Center of the instance's bounding box (rotation with Shift) — from the shown meshes of its bodies.
    BBox3 bb;
    bb.setEmpty();
    for (const auto& b : view_->bodies())
        if (b.mesh.bodyId.rfind(instance + "/", 0) == 0)
            for (size_t i = 0; i + 2 < b.mesh.positions.size(); i += 3)
                bb.extend(Point3(b.mesh.positions[i], b.mesh.positions[i + 1], b.mesh.positions[i + 2]));
    dragCenter_ = bb.isEmpty() ? Point3(e->world.t) : Point3(bb.center());
    // The grab point (S7F-ASM-002): the point of the face under the press, else the centre — the drag pulls it like the
    // manipulator does.
    pullStart_ = Vec3(dragCenter_);
    if (const auto q = view_->pointUnder(press)) pullStart_ = Vec3(q->x(), q->y(), q->z());
    pullGrab_ = e->world.R.transpose() * (pullStart_ - e->world.t);
    dragSolved_.clear();
    return true;
}

void MainWindow::instanceDragUpdate(const QPoint& d, bool rotate) {
    if (dragInstance_.empty()) return;
    const Camera& cam = view_->camera();
    auto V = [](const QVector3D& v) { return Vec3(v.x(), v.y(), v.z()); };
    const Vec3 right = V(cam.right()), up = V(cam.up).normalized();
    if (!rotate) {
        // A plain drag solves all mates like the manipulator (S7F-ASM-002, assembly.md §7.6б): the grab point is pulled to
        // the point under the cursor in the screen plane through it, the others move within their degrees of freedom.
        const Vec3 target = pullStart_ + (right * d.x() - up * d.y()) * cam.mmPerPx();
        auto pv = model_->session().previewInstancePull(dragInstance_, pullGrab_, target);
        if (pv) {
            dragSolved_.clear();
            for (const auto& [id, p] : *pv) dragSolved_[id] = api::Session::motionOf(p);
            if (auto it = dragSolved_.find(dragInstance_); it != dragSolved_.end()) dragMotion_ = it->second;
        }
        rebuildScene();
        return;
    }
    regen::Motion3 delta;
    const Eigen::Matrix3d Rd = (Eigen::AngleAxisd(d.x() * 0.01, up) * Eigen::AngleAxisd(d.y() * 0.01, right)).toRotationMatrix();
    delta.R = Rd;
    delta.t = Vec3(dragCenter_) - Rd * Vec3(dragCenter_);
    dragMotion_ = delta * dragStart_;
    // With mates — solve (assembly.md §7.6): positions of all top-level instances.
    dragSolved_.clear();
    const Eigen::Quaterniond q(dragMotion_.R);
    if (auto pv = model_->session().previewInstanceDrag(dragInstance_, {{dragMotion_.t.x(), dragMotion_.t.y(), dragMotion_.t.z()}, {q.w(), q.x(), q.y(), q.z()}}))
        for (const auto& [id, p] : *pv) dragSolved_[id] = api::Session::motionOf(p);
    rebuildScene();
}

void MainWindow::instanceDragEnd() {
    if (dragInstance_.empty()) return;
    const std::string id = dragInstance_;
    std::map<std::string, regen::Motion3> moved = dragSolved_;
    if (!moved.count(id)) moved[id] = dragMotion_;
    dragInstance_.clear();
    dragSolved_.clear();
    commitInstancePlacements(moved);
}

void MainWindow::commitInstancePlacements(const std::map<std::string, regen::Motion3>& moved) {
    std::map<std::string, api::Session::Placement> pos;
    const auto a = model_->session().assembly();
    for (const auto& [mid, m] : moved) {
        const auto* e = a ? a->find(mid) : nullptr;
        if (!e || (e->fixed && !(a->exploded && !e->connection.empty()))) continue;  // §7д.5: connection fasteners
        // Only the changed ones (translation > 1e-9 or rotation).
        if ((e->world.t - m.t).norm() <= 1e-9 && (e->world.R - m.R).norm() <= 1e-12) continue;
        const Eigen::Quaterniond q(m.R);
        pos[mid] = {{m.t.x(), m.t.y(), m.t.z()}, {q.w(), q.x(), q.y(), q.z()}};
    }
    committedMotions_.clear();
    if (!pos.empty()) {
        if (auto r = model_->session().moveInstances(pos); !r) {
            showToast(errorText(r.error()), true);
        } else {
            // Until the new result is applied the view keeps the released placements (S7F-ASM-001): the published
            // assembly state is still the old one.
            committedMotions_ = moved;
            committedRevision_ = model_->session().revision();
        }
    }
    rebuildScene();
}

// ---------------------------------------------------------------- free-degree manipulator (§8а, FX8-ASM-003)

// The instance being mated (FX9-ASM-003, app/mate_logic.h).
std::string MainWindow::matedInstance() const {
    const auto d = model_->session().document().document;  // hold the document: fields of the temporary snapshot
    std::vector<std::string> order;
    for (const auto& in : d->active().instances) order.push_back(in.id);
    const auto a = model_->session().assembly();
    return matedInstanceOf(mate_->refs(), mateSeries_, order, [&](const std::string& top) {
        const auto* e = a ? a->find(top) : nullptr;
        return !e || e->fixed;
    });
}

// The selected top-level instance that can still move: the tree node or view picks of one instance only; in the mate
// panel — the instance being mated (FX9-ASM-003). A subassembly moves as a rigid whole (FX9-ASM-009): a pick of any of
// its parts or of its nested node selects it.
std::optional<std::string> MainWindow::freeManipInstance() const {
    if ((mode_ != Mode::Part && mode_ != Mode::Mate) || !assemblyActive()) return std::nullopt;
    if (!pullInstance_.empty()) return pullInstance_;
    std::string top;
    const auto a = model_->session().assembly();
    if (mode_ == Mode::Mate) {
        top = matedInstance();
    } else {
        // A group (a part and a fastener chosen in the tree, FX14-ASM-001) has the group manipulator, not that of one of them.
        if (!freeManipGroup().empty()) return std::nullopt;
        // A nested part of an underdetermined subassembly (FX9-ASM-010): chosen by a second click while the view picks
        // stay on its subassembly, or its node in the tree.
        if (!manipNested_.empty() && a && nestedManipUnit(*a, manipNested_) == manipNested_ && !view_->selection().empty() &&
            std::all_of(view_->selection().begin(), view_->selection().end(),
                        [&](const PickTarget& s) { return !s.body.empty() && topInstanceOf(s.body) == topInstanceOf(manipNested_); }))
            return manipNested_;
        if (view_->selection().empty() && a)
            for (auto* it : tree_->selectedItems())
                if (it->data(0, FeatureTree::KindRole).toString() == "subinstance")
                    if (const std::string u = nestedManipUnit(*a, it->data(0, FeatureTree::IdRole).toString().toStdString()); !u.empty())
                        return u;
        if (const auto t = tree_->selectedInstance()) top = topInstanceOf(*t);
        for (auto* it : tree_->selectedItems())  // a nested node selects its top-level subassembly (FX9-ASM-009)
            if (top.empty() && it->data(0, FeatureTree::KindRole).toString() == "subinstance") {
                top = topInstanceOf(it->data(0, FeatureTree::IdRole).toString().toStdString());
            }
        for (const auto& sel : view_->selection()) {
            if (sel.body.empty()) return std::nullopt;
            const std::string p = topInstanceOf(sel.body);
            if (!top.empty() && top != p) return std::nullopt;
            top = p;
        }
    }
    if (top.empty()) return std::nullopt;
    return movableInstance(top) ? std::optional<std::string>(top) : std::nullopt;
}

void MainWindow::noteInstanceClick(const std::set<PickTarget>& before, const PickTarget& t, bool add) {
    const std::string was = manipNested_;
    manipNested_.clear();
    if (add || t.body.empty() || mode_ != Mode::Part || !assemblyActive()) return;
    const std::string top = topInstanceOf(t.body);
    for (const auto& b : before)  // the subassembly (or one of its parts) is already selected
        if (b.body.empty() || topInstanceOf(b.body) != top) return;
    if (before.empty() && was.empty()) return;
    const auto a = model_->session().assembly();
    if (a) manipNested_ = nestedManipUnit(*a, t.body);
}

bool MainWindow::movableInstance(const std::string& id) const {
    const auto a = model_->session().assembly();
    return movableEntry(a ? a->find(id) : nullptr);
}

namespace {
// World centre of the shown meshes of an instance (and its descendants).
std::optional<Point3> shownCenter(const Viewport& v, const std::string& instance) {
    BBox3 bb;
    bb.setEmpty();
    for (const auto& b : v.bodies())
        if (b.mesh.bodyId.rfind(instance + "/", 0) == 0)
            for (size_t i = 0; i + 2 < b.mesh.positions.size(); i += 3)
                bb.extend(Point3(b.mesh.positions[i], b.mesh.positions[i + 1], b.mesh.positions[i + 2]));
    if (bb.isEmpty()) return std::nullopt;
    return Point3(bb.center());
}
}  // namespace

// World centre of the common bounding box of the shown meshes of a group of instances (FX13-ASM-002).
static std::optional<Point3> shownGroupCenter(const Viewport& v, const std::vector<std::string>& group) {
    BBox3 bb;
    bb.setEmpty();
    for (const auto& b : v.bodies())
        for (const auto& id : group)
            if (b.mesh.bodyId.rfind(id + "/", 0) == 0)
                for (size_t i = 0; i + 2 < b.mesh.positions.size(); i += 3)
                    bb.extend(Point3(b.mesh.positions[i], b.mesh.positions[i + 1], b.mesh.positions[i + 2]));
    if (bb.isEmpty()) return std::nullopt;
    return Point3(bb.center());
}

std::optional<QPointF> MainWindow::freeManipulatorScreen() const {
    std::optional<Point3> c;
    if (const auto id = freeManipInstance()) c = shownCenter(*view_, *id);
    else if (const auto g = freeManipGroup(); !g.empty()) c = shownGroupCenter(*view_, g);
    if (!c) return std::nullopt;
    return view_->camera().project(QVector3D(float(c->x()), float(c->y()), float(c->z())));
}

// The group manipulator (FX13-ASM-002, §8а): view picks on two or more top-level instances (each pick with a body), or
// two or more instance nodes in the tree with an empty view selection; each of them movable.
std::vector<std::string> MainWindow::freeManipGroup() const {
    if (mode_ != Mode::Part || !assemblyActive()) return {};
    if (!pullGroup0_.empty()) {
        std::vector<std::string> g;
        for (const auto& [id, m] : pullGroup0_) g.push_back(id);
        return g;
    }
    // On the exploded view a derived connection fastener is a unit of its own (FX14-ASM-001, app-ux §8а).
    const auto a = model_->session().assembly();
    if (!a) return {};
    std::vector<std::string> tops;
    if (!view_->selection().empty()) {
        for (const auto& sel : view_->selection()) {
            if (sel.body.empty()) return {};
            tops.push_back(moveUnitOf(*a, sel.body));
        }
    } else {
        for (auto* it : tree_->selectedItems()) {
            const QString kind = it->data(0, FeatureTree::KindRole).toString();
            const std::string id = it->data(0, FeatureTree::IdRole).toString().toStdString();
            if (kind == "instance") tops.push_back(topInstanceOf(id));
            else if (kind == "subinstance" && moveUnitOf(*a, id) == id) tops.push_back(id);  // a connection fastener node
        }
    }
    return manipGroupOf(tops, [&](const std::string& id) { return movableUnit(*a, id); });
}

std::optional<QRectF> MainWindow::selectionBox() const { return partTool_ ? partTool_->selectionBox() : std::nullopt; }

// The selection box over the assembly view (FX13-ASM-002, §8а): the chosen instances give all faces of their bodies.
void MainWindow::boxSelectInstances(const QRectF& box, bool window, bool add, const QPoint& release) {
    const Camera& cam = view_->camera();
    // The units of the box (FX14-ASM-001): top-level instances; on the exploded view also each connection fastener.
    const auto a = model_->session().assembly();
    auto unitOf = [&](const std::string& body) { return a ? moveUnitOf(*a, body) : topInstanceOf(body); };
    std::map<std::string, std::vector<QPointF>> projected;
    for (const auto& b : view_->bodies()) {
        if (b.mesh.bodyId.find('/') == std::string::npos) continue;  // not an instance body
        auto& pts = projected[unitOf(b.mesh.bodyId)];
        for (size_t i = 0; i + 2 < b.mesh.positions.size(); i += 3)
            pts.push_back(cam.project(QVector3D(b.mesh.positions[i], b.mesh.positions[i + 1], b.mesh.positions[i + 2])));
    }
    const auto chosen = instancesInBox(box, window, projected);
    std::set<PickTarget> sel = add ? view_->selection() : std::set<PickTarget>{};
    for (const auto& b : view_->bodies()) {
        if (!b.pickable || b.mesh.bodyId.find('/') == std::string::npos) continue;
        if (std::find(chosen.begin(), chosen.end(), unitOf(b.mesh.bodyId)) == chosen.end()) continue;
        for (const auto& fr : b.mesh.faceRanges) sel.insert({PickTarget::Kind::Face, b.mesh.bodyId, fr.name});
    }
    manipNested_.clear();
    view_->setSelection(sel);
    {
        const QSignalBlocker block(tree_);  // the tree signal must not reset the selection in the view
        tree_->clearSelection();
    }
    tree_->viewport()->update();
    viewSelectionPicked(release);  // the context bar next to the release point
}

bool MainWindow::freeManipBegin(const QPoint& press) {
    const auto id = freeManipInstance();
    if (!id) return groupManipBegin(press);
    const auto c = shownCenter(*view_, *id);
    const auto a = model_->session().assembly();
    const auto* e = a ? a->find(*id) : nullptr;
    if (!c || !e) return false;
    pullInstance_ = *id;
    pullStart_ = Vec3(*c);
    pullWorld0_ = e->world;
    // The grip under the press (S7F-ASM-003): the ring — a turn about its axis (the view direction through the centre);
    // an arrow — a pull along it; the centre — a free pull.
    const QPointF cs = view_->camera().project(QVector3D(float(c->x()), float(c->y()), float(c->z())));
    const QPointF off = QPointF(press) - cs;
    const ManipGrip g = manipGripAt(off, FREE_MANIP_PX);
    pullGrip_ = g.kind == ManipGrip::Kind::Ring ? PullGrip::Ring : g.kind == ManipGrip::Kind::Arrow ? PullGrip::Arrow : PullGrip::Free;
    pullAxis_ = g.axis;
    pullGrab_ = e->world.R.transpose() * (pullStart_ - e->world.t);  // the grab point in the instance frame
    pullPress_ = press;
    pullTurn_.start(cs, QPointF(press));
    pullTurnApplied_ = 0;
    dragSolved_.clear();
    return true;
}

// Group manipulator press (FX13-ASM-002): the placements and the group centre at the press, the grip as for one instance.
bool MainWindow::groupManipBegin(const QPoint& press) {
    const auto g = freeManipGroup();
    if (g.empty()) return false;
    const auto c = shownGroupCenter(*view_, g);
    const auto a = model_->session().assembly();
    if (!c || !a) return false;
    std::map<std::string, regen::Motion3> start;
    for (const auto& id : g) {
        const auto* e = a->find(id);
        if (!e) return false;
        start[id] = e->world;
    }
    pullGroup0_ = std::move(start);
    pullGroupCentre_ = Vec3(*c);
    const QPointF cs = view_->camera().project(QVector3D(float(c->x()), float(c->y()), float(c->z())));
    const ManipGrip grip = manipGripAt(QPointF(press) - cs, FREE_MANIP_PX);
    pullGrip_ = grip.kind == ManipGrip::Kind::Ring ? PullGrip::Ring : grip.kind == ManipGrip::Kind::Arrow ? PullGrip::Arrow : PullGrip::Free;
    pullAxis_ = grip.axis;
    pullPress_ = press;
    pullTurn_.start(cs, QPointF(press));
    pullTurnApplied_ = 0;
    dragSolved_ = pullGroup0_;
    return true;
}

// The group moves as a rigid whole without solving mates (FX13-ASM-002): a translation in the screen plane (along the
// arrow) or a turn about the view axis through the press-time centre by the accumulated ring angle.
void MainWindow::groupManipUpdate(const QPoint& pos) {
    const Camera& cam = view_->camera();
    auto V = [](const QVector3D& v) { return Vec3(v.x(), v.y(), v.z()); };
    const Vec3 right = V(cam.right()), up = V(cam.up).normalized();
    regen::Motion3 delta;
    if (pullGrip_ == PullGrip::Ring) {
        const double acc = pullTurn_.update(QPointF(pos));
        delta.R = Eigen::AngleAxisd(acc, right.cross(up).normalized()).toRotationMatrix();
        delta.t = pullGroupCentre_ - delta.R * pullGroupCentre_;
    } else {
        QPointF d = QPointF(pos - pullPress_);
        if (pullGrip_ == PullGrip::Arrow) d = pullAxis_ * (d.x() * pullAxis_.x() + d.y() * pullAxis_.y());
        delta.t = (right * d.x() - up * d.y()) * cam.mmPerPx();
    }
    dragSolved_ = groupMoved(pullGroup0_, delta);
    rebuildScene();
}

void MainWindow::freeManipUpdate(const QPoint& pos) {
    if (!pullGroup0_.empty()) return groupManipUpdate(pos);
    if (pullInstance_.empty()) return;
    const Camera& cam = view_->camera();
    auto V = [](const QVector3D& v) { return Vec3(v.x(), v.y(), v.z()); };
    const Vec3 right = V(cam.right()), up = V(cam.up).normalized();
    if (pullGrip_ == PullGrip::Ring) {
        // The turn by the screen angle swept about the press-time centre, accumulated over the drag (FX12-ASM-001): each
        // move turns the current (previewed) placement by the angle added since the last one about the axis through its
        // centre, and the mates are solved from there (§7.6) — a long drag follows the cursor without jumps.
        const double acc = pullTurn_.update(QPointF(pos));
        const double dphi = acc - pullTurnApplied_;
        if (std::abs(dphi) < 1e-9) return;
        pullTurnApplied_ = acc;
        const auto cur = dragSolved_.find(pullInstance_);
        const regen::Motion3 base = cur != dragSolved_.end() ? cur->second : pullWorld0_;
        const Vec3 centre = base.apply(pullGrab_);
        const Vec3 axis = right.cross(up).normalized();  // towards the viewer
        regen::Motion3 turn;
        turn.R = Eigen::AngleAxisd(dphi, axis).toRotationMatrix();
        turn.t = centre - turn.R * centre;
        const regen::Motion3 m = turn * base;
        const Eigen::Quaterniond q(m.R);
        std::map<std::string, api::Session::Placement> from;
        for (const auto& [id, w] : dragSolved_) {
            const Eigen::Quaterniond qw(w.R);
            from[id] = {{w.t.x(), w.t.y(), w.t.z()}, {qw.w(), qw.x(), qw.y(), qw.z()}};
        }
        if (auto pv = model_->session().previewInstanceDrag(pullInstance_, {{m.t.x(), m.t.y(), m.t.z()}, {q.w(), q.x(), q.y(), q.z()}},
                                                            from.empty() ? nullptr : &from)) {
            dragSolved_.clear();
            for (const auto& [id, p] : *pv) dragSolved_[id] = api::Session::motionOf(p);
        }
        rebuildScene();
        return;
    }
    QPointF d = QPointF(pos - pullPress_);
    if (pullGrip_ == PullGrip::Arrow) d = pullAxis_ * (d.x() * pullAxis_.x() + d.y() * pullAxis_.y());  // along the arrow
    // The target: the point under the cursor in the plane through the grab point parallel to the screen.
    const Vec3 target = pullStart_ + (right * d.x() - up * d.y()) * cam.mmPerPx();
    if (auto pv = model_->session().previewInstancePull(pullInstance_, pullGrab_, target)) {
        dragSolved_.clear();
        for (const auto& [id, p] : *pv) dragSolved_[id] = api::Session::motionOf(p);
    }
    rebuildScene();
}

void MainWindow::freeManipCancel() {
    if (pullInstance_.empty() && pullGroup0_.empty()) return;
    pullInstance_.clear();
    pullGroup0_.clear();
    dragSolved_.clear();
    rebuildScene();
}

void MainWindow::freeManipEnd() {
    if (pullInstance_.empty() && pullGroup0_.empty()) return;
    const auto moved = dragSolved_;  // a group — all its instances, one moveInstances (one undo step, FX13-ASM-002)
    pullInstance_.clear();
    pullGroup0_.clear();
    dragSolved_.clear();
    matePreview_.clear();  // in the mate panel: the preview is solved again over the moved placements (FX9-ASM-003)
    commitInstancePlacements(moved);
}

// ---------------------------------------------------------------- assembly analysis (§8а, M17)

void MainWindow::cmdInterference() {
    if (mode_ != Mode::Part || !assemblyActive()) return;
    auto r = model_->session().interferences();
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    interferences_ = *r;
    if (interferences_.empty()) {
        showToast(text("ui.interference.none"));
        return;
    }
    const auto a = model_->session().assembly();
    auto nameOf = [&](const std::string& p) {
        const auto* e = a ? a->find(p) : nullptr;
        return e ? QString::fromStdString(e->name) : QString::fromStdString(p);
    };
    {
        const QSignalBlocker b(interferenceList_);
        interferenceList_->clear();
        for (const auto& x : interferences_)
            interferenceList_->addItem(QString::fromUtf8(txt("ui.assembly.nNNMm")).arg(nameOf(x.a), nameOf(x.b), formatNumber(x.volume)));
    }
    setMode(Mode::Interference);
    interferenceList_->setCurrentRow(0);
}

// ---------------------------------------------------------------- mates (§8а, M16)

// Mate reference from a view pick object (assembly.md §7.1).
std::string mateRefOf(const PickTarget& t, const doc::Element& el) {
    // Construction of a part (FX9-ASM-001): source geometry, reference planes, axes, a sketch — with the instance path.
    if ((t.kind == PickTarget::Kind::Plane || t.kind == PickTarget::Kind::Axis || t.kind == PickTarget::Kind::Origin) && !t.body.empty())
        return t.body + "/" + t.name;
    // Origin planes and axes, reference planes and axes of the assembly (feature id, assembly.md §7б).
    // View picking also yields temporary face axes — they are not among the references.
    if (t.kind == PickTarget::Kind::Plane && t.name.rfind("plane:", 0) == 0) return t.name;
    if (t.kind == PickTarget::Kind::Axis && t.name.rfind("axis:", 0) == 0) return t.name;
    if (t.kind == PickTarget::Kind::Plane || t.kind == PickTarget::Kind::Axis)
        if (const auto f = el.find(t.name))
            if (f->type == (t.kind == PickTarget::Kind::Plane ? doc::FeatureType::Plane : doc::FeatureType::Axis)) return t.name;
    if (t.kind == PickTarget::Kind::Origin) return "origin";
    // Assembly sketch: an element "<sketch>:<entity>", the sketch itself — its plane (FX7-ASM-006, selection.md §3.6).
    if (t.kind == PickTarget::Kind::SketchEntity) return t.body + ":" + t.name;
    if (t.kind == PickTarget::Kind::Plane)
        if (const auto f = el.find(t.name); f && f->sketchData()) return t.name;
    if ((t.kind == PickTarget::Kind::Face || t.kind == PickTarget::Kind::Edge || t.kind == PickTarget::Kind::Vertex) && !t.body.empty()) {
        const auto slash = t.body.rfind('/');
        if (slash == std::string::npos) return {};
        return t.body.substr(0, slash) + "/" + t.name;
    }
    return {};
}

void MainWindow::cmdMate() {
    if (mode_ != Mode::Part || !assemblyActive()) return;
    std::vector<std::string> refs;
    std::map<std::string, PickTarget> targets;
    const auto d = model_->session().document().document;
    for (const auto& t : view_->selection())
        if (auto r = mateRefOf(t, d->active()); !r.empty() && refs.size() < 2) {
            refs.push_back(r);
            targets[r] = t;
        }
    mate_->start(std::nullopt, refs);
    setMode(Mode::Mate);
    mateTargets_ = std::move(targets);
    syncMateSelection();
    mateChanged();
}

void MainWindow::editMate(const std::string& id) {
    if (mode_ != Mode::Part) return;
    const auto d = model_->session().document().document;  // hold the document: fields of the temporary snapshot
    for (const auto& m : d->active().mates)
        if (m.id == id) {
            mate_->start(id, {m.refs[0], m.refs[1]}, m.type, m.value.value_or(""));
            setMode(Mode::Mate);
            syncMateSelection();
            mateChanged();
            return;
        }
}

// Mate object caption (§8а): the Assembly style of the object captions (app/captions.h, app-ux §5.5г); the kind of an
// instance object — from the view object of the reference.
QString MainWindow::mateRefLabel(const std::string& ref) const {
    std::optional<PickTarget::Kind> kind;
    if (ref.find('/') != std::string::npos && !api::Session::partConstructionName(ref))
        if (const auto t = pickTargetOfRef(ref)) kind = t->kind;
    return objectCaption(model_->session(), ref, CaptionStyle::Assembly, api::StateRef::final(), kind);
}

// View pick object from a mate reference (highlight): the remembered click or the instance body with this face,
// edge, vertex name (mate editing).
std::optional<PickTarget> MainWindow::pickTargetOfRef(const std::string& ref) const {
    if (auto it = mateTargets_.find(ref); it != mateTargets_.end()) return it->second;
    if (ref.rfind("plane:", 0) == 0) return PickTarget{PickTarget::Kind::Plane, {}, ref};
    if (ref.rfind("axis:", 0) == 0) return PickTarget{PickTarget::Kind::Axis, {}, ref};
    if (ref == "origin") return PickTarget{PickTarget::Kind::Origin, {}, ref};
    if (ref.find('/') == std::string::npos) {
        if (const size_t colon = ref.find(':'); colon != std::string::npos)  // sketch element (FX7-ASM-006)
            return PickTarget{PickTarget::Kind::SketchEntity, ref.substr(0, colon), ref.substr(colon + 1)};
        const auto f = model_->session().document().document->active().find(ref);
        if (f) return PickTarget{f->type == doc::FeatureType::Axis ? PickTarget::Kind::Axis : PickTarget::Kind::Plane, {}, ref};
        return std::nullopt;
    }
    // Plane, axis or sketch element of a nested subassembly (FX9-ASM-014).
    {
        const size_t sl = ref.rfind('/');
        const auto& n = view_->nestedReference();
        for (const auto& pd : n.planes)
            if (pd.id == ref) return PickTarget{PickTarget::Kind::Plane, ref.substr(0, sl), ref.substr(sl + 1)};
        for (const auto& a : n.axes)
            if (a.id == ref) return PickTarget{PickTarget::Kind::Axis, ref.substr(0, sl), ref.substr(sl + 1)};
        if (const size_t colon = ref.rfind(':'); colon != std::string::npos && colon > sl)
            for (const auto& sp : view_->sketchPicks())
                if (sp.sketch == ref.substr(0, colon) && sp.entity == ref.substr(colon + 1))
                    return PickTarget{PickTarget::Kind::SketchEntity, sp.sketch, sp.entity};
    }
    for (const auto& b : view_->bodies()) {
        const auto slash = b.mesh.bodyId.rfind('/');
        if (slash == std::string::npos) continue;
        const std::string path = b.mesh.bodyId.substr(0, slash);
        if (ref.rfind(path + "/", 0) != 0) continue;
        const std::string name = ref.substr(path.size() + 1);
        for (const auto& f : b.mesh.faceRanges)
            if (f.name == name) return PickTarget{PickTarget::Kind::Face, b.mesh.bodyId, name};
        for (const auto& e : b.mesh.edges)
            if (e.name == name) return PickTarget{PickTarget::Kind::Edge, b.mesh.bodyId, name};
        for (const auto& v : b.mesh.vertices)
            if (v.name == name) return PickTarget{PickTarget::Kind::Vertex, b.mesh.bodyId, name};
    }
    return std::nullopt;
}

// Selected mate objects are highlighted in the view as a selection (FX1-ASM-001: shows the click was accepted).
void MainWindow::syncMateSelection() {
    if (mode_ != Mode::Mate) return;
    std::set<PickTarget> sel;
    for (const auto& r : mate_->refs())
        if (auto t = pickTargetOfRef(r)) sel.insert(*t);
    view_->setSelection(sel);
}

void MainWindow::pickedInMate(const PickTarget& t) {
    const auto d = model_->session().document().document;
    const std::string r = mateRefOf(t, d->active());
    if (r.empty()) return;
    mateTargets_[r] = t;
    mate_->toggleRef(r);
    // Two cylindrical faces — concentric, otherwise — coincident (§8а); when editing, the type is not replaced.
    if (mate_->refs().size() == 2 && !mate_->editing()) {
        bool cyl = true;
        for (const auto& ref : mate_->refs()) cyl = cyl && model_->session().assemblyFaceKind(ref) == kernel::SurfaceInfo::Kind::Cylinder;
        // Two pitch cylinders of gear rims — the gear mate (FX10-GEAR-004).
        bool rims = true;
        for (const auto& ref : mate_->refs()) rims = rims && model_->session().mateRefTeeth(ref).has_value();
        mate_->suggestType(rims ? "gear" : cyl ? "concentric" : "coincident");
    }
    syncMateSelection();
    mateChanged();
}

// Mate preview (FX8-ASM-001, §8а): the solve with the panel's mate is shown at once; the status line and «OK / Next».
void MainWindow::mateChanged() {
    if (mode_ != Mode::Mate) return;
    matePreview_.clear();
    QString status;
    bool error = false, next = false;
    mate_->setTurnAvailable(mate_->ready() &&
                            model_->session().mateHasSide(mate_->definition()["type"].get<std::string>(),
                                                          {mate_->refs()[0], mate_->refs()[1]}));  // FX9-ASM-005
    if (mate_->ready()) {
        auto& s = model_->session();
        api::Json def = mate_->definition();
        auto pv = s.previewMate(def, mate_->editing());
        // The type adjusts to the pair (FX9-ASM-002, §8а): an impossible automatic type of a new mate — the nearest
        // possible one by the preference order, with the hint why; a type chosen by the user stays.
        if (pv && pv->error && !mate_->editing() && !mate_->typeChosenByUser()) {
            const std::string from = mate_->type();
            for (const std::string& to : mateTypeFallbacks(from)) {
                api::Json alt = def;
                alt["type"] = to;
                alt.erase("value");
                std::optional<QString> value;  // distance / angle — with the current value of the pair
                if (to == "distance" || to == "angle") {
                    const auto v = s.mateCurrentValue(to, {mate_->refs()[0], mate_->refs()[1]});
                    if (!v) continue;
                    value = formatNumber(*v);
                    alt["value"] = value->toStdString();
                }
                auto apv = s.previewMate(alt, std::nullopt);
                if (!apv || apv->error) continue;
                const auto d = s.document().document;
                mate_->adjustType(to,
                                  text("ui.mate.typeChanged", {{"to", MatePanel::typeLabel(to)}, {"from", MatePanel::typeLabel(from)},
                                                               {"reason", errorText(*pv->error, d.get())}}),
                                  value);
                pv = std::move(apv);
                break;
            }
        }
        if (pv) {
            const auto a = s.assembly();
            for (const auto& [id, p] : pv->placements) matePreview_[id] = api::Session::motionOf(p);
            // Underdefined instances of the pair (top-level instances of the references, not fixed).
            int freeDof = 0;
            for (const auto& ref : mate_->refs()) {
                const std::string top = api::Session::refTopInstance(ref);
                if (top.empty()) continue;
                const auto it = pv->freeDof.find(top);
                const auto* e = a ? a->find(top) : nullptr;
                if (it == pv->freeDof.end() || !e || e->fixed || it->second == 0) continue;
                next = true;
                freeDof = std::max(freeDof, it->second);
            }
            const auto d = s.document().document;
            if (pv->error) status = errorText(*pv->error, d.get()), error = true;
            else if (pv->warning) status = errorText(*pv->warning, d.get());
            else if (freeDof > 0) status = text("ui.mate.freeDof", {{"n", QString::number(freeDof)}});
        }
        // Gear mate: the ratio by the teeth (FX10-GEAR-004, §8а) — before the free degrees of freedom.
        const auto z1 = s.mateRefTeeth(mate_->refs()[0]), z2 = s.mateRefTeeth(mate_->refs()[1]);
        if (mate_->type() == "gear" && z1 && z2 && !error) {
            const QString ratio = text("ui.mate.gearRatio", {{"i", formatNumber(double(*z2) / *z1)}, {"first", QString::number(*z1)}, {"second", QString::number(*z2)}});
            status = status.isEmpty() ? ratio : ratio + " " + status;
        }
    }
    mate_->setStatus(status, error);
    mate_->setNextVisible(next);
    rebuildScene();
}

void MainWindow::acceptMate(bool next) {
    if (!mate_->ready()) {
        showToast(text("ui.mate.needTwo"), true);
        return;
    }
    auto& s = model_->session();
    Status r = okStatus();
    if (mate_->editing()) {
        api::Json def = mate_->definition();  // type, objects, value, flip (FX1-ASM-004)
        if (!def.contains("flip")) def["flip"] = false;
        r = s.updateMate(*mate_->editing(), def);
    } else if (auto a = s.addMate(mate_->definition()); !a) {
        r = a.error();
    }
    if (!r) {
        showToast(errorText(r.error(), s.document().document.get()), true);
        return;
    }
    if (next) {  // «OK / Next» (FX8-ASM-002): the next mate of the same pair
        const auto a = s.assembly();
        QStringList names;
        for (const auto& ref : mate_->refs()) {
            const std::string top = api::Session::refTopInstance(ref);
            const auto* e = a && !top.empty() ? a->find(top) : nullptr;
            names << (e ? QString::fromStdString(e->name) : mateRefLabel(ref));
        }
        mate_->start(std::nullopt, {});
        mateTargets_.clear();
        view_->setSelection({});
        mate_->setSeriesHint(text("ui.mate.next", {{"a", names.value(0)}, {"b", names.value(1)}}));
        mateChanged();
        return;
    }
    if (!mateSeries_.empty() && !mate_->editing()) {  // series (FX1-ASM-002): next mate until the part is fully defined
        mate_->start(std::nullopt, {});
        mateTargets_.clear();
        view_->setSelection({});
        mateChanged();
        return;
    }
    setMode(Mode::Part);
}

// Mate series after inserting an instance into a non-empty assembly (FX1-ASM-002, §8а).
void MainWindow::startMateSeries(const std::string& instance) {
    if (mode_ != Mode::Part || !assemblyActive()) return;
    QString name = QString::fromStdString(instance);
    const auto d = model_->session().document().document;  // hold the document: fields of the temporary snapshot
    for (const auto& in : d->active().instances)
        if (in.id == instance) name = QString::fromStdString(in.name);
    mate_->start(std::nullopt, {});
    setMode(Mode::Mate);
    mateSeries_ = instance;  // after setMode: leaving the mode resets the series
    mate_->setSeriesHint(text("ui.mate.series", {{"name", name}}));
}

// After a rebuild: the series instance is fully defined (fixed or with no free DOF) — exit the mode.
void MainWindow::checkMateSeries() {
    if (mateSeries_.empty() || mode_ != Mode::Mate) return;
    const auto doc = model_->session().document().document;
    const auto& ins = doc->active().instances;
    if (!doc->active().isAssembly() || std::none_of(ins.begin(), ins.end(), [&](const doc::Instance& i) { return i.id == mateSeries_; })) {
        setMode(Mode::Part);  // instance removed (insert undone) or left the assembly
        return;
    }
    const auto a = model_->session().assembly();
    const auto* e = a ? a->find(mateSeries_) : nullptr;
    if (!e || !(e->fixed || e->freeDof == 0)) return;  // not rebuilt yet or not fully defined
    const QString name = QString::fromStdString(e->name);
    setMode(Mode::Part);
    showToast(text("ui.mate.seriesDone", {{"name", name}}));
}

// Leaving mate mode (from setMode): the series and object highlight are cleared.
void MainWindow::leftMate() {
    mateSeries_.clear();
    mateTargets_.clear();
    mate_->setSeriesHint({});
    mate_->setStatus({}, false);
    mate_->setNextVisible(false);
    matePreview_.clear();  // the view returns to the document placements (FX8-ASM-001)
    view_->setSelection({});
}

// ---------------------------------------------------------------- sections (§5.11)

void MainWindow::cmdMeasure() {
    if (mode_ != Mode::Part) return;
    if (!measure_) measure_ = new MeasurePanel(*model_, view_);
    if (measure_->isVisible()) measure_->hide();
    else measure_->open();
}

void MainWindow::cmdSection() {
    if (mode_ != Mode::Part) return;
    api::Json def = {{"plane", {{"datum", "plane:front"}}}, {"offset", "0"}};
    // Preselection: a selected plane or planar face, a selected sketch.
    std::optional<PickTarget> pick;
    for (const auto& t : view_->selection())
        if (t.kind == PickTarget::Kind::Plane || t.kind == PickTarget::Kind::Face) pick = t;
    if (const auto f = tree_->selectedFeature())
        if (const auto ff = model_->session().document().document->active().find(*f)) {
            if (ff->sketchData()) def = {{"sketch", *f}};
            else if (ff->type == doc::FeatureType::Plane) pick = PickTarget{PickTarget::Kind::Plane, {}, *f};
        }
    section_->start(std::nullopt, def);
    setMode(Mode::Section);
    if (pick) pickedInSection(*pick);
    sectionChanged();
}

void MainWindow::editSection(const std::string& id) {
    if (mode_ != Mode::Part) return;
    for (const auto& s : model_->session().sections())
        if (s.id == id) {
            section_->start(id, s.def);
            setMode(Mode::Section);
            sectionChanged();
        }
}

void MainWindow::sectionChanged() {
    if (mode_ != Mode::Section) return;
    auto st = model_->session().sectionStatus(section_->definition(), api::StateRef::final());
    section_->setStatus(st && st->has_value() ? errorText(**st, model_->session().document().document.get()) : QString());
    rebuildScene();
}

void MainWindow::acceptSection() {
    auto& s = model_->session();
    const api::Json def = section_->definition();
    Status r = okStatus();
    if (section_->editing()) {
        r = s.updateSection(*section_->editing(), def);
    } else if (auto a = s.addSection(def); !a) {
        r = a.error();
    }
    if (!r) {
        showToast(errorText(r.error(), s.document().document.get()), true);
        return;
    }
    setMode(Mode::Part);
    rebuildScene();
}

void MainWindow::pickedInSection(const PickTarget& t) {
    if (!section_->planeKind()) return;
    const auto d = model_->session().document().document;
    if (t.kind == PickTarget::Kind::Plane) {
        if (t.name.rfind("plane:", 0) == 0) {
            section_->setPlane(api::Json{{"datum", t.name}});
        } else if (d->active().find(t.name)) {
            section_->setPlane(api::Json{{"feature", t.name}});
        }
    } else if (t.kind == PickTarget::Kind::Face && assemblyActive()) {  // assembly instance face (sections.md §7а)
        const auto slash = t.body.rfind('/');
        if (slash == std::string::npos) return;
        const std::string ref = t.body.substr(0, slash) + "/" + t.name;
        if (model_->session().assemblyFaceKind(ref) == kernel::SurfaceInfo::Kind::Plane) section_->setPlane(api::Json{{"face", ref}});
        else showToast(text("ui.section.facePlanar"));
    } else if (t.kind == PickTarget::Kind::Face) {
        auto ti = model_->session().topologyInfo(t.name, api::StateRef::final());
        if (ti && ti->surface && ti->surface->kind == kernel::SurfaceInfo::Kind::Plane)
            section_->setPlane(api::Json{{"face", t.name}});
        else showToast(text("ui.section.facePlanar"));
    }
}

std::optional<Plane> MainWindow::sectionManipulatorPlane() const {
    if (mode_ != Mode::Section || !section_->planeKind()) return std::nullopt;
    auto p = model_->session().sectionPlane(section_->definition(), api::StateRef::final());
    if (!p) return std::nullopt;
    return *p;
}

}  // namespace construe::app
