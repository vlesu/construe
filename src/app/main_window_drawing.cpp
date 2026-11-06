// Main window: drawing — sheet window, current sheet, sheet properties row, views, title block, sheet export
// (spec/app-ux.md §8б).
#include "app/main_window.h"

#include "app/icons.h"
#include "app/messages.h"
#include "app/panel_header.h"
#include "app/title_block_form.h"
#include "drawing/cut_plan.h"
#include "drawing/standard.h"

#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace construe::app {

namespace {
const char* const ORIENTATIONS[][2] = {{"landscape", "ui.drawing.landscape"}, {"portrait", "ui.drawing.portrait"}};
// Orientations of the main view (drawing.md §6.1) — captions as for standard views (§3.3).
const char* const VIEW_ORIENTATIONS[][2] = {{"front", "ui.common.front"}, {"back", "ui.common.back"}, {"top", "ui.common.top"},    {"bottom", "ui.common.bottom"},
                                            {"left", "ui.common.left"},    {"right", "ui.common.right"}, {"iso", "ui.common.isometric"}};

const doc::DrawingView* findView(const doc::Sheet& sh, const std::string& id) {
    const int vi = sh.viewIndex(id);
    return vi < 0 ? nullptr : &sh.views[size_t(vi)];
}
bool verticalDirection(const std::string& d) { return d == "top" || d == "bottom"; }  // the coordinate shared with the parent is x

// Segment a–b intersects the rectangle (or lies inside it) — Liang–Barsky clipping.
bool segmentHitsBox(const Point2& a, const Point2& b, const Eigen::AlignedBox2d& box) {
    if (box.isEmpty()) return false;
    double t0 = 0, t1 = 1;
    const Point2 d = b - a;
    for (int k = 0; k < 2; ++k) {
        const double lo = box.min()[k], hi = box.max()[k];
        if (std::abs(d[k]) < 1e-12) {
            if (a[k] < lo || a[k] > hi) return false;
            continue;
        }
        double ta = (lo - a[k]) / d[k], tb = (hi - a[k]) / d[k];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}
}  // namespace

bool MainWindow::drawingActive() const { return model_->session().document().document->active().isDrawing(); }

const std::string& MainWindow::selectedView() const {
    static const std::string none;
    return drawingView_ ? drawingView_->selectedView() : none;
}

QWidget* MainWindow::buildDrawingPage() {
    drawingPage_ = new QWidget;
    drawingPage_->setObjectName("drawingPage");
    auto* v = new QVBoxLayout(drawingPage_);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    // Drawing and current sheet properties (§8б, FX1-UI-007) — top block of the title block panel; no rows above sheet.
    drawingProps_ = new QWidget;
    drawingProps_->setObjectName("drawingProps");
    auto* pf = new QFormLayout(drawingProps_);
    pf->setContentsMargins(0, 0, 0, 0);
    drawingStandard_ = new QComboBox;
    drawingStandard_->setObjectName("drawingStandard");
    drawingStandard_->addItem(txt("ui.common.gost"), QString());
    drawingStandard_->addItem("ISO", QString("iso"));
    drawingStandard_->addItem(txt("ui.drawing.cutPlan"), QString("cut"));  // FX11-CUT-001 (drawing.md §2.7)
    pf->addRow(txt("ui.common.standard"), drawingStandard_);
    drawingProjection_ = new QComboBox;
    drawingProjection_->setObjectName("drawingProjection");
    drawingProjection_->addItem(txt("ui.common.firstAngle"), QString());
    drawingProjection_->addItem(txt("ui.common.thirdAngle"), QString("third"));
    pf->addRow(txt("ui.drawing.projectionMethod"), drawingProjection_);
    sheetFormat_ = new QComboBox;
    sheetFormat_->setObjectName("sheetFormat");
    for (const auto& f : drawing::sheetFormats()) sheetFormat_->addItem(QString::fromStdString(f).replace('x', QChar(0x00D7)), QString::fromStdString(f));
    pf->addRow(txt("ui.drawing.sheetSize"), sheetFormat_);
    sheetOrientation_ = new QComboBox;
    sheetOrientation_->setObjectName("sheetOrientation");
    for (const auto& o : ORIENTATIONS) sheetOrientation_->addItem(QString::fromUtf8(txt(o[1])), QString(o[0]));
    pf->addRow(txt("ui.drawing.orientation"), sheetOrientation_);
    sheetScale_ = new QComboBox;
    sheetScale_->setObjectName("sheetScale");
    sheetScale_->setEditable(true);
    sheetScale_->setInsertPolicy(QComboBox::NoInsert);
    for (const auto& s : drawing::standardScales()) sheetScale_->addItem(QString::fromStdString(s));
    pf->addRow(txt("ui.drawing.sheetScale"), sheetScale_);
    // Selected view properties (§8б): main view orientation, view scale — a panel block while a view is selected.
    viewBar_ = new QWidget;
    viewBar_->setObjectName("viewBar");
    auto* vf = new QFormLayout(viewBar_);
    vf->setContentsMargins(0, 6, 0, 0);
    {
        auto* hdr = new QLabel(txt("ui.common.view"));
        hdr->setObjectName("viewPropsHeader");
        QFont f = hdr->font();
        f.setBold(true);
        hdr->setFont(f);
        vf->addRow(hdr);
    }
    viewOrientation_ = new QComboBox;
    viewOrientation_->setObjectName("viewOrientation");
    for (const auto& o : VIEW_ORIENTATIONS) viewOrientation_->addItem(QString::fromUtf8(txt(o[1])), QString(o[0]));
    viewOrientation_->addItem(txt("ui.drawing.byPlane"), QString("plane"));  // FX7-DRW-007: the plane — in «Projection plane»
    vf->addRow(txt("ui.drawing.viewOrientation"), viewOrientation_);
    viewScale_ = new QComboBox;
    viewScale_->setObjectName("viewScale");
    viewScale_->addItem(txt("ui.drawing.fromSheet"), QString());
    for (const auto& s : drawing::standardScales()) viewScale_->addItem(QString::fromStdString(s), QString::fromStdString(s));
    vf->addRow(txt("ui.drawing.viewScale"), viewScale_);
    // FX7-DRW-007, FX7-DRW-008: model of the main view, projection plane, display, sheet metal state.
    viewSource_ = new QComboBox;
    viewSource_->setObjectName("viewSource");
    vf->addRow(txt("ui.drawing.model"), viewSource_);
    viewPlane_ = new QComboBox;
    viewPlane_->setObjectName("viewPlane");
    vf->addRow(txt("ui.drawing.projectionPlane"), viewPlane_);
    viewDisplay_ = new QComboBox;
    viewDisplay_->setObjectName("viewDisplay");
    viewDisplay_->addItem(txt("ui.drawing.standardDrawing"), QString());
    viewDisplay_->addItem(txt("ui.drawing.render"), QString("render"));
    vf->addRow(txt("ui.common.display"), viewDisplay_);
    viewFlat_ = new QComboBox;
    viewFlat_->setObjectName("viewFlat");
    viewFlat_->addItem(txt("ui.drawing.folded"), false);
    viewFlat_->addItem(txt("ui.drawing.flatPattern"), true);
    vf->addRow(txt("ui.common.sheetMetalPart"), viewFlat_);
    viewExploded_ = new QComboBox;  // FX7-ASM-005 (drawing.md §6.24)
    viewExploded_->setObjectName("viewExploded");
    viewExploded_->addItem(txt("ui.drawing.assembled"), false);
    viewExploded_->addItem(txt("ui.drawing.exploded"), true);
    vf->addRow(txt("ui.common.assembly"), viewExploded_);
    // Model sections the view is cut with (S7F-DRW-005, drawing.md §6.26): check boxes filled by refreshViewExtras.
    viewSections_ = new QWidget;
    viewSections_->setObjectName("viewSections");
    auto* secl = new QVBoxLayout(viewSections_);
    secl->setContentsMargins(0, 0, 0, 0);
    secl->setSpacing(2);
    vf->addRow(txt("ui.common.sections"), viewSections_);
    viewBar_->hide();
    drawingView_ = new DrawingView;
    // Dimensions (§8б, S3-DRW-006): sheet window tool and the dimension properties row.
    DimensionTool::Hooks hooks;
    hooks.session = [this]() -> api::Session& { return model_->session(); };
    hooks.currentSheet = [this] { return currentSheet_; };
    hooks.toast = [this](const QString& t, bool error) { showToast(t, error); };
    hooks.status = [this](const QString& t) {
        setPrompt(t);
    };
    hooks.selected = [this] {
        drawingView_->setSelectedView({});
        if (texts_) texts_->select({});
        if (marks_) marks_->select({});
        if (cons_) cons_->select({});
        refreshDrawing();
    };
    hooks.activeChanged = [this](bool on) {
        if (auto* b = buttons_["drawingDimension"]) b->setChecked(on);
        if (on && texts_) texts_->setPlacing(false);
        if (on && cons_) cons_->setPlacing({});
    };
    dims_ = new DimensionTool(drawingView_, std::move(hooks), this);
    // Texts (§8б, FX7-DRW-006): a tool of the same window; dimensions get the press first (in their mode — always).
    SheetTextTool::Hooks th;
    th.session = [this]() -> api::Session& { return model_->session(); };
    th.currentSheet = [this] { return currentSheet_; };
    th.toast = [this](const QString& t, bool error) { showToast(t, error); };
    th.status = [this](const QString& t) {
        setPrompt(t);
    };
    th.selected = [this] {
        drawingView_->setSelectedView({});
        dims_->select({});
        if (marks_) marks_->select({});
        if (cons_) cons_->select({});
        refreshDrawing();
    };
    th.placingChanged = [this](bool on) {
        if (auto* b = buttons_["sheetText"]) b->setChecked(on);
        if (on) {
            dims_->setActive(false);
            if (cons_) cons_->setPlacing({});
            setDrawingTool(DrawingTool::None);
        }
    };
    texts_ = new SheetTextTool(drawingView_, std::move(th), this);
    // Sheet marks (§8б, FX9-DRW-008): selection, dragging of the sign, deletion.
    SheetMarkTool::Hooks mh;
    mh.session = [this]() -> api::Session& { return model_->session(); };
    mh.toast = [this](const QString& t, bool error) { showToast(t, error); };
    mh.selected = [this] {
        drawingView_->setSelectedView({});
        dims_->select({});
        texts_->select({});
        if (cons_) cons_->select({});
        refreshDrawing();
    };
    marks_ = new SheetMarkTool(drawingView_, std::move(mh), this);
    // Constructions (§8б, FX14-DRW-006): placement with snapping to the view model, selection, dragging of free points.
    ConstructionTool::Hooks ch;
    ch.session = [this]() -> api::Session& { return model_->session(); };
    ch.currentSheet = [this] { return currentSheet_; };
    ch.toast = [this](const QString& t, bool error) { showToast(t, error); };
    ch.status = [this](const QString& t) { setPrompt(t); };
    ch.selected = [this] {
        drawingView_->setSelectedView({});
        dims_->select({});
        texts_->select({});
        marks_->select({});
        refreshDrawing();
    };
    ch.placingChanged = [this](bool on) {
        if (auto* b = buttons_["drawingConsGroup"]) b->setChecked(on);
        if (on) {
            dims_->setActive(false);
            texts_->setPlacing(false);
            setDrawingTool(DrawingTool::None);
        }
    };
    ch.busy = [this] { return dims_->active(); };
    cons_ = new ConstructionTool(drawingView_, std::move(ch), this);
    // Constructions first: in their placement every press is theirs (a click on a dimension line places a point).
    sheetTools_ = std::make_unique<DrawingView::ToolGroup>(std::vector<DrawingView::Tool*>{cons_, dims_, texts_, marks_});
    drawingView_->setTool(sheetTools_.get());
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(drawingView_, 1);
    drawingRow_ = row;
    row->addWidget(buildDrawingPickPanel());
    row->addWidget(buildTitleBlockPanel());
    v->addLayout(row, 1);
    connect(drawingStandard_, &QComboBox::activated, this, [this] { applyDrawingField("standard", drawingStandard_->currentData().toString()); });
    connect(drawingProjection_, &QComboBox::activated, this,
            [this] { applyDrawingField("projection", drawingProjection_->currentData().toString()); });
    connect(sheetFormat_, &QComboBox::activated, this, [this] { applySheetField("format", sheetFormat_->currentData().toString()); });
    connect(sheetOrientation_, &QComboBox::activated, this,
            [this] { applySheetField("orientation", sheetOrientation_->currentData().toString()); });
    connect(sheetScale_, &QComboBox::activated, this, [this] { applySheetField("scale", sheetScale_->currentText().trimmed()); });
    connect(sheetScale_->lineEdit(), &QLineEdit::editingFinished, this, [this] {
        // Focus loss without input (incl. on window close, when the session is gone) — without touching the session.
        if (!sheetScale_->lineEdit()->isModified()) return;
        sheetScale_->lineEdit()->setModified(false);
        const QString t = sheetScale_->currentText().trimmed();
        if (drawingActive() && !currentSheet_.empty()) {
            const auto d = model_->session().document().document;
            const int si = d->active().sheetIndex(currentSheet_);
            if (si >= 0 && t != QString::fromStdString(d->active().sheets[size_t(si)].scale)) applySheetField("scale", t);
        }
    });
    connect(viewOrientation_, &QComboBox::activated, this, [this] {
        const std::string o = viewOrientation_->currentData().toString().toStdString();
        // «By plane» without a chosen plane — the plane «Front»; then any in «Projection plane».
        if (o == "plane") applyViewField(selectedView(), api::Json{{"plane", {{"datum", "plane:front"}}}});
        else applyViewField(selectedView(), api::Json{{"orientation", o}});
    });
    connect(viewScale_, &QComboBox::activated, this, [this] {
        const QString s = viewScale_->currentData().toString();
        if (s.isEmpty() && !viewScale_->model()->flags(viewScale_->model()->index(0, 0)).testFlag(Qt::ItemIsEnabled)) {
            refreshDrawing();  // a detail view requires a scale (§8б)
            return;
        }
        applyViewField(selectedView(), s.isEmpty() ? api::Json{{"scale", nullptr}} : api::Json{{"scale", s.toStdString()}});
    });
    connect(viewSource_, &QComboBox::activated, this, [this] {
        applyViewField(selectedView(), api::Json{{"ref", {{"element", viewSource_->currentData().toString().toStdString()}}}});
    });
    connect(viewPlane_, &QComboBox::activated, this, [this] {
        const QString ref = viewPlane_->currentData().toString();
        if (ref.isEmpty()) {
            refreshDrawing();  // «—»: the orientation is chosen in «View orientation»
            return;
        }
        applyViewField(selectedView(), api::Json{{"plane", api::Json::parse(ref.toStdString())}});
    });
    connect(viewDisplay_, &QComboBox::activated, this, [this] {
        const QString d = viewDisplay_->currentData().toString();
        applyViewField(selectedView(), api::Json{{"display", d.isEmpty() ? api::Json(nullptr) : api::Json(d.toStdString())}});
    });
    connect(viewFlat_, &QComboBox::activated, this, [this] {
        applyViewField(selectedView(), viewFlat_->currentData().toBool() ? api::Json{{"flat", true}} : api::Json{{"flat", nullptr}});
    });
    connect(viewExploded_, &QComboBox::activated, this, [this] {
        applyViewField(selectedView(), viewExploded_->currentData().toBool() ? api::Json{{"exploded", true}} : api::Json{{"exploded", nullptr}});
    });
    connect(drawingView_, &DrawingView::sheetClicked, this, [this](const Point2& at) { sheetClicked(at); });
    connect(drawingView_, &DrawingView::viewDragged, this, [this](const QString& id, const Point2& d) { viewDragged(id.toStdString(), d); });
    connect(drawingView_, &DrawingView::contextRequested, this, [this](const Point2& at, const QPoint& global) {
        if (dims_->contextMenu(at, global)) return;  // dimension under the cursor (§8б)
        if (const std::string cid = cons_->placing().empty() ? cons_->constructionAt(at) : std::string(); !cid.empty()) {
            cons_->select(cid);  // a construction under the cursor (FX14-DRW-006): «Delete»
            QMenu menu(drawingView_);
            menu.setObjectName("consContextMenu");
            menu.addAction(txt("ui.common.delete"), this, [this, cid] {
                cons_->select({});
                if (auto s = model_->session().deleteConstruction(cid); !s) showToast(errorText(s.error()), true);
            });
            menu.exec(global);
            return;
        }
        const std::string id = drawingView_->viewAt(at);
        if (id.empty()) return;
        selectView(id);
        showViewMenu(id, global);
    });
    connect(drawingView_, &DrawingView::escapePressed, this, [this] { drawingEscape(); });
    // Double click on a blue (undimensioned) dimension makes it a sheet dimension (FX7-DRW-003, §8б).
    drawingView_->setDoubleClickHandler([this](const Point2& at) {
        if (drawingTool_ != DrawingTool::None) return false;
        // a text — editing in place (FX7-DRW-006); an annotation mark — its panel (FX11-DRW-001)
        return texts_->doubleClick(at) || dims_->adoptUndimensioned(at) || editSheetMark(at);
    });
    connect(drawingView_, &DrawingView::sheetHovered, this, [this](const Point2& at) { updateToolOverlay(at); });
    // The views dragged with a main view (FX12-DRW-003): its projected views.
    drawingView_->setDragGroup([this](const std::string& id) {
        const auto doc = model_->session().document().document;
        const int si = doc->active().isDrawing() ? doc->active().sheetIndex(currentSheet_) : -1;
        const doc::DrawingView* v = si >= 0 ? findView(doc->active().sheets[size_t(si)], id) : nullptr;
        if (!v || v->kind == "projected") return std::vector<std::string>{id};
        auto out = projectedDescendants(doc->active().sheets[size_t(si)], id);
        out.push_back(id);
        return out;
    });
    // A projected view is dragged only along its free coordinate (§8б, drawing.md §6.2).
    drawingView_->setDragConstraint([this](const std::string& id, const Point2& d) {
        const auto doc = model_->session().document().document;
        const int si = doc->active().isDrawing() ? doc->active().sheetIndex(currentSheet_) : -1;
        const doc::DrawingView* v = si >= 0 ? findView(doc->active().sheets[size_t(si)], id) : nullptr;
        if (!v || v->kind != "projected") return d;
        return verticalDirection(v->direction) ? Point2(0, d.y()) : Point2(d.x(), 0);
    });
    return drawingPage_;
}

void MainWindow::applySheetField(const char* field, const QString& value) {
    if (!drawingActive() || currentSheet_.empty()) return;
    auto s = model_->session().updateSheet(currentSheet_, api::Json{{field, value.toStdString()}});
    if (!s) {
        showToast(errorText(s.error()), true);
        refreshDrawing();  // put the effective values back into the fields
    }
}

void MainWindow::applyDrawingField(const char* field, const QString& value) {
    if (!drawingActive()) return;
    auto s = model_->session().updateDrawing(api::Json{{field, value.isEmpty() ? api::Json() : api::Json(value.toStdString())}});
    if (!s) showToast(errorText(s.error()), true);
    refreshDrawing();  // fields — from the document (incl. after an error)
}

void MainWindow::applyViewField(const std::string& viewId, const api::Json& def) {
    if (!drawingActive() || viewId.empty()) return;
    auto s = model_->session().updateView(viewId, def);
    if (!s) {
        showToast(errorText(s.error()), true);
        refreshDrawing();
    }
}

void MainWindow::selectSheet(const std::string& sheetId) {
    if (sheetId != currentSheet_) drawingView_->setSelectedView({});
    currentSheet_ = sheetId;
    refreshDrawing();
    updateAvailability();  // view commands depend on the views of the current sheet (§7.5)
}

void MainWindow::refreshDrawing() {
    if (!center_) return;
    const BusyScope busy;  // §9.4
    const auto info = model_->session().document();
    const auto& el = info.document->active();
    if (el.isSpecification()) {  // bill of materials (§8в)
        if (drawingTool_ != DrawingTool::None) setDrawingTool(DrawingTool::None);
        refreshSpecification();
        return;
    }
    if (!el.isDrawing()) {
        center_->setCurrentWidget(view_);
        drawingView_->setSelectedView({});
        if (drawingTool_ != DrawingTool::None) setDrawingTool(DrawingTool::None);
        dims_->setActive(false);
        dims_->select({});
        texts_->escape();
        texts_->select({});
        marks_->select({});
        if (drawingPickPanel_) drawingPickPanel_->hide();
        return;
    }
    if (el.sheetIndex(currentSheet_) < 0) currentSheet_ = el.sheets.front().id;
    rebuildBaseViewMenu();  // document objects for «View ▾» — in advance, not when the menu is shown
    const auto& sh = el.sheets[size_t(el.sheetIndex(currentSheet_))];
    {
        const QSignalBlocker b1(sheetFormat_), b2(sheetOrientation_), b3(sheetScale_);
        sheetFormat_->setCurrentIndex(sheetFormat_->findData(QString::fromStdString(sh.format)));
        // GOST: A4 is portrait only (drawing.md §2.1, FX6-DRW-001) — the list shows the effective orientation and is locked.
        const auto size = drawing::sheetSize(el, sh);
        const bool locked = sh.format == "A4" && drawing::drawingStandard(el) == font::DrawingStandard::Gost;
        const QString effective = locked && size ? QString("portrait") : QString::fromStdString(sh.orientation);
        sheetOrientation_->setCurrentIndex(sheetOrientation_->findData(effective));
        sheetOrientation_->setEnabled(!locked);
        sheetOrientation_->setToolTip(locked ? QString::fromUtf8(txt("ui.drawing.gostR21042023")) : QString());
        // The current index follows the sheet too: a stale index equal to the typed value would make Enter a no-op
        // (QComboBox emits no change for the same item) and the scale would not be applied.
        const QString scale = QString::fromStdString(sh.scale);
        sheetScale_->setCurrentIndex(sheetScale_->findText(scale));
        sheetScale_->setEditText(scale);
    }
    {
        const QSignalBlocker b1(drawingStandard_), b2(drawingProjection_);
        const bool iso = drawing::drawingStandard(el) == font::DrawingStandard::Iso;
        const bool cut = drawing::isCutPlan(el);
        drawingStandard_->setCurrentIndex(cut ? 2 : iso ? 1 : 0);
        drawingProjection_->setCurrentIndex(drawing::projectionMethod(el) == drawing::ProjectionMethod::Third ? 1 : 0);
        drawingProjection_->setEnabled(iso && !cut);  // GOST — first angle only (drawing.md §2.5)
        // Cut plan (drawing.md §2.7): the sheet grows by itself, the scale is always 1:1 — the fields do not apply.
        if (cut)
            for (QComboBox* c : {sheetFormat_, sheetOrientation_, sheetScale_}) c->setEnabled(false);
        else
            for (QComboBox* c : {sheetFormat_, sheetScale_}) c->setEnabled(true);
    }
    // The selected view and its properties row.
    const doc::DrawingView* sel = findView(sh, drawingView_->selectedView());
    if (!sel) drawingView_->setSelectedView({});
    if (sel) refreshViewExtras(*info.document, *sel);
    if (sel) {
        const QSignalBlocker b1(viewOrientation_), b2(viewScale_);
        const bool base = sel->kind == "base";
        const bool cutPlan = drawing::isCutPlan(el);  // a part of a cut plan: always the flat pattern at 1:1 (§2.7)
        viewOrientation_->setEnabled(base && !cutPlan);
        viewScale_->setEnabled(!cutPlan);
        if (cutPlan)
            for (QComboBox* c : {viewDisplay_, viewFlat_, viewPlane_}) c->setEnabled(false);
        if (base) viewOrientation_->setCurrentIndex(viewOrientation_->findData(QString::fromStdString(sel->orientation)));
        viewScale_->setItemText(0, base ? txt("ui.drawing.fromSheet") : txt("ui.drawing.fromParent"));
        if (auto* m = qobject_cast<QStandardItemModel*>(viewScale_->model()))
            if (auto* it = m->item(0)) it->setEnabled(sel->kind != "detail");  // a detail view's scale is mandatory
        viewScale_->setCurrentIndex(sel->scale ? std::max(0, viewScale_->findData(QString::fromStdString(*sel->scale))) : 0);
    }
    // Composing the sheet may be one long kernel call (an assembly view): in a worker thread, the stripe runs (§9.4, FX13-UI-001).
    if (auto g = busyRun([&] { return model_->session().sheetGraphics(currentSheet_); })) {
        // Dimensions whose model elements are gone are deleted with a message (§5.5в, S7F-FEAT-003): after this refresh,
        // not inside it (the command changes the document); the ones brought back by undo are not deleted again.
        if (std::any_of(g->errors.begin(), g->errors.end(), [this](const drawing::ViewError& e) {
                return e.error.code == "DRAWING_DIM_REF_LOST" && !lostDimsDeleted_.count(currentSheet_ + "/" + e.view);
            }))
            QTimer::singleShot(0, this, [this, sheet = currentSheet_] { deleteLostDimensions(sheet); });
        drawingView_->setGraphics(std::move(*g));
    }
    dims_->refresh();
    texts_->refresh();
    marks_->refresh();
    cons_->refresh();
    center_->setCurrentWidget(drawingPage_);
    updateDrawingPanels();  // one panel at a time (FX12-DRW-001); fills the title block
}

MainWindow::DrawingPanelContent MainWindow::drawingPanelContent() const {
    // The selections are exclusive (each tool drops the others); the annotation panel of a sheet mark wins (FX12-DRW-001).
    if (mode_ == Mode::Annotation && sheetAnnotationSlot_ && annotation_ && annotation_->parentWidget() == sheetAnnotationSlot_->parentWidget())
        return DrawingPanelContent::MarkEdit;
    if (marks_ && !marks_->selectedMark().empty()) return DrawingPanelContent::Mark;
    if (texts_ && !texts_->selectedText().empty()) return DrawingPanelContent::Text;
    if (cons_ && !cons_->selectedConstruction().empty()) return DrawingPanelContent::Construction;
    if (dims_ && !dims_->selectedDimension().empty()) return DrawingPanelContent::Dimension;
    if (drawingView_ && !drawingView_->selectedView().empty()) return DrawingPanelContent::View;
    return DrawingPanelContent::Drawing;
}

void MainWindow::updateDrawingPanels() {
    if (!titleBlockPanel_) return;
    const DrawingPanelContent c = drawingPanelContent();
    const bool drawing = c == DrawingPanelContent::Drawing;
    // The panel: while the «Edit title block data» button is pressed (the released button gives the sheet the room); the
    // annotation panel of a sheet mark — always. Other modes (properties, settings, a command panel under the tree) — no
    // drawing panel.
    const bool button = buttons_.count("titleBlock") && buttons_["titleBlock"] && buttons_["titleBlock"]->isChecked();
    const bool on = drawingActive() && (c == DrawingPanelContent::MarkEdit || (mode_ == Mode::Part && button));
    titleBlockPanel_->setVisible(on);
    if (drawingPanelHead_) drawingPanelHead_->setVisible(c != DrawingPanelContent::MarkEdit);  // the annotation panel has its own
    drawingProps_->setVisible(drawing);
    if (titleBlockHeader_) titleBlockHeader_->setVisible(drawing);
    if (titleBlockForm_) titleBlockForm_->setVisible(drawing);
    viewBar_->setVisible(c == DrawingPanelContent::View);
    // The dimension and text blocks show themselves for their selection; here — only for theirs.
    dims_->bar()->setVisible(c == DrawingPanelContent::Dimension);
    texts_->bar()->setVisible(c == DrawingPanelContent::Text);
    cons_->bar()->setVisible(c == DrawingPanelContent::Construction);
    if (markBar_) markBar_->setVisible(c == DrawingPanelContent::Mark);
    if (c == DrawingPanelContent::Mark) refreshMarkBar();
    if (drawingPanelTitle_) {
        const char* key = c == DrawingPanelContent::View        ? "ui.drawing.viewProperties"
                          : c == DrawingPanelContent::Dimension ? "ui.drawing.dimensionProperties"
                          : c == DrawingPanelContent::Text      ? "ui.drawing.textProperties"
                          : c == DrawingPanelContent::Mark      ? "ui.drawing.markProperties"
                          : c == DrawingPanelContent::Construction ? "ui.drawing.constructionProperties"
                                                                : "ui.drawing.drawingAndSheet";
        drawingPanelTitle_->setText(txt(key));
    }
    refreshTitleBlockPanel();
}

void MainWindow::hostSheetAnnotation(bool on) {
    if (!sheetAnnotationSlot_ || !annotation_) return;
    QWidget* host = sheetAnnotationSlot_->parentWidget();
    const bool hosted = annotation_->parentWidget() == host;
    if (on && !hosted) {
        props_->removeWidget(annotation_);
        sheetAnnotationSlot_->addWidget(annotation_, 1);
        annotation_->show();
    } else if (!on && hosted) {
        sheetAnnotationSlot_->removeWidget(annotation_);
        props_->addWidget(annotation_);
    }
}

void MainWindow::refreshMarkBar() {
    if (!markBar_ || !drawingActive() || currentSheet_.empty()) return;
    const auto d = model_->session().document().document;
    const int si = d->active().sheetIndex(currentSheet_);
    if (si < 0) return;
    const doc::Sheet& sh = d->active().sheets[size_t(si)];
    const std::string id = marks_->selectedMark();
    const auto it = std::find_if(sh.marks.begin(), sh.marks.end(), [&](const doc::SheetMark& m) { return m.id == id; });
    if (it == sh.marks.end()) return;
    // The kind of the mark: an annotation — its type (set in the drawing — its def, otherwise the model's).
    std::string type;
    if (it->kind == "annotation") {
        if (it->local && it->def) type = it->def->value("type", std::string());
        else
            for (const auto& el : d->elements)
                for (const auto& a : el.annotations)
                    if (!it->source.empty() && a.value("id", std::string()) == it->source) type = a.value("type", std::string());
    }
    const char* key = it->kind == "weld"                  ? "ui.common.weld"
                      : it->kind == "balloon"             ? "ui.ribbon.itemNumber"
                      : it->kind == "partsList"           ? "ui.ribbon.partsList"
                      : it->kind == "restRoughness"       ? "ui.pmi.roughnessOfOtherSurfaces"
                      : type == "roughness"               ? "ui.common.roughness"
                      : type == "geometric"               ? "ui.pmi.geometricTolerance"
                      : type == "datum"                   ? "ui.common.datum"
                                                          : "ui.drawing.markProperties";
    QString t = txt(key);
    if (it->kind == "annotation") t += "\n" + QString(txt(it->local ? "ui.drawing.markSetInDrawing" : "ui.drawing.markFromModel"));
    markInfo_->setText(t);
    markEditBtn_->setEnabled(it->kind == "annotation" && (type == "roughness" || type == "geometric" || type == "datum"));
}

void MainWindow::deleteLostDimensions(const std::string& sheet) {
    if (!model_->session().document().document->active().isDrawing()) return;
    auto r = model_->session().deleteLostDimensions(sheet);
    if (!r || r->empty()) return;
    for (const auto& id : *r) lostDimsDeleted_.insert(sheet + "/" + id);
    showToast(text("ui.dim.lostDeleted", {{"count", QString::number(r->size())}}), false, {{text("action.undo"), [this] { cmdUndo(); }}});
}

// ---------------------------------------------------------------- title block (§8б, S3-DRW-002)

QWidget* MainWindow::buildTitleBlockPanel() {
    auto* panel = new QFrame;
    panel->setObjectName("titleBlockPanel");
    panel->setFrameShape(QFrame::StyledPanel);
    panel->setFixedWidth(340);
    titleBlockPanel_ = panel;  // visible by default (§8б, FX1-UI-007); the "Title block" button hides and shows it
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(6, 6, 6, 6);
    // Top blocks: drawing and sheet, selected view, selected dimension.
    // Header with the close cross (app-ux §2.1в, FX8-UI-008): closing is the same as releasing the «Title block» button.
    auto* head = new PanelHeader(txt("ui.drawing.drawingAndSheet"), "titleBlockClose");
    head->title()->setObjectName("drawingPropsHeader");
    drawingPanelTitle_ = head->title();
    drawingPanelHead_ = head;
    connect(head, &PanelHeader::closeRequested, this, [this] { showDrawingPanel(false); });
    v->addWidget(head);
    v->addWidget(drawingProps_);
    v->addWidget(viewBar_);      // the «View» header is inside the block
    v->addWidget(dims_->bar());  // the «Dimension» header is inside the block
    v->addWidget(texts_->bar());  // «Text» (FX7-DRW-006)
    v->addWidget(cons_->bar());   // «Construction» (FX14-DRW-006)
    // The selected sheet mark (FX12-DRW-001): its kind, «Edit» — the annotation panel here, «Delete» — deleteMark.
    markBar_ = new QWidget;
    markBar_->setObjectName("markBar");
    {
        auto* ml = new QVBoxLayout(markBar_);
        ml->setContentsMargins(0, 6, 0, 0);
        markInfo_ = new QLabel;
        markInfo_->setObjectName("markInfo");
        markInfo_->setWordWrap(true);
        ml->addWidget(markInfo_);
        auto* br = new QHBoxLayout;
        markEditBtn_ = new QPushButton(txt("ui.drawing.markEdit"));
        markEditBtn_->setObjectName("markEdit");
        markDeleteBtn_ = new QPushButton(txt("ui.common.delete"));
        markDeleteBtn_->setObjectName("markDelete");
        br->addWidget(markEditBtn_);
        br->addWidget(markDeleteBtn_);
        br->addStretch(1);
        ml->addLayout(br);
        connect(markEditBtn_, &QPushButton::clicked, this, [this] { editSelectedSheetMark(); });
        connect(markDeleteBtn_, &QPushButton::clicked, this, [this] {
            const std::string id = marks_->selectedMark();
            if (id.empty()) return;
            marks_->select({});
            if (auto st = model_->session().deleteMark(id); !st) showToast(errorText(st.error()), true);
            refreshDrawing();
        });
    }
    markBar_->hide();
    v->addWidget(markBar_);
    // The annotation panel of a sheet mark being placed or edited is hosted here (FX12-DRW-001).
    sheetAnnotationSlot_ = new QVBoxLayout;
    sheetAnnotationSlot_->setContentsMargins(0, 0, 0, 0);
    v->addLayout(sheetAnnotationSlot_);
    titleBlockHeader_ = new QLabel;
    titleBlockHeader_->setObjectName("titleBlockHeader");
    QFont hf = titleBlockHeader_->font();
    hf.setBold(true);
    titleBlockHeader_->setFont(hf);
    v->addWidget(titleBlockHeader_);
    titleBlockForm_ = new TitleBlockForm(TitleBlockForm::Kind::Drawing, "titleBlock_", "titleBlockLabel_");
    connect(titleBlockForm_, &TitleBlockForm::fieldEdited, this, [this](const std::string& key, const QString& t) { applyTitleBlockField(key, t); });
    v->addWidget(titleBlockForm_, 1);
    v->addStretch(0);  // the view settings alone stay at the top (S7F-DRW-004)
    return panel;
}

void MainWindow::showDrawingPanel(bool on, bool drawingSettings) {
    if (!titleBlockPanel_) return;
    if (auto* b = buttons_["titleBlock"]) b->setChecked(on);
    if (on && drawingSettings && drawingView_) {  // the drawing settings: every selection is dropped (FX12-DRW-001)
        drawingView_->setSelectedView({});
        dims_->select({});
        texts_->select({});
        marks_->select({});
        cons_->select({});
        refreshDrawing();
        return;
    }
    updateDrawingPanels();
}

void MainWindow::refreshTitleBlockPanel() {
    if (!titleBlockPanel_ || !titleBlockPanel_->isVisible() || !drawingActive() || currentSheet_.empty()) return;
    const auto d = model_->session().document().document;
    const int si = d->active().sheetIndex(currentSheet_);
    if (si < 0) return;
    const auto& sh = d->active().sheets[size_t(si)];
    titleBlockHeader_->setText(QString(txt("ui.drawing.titleBlockN")).arg(QString::fromStdString(sh.name)));
    titleBlockForm_->setIso(drawing::drawingStandard(d->active()) == font::DrawingStandard::Iso);  // rows by standard (§8б)
    const auto defaults = model_->session().titleBlockDefaults(currentSheet_);
    titleBlockForm_->setValues(sh.titleBlock, defaults ? *defaults : std::map<std::string, std::string>{});
}

void MainWindow::applyTitleBlockField(const std::string& key, const QString& text) {
    if (!drawingActive() || currentSheet_.empty()) return;
    const auto d = model_->session().document().document;
    const int si = d->active().sheetIndex(currentSheet_);
    if (si < 0) return;
    const auto tb = TitleBlockForm::patch(d->active().sheets[size_t(si)].titleBlock, key, text);
    if (!tb) return;  // no changes — no undo step
    if (auto s = model_->session().updateSheet(currentSheet_, api::Json{{"titleBlock", *tb}}); !s) {
        showToast(errorText(s.error()), true);
        refreshTitleBlockPanel();
    }
}

// ---------------------------------------------------------------- sheet export (§8б, S3-IO-001, S3-IO-002, S3-IO-003)

void MainWindow::cmdExportSheet(io::SheetFileFormat format) {
    if (mode_ != Mode::Part || exportPending_ || !(drawingActive() || specificationActive()) || currentSheet_.empty()) return;
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    const std::string drawing = model_->session().document().document->active().id;
    const QString kind = format == io::SheetFileFormat::Pdf ? "PDF" : format == io::SheetFileFormat::Svg ? "SVG" : "DXF";
    const QString ext = "." + kind.toLower();
    const QString suggested = QString::fromStdString(model_->session().exportFileName(drawing, currentSheet_)) + ext;
    QString path = QFileDialog::getSaveFileName(this, txt("ui.drawing.export") + kind, suggested, kind + " (*" + ext + ")", nullptr, opt);
    if (path.isEmpty()) return;
    if (!path.endsWith(ext, Qt::CaseInsensitive)) path += ext;
    auto r = model_->session().exportSheet(currentSheet_, path.toStdString(), format);
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    setProperty("exportPath", path);
    exportPending_ = true;
    model_->noteExportStarted();
    statusRegen_->setText(text("ui.export.running"));
    updateExportActions();
}

void MainWindow::updateExportActions() {
    // Export items (meshes and STEP; PDF, SVG, DXF; all unavailable while an export runs) — the §7.5 table.
    updateAvailability();
}

// ---------------------------------------------------------------- views (§8б)

void MainWindow::rebuildBaseViewMenu() {
    // «View ▾» and «3 projections ▾» (FX1-DRW-001): document models in object panel order.
    for (QMenu* menu : {baseViewMenu_, threeViewsMenu_}) {
        if (!menu) continue;
        for (QAction* old : menu->actions()) {
            menu->removeAction(old);
            old->deleteLater();
        }
    }
    const auto d = model_->session().document().document;
    int k = 0;
    for (const auto& e : d->elements) {
        ++k;
        if (e.isDrawing() || e.isSpecification()) continue;
        const QIcon icon = commandIcon(e.isAssembly() ? "assembly" : e.isBody() ? "body" : "part");
        QAction* a = baseViewMenu_->addAction(icon, QString::fromStdString(e.name));
        a->setObjectName(QString("baseView_%1").arg(k));
        connect(a, &QAction::triggered, this, [this, id = e.id] {
            // Cut plan (drawing.md §2.7, FX11-CUT-004): the part is laid out in a row at once, then moved like a view.
            if (drawingActive() && drawing::isCutPlan(model_->session().document().document->active()) && !currentSheet_.empty()) {
                auto r = model_->session().addView(currentSheet_, api::Json{{"kind", "base"}, {"ref", {{"element", id}}}, {"orientation", "top"}});
                if (!r) showToast(errorText(r.error()), true);
                else selectView(*r);
                return;
            }
            placeModel_ = id;
            placeFile_.clear();
            setDrawingTool(DrawingTool::PlaceBase);
        });
        if (!threeViewsMenu_) continue;
        QAction* t = threeViewsMenu_->addAction(icon, QString::fromStdString(e.name));
        t->setObjectName(QString("threeViews_%1").arg(k));
        connect(t, &QAction::triggered, this, [this, id = e.id] {
            placeModel_ = id;
            placeFile_.clear();
            setDrawingTool(DrawingTool::PlaceThree);
        });
    }
    // Model from another file (FX3-DRW-005): the last item of both menus.
    QAction* bf = baseViewMenu_->addAction(txt("ui.common.fromFile"));
    bf->setObjectName("baseViewFromFile");
    connect(bf, &QAction::triggered, this, [this] { placeViewFromFile(DrawingTool::PlaceBase); });
    if (threeViewsMenu_) {
        QAction* tf = threeViewsMenu_->addAction(txt("ui.common.fromFile"));
        tf->setObjectName("threeViewsFromFile");
        connect(tf, &QAction::triggered, this, [this] { placeViewFromFile(DrawingTool::PlaceThree); });
        threeViewsMenu_->setToolTipsVisible(true);
    }
    baseViewMenu_->setToolTipsVisible(true);  // hint for the unavailable «From file…» (§7.5)
    updateAvailability();
}

void MainWindow::placeViewFromFile(DrawingTool tool) {
    const auto self = model_->session().path();
    if (!self) {
        showToast(errorText(Error("API_NO_PATH")), true);
        return;
    }
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    const QString path = QFileDialog::getOpenFileName(this, txt("ui.drawing.modelViewFromFile"), {}, txt("ui.common.construeDocumentConstrue"), nullptr, opt);
    if (path.isEmpty()) return;
    auto els = model_->session().fileElements(path.toStdString());
    if (!els) {
        showToast(errorText(els.error(), nullptr, path), true);
        return;
    }
    std::string element;
    if (els->size() == 1) {
        element = els->front().id;
    } else {  // several objects — a menu at the cursor (like «Insert ▾ → From file…»)
        QMenu menu(this);
        menu.setObjectName("viewFileMenu");
        for (const auto& e : *els) {
            QAction* a = menu.addAction(commandIcon(e.kind == "assembly" ? "assembly" : "part"), QString::fromStdString(e.name));
            a->setData(QString::fromStdString(e.id));
        }
        QAction* chosen = menu.exec(QCursor::pos());
        if (!chosen) return;
        element = chosen->data().toString().toStdString();
    }
    const QString dir = QFileInfo(QString::fromStdString(*self)).absolutePath();
    placeModel_ = element;
    placeFile_ = QDir(dir).relativeFilePath(path).toStdString();
    setDrawingTool(tool);
}

void MainWindow::cmdProjectedView() {
    if (!drawingActive()) return;
    if (!selectedView().empty()) {
        placeParent_ = selectedView();
        setDrawingTool(DrawingTool::PlaceProjected);
    } else {
        setDrawingTool(DrawingTool::PickParent);
    }
}

void MainWindow::setDrawingTool(DrawingTool t) {
    if (t == DrawingTool::SectionStart || t == DrawingTool::None) {  // a new line input: fresh parent entities (FX9-DRW-002)
        sectionEntsView_.clear();
        sectionHover_.reset();
    }
    drawingTool_ = t;
    if (t != DrawingTool::None && dims_) dims_->setActive(false);  // view placement excludes dimension mode
    if (t != DrawingTool::None && texts_) texts_->setPlacing(false);
    if (t != DrawingTool::None && cons_) cons_->setPlacing({});
    if (!statusHint_ || !drawingView_) return;
    switch (t) {
        case DrawingTool::PlaceBase:
        case DrawingTool::PlaceThree:
        case DrawingTool::PlaceProjected: setPrompt(text("ui.status.placeView")); break;
        case DrawingTool::PickParent:
        case DrawingTool::PickSectionParent:
        case DrawingTool::PickDetailParent: setPrompt(text("ui.status.pickParentView")); break;
        case DrawingTool::SectionStart: setPrompt(text("ui.status.sectionStart")); break;
        case DrawingTool::SectionEnd: setPrompt(text("ui.status.sectionEnd")); break;
        case DrawingTool::PlaceSection: setPrompt(text("ui.status.placeSection")); break;
        case DrawingTool::DetailCenter: setPrompt(text("ui.status.detailCenter")); break;
        case DrawingTool::DetailRadius: setPrompt(text("ui.status.detailRadius")); break;
        case DrawingTool::PlaceDetail: setPrompt(text("ui.status.placeDetail")); break;
        case DrawingTool::PickMarkFace: setPrompt(text("ui.status.pickMarkFace")); break;
        case DrawingTool::PickBalloonFace: setPrompt(text("ui.status.pickBalloonFace")); break;
        case DrawingTool::None: setPrompt({}); break;
    }
    if (t == DrawingTool::None || t == DrawingTool::SectionStart || t == DrawingTool::DetailCenter) drawingView_->setOverlay({});
    drawingView_->setToolActive(t != DrawingTool::None);
    drawingView_->setCursor(t == DrawingTool::None ? Qt::ArrowCursor : Qt::CrossCursor);
}

// ---------------------------------------------------------------- sections and detail views (§8б, S3-DRW-005)

void MainWindow::cmdSectionView(bool detail) {
    if (!drawingActive()) return;
    if (!selectedView().empty()) {
        placeParent_ = selectedView();
        setDrawingTool(detail ? DrawingTool::DetailCenter : DrawingTool::SectionStart);
    } else {
        setDrawingTool(detail ? DrawingTool::PickDetailParent : DrawingTool::PickSectionParent);
    }
}

std::optional<MainWindow::SectionSnap> MainWindow::sectionSnapAt(const Point2& at) {
    if (placeParent_.empty() || currentSheet_.empty()) return std::nullopt;
    if (sectionEntsView_ != placeParent_) {
        auto r = model_->session().viewEntities(currentSheet_, placeParent_);
        sectionEnts_ = r ? std::move(*r) : std::vector<drawing::ViewEntity>();
        sectionEntsView_ = placeParent_;
    }
    const double z = std::max(1e-9, drawingView_->zoom());
    const double ptTol = PICK_POINT_PX / z, crvTol = PICK_CURVE_PX / z;
    using K = drawing::ViewEntity::Kind;
    auto segDist = [](const Point2& a, const Point2& b, const Point2& p, double& t) {
        const Point2 ab = b - a;
        t = ab.squaredNorm() > 0 ? std::clamp((p - a).dot(ab) / ab.squaredNorm(), 0.0, 1.0) : 0.0;
        return (a + t * ab - p).norm();
    };
    // Points first — vertices, centers (a center wins a tie) and edge midpoints; then lines — an axis (wins a tie), an
    // edge point (drawing.md §6.11, §6а.7).
    std::optional<SectionSnap> best;
    double bestD = 0;
    // An axis or a center wins a tie only over what coincides with it (a point at the same place, a parallel edge).
    auto coincide = [](const SectionSnap& a, const SectionSnap& b) {
        const bool la = a.kind == "axis" || a.kind == "edge", lb = b.kind == "axis" || b.kind == "edge";
        if (!la && !lb) return (a.at - b.at).norm() <= drawing::DIM_PICK_TIE;
        if (la != lb) return false;
        const Point2 ta = (a.b - a.a).normalized(), tb = (b.b - b.a).normalized();
        return std::abs(ta.x() * tb.y() - ta.y() * tb.x()) <= 1e-9;
    };
    auto offer = [&](SectionSnap s, double d, bool axial) {
        const bool tie = best && std::abs(d - bestD) <= drawing::DIM_PICK_TIE;
        const bool bestAxial = best && (best->kind == "center" || best->kind == "axis");
        const bool mixed = tie && axial != bestAxial;  // axial vs model: the axial one only if they coincide
        if (!best || d < bestD - drawing::DIM_PICK_TIE || (mixed && (coincide(s, *best) ? axial : !axial))) best = std::move(s), bestD = d;
    };
    for (const auto& e : sectionEnts_) {
        if (e.kind == K::Point && (e.ref == "vertex" || e.ref == "center")) {
            const double d = (e.a - at).norm();
            if (d <= ptTol) offer(SectionSnap{e.ref, e.name, e.a}, d, e.ref == "center");
        } else if (e.kind == K::Line && e.ref == "edge") {
            const Point2 m = (e.a + e.b) / 2;
            const double d = (m - at).norm();
            if (d <= ptTol) offer(SectionSnap{"mid", e.name, m, e.a, e.b}, d, false);
        }
    }
    if (best) return best;
    for (const auto& e : sectionEnts_) {
        if (e.kind != K::Line || (e.ref != "edge" && e.ref != "axis")) continue;
        double t = 0;
        const double d = segDist(e.a, e.b, at, t);
        if (d > crvTol) continue;
        SectionSnap s{e.ref, e.name, Point2(e.a + t * (e.b - e.a)), e.a, e.b, t};
        offer(std::move(s), d, e.ref == "axis");
    }
    return best;
}

void MainWindow::updateToolOverlay(const Point2& cursor) {
    std::vector<drawing::Primitive> o;
    auto line = [&](const Point2& a, const Point2& b) {
        drawing::Primitive p;
        p.kind = drawing::PrimitiveKind::Line;
        p.points = {a, b};
        o.push_back(p);
    };
    auto circle = [&](const Point2& c, double r) {
        drawing::Primitive p;
        p.kind = drawing::PrimitiveKind::Circle;
        p.center = c;
        p.radius = r;
        o.push_back(p);
    };
    // Section line input: the parent element under the cursor is highlighted (FX9-DRW-002).
    if (drawingTool_ == DrawingTool::SectionStart || drawingTool_ == DrawingTool::SectionEnd) {
        sectionHover_ = sectionSnapAt(cursor);
        if (sectionHover_) {
            drawing::Primitive h;
            h.role = "hover";
            if (sectionHover_->kind == "axis" || sectionHover_->kind == "edge") {
                h.kind = drawing::PrimitiveKind::Line;
                h.points = {sectionHover_->a, sectionHover_->b};
                o.push_back(h);
            }
            h.kind = drawing::PrimitiveKind::Circle;
            h.center = sectionHover_->at;
            h.radius = 0.8;
            o.push_back(h);
        }
        if (drawingTool_ == DrawingTool::SectionStart) {
            drawingView_->setOverlay(std::move(o));
            return;
        }
    }
    switch (drawingTool_) {
        case DrawingTool::SectionEnd: line(toolPt1_, sectionHover_ && sectionHover_->kind != "axis" ? sectionHover_->at : cursor); break;
        case DrawingTool::PlaceSection: line(toolPt1_, toolPt2_); break;
        case DrawingTool::DetailRadius: circle(toolPt1_, (cursor - toolPt1_).norm()); break;
        case DrawingTool::PlaceDetail: circle(toolPt1_, (toolPt2_ - toolPt1_).norm()); break;
        default: return;
    }
    drawingView_->setOverlay(std::move(o));
}

void MainWindow::sectionToolClick(const Point2& at) {
    constexpr double MIN_PICK_MM = 0.5;  // §8б: coincident line points and a zero radius are rejected
    auto& s = model_->session();
    const auto d = s.document().document;  // hold the document: fields of the temporary snapshot
    const int si = d->active().sheetIndex(currentSheet_);
    if (si < 0) return;
    const doc::Sheet& sh = d->active().sheets[size_t(si)];
    const auto& pls = drawingView_->graphics().placements;
    const auto pl = std::find_if(pls.begin(), pls.end(), [&](const drawing::ViewPlacement& p) { return p.view == placeParent_; });
    auto toView = [&](const Point2& q) { return Point2((q - pl->offset) / pl->scale); };
    switch (drawingTool_) {
        case DrawingTool::PickSectionParent:
        case DrawingTool::PickDetailParent: {
            const std::string v = drawingView_->viewAt(at);
            if (v.empty()) return;
            const bool detail = drawingTool_ == DrawingTool::PickDetailParent;
            selectView(v);
            placeParent_ = v;
            setDrawingTool(detail ? DrawingTool::DetailCenter : DrawingTool::SectionStart);
            return;
        }
        case DrawingTool::SectionStart:
        case DrawingTool::DetailCenter:
            if (pl == pls.end()) return;  // parent not built yet — no placement
            toolPt1_ = at;
            if (drawingTool_ == DrawingTool::SectionStart) {  // snapping (FX9-DRW-002): the same element as highlighted
                sectionSnap1_ = sectionSnapAt(at);
                sectionSnap2_.reset();
                if (sectionSnap1_ && sectionSnap1_->kind == "axis") {  // along the axis: both ends at once
                    const Point2 d = (sectionSnap1_->b - sectionSnap1_->a).normalized();
                    toolPt1_ = sectionSnap1_->a - drawing::SECTION_BIND_OVERSHOOT_MM * d;
                    toolPt2_ = sectionSnap1_->b + drawing::SECTION_BIND_OVERSHOOT_MM * d;
                    setDrawingTool(DrawingTool::PlaceSection);
                    updateToolOverlay(at);
                    return;
                }
                if (sectionSnap1_) toolPt1_ = sectionSnap1_->at;
            }
            setDrawingTool(drawingTool_ == DrawingTool::SectionStart ? DrawingTool::SectionEnd : DrawingTool::DetailRadius);
            updateToolOverlay(at);
            return;
        case DrawingTool::SectionEnd:
        case DrawingTool::DetailRadius: {
            Point2 end = at;
            if (drawingTool_ == DrawingTool::SectionEnd) {
                sectionSnap2_ = sectionSnapAt(at);
                if (sectionSnap2_ && sectionSnap2_->kind == "axis") sectionSnap2_.reset();  // an axis binds only the first click
                if (sectionSnap2_) end = sectionSnap2_->at;
            }
            if ((end - toolPt1_).norm() < MIN_PICK_MM) return;
            toolPt2_ = end;
            // Parent — the view where the line was drawn or the center placed (FX1-DRW-004, §8б): the selected view is
            // still the parent if the line touches it (center inside it), else — the last drawn of the touched ones.
            {
                std::string hit;
                bool keep = false;
                for (const auto& p : pls) {
                    const auto box = drawingView_->viewRect(p.view);
                    const bool h = drawingTool_ == DrawingTool::SectionEnd ? segmentHitsBox(toolPt1_, at, box) : box.contains(toolPt1_);
                    if (!h) continue;
                    if (p.view == placeParent_) keep = true;
                    hit = p.view;
                }
                if (!keep && !hit.empty() && hit != placeParent_) {
                    placeParent_ = hit;
                    drawingView_->setSelectedView(hit);
                    drawingView_->update();
                }
            }
            setDrawingTool(drawingTool_ == DrawingTool::SectionEnd ? DrawingTool::PlaceSection : DrawingTool::PlaceDetail);
            updateToolOverlay(at);
            return;
        }
        case DrawingTool::PlaceSection: {
            if (pl == pls.end()) return;
            setDrawingTool(DrawingTool::None);
            const Point2 dd = (toolPt2_ - toolPt1_).normalized();
            Point2 sd(dd.y(), -dd.x());
            const bool flip = (at - (toolPt1_ + toolPt2_) / 2).dot(sd) < 0;  // look toward the section location
            if (flip) sd = -sd;
            const Point2 q = drawing::effectiveViewPosition(sh, placeParent_).value_or(at);
            const Point2 pos = q + (at - q).dot(sd) * sd;  // projected placement
            const Point2 a = toView(toolPt1_), b = toView(toolPt2_);
            api::Json def{{"kind", "section"}, {"parent", placeParent_},
                          {"line", api::Json::array({api::Json::array({a.x(), a.y()}), api::Json::array({b.x(), b.y()})})},
                          {"position", api::Json::array({pos.x(), pos.y()})}};
            if (flip) def["flip"] = true;
            // Binding by stable names (FX9-DRW-002, drawing.md §6.11): an axis, or the snapped points with t 0 and 1.
            if (sectionSnap1_ && sectionSnap1_->kind == "axis") {
                def["lineBind"] = api::Json{{"axis", sectionSnap1_->name}};
            } else {
                api::Json pts = api::Json::array();
                for (const auto& [snap, t] : {std::pair{&sectionSnap1_, 0.0}, std::pair{&sectionSnap2_, 1.0}}) {
                    if (!*snap) continue;
                    api::Json p{{(*snap)->kind, (*snap)->name}, {"t", t}};
                    if ((*snap)->kind == "edge") p["at"] = (*snap)->param;
                    pts.push_back(p);
                }
                if (!pts.empty()) def["lineBind"] = api::Json{{"points", pts}};
            }
            sectionSnap1_.reset();
            sectionSnap2_.reset();
            auto r = s.addView(currentSheet_, def);
            if (!r) showToast(errorText(r.error()), true);
            else selectView(*r);
            return;
        }
        case DrawingTool::PlaceDetail: {
            if (pl == pls.end()) return;
            setDrawingTool(DrawingTool::None);
            const std::string scale = drawing::detailScale(pl->scale);  // twice the parent's size (drawing.md §6.12)
            const Point2 c = toView(toolPt1_);
            const double radius = (toolPt2_ - toolPt1_).norm() / pl->scale;
            auto r = s.addView(currentSheet_, api::Json{{"kind", "detail"}, {"parent", placeParent_}, {"center", api::Json::array({c.x(), c.y()})},
                                                       {"radius", radius}, {"scale", scale}, {"position", api::Json::array({at.x(), at.y()})}});
            if (!r) showToast(errorText(r.error()), true);
            else selectView(*r);
            return;
        }
        default: return;
    }
}

void MainWindow::sheetClicked(const Point2& at) {
    if (!drawingActive() || currentSheet_.empty()) return;
    auto& s = model_->session();
    const auto d = s.document().document;  // hold the document: fields of the temporary snapshot
    const int si = d->active().sheetIndex(currentSheet_);
    if (si < 0) return;
    const doc::Sheet& sh = d->active().sheets[size_t(si)];
    switch (drawingTool_) {
        case DrawingTool::PlaceBase: {
            setDrawingTool(DrawingTool::None);
            api::Json ref{{"element", placeModel_}};
            if (!placeFile_.empty()) ref["file"] = placeFile_;
            auto r = s.addView(currentSheet_, api::Json{{"kind", "base"}, {"ref", ref}, {"orientation", "front"}, {"position", {at.x(), at.y()}}});
            if (!r) showToast(errorText(r.error()), true);
            else selectView(*r);
            return;
        }
        case DrawingTool::PlaceThree: {  // three projections (FX1-DRW-001): the main view at the click point
            setDrawingTool(DrawingTool::None);
            api::Json ref{{"element", placeModel_}};
            if (!placeFile_.empty()) ref["file"] = placeFile_;
            auto r = s.addThreeViews(currentSheet_, api::Json{{"ref", ref}, {"position", {at.x(), at.y()}}});
            if (!r) showToast(errorText(r.error()), true);
            else selectView(r->front());
            return;
        }
        case DrawingTool::PickParent: {
            const std::string v = drawingView_->viewAt(at);
            if (v.empty()) return;
            selectView(v);
            placeParent_ = v;
            setDrawingTool(DrawingTool::PlaceProjected);
            return;
        }
        case DrawingTool::PlaceProjected: {
            setDrawingTool(DrawingTool::None);
            const auto q = drawing::effectiveViewPosition(sh, placeParent_);
            if (!q) return;
            const double dx = at.x() - q->x(), dy = at.y() - q->y();
            std::string dir = std::abs(dy) >= std::abs(dx) ? (dy < 0 ? "top" : "bottom") : (dx > 0 ? "left" : "right");
            if (drawing::projectionMethod(d->active()) == drawing::ProjectionMethod::Third)  // third angle — the other way round (§8б)
                dir = dir == "top" ? "bottom" : dir == "bottom" ? "top" : dir == "left" ? "right" : "left";
            const Point2 pos = verticalDirection(dir) ? Point2(q->x(), at.y()) : Point2(at.x(), q->y());
            auto r = s.addView(currentSheet_, api::Json{{"kind", "projected"}, {"parent", placeParent_}, {"direction", dir},
                                                       {"position", {pos.x(), pos.y()}}});
            if (!r) showToast(errorText(r.error()), true);
            else selectView(*r);
            return;
        }
        case DrawingTool::PickMarkFace: {  // FX11-DRW-001: the face whose line is under the click
            const auto f = dims_->faceAt(at);
            if (!f) {
                showToast(text("ui.status.pickMarkFace"), true);
                return;
            }
            setDrawingTool(DrawingTool::None);
            startDrawingMark(f->first, f->second);
            return;
        }
        case DrawingTool::PickBalloonFace: {  // FX14-DRW-002: an item number on the part under the click; the tool stays
            const auto f = dims_->faceAt(at);
            if (!f) {
                showToast(text("ui.status.pickBalloonFace"), true);
                return;
            }
            auto r = s.addMark(currentSheet_, api::Json{{"kind", "balloon"}, {"view", f->first}, {"source", f->second}});
            if (!r) showToast(errorText(r.error()), true);
            refreshDrawing();
            return;
        }
        case DrawingTool::None:
            dims_->select({});
            texts_->select({});
            marks_->select({});
            selectView(drawingView_->viewAt(at));
            return;
        default: sectionToolClick(at); return;
    }
}

// Item numbers (FX14-DRW-002, §8б): a click on the line of a part on a view places its number; Esc — exit.
void MainWindow::cmdDrawingBalloon() {
    if (!drawingActive() || mode_ != Mode::Part || currentSheet_.empty()) return;
    dims_->setActive(false);
    setDrawingTool(DrawingTool::PickBalloonFace);
}

// Parts list (FX14-DRW-003, §8б): the table of the drawing assembly's items above the title block, selected.
void MainWindow::cmdPartsList() {
    if (!drawingActive() || mode_ != Mode::Part || currentSheet_.empty()) return;
    auto r = model_->session().addMark(currentSheet_, api::Json{{"kind", "partsList"}});
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    refreshDrawing();
    marks_->select(*r);
}

// "Dimensions from model" (FX1-DRW-003, §8б): driving model dimensions onto suitable views of the current sheet.
void MainWindow::cmdModelDimensions() {
    if (!drawingActive() || currentSheet_.empty()) return;
    // The selected view only (FX7-DRW-009); no selection — the connected views of each model.
    std::vector<std::string> only;
    if (!selectedView().empty()) only.push_back(selectedView());
    std::vector<drawing::UnplacedDimension> unplaced;
    auto r = model_->session().addModelDimensions(currentSheet_, only, &unplaced);
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    if (r->empty()) showToast(text("ui.modelDims.none"));
    else showToast(text("ui.modelDims.done", {{"count", QString::number(r->size())}}));
    // Dimensions no view shows (FX8-DRW-002, drawing.md §6а.10 item 8): «Sketch1: 30», «Fillet1» — one more message.
    if (!unplaced.empty()) {
        QStringList items;
        for (const auto& u : unplaced)
            items << (u.value.empty() ? QString::fromStdString(u.featureName)
                                      : QString::fromStdString(u.featureName) + ": " + QString::fromStdString(u.value));
        showToast(text("ui.modelDims.unplaced", {{"count", QString::number(items.size())}, {"list", items.join(", ")}}));
    }
}

// «Place all additional information» (FX9-DRW-008, §8б): the model
// annotations, rest roughness and welds onto the views of the current sheet as stored marks, one undo step.
void MainWindow::cmdModelMarks() {
    if (!drawingActive() || currentSheet_.empty()) return;
    // The selected view first (FX14-DRW-005); no selection — onto views with room.
    std::vector<std::string> only;
    if (!selectedView().empty()) only.push_back(selectedView());
    auto r = model_->session().addModelMarks(currentSheet_, only);
    if (!r) {
        showToast(errorText(r.error()), true);
        return;
    }
    if (r->empty()) showToast(text("ui.modelMarks.none"));
    else showToast(text("ui.modelMarks.done", {{"count", QString::number(r->size())}}));
}

void MainWindow::selectView(const std::string& viewId) {
    if (!viewId.empty() && texts_) texts_->select({});
    if (!viewId.empty() && marks_) marks_->select({});
    if (!viewId.empty() && dims_) dims_->select({});  // one selection, one panel (FX12-DRW-001)
    drawingView_->setSelectedView(viewId);
    refreshDrawing();
}

void MainWindow::viewDragged(const std::string& viewId, const Point2& delta) {
    const auto d = model_->session().document().document;
    const int si = d->active().isDrawing() ? d->active().sheetIndex(currentSheet_) : -1;
    if (si < 0) return;
    const doc::Sheet& sh = d->active().sheets[size_t(si)];
    const doc::DrawingView* v = findView(sh, viewId);
    const auto eff = drawing::effectiveViewPosition(sh, viewId);
    if (!v || !eff) return;
    Point2 pos = *eff + delta;
    if (v->kind == "projected")  // shared coordinate — the parent's (drawing.md §6.2)
        if (const auto q = drawing::effectiveViewPosition(sh, v->parent))
            pos = verticalDirection(v->direction) ? Point2(q->x(), pos.y()) : Point2(pos.x(), q->y());
    drawingView_->setSelectedView(viewId);
    // A main view moves with its projected views as one whole (FX12-DRW-003): their free coordinates shift by the same
    // offset, in one undo step; a projected view — alone, along its free coordinate (the main view stays).
    const auto linked = v->kind == "projected" ? std::vector<std::string>{} : projectedDescendants(sh, viewId);
    if (linked.empty()) {
        applyViewField(viewId, api::Json{{"position", {pos.x(), pos.y()}}});
        return;
    }
    std::vector<std::pair<std::string, Point2>> moves = {{viewId, pos}};
    for (const auto& id : linked)
        if (const doc::DrawingView* pv = findView(sh, id))
            moves.push_back({id, Point2(pv->position[0], pv->position[1]) + delta});
    auto& s = model_->session();
    s.beginGroup("undo.group.moveViews");
    for (const auto& [id, p] : moves)
        if (auto st = s.updateView(id, api::Json{{"position", {p.x(), p.y()}}}); !st) {
            showToast(errorText(st.error()), true);
            break;
        }
    s.endGroup();
    refreshDrawing();
}

std::vector<std::string> MainWindow::projectedDescendants(const doc::Sheet& sh, const std::string& viewId) const {
    std::vector<std::string> out;
    std::vector<std::string> todo = {viewId};
    while (!todo.empty()) {
        const std::string p = todo.back();
        todo.pop_back();
        for (const auto& v : sh.views)
            if (v.kind == "projected" && v.parent == p && std::find(out.begin(), out.end(), v.id) == out.end()) {
                out.push_back(v.id);
                todo.push_back(v.id);
            }
    }
    return out;
}

void MainWindow::showViewMenu(const std::string& viewId, const QPoint& globalPos) {
    const auto d = model_->session().document().document;
    const doc::DrawingView* v = nullptr;
    for (const auto& sh : d->active().sheets)
        if ((v = findView(sh, viewId))) break;
    if (!v) return;
    QMenu menu(this);
    menu.setObjectName("viewContextMenu");
    // The view settings in the right panel instead of the drawing settings (S7F-DRW-004).
    menu.addAction(txt("ui.common.properties"), this, [this, viewId] {
        selectView(viewId);
        showDrawingPanel(true);
    })->setObjectName("view_properties");
    menu.addSeparator();
    QAction* hidden = menu.addAction(txt("ui.drawing.hiddenLines"), this, [this, viewId, on = v->hiddenLines] {
        applyViewField(viewId, on ? api::Json{{"hiddenLines", nullptr}} : api::Json{{"hiddenLines", true}});
    });
    hidden->setCheckable(true);
    hidden->setChecked(v->hiddenLines);
    QAction* tangent = menu.addAction(txt("ui.drawing.tangentEdges"), this, [this, viewId, on = v->tangentEdges] {
        applyViewField(viewId, on ? api::Json{{"tangentEdges", nullptr}} : api::Json{{"tangentEdges", true}});
    });
    tangent->setCheckable(true);
    tangent->setChecked(v->tangentEdges);
    // Centerlines (S3-DRW-007); for a section — the cut and the line-of-sight direction (S3-DRW-005).
    QAction* center = menu.addAction(txt("ui.drawing.centerLines"), this, [this, viewId, on = v->centerLines] {
        applyViewField(viewId, on ? api::Json{{"centerLines", false}} : api::Json{{"centerLines", nullptr}});
    });
    center->setCheckable(true);
    center->setChecked(v->centerLines);
    // Model technical-requirement annotations on the view (S3-DRW-007, drawing.md §6.18); a detail view has none.
    if (v->kind != "detail") {
        QAction* pmiOn = menu.addAction(txt("ui.drawing.technicalRequirements"), this, [this, viewId, on = v->pmi] {
            applyViewField(viewId, on ? api::Json{{"pmi", false}} : api::Json{{"pmi", nullptr}});
        });
        pmiOn->setCheckable(true);
        pmiOn->setChecked(v->pmi);
    }
    if (v->kind == "section") {
        QAction* only = menu.addAction(txt("ui.drawing.sectionOnly"), this, [this, viewId, on = v->sectionOnly] {
            applyViewField(viewId, on ? api::Json{{"sectionOnly", nullptr}} : api::Json{{"sectionOnly", true}});
        });
        only->setCheckable(true);
        only->setChecked(v->sectionOnly);
        QAction* flip = menu.addAction(txt("ui.drawing.reverseDirection"), this, [this, viewId, on = v->flip] {
            applyViewField(viewId, on ? api::Json{{"flip", nullptr}} : api::Json{{"flip", true}});
        });
        flip->setCheckable(true);
        flip->setChecked(v->flip);
    }
    // The view model: copy into the project, replace (FX7-ASM-009, FX7-ASM-010; assembly.md §5б).
    if (v->kind == "base") {
        menu.addSeparator();
        if (!v->refFile.empty()) menu.addAction(txt("ui.common.makeLocalCopy"), this, [this, viewId] { localizeView(viewId); })->setObjectName("view_localize");
        menu.addAction(txt("ui.common.replaceWith"), this, [this, viewId] { chooseReplacement(viewId, true); })->setObjectName("view_replace");
    }
    menu.addSeparator();
    menu.addAction(txt("ui.common.delete"), this, [this, viewId] {
        if (auto s = model_->session().deleteView(viewId); !s) showToast(errorText(s.error()), true);
    });
    menu.exec(globalPos);
}

bool MainWindow::drawingEscape() {
    if (!drawingActive()) return false;
    // A cleared selection — the panel of the drawing again (FX12-DRW-001).
    if (texts_->escape()) return updateDrawingPanels(), true;  // text editor, text placement, selected text (FX7-DRW-006)
    if (marks_->escape()) return updateDrawingPanels(), true;  // selected mark (FX9-DRW-008)
    if (cons_->escape()) return updateDrawingPanels(), true;  // construction placement, selected construction (FX14-DRW-006)
    if (dims_->escape()) return updateDrawingPanels(), true;  // dimension mode, selected objects, selected dimension (§8б)
    if (drawingTool_ != DrawingTool::None) {
        setDrawingTool(DrawingTool::None);
        return true;
    }
    if (!selectedView().empty()) {
        selectView({});
        return true;
    }
    return false;
}

}  // namespace construe::app
