// Main window: technical requirements — annotation panel, face picking, view display (spec/app-ux.md §3.4, §4.2в,
// §5.12; pmi.md; S3-PMI-001…003).
#include "app/main_window.h"

#include "app/feature_panel.h"
#include "app/drawing_dims.h"
#include "app/drawing_marks.h"
#include "app/messages.h"

#include <algorithm>

namespace construe::app {

namespace {

// Annotation face reference of a picked face (FX12-PMI-003): in an assembly «<instance path>/<face>» (instance body
// «i2/body:f2»), in a part — the face name.
std::string annotationFaceRef(const PickTarget& t, bool assembly) {
    if (t.kind != PickTarget::Kind::Face) return {};
    if (!assembly) return t.name;
    const auto slash = t.body.rfind('/');
    return slash == std::string::npos ? std::string() : t.body.substr(0, slash) + "/" + t.name;
}

std::vector<std::string> datumLetters(const doc::Element& el) {
    std::vector<std::string> out;
    for (const auto& a : el.annotations)
        if (a.value("type", std::string()) == "datum") out.push_back(a["letter"].get<std::string>());
    return out;
}

}  // namespace

bool MainWindow::annotationsShown() const {
    // Also in an assembly — annotations on instance faces (FX12-PMI-003).
    return (mode_ == Mode::Part || mode_ == Mode::Properties || mode_ == Mode::Section || mode_ == Mode::Annotation) && !drawingActive();
}

std::string MainWindow::selectedAnnotation() const {
    if (mode_ == Mode::Annotation && annotation_->editing()) return *annotation_->editing();
    return tree_->selectedAnnotation().value_or(std::string());
}

void MainWindow::refreshAnnotations() {
    overlay_.clear();
    if (model_->result() && !drawingActive())
        if (auto a = model_->session().annotations(api::StateRef::final())) overlay_.setAnnotations(*a);
    overlay_.setHidden(tree_->hiddenAnnotations());  // tree eye (§4.2а, FX1-UI-009)
    view_->update();
}

void MainWindow::cmdAnnotation(const std::string& type) {
    if (mode_ != Mode::Part) return;
    markView_.clear();  // a part annotation, not a sheet mark
    const auto d = model_->session().document().document;
    if (d->active().isDrawing() || d->active().isSpecification()) {  // assemblies — instance faces (FX12-PMI-003)
        showToast(errorText(Error("API_NOT_PART").with("id", d->active().id), d.get()), true);
        return;
    }
    // Preselection — the face selected in the view (§5.12).
    std::optional<PickTarget> pick;
    for (const auto& t : view_->selection())
        if (t.kind == PickTarget::Kind::Face) pick = t;
    // The wall of a hole feature (FX12-PMI-007, pmi.md §2.2): its tolerance is the tolerance of the hole diameter — one
    // source of truth; the hole panel opens with the cursor in the diameter tolerance field.
    if (type == "fit" && pick && !assemblyActive())
        if (const auto p = pick->name.find(".side(s1/"); p != std::string::npos)
            if (const auto f = d->active().find(pick->name.substr(0, p)); f && f->type == doc::FeatureType::Hole) {
                editFeature(f->id);
                if (mode_ == Mode::FeatureEdit && panel_ == hole_)
                    if (auto* df = DimensionField::of(hole_->sizeBox())) df->toleranceEdit()->setFocus();
                return;
            }
    const auto letters = datumLetters(d->active());
    api::Json def = {{"type", type}};
    if (type == "datum") def["letter"] = pmi::nextDatumLetter(letters);  // pmi.md §2.4
    annotation_->start(type, std::nullopt, def, letters, QString());
    if (type == "fit") annotation_->toleranceEditor()->load(api::Json::object(), true, std::nullopt, QString());
    setMode(Mode::Annotation);
    if (pick) pickedInAnnotation(*pick);
}

void MainWindow::cmdRestRoughness() {
    if (mode_ != Mode::Part) return;
    markView_.clear();  // a part annotation, not a sheet mark
    const auto d = model_->session().document().document;
    const auto& el = d->active();
    if (el.kind != "part" && !el.isBody()) {
        showToast(errorText(Error("API_NOT_PART").with("id", el.id), d.get()), true);
        return;
    }
    annotation_->start("rest", std::nullopt, el.restRoughness ? *el.restRoughness : api::Json(nullptr), {}, QString());
    setMode(Mode::Annotation);
}

void MainWindow::editAnnotation(const std::string& id) {
    if (mode_ != Mode::Part) return;
    markView_.clear();  // a part annotation, not a sheet mark
    const auto d = model_->session().document().document;
    const int i = d->active().annotationIndex(id);
    if (i < 0) return;
    const api::Json def = d->active().annotations[size_t(i)];
    const std::string type = def["type"].get<std::string>();
    QString subject = def.contains("face") ? QString::fromStdString(def["face"].get<std::string>()) : QString();
    bool sizeAllowed = true;
    std::optional<double> nominal;
    for (const auto& a : overlay_.annotations())
        if (a.id == id) nominal = a.nominal;
    QString label = nominal ? "⌀" + QString::fromStdString(pmi::formatNumber(*nominal, pmi::SIZE_DECIMALS)) : QString();
    if (def.contains("dimension")) {
        const std::string sk = def["dimension"]["sketch"], cid = def["dimension"]["constraint"];
        const auto f = d->active().find(sk);
        subject = QString(txt("ui.pmi.dimensionDNN")).arg(QString::fromStdString(cid.substr(1)), f ? QString::fromStdString(f->name) : QString::fromStdString(sk));
        if (f && f->sketchData())
            for (const auto& c : f->sketchData()->constraints)
                if (c.id == cid) {
                    sizeAllowed = pmi::sizeDimension(c.type);
                    const QString prefix = c.type == "diameter" || c.diametral ? "⌀" : c.type == "radius" ? "R" : "";
                    label = nominal ? prefix + QString::fromStdString(pmi::formatNumber(*nominal, pmi::SIZE_DECIMALS)) : QString();
                }
    }
    annotation_->start(type, id, def, datumLetters(d->active()), subject);
    if (type == "fit") annotation_->toleranceEditor()->load(def, sizeAllowed, nominal, label);
    setMode(Mode::Annotation);
}

void MainWindow::acceptAnnotation() {
    auto& s = model_->session();
    QString err;
    const bool sheetMark = !markView_.empty() && drawingActive();
    // A model mark without «Set in drawing»: the values are the model's, not read from the panel.
    const auto def = sheetMark && !annotation_->drawingLocal() ? std::optional<doc::Json>(doc::Json::object()) : annotation_->definition(&err);
    if (!def) {
        annotation_->setStatus(err);
        showToast(err, true);
        return;
    }
    Status r = okStatus();
    if (drawingRest_ && drawingActive()) {  // the rest roughness of the drawing (FX12-DRW-007): only in this drawing
        r = s.setDrawingRestRoughness(currentSheet_, *def);
    } else if (sheetMark) {  // a mark set in the drawing (FX9-DRW-009, FX11-DRW-001): only in this drawing, not in the model
        if (markEdit_.empty()) {
            if (auto a = s.addMark(currentSheet_, api::Json{{"kind", "annotation"}, {"view", markView_}, {"def", *def}}); !a) r = a.error();
            else marks_->select(*a);
        } else if (annotation_->drawingLocal()) {
            r = s.updateMark(markEdit_, api::Json{{"local", true}, {"def", *def}});
        } else if (markLocal_) {
            r = s.updateMark(markEdit_, api::Json{{"local", false}});  // back to the values of the model
        }
    } else if (annotation_->restMode()) {  // part property (FX5-PMI-001): one undo step, none if unchanged
        const auto d = s.document().document;
        if (d->active().restRoughness.value_or(api::Json(nullptr)) != *def) {
            s.beginGroup("undo.group.restRoughness");
            r = s.setElementProperties(d->active().id, api::Json{{"restRoughness", *def}});
            s.endGroup();
        }
    } else if (!annotation_->hasSubject()) {
        showToast(text("ui.pmi.pickFace"), true);
        return;
    } else if (annotation_->editing()) {
        r = s.updateAnnotation(*annotation_->editing(), *def);
    } else if (auto a = s.addAnnotation(*def); !a) {
        r = a.error();
    }
    if (!r) {
        const QString t = errorText(r.error(), s.document().document.get());
        annotation_->setStatus(t);
        showToast(t, true);
        return;
    }
    markView_.clear();
    drawingRest_ = false;
    setMode(Mode::Part);
}

// ---------------------------------------------------------------- marks set in the drawing (FX9-DRW-009, FX11-DRW-001)

std::vector<std::string> MainWindow::drawingDatumLetters(const std::string& view) const {
    // Datum letters of the models of the sheet views (same document) and of the datums set in the drawing on the sheet.
    (void)view;
    std::vector<std::string> out;
    const auto d = model_->session().document().document;
    const int si = d->active().isDrawing() ? d->active().sheetIndex(currentSheet_) : -1;
    if (si < 0) return out;
    const doc::Sheet& sh = d->active().sheets[size_t(si)];
    for (const auto& v : sh.views)
        if (v.refFile.empty() && !v.refElement.empty())
            if (const int ei = d->elementIndex(v.refElement); ei >= 0)
                for (const auto& l : datumLetters(d->elements[size_t(ei)]))
                    if (std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
    for (const auto& m : sh.marks)
        if (m.local && m.def && m.def->value("type", std::string()) == "datum") {
            const std::string l = m.def->value("letter", std::string());
            if (!l.empty() && std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
        }
    return out;
}

void MainWindow::cmdDrawingMark(const std::string& type) {
    if (!drawingActive() || mode_ != Mode::Part || currentSheet_.empty()) return;
    dims_->setActive(false);
    markType_ = type;
    setDrawingTool(DrawingTool::PickMarkFace);
}

void MainWindow::cmdDrawingRestRoughness() {
    if (!drawingActive() || mode_ != Mode::Part || currentSheet_.empty()) return;
    const auto d = model_->session().document().document;
    const int si = d->active().sheetIndex(currentSheet_);
    if (si < 0) return;
    const doc::Sheet& sh = d->active().sheets[size_t(si)];
    // The values: set in the drawing — its own; otherwise the model's of the sign on the sheet, of the first view's model.
    api::Json def(nullptr);
    for (const auto& m : sh.marks)
        if (m.kind == "restRoughness" && m.local && m.def) def = *m.def;
    // The main view of a view's chain: up the parents on the sheet (a chain error — none).
    auto mainView = [&](const doc::DrawingView* v) {
        for (int guard = 0; v && v->kind != "base" && guard < int(sh.views.size()); ++guard) {
            const std::string p = v->parent;
            const auto it = std::find_if(sh.views.begin(), sh.views.end(), [&](const doc::DrawingView& x) { return x.id == p; });
            v = it == sh.views.end() ? nullptr : &*it;
        }
        return v && v->kind == "base" ? v : nullptr;
    };
    if (def.is_null())
        for (const auto& v : sh.views)
            if (v.pmi && def.is_null())
                if (const doc::DrawingView* b = mainView(&v); b && b->refFile.empty())
                    if (const int ei = d->elementIndex(b->refElement); ei >= 0 && d->elements[size_t(ei)].restRoughness)
                        def = *d->elements[size_t(ei)].restRoughness;
    dims_->setActive(false);
    setDrawingTool(DrawingTool::None);
    markView_.clear();
    drawingRest_ = true;
    annotation_->start("rest", std::nullopt, def, {}, QString());
    setMode(Mode::Annotation);
}

void MainWindow::startDrawingMark(const std::string& view, const std::string& face) {
    const auto letters = drawingDatumLetters(view);
    api::Json def = {{"type", markType_}, {"face", face}};
    if (markType_ == "datum") def["letter"] = pmi::nextDatumLetter(letters);
    markView_ = view;
    markEdit_.clear();
    markLocal_ = true;
    annotation_->start(markType_, std::nullopt, def, letters, QString());
    setMode(Mode::Annotation);
}

bool MainWindow::editSheetMark(const Point2& at) {
    if (!drawingActive() || mode_ != Mode::Part) return false;
    const std::string id = marks_->markAt(at);
    if (id.empty()) return false;
    return editSheetMarkId(id);
}

bool MainWindow::editSelectedSheetMark() {
    if (!drawingActive() || mode_ != Mode::Part || marks_->selectedMark().empty()) return false;
    return editSheetMarkId(marks_->selectedMark());
}

bool MainWindow::editSheetMarkId(const std::string& id) {
    const auto d = model_->session().document().document;
    const int si = d->active().sheetIndex(currentSheet_);
    if (si < 0) return false;
    const doc::Sheet& sh = d->active().sheets[size_t(si)];
    const auto it = std::find_if(sh.marks.begin(), sh.marks.end(), [&](const doc::SheetMark& m) { return m.id == id; });
    if (it == sh.marks.end() || it->kind != "annotation") return false;
    // The values: set in the drawing — its own; otherwise the model annotation (same document).
    api::Json def;
    if (it->local && it->def) def = *it->def;
    else  // the model annotation of the mark (IDs are unique in the document; another file — not edited here)
        for (const auto& el : d->elements)
            for (const auto& a : el.annotations)
                if (!it->source.empty() && a.value("id", std::string()) == it->source) def = a;
    const std::string type = def.is_object() ? def.value("type", std::string()) : std::string();
    if (type != "roughness" && type != "geometric" && type != "datum") return false;
    def.erase("id");
    marks_->select(id);
    markView_ = it->view;
    markEdit_ = id;
    markLocal_ = it->local;
    annotation_->start(type, id, def, drawingDatumLetters(it->view), QString());
    // «Set in drawing»: a model mark may be switched; a mark placed in the drawing is always its own.
    annotation_->setDrawingLocal(true, it->local, !it->source.empty());
    setMode(Mode::Annotation);
    return true;
}

void MainWindow::pickedInAnnotation(const PickTarget& t, const std::optional<QVector3D>& at) {
    if (mode_ != Mode::Annotation || t.kind != PickTarget::Kind::Face || annotation_->dimensionSubject() || annotation_->restMode()) return;
    const std::string ref = annotationFaceRef(t, assemblyActive());  // instance face in an assembly (FX12-PMI-003)
    if (ref.empty()) return;
    if (annotation_->type() == "fit") {
        auto ti = model_->session().topologyInfo(ref, api::StateRef::final());
        if (!ti || !ti->surface || ti->surface->kind != kernel::SurfaceInfo::Kind::Cylinder) {
            showToast(text("ui.pmi.faceCylinder"));
            return;
        }
        const double dia = 2 * ti->surface->radius;
        annotation_->toleranceEditor()->setNominal(dia, "⌀" + QString::fromStdString(pmi::formatNumber(dia, pmi::SIZE_DECIMALS)));
        annotation_->toleranceEditor()->suggestField(!ti->surface->convex);
    }
    std::optional<std::array<double, 3>> point;
    if (at) {
        // The exploded view (FX14-PMI-001): the click point goes back to assembled coordinates of the instance.
        Point3 q(at->x(), at->y(), at->z());
        if (const auto a = assemblyActive() ? model_->session().assembly() : nullptr; a && a->exploded && !t.body.empty()) {
            const regen::Motion3 m = a->explodedMoveOf(t.body);
            q = Point3(m.R.transpose() * (Vec3(q) - m.t));
        }
        point = std::array<double, 3>{q.x(), q.y(), q.z()};
    }
    annotation_->setFace(ref, point);
    view_->setSelection({t});
}

// ---------------------------------------------------------------- dragging in the 3D view (FX12-PMI-002)

namespace {

// The annotation definition with `at` and `labelOffset` set (document order: `at` after `face`, `labelOffset` after `at`).
api::Json withPlacement(const api::Json& def, const Point3& at, const Point3& offset) {
    api::Json out = api::Json::object();
    auto arr = [](const Point3& p) { return api::Json::array({p.x(), p.y(), p.z()}); };
    for (auto it = def.begin(); it != def.end(); ++it) {
        if (it.key() == "id" || it.key() == "at" || it.key() == "labelOffset") continue;  // the command takes no id
        out[it.key()] = it.value();
        if (it.key() == "face") {
            out["at"] = arr(at);
            out["labelOffset"] = arr(offset);
        }
    }
    return out;
}

Point3 p3(const QVector3D& v) { return Point3(v.x(), v.y(), v.z()); }

}  // namespace

bool MainWindow::annotationDragEnd(const std::string& id, bool byAnchor, const QPoint& at) {
    overlay_.clearDragPreview();
    if (mode_ != Mode::Part || drawingActive()) return false;
    const api::Session::AnnotationInfo* info = nullptr;
    for (const auto& a : overlay_.annotations())
        if (a.id == id) info = &a;
    if (!info || !info->anchor || !info->def.contains("face")) {
        view_->update();
        return false;
    }
    const api::Session::AnnotationInfo a = *info;  // the overlay is refreshed by the commands below
    const Camera& cam = view_->camera();
    const Point3 anchor = *a.anchor;
    // Current label world point: stored offset, or the drawn label point in the plane through the anchor ∥ screen.
    auto onScreenPlane = [&](const QPointF& s, const Point3& through) {
        QVector3D o, dir;
        cam.ray(s, o, dir);
        const QVector3D n = dir.normalized(), x = QVector3D(float(through.x()), float(through.y()), float(through.z()));
        const float t = QVector3D::dotProduct(x - o, n) / QVector3D::dotProduct(dir, n);
        return p3(o + dir * t);
    };
    Point3 label = anchor;
    if (const auto w = AnnotationOverlay::labelWorld(a)) label = p3(*w);
    else if (const auto ls = overlay_.labelScreen(id)) label = onScreenPlane(*ls, anchor);
    auto& s = model_->session();
    // New anchor request (`at`) and the label point.
    Point3 newAt = anchor, newLabel = label;
    if (byAnchor) {
        const PickTarget t = view_->pickAt(at, true, false, false);
        const auto pt = view_->pointUnder(at);
        if (annotationFaceRef(t, assemblyActive()) != a.face || !pt) {
            view_->update();
            return false;  // the anchor stays on its face
        }
        newAt = p3(*pt);
    } else {
        newLabel = onScreenPlane(QPointF(at), label);
        newAt = newLabel;  // the anchor — the nearest face point (core-api §5 annotations)
    }
    const double flag = (label - anchor).norm();
    // Shown (exploded, FX14-PMI-001) → stored (assembled) coordinates: a point by the inverse displacement, a vector by
    // the inverse turn; identity outside the exploded view.
    const regen::Motion3& mv = a.move;
    auto docPoint = [&](const Point3& p) { return Point3(mv.R.transpose() * (Vec3(p) - mv.t)); };
    auto docVec = [&](const Point3& v) { return Point3(mv.R.transpose() * Vec3(v)); };
    s.beginGroup("undo.group.annotationMove");
    Status r = s.updateAnnotation(id, withPlacement(a.def, docPoint(newAt), docVec(newLabel - anchor)));
    if (r) {
        // Resolved anchor: the label offset — from it (sign drag) or along its normal with the same flag length.
        std::optional<Point3> resolved;
        std::optional<Vec3> normal;
        if (auto all = s.annotations(api::StateRef::final()))
            for (const auto& x : *all)
                if (x.id == id) resolved = x.anchor, normal = x.normal;
        if (resolved) {
            const Point3 off = byAnchor ? (normal ? Point3(normal->normalized() * flag) : Point3(label - anchor)) : Point3(newLabel - *resolved);
            r = s.updateAnnotation(id, withPlacement(a.def, docPoint(newAt), docVec(off)));
        }
    }
    s.endGroup();
    if (!r) showToast(errorText(r.error(), s.document().document.get()), true);
    refreshAnnotations();
    return bool(r);
}

}  // namespace construe::app
