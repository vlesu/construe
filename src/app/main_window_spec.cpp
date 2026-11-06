// Main window: bill of materials — sheet window, rows table, manual rows, title block (spec/app-ux.md §8в,
// spec/specification.md).
#include "app/main_window.h"

#include "app/messages.h"
#include "app/theme.h"
#include "app/title_block_form.h"

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace construe::app {

namespace {

enum SpecColumn { COL_SECTION, COL_POSITION, COL_DESIGNATION, COL_NAME, COL_QTY, COL_NOTE, COL_COUNT };
constexpr int SPEC_PANEL_W = 480;  // BOM panel width, pixels (like the drawing's right panel, wider — for the table)

// extraRows entry field for a table column (manual row, §8в).
const char* extraField(int column) {
    switch (column) {
        case COL_POSITION: return "position";
        case COL_DESIGNATION: return "designation";
        case COL_NAME: return "name";
        case COL_QTY: return "quantity";
        case COL_NOTE: return "note";
        default: return nullptr;
    }
}

// Item number from text: empty — nullopt (the field is removed), error — -1.
std::optional<long long> parsePosition(const QString& s, bool& ok) {
    ok = true;
    if (s.isEmpty()) return std::nullopt;
    bool num = false;
    const long long v = s.toLongLong(&num);
    if (!num || v < 1 || v > 999999) ok = false;
    return v;
}

}  // namespace

bool MainWindow::specificationActive() const { return model_->session().document().document->active().isSpecification(); }

QWidget* MainWindow::buildSpecPage() {
    specPage_ = new QWidget;
    specPage_->setObjectName("specPage");
    auto* row = new QHBoxLayout(specPage_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    specView_ = new DrawingView;
    specView_->setObjectName("specView");
    row->addWidget(specView_, 1);
    auto* panel = new QFrame;
    panel->setObjectName("specPanel");
    panel->setFrameShape(QFrame::StyledPanel);
    panel->setFixedWidth(SPEC_PANEL_W);
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(6, 6, 6, 6);
    specHeader_ = new QLabel;
    specHeader_->setObjectName("specHeader");
    QFont hf = specHeader_->font();
    hf.setBold(true);
    specHeader_->setFont(hf);
    v->addWidget(specHeader_);
    specErrors_ = new QLabel;
    specErrors_->setObjectName("specErrors");
    specErrors_->setWordWrap(true);
    specErrors_->setStyleSheet(QString("color: %1").arg(COLOR_ERROR_TEXT.color.name()));
    specErrors_->hide();
    v->addWidget(specErrors_);
    // Rows table (§8в): "Item", "Note" of an automatic row and all fields of a manual one — edit by double click.
    specRows_ = new QTableWidget(0, COL_COUNT);
    specRows_->setObjectName("specRows");
    specRows_->setHorizontalHeaderLabels({txt("ui.common.section2"), txt("ui.spec.item"), txt("ui.common.designation"), txt("ui.common.name2"), txt("ui.spec.qty"), txt("ui.spec.note")});
    specRows_->verticalHeader()->hide();
    specRows_->setSelectionBehavior(QAbstractItemView::SelectRows);
    specRows_->setSelectionMode(QAbstractItemView::SingleSelection);
    specRows_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    specRows_->horizontalHeader()->setSectionResizeMode(COL_NAME, QHeaderView::Stretch);
    for (int c : {COL_SECTION, COL_POSITION, COL_QTY}) specRows_->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    v->addWidget(specRows_, 3);
    connect(specRows_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (specFilling_ || !item) return;
        // Applied after returning from the handler: editing the document rebuilds the table.
        const int r = item->row(), c = item->column();
        const QString t = item->text().trimmed();
        QMetaObject::invokeMethod(this, [this, r, c, t] { specCellEdited(r, c, t); }, Qt::QueuedConnection);
    });
    connect(specRows_, &QTableWidget::itemSelectionChanged, this, [this] {
        const int r = specRows_->currentRow();
        specDeleteRow_->setEnabled(r >= 0 && r < int(specItems_.size()) && specItems_[size_t(r)].extra >= 0 &&
                                   specRows_->selectionModel()->hasSelection());
    });
    // Manual rows: section, «Add row», «Delete row».
    auto* rowBar = new QHBoxLayout;
    rowBar->addWidget(new QLabel(txt("ui.common.section2")));
    specSection_ = new QComboBox;
    specSection_->setObjectName("specSection");
    for (const auto& s : drawing::specSections()) specSection_->addItem(QString::fromStdString(drawing::specSectionTitle(s)), QString::fromStdString(s));
    specSection_->setCurrentIndex(specSection_->findData("other"));
    rowBar->addWidget(specSection_, 1);
    auto* add = new QToolButton;
    add->setObjectName("specAddRow");
    add->setText(txt("ui.spec.addRow"));
    add->setToolTip(txt("ui.spec.addAManualRowTo"));
    connect(add, &QToolButton::clicked, this, [this] { specAddRow(); });
    rowBar->addWidget(add);
    specDeleteRow_ = new QToolButton;
    specDeleteRow_->setObjectName("specDeleteRow");
    specDeleteRow_->setText(txt("ui.spec.deleteRow"));
    specDeleteRow_->setToolTip(txt("ui.spec.deleteTheSelectedManualRow"));
    specDeleteRow_->setEnabled(false);
    connect(specDeleteRow_, &QToolButton::clicked, this, [this] { specDeleteRow(); });
    rowBar->addWidget(specDeleteRow_);
    v->addLayout(rowBar);
    // Title block (form 2, specification.md §5): fields as in the drawing (§8б).
    auto* tbTitle = new QLabel(txt("ui.spec.titleBlock"));
    tbTitle->setFont(hf);
    v->addWidget(tbTitle);
    specTitleForm_ = new TitleBlockForm(TitleBlockForm::Kind::Specification, "specTitle_", "specTitleLabel_");
    connect(specTitleForm_, &TitleBlockForm::fieldEdited, this, [this](const std::string& key, const QString& t) { applySpecTitleField(key, t); });
    v->addWidget(specTitleForm_, 2);
    row->addWidget(panel);
    return specPage_;
}

void MainWindow::refreshSpecification() {
    if (!center_ || !specPage_) return;
    auto& s = model_->session();
    const auto d = s.document().document;  // hold the document: fields of the temporary snapshot
    const doc::Element& el = d->active();
    if (!el.isSpecification()) return;
    auto info = s.specification();
    if (!info) return;
    const int page = drawing::specPageIndex(currentSheet_);
    if (page < 0 || page >= info->pages) currentSheet_ = drawing::specPageId(0);
    if (auto g = s.sheetGraphics(currentSheet_)) specView_->setGraphics(std::move(*g));
    center_->setCurrentWidget(specPage_);
    specHeader_->setText(QString(txt("ui.spec.billOfMaterialsN")).arg(QString::fromStdString(el.name)));
    QStringList errs;
    for (const auto& e : info->content.errors) errs << errorText(e, d.get());
    specErrors_->setText(errs.join("\n"));
    specErrors_->setVisible(!errs.isEmpty());
    // Rows table — anew, unless a cell is being edited.
    QWidget* fw = QApplication::focusWidget();  // the cell editor is a focused child widget of the table
    const bool editing = fw && fw != specRows_ && specRows_->isAncestorOf(fw);
    if (!editing) {
        const int selected = specRows_->currentRow();
        specFilling_ = true;
        specItems_ = info->content.items;
        specRows_->setRowCount(int(specItems_.size()));
        for (int i = 0; i < int(specItems_.size()); ++i) {
            const auto& it = specItems_[size_t(i)];
            const QString cells[COL_COUNT] = {QString::fromStdString(drawing::specSectionTitle(it.section)), QString::fromStdString(it.position),
                                              QString::fromStdString(it.designation), QString::fromStdString(it.name),
                                              QString::fromStdString(it.quantity), QString::fromStdString(it.note)};
            for (int c = 0; c < COL_COUNT; ++c) {
                auto* cell = new QTableWidgetItem(cells[c]);
                const bool editable = c != COL_SECTION && (it.extra >= 0 || c == COL_POSITION || c == COL_NOTE);
                cell->setFlags(editable ? Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable : Qt::ItemIsSelectable | Qt::ItemIsEnabled);
                specRows_->setItem(i, c, cell);
            }
        }
        if (selected >= 0 && selected < specRows_->rowCount()) specRows_->selectRow(selected);
        else specRows_->clearSelection();
        specFilling_ = false;
        emit specRows_->itemSelectionChanged();
    }
    // Title block: value — the BOM's titleBlock, placeholder — the default value.
    const auto defaults = s.titleBlockDefaults(currentSheet_);
    specTitleForm_->setValues(el.specTitleBlock, defaults ? *defaults : std::map<std::string, std::string>{});
}

void MainWindow::specCellEdited(int row, int column, const QString& text) {
    auto& s = model_->session();
    const auto d = s.document().document;
    const doc::Element& el = d->active();
    if (!el.isSpecification() || row < 0 || row >= int(specItems_.size())) return;
    const drawing::SpecItem it = specItems_[size_t(row)];
    bool ok = true;
    std::optional<long long> pos;
    if (column == COL_POSITION) {
        pos = parsePosition(text, ok);
        if (!ok) {
            showToast(::construe::app::text("ui.spec.badPosition"), true);
            refreshSpecification();
            return;
        }
    }
    Status st = okStatus();
    if (it.extra >= 0) {  // manual row — the extraRows entry field
        const char* f = extraField(column);
        if (!f || it.extra >= int(el.specExtraRows.size())) return;
        api::Json rows = api::Json::array();
        for (const auto& r : el.specExtraRows) rows.push_back(r);
        api::Json& rec = rows[size_t(it.extra)];
        if (column == COL_POSITION) {
            if (pos) rec[f] = *pos;
            else rec.erase(f);
        } else if (text.isEmpty()) {
            rec.erase(f);
        } else {
            rec[f] = text.toStdString();
        }
        st = s.updateSpecification(api::Json{{"extraRows", rows}});
    } else {  // automatic row — override by key
        if (column != COL_POSITION && column != COL_NOTE) return;
        const char* f = column == COL_POSITION ? "position" : "note";
        api::Json rows = api::Json::array();
        bool found = false;
        for (const auto& r : el.specRows) {
            api::Json rec = r;
            if (!found && rec.value("key", std::string()) == it.key) {
                found = true;
                if (column == COL_POSITION && pos) rec[f] = *pos;
                else if (column == COL_NOTE && !text.isEmpty()) rec[f] = text.toStdString();
                else rec.erase(f);
                if (rec.size() == 1) continue;  // only the key field is left — no entry needed
            }
            rows.push_back(rec);
        }
        if (!found) {
            api::Json rec{{"key", it.key}};
            if (column == COL_POSITION && pos) rec[f] = *pos;
            else if (column == COL_NOTE && !text.isEmpty()) rec[f] = text.toStdString();
            if (rec.size() > 1) rows.push_back(rec);
        }
        st = s.updateSpecification(api::Json{{"rows", rows.empty() ? api::Json(nullptr) : rows}});
    }
    if (!st) {
        showToast(errorText(st.error()), true);
        refreshSpecification();
    }
}

void MainWindow::specAddRow() {
    auto& s = model_->session();
    const auto d = s.document().document;
    const doc::Element& el = d->active();
    if (!el.isSpecification()) return;
    api::Json rows = api::Json::array();
    for (const auto& r : el.specExtraRows) rows.push_back(r);
    rows.push_back(api::Json{{"section", specSection_->currentData().toString().toStdString()}});
    const int added = int(rows.size()) - 1;
    if (auto st = s.updateSpecification(api::Json{{"extraRows", rows}}); !st) {
        showToast(errorText(st.error()), true);
        return;
    }
    for (int i = 0; i < int(specItems_.size()); ++i)  // new row — selected
        if (specItems_[size_t(i)].extra == added) specRows_->selectRow(i);
}

void MainWindow::specDeleteRow() {
    auto& s = model_->session();
    const auto d = s.document().document;
    const doc::Element& el = d->active();
    const int r = specRows_->currentRow();
    if (!el.isSpecification() || r < 0 || r >= int(specItems_.size())) return;
    const int extra = specItems_[size_t(r)].extra;
    if (extra < 0 || extra >= int(el.specExtraRows.size())) return;
    api::Json rows = api::Json::array();
    for (int i = 0; i < int(el.specExtraRows.size()); ++i)
        if (i != extra) rows.push_back(el.specExtraRows[size_t(i)]);
    specRows_->clearSelection();
    if (auto st = s.updateSpecification(api::Json{{"extraRows", rows.empty() ? api::Json(nullptr) : rows}}); !st)
        showToast(errorText(st.error()), true);
}

void MainWindow::applySpecTitleField(const std::string& key, const QString& text) {
    auto& s = model_->session();
    const auto d = s.document().document;
    const doc::Element& el = d->active();
    if (!el.isSpecification()) return;
    const auto tb = TitleBlockForm::patch(el.specTitleBlock, key, text);
    if (!tb) return;  // no changes — no undo step
    if (auto st = s.updateSpecification(api::Json{{"titleBlock", *tb}}); !st) {
        showToast(errorText(st.error()), true);
        refreshSpecification();
    }
}

}  // namespace construe::app
