// Main window: drawing — view model, projection plane, display and sheet metal state (FX7-DRW-007, FX7-DRW-008); choice
// of drawings for PDF and printing (FX7-DRW-004, FX7-DRW-005) (spec/app-ux.md §8б).
#include "app/main_window.h"

#include "app/captions.h"
#include "app/dialog_buttons.h"
#include "app/icons.h"
#include "app/messages.h"
#include "app/panel_header.h"
#include "app/sheet_print.h"
#include "drawing/cut_plan.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPrintDialog>
#include <QPrinter>
#include <QPrinterInfo>
#include <QSignalBlocker>
#include <QToolButton>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace construe::app {

namespace {
bool hasSheetMetal(const doc::Element& el) {
    for (const auto& f : el.features)
        if (f->type == doc::FeatureType::SheetMetal) return true;
    return false;
}
}  // namespace

// ---------------------------------------------------------------- view properties (§8б, FX7-DRW-007, FX7-DRW-008)

void MainWindow::refreshViewExtras(const doc::Document& d, const doc::DrawingView& v) {
    const QSignalBlocker b1(viewSource_), b2(viewPlane_), b3(viewDisplay_), b4(viewFlat_), b5(viewExploded_);
    const bool base = v.kind == "base";
    const doc::Element* model = nullptr;
    if (base && v.refFile.empty())
        if (const int ei = d.elementIndex(v.refElement); ei >= 0) model = &d.elements[size_t(ei)];
    // Model: parts, bodies and assemblies of the document (a model of another file is not changed here).
    viewSource_->clear();
    if (base && !v.refFile.empty()) {
        viewSource_->addItem(QString::fromStdString(v.refFile), QString::fromStdString(v.refElement));
    } else {
        for (const auto& e : d.elements)
            if (!e.isDrawing() && !e.isSpecification())
                viewSource_->addItem(commandIcon(e.isAssembly() ? "assembly" : e.isBody() ? "body" : "part"),
                                     QString::fromStdString(e.name), QString::fromStdString(e.id));
    }
    viewSource_->setCurrentIndex(std::max(0, viewSource_->findData(QString::fromStdString(v.refElement))));
    viewSource_->setEnabled(base && v.refFile.empty());
    // Projection plane: datum planes and the reference planes of the model (PlaneRef as JSON text in the item data).
    viewPlane_->clear();
    viewPlane_->addItem("—", QString());
    for (const auto& dp : DATUM_PLANES)
        viewPlane_->addItem(QString::fromUtf8(txt(dp.label)), QString::fromStdString(api::Json{{"datum", std::string(dp.id)}}.dump()));
    if (model)
        for (const auto& f : model->features)
            if (f->type == doc::FeatureType::Plane)
                viewPlane_->addItem(QString::fromStdString(f->name), QString::fromStdString(api::Json{{"feature", f->id}}.dump()));
    int planeIndex = 0;
    if (v.orientation == "plane" && v.plane) {
        planeIndex = viewPlane_->findData(QString::fromStdString(v.plane->dump()));
        if (planeIndex < 0) {  // a face or a plane of another model — shown as is
            viewPlane_->addItem(QString::fromStdString(v.plane->dump()), QString::fromStdString(v.plane->dump()));
            planeIndex = viewPlane_->count() - 1;
        }
    }
    viewPlane_->setCurrentIndex(planeIndex);
    viewPlane_->setEnabled(base);
    viewDisplay_->setCurrentIndex(v.display == "render" ? 1 : 0);
    viewDisplay_->setEnabled(base || v.kind == "projected");
    viewFlat_->setCurrentIndex(v.flat ? 1 : 0);
    viewFlat_->setEnabled(base && (v.flat || (model && hasSheetMetal(*model))));
    // Exploded view of an assembly (FX7-ASM-005, drawing.md §6.24): an assembly model of this or another file.
    viewExploded_->setCurrentIndex(v.exploded ? 1 : 0);
    viewExploded_->setEnabled(base && (v.exploded || (model ? model->isAssembly() : !v.refFile.empty())));
    // Model sections (S7F-DRW-005, drawing.md §6.26): a check box per section of the model of this file, checked — the
    // view is cut with it (by default — the sections enabled in the model); a click writes the view's list.
    for (auto* old : viewSections_->findChildren<QCheckBox*>()) {
        old->hide();
        old->deleteLater();
    }
    // A projected view has its own check boxes too (FX10-DRW-003): the model of the main view of its chain.
    std::optional<std::vector<std::string>> list = v.sections;
    if (v.kind == "projected")
        if (const auto vs = model_->session().viewSections(v.id); vs.found) {
            list = vs.list;
            if (vs.baseFile.empty() && !vs.baseFlat)
                if (const int ei = d.elementIndex(vs.baseElement); ei >= 0) model = &d.elements[size_t(ei)];
        }
    // Effective sections (drawing.md §6.26): the view's list (own or inherited), none — those not hidden in the model.
    std::vector<std::string> effective;
    if (model)
        for (const auto& s : model->sections)
            if (list ? std::find(list->begin(), list->end(), s.id) != list->end() : !s.hidden) effective.push_back(s.id);
    if (model)
        for (const auto& s : model->sections) {
            auto* box = new QCheckBox(QString::fromStdString(s.name));
            box->setObjectName(QString::fromStdString("viewSection_" + s.id));
            box->setChecked(std::find(effective.begin(), effective.end(), s.id) != effective.end());
            viewSections_->layout()->addWidget(box);
            connect(box, &QCheckBox::clicked, this, [this, viewId = v.id] {
                api::Json ids = api::Json::array();
                for (auto* b : viewSections_->findChildren<QCheckBox*>())
                    if (!b->isHidden() && b->isChecked()) ids.push_back(b->objectName().mid(12).toStdString());
                // Later: the click handler must not delete its own check box (the refresh rebuilds them).
                QTimer::singleShot(0, this, [this, viewId, ids] { applyViewField(viewId, api::Json{{"sections", ids}}); });
            });
        }
    if (auto* form = qobject_cast<QFormLayout*>(viewBar_->layout())) form->setRowVisible(viewSections_, model && !model->sections.empty());

}

// ---------------------------------------------------------------- PDF of several drawings, printing (§8б, FX7-DRW-004, -005)

QWidget* MainWindow::buildDrawingPickPanel() {
    drawingPickPanel_ = new QFrame;
    drawingPickPanel_->setObjectName("drawingPickPanel");
    drawingPickPanel_->setFrameShape(QFrame::StyledPanel);
    drawingPickPanel_->setFixedWidth(260);
    auto* v = new QVBoxLayout(drawingPickPanel_);
    v->setContentsMargins(6, 6, 6, 6);
    // Title row with the close cross in the top right corner (app-ux §2.1в, FX8-UI-008): the same as «Cancel».
    auto* head = new PanelHeader(QString(), "drawingPickClose");
    drawingPickTitle_ = head->title();
    drawingPickTitle_->setObjectName("drawingPickTitle");
    connect(head, &PanelHeader::closeRequested, this, [this] { closeDrawingPick(); });
    v->addWidget(head);
    v->addWidget(new QLabel(txt("ui.drawing.drawingsAndBillsOfMaterials")));
    drawingPickList_ = new CheckList("drawingPickList");
    v->addWidget(drawingPickList_, 1);
    auto* all = new QHBoxLayout;
    auto* selAll = new QPushButton(txt("ui.drawing.selectAll"));
    selAll->setObjectName("drawingPickAll");
    auto* selNone = new QPushButton(txt("ui.drawing.clearAll"));
    selNone->setObjectName("drawingPickNone");
    all->addWidget(selAll);
    all->addWidget(selNone);
    v->addLayout(all);
    auto* btns = new DialogButtons("drawingPickOk", "drawingPickCancel");
    drawingPickOk_ = btns->ok();
    v->addWidget(btns);
    connect(selAll, &QPushButton::clicked, this, [this] { drawingPickList_->setAllChecked(true); });
    connect(selNone, &QPushButton::clicked, this, [this] { drawingPickList_->setAllChecked(false); });
    connect(drawingPickList_, &CheckList::changed, this, [this] { drawingPickOk_->setEnabled(!pickedDrawings().empty()); });
    connect(btns, &DialogButtons::accepted, this, [this] { acceptDrawingPick(); });
    connect(btns, &DialogButtons::cancelled, this, [this] { closeDrawingPick(); });
    drawingPickPanel_->hide();
    return drawingPickPanel_;
}

void MainWindow::showDrawingPick(PickPurpose purpose) {
    if (!(drawingActive() || specificationActive()) || exportPending_) return;
    pickPurpose_ = purpose;
    // The panel is shown beside the sheet window of the active drawing or specification (FX8-DRW-005).
    auto* host = specificationActive() ? qobject_cast<QHBoxLayout*>(specPage_->layout()) : drawingRow_;
    if (host && host->indexOf(drawingPickPanel_) < 0) host->insertWidget(1, drawingPickPanel_);
    drawingPickTitle_->setText(purpose == PickPurpose::Pdf   ? txt("ui.drawing.exportPdf")
                               : purpose == PickPurpose::Dxf ? txt("ui.drawing.exportDxf")
                                                             : txt("ui.common.print"));
    // The title block panel gives way to the pick panel (FX8-DRW-004) and comes back when the pick panel closes.
    if (!drawingPickPanel_->isVisible()) titleBlockBeforePick_ = titleBlockPanel_ && titleBlockPanel_->isVisible();
    if (titleBlockPanel_ && titleBlockPanel_->isVisible()) showDrawingPanel(false);
    const auto d = model_->session().document().document;
    {
        // Drawings and specifications of the project in the document order (FX8-DRW-005); the active one is checked.
        // Cut plans (FX11-CUT-006, app-ux §8б): unchecked for PDF and printing, checked for DXF.
        std::vector<CheckList::Item> items;
        for (const auto& e : d->elements)
            if (e.isDrawing() || e.isSpecification()) {
                const bool cut = e.isDrawing() && drawing::isCutPlan(e);
                const bool checked = drawingPickChecked(cut, e.id == d->activeElement, purpose == PickPurpose::Dxf);
                items.push_back({QString::fromStdString(e.id), QString::fromStdString(e.name), checked, true,
                                 commandIcon(e.isDrawing() ? "newDrawing" : "specification")});
            }
        drawingPickList_->setItems(items);
    }
    drawingPickOk_->setEnabled(!pickedDrawings().empty());
    drawingPickPanel_->show();
}

std::vector<std::string> MainWindow::pickedDrawings() const { return drawingPickList_->checkedIds(); }

void MainWindow::closeDrawingPick() {
    drawingPickPanel_->hide();
    if (titleBlockBeforePick_ && drawingActive()) showDrawingPanel(true);
    titleBlockBeforePick_ = false;
}

void MainWindow::acceptDrawingPick() {
    const auto drawings = pickedDrawings();
    if (drawings.empty()) return;
    closeDrawingPick();
    if (pickPurpose_ == PickPurpose::Print) {
        printDrawings(drawings);
        return;
    }
    QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    const std::string active = model_->session().document().document->active().id;
    // One drawing — the name as for its current sheet; several — the name of the document file (§8б).
    QString suggested;
    if (drawings.size() == 1 && drawings.front() == active && !currentSheet_.empty()) {
        suggested = QString::fromStdString(model_->session().exportFileName(active, currentSheet_));
    } else if (const auto p = model_->session().path()) {
        suggested = QFileInfo(QString::fromStdString(*p)).completeBaseName();
    } else {
        suggested = txt("ui.drawing.drawings");
    }
    const bool dxf = pickPurpose_ == PickPurpose::Dxf;  // DXF by sheets: a file per sheet, "-2", "-3"… (FX11-DXF-001)
    const QString ext = dxf ? ".dxf" : ".pdf";
    QString path = QFileDialog::getSaveFileName(this, dxf ? txt("ui.drawing.exportDxf") : txt("ui.drawing.exportPdf"), suggested + ext,
                                                dxf ? "DXF (*.dxf)" : "PDF (*.pdf)", nullptr, opt);
    if (path.isEmpty()) return;
    if (!path.endsWith(ext, Qt::CaseInsensitive)) path += ext;
    auto r = dxf ? model_->session().exportDrawingsDxf(drawings, path.toStdString())
                 : model_->session().exportDrawingsPdf(drawings, path.toStdString());
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

void MainWindow::cmdPrint() {
    if (mode_ != Mode::Part || exportPending_) return;
    if (drawingActive() || specificationActive()) {  // drawings and specifications — one choice (FX8-DRW-005)
        showDrawingPick(PickPurpose::Print);
        return;
    }
    if (!specificationActive() || currentSheet_.empty()) return;
    // Bill of materials: its current sheet (as «PDF…»).
    auto g = model_->session().sheetGraphics(currentSheet_);
    if (!g) {
        showToast(errorText(g.error()), true);
        return;
    }
    QPrinter printer(QPrinter::HighResolution);
    if (!testPrintTarget_.isEmpty()) {
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(testPrintTarget_);
    } else {
        QPrintDialog dlg(&printer, this);
        if (dlg.exec() != QDialog::Accepted) return;
    }
    if (!printSheets(printer, {*g})) showToast(text("ui.print.failed"), true);
    else showToast(text("ui.print.done", {{"count", "1"}}));
}

void MainWindow::printDrawings(const std::vector<std::string>& drawings) {
    // The same sheets as the PDF (io.md §2в); the system print dialog chooses the printer (§10.2); in the test mode —
    // a PDF file instead of the printer, without the dialog.
    auto sheets = model_->session().drawingSheets(drawings);
    if (!sheets) {
        showToast(errorText(sheets.error()), true);
        return;
    }
    QPrinter printer(QPrinter::HighResolution);
    if (!testPrintTarget_.isEmpty()) {
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(testPrintTarget_);
    } else {
        QPrintDialog dlg(&printer, this);
        dlg.setObjectName("printDialog");
        if (dlg.exec() != QDialog::Accepted) return;
    }
    if (!printSheets(printer, *sheets)) {
        showToast(text("ui.print.failed"), true);
        return;
    }
    showToast(text("ui.print.done", {{"count", QString::number(sheets->size())}}));
}

}  // namespace construe::app
