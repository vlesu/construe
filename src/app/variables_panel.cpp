// Variables panel (spec/app-ux.md §8): types and help (S6-PARAM-001, -002; the list values and the help — in the
// properties box under the table, FX7-UI-004), variant tables (S6-PARAM-003).
#include "app/variables_panel.h"

#include "app/app_model.h"
#include "app/expr_input.h"
#include "app/keys.h"
#include "app/messages.h"
#include "app/panel_header.h"
#include "app/theme.h"
#include "doc/document.h"
#include "expr/expr.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace construe::app {

namespace {

// Variable value (app-ux §5.6): length and angle — a number in document units without the unit symbol.
QString valueText(const expr::Quantity& q, const expr::DocumentUnits& u) {
    if (q.dim == expr::LENGTH) return formatNumber(q.value / expr::lengthFactor(u.length));
    if (q.dim == expr::ANGLE) return formatNumber(q.value / expr::angleFactor(u.angle));
    return QString::fromStdString(expr::format(q, u, UI_DECIMALS));
}

// Types of the «Type» cell in order (§8).
const char* const kTypes[] = {"float", "int", "string", "list"};
const char* const kTypeLabels[] = {"ui.variables.number", "ui.variables.integer", "ui.variables.string", "ui.variables.list"};

int typeIndex(const std::string& t) {
    for (int i = 0; i < 4; ++i)
        if (t == kTypes[i]) return i;
    return 0;
}

// "a; b; c" → list values (§8: trimmed, empty ones dropped).
std::vector<std::string> splitValues(const QString& text) {
    std::vector<std::string> out;
    for (const QString& p : text.split(';')) {
        const QString t = p.trimmed();
        if (!t.isEmpty()) out.push_back(t.toStdString());
    }
    return out;
}

QString joinValues(const std::vector<std::string>& v) {
    QStringList l;
    for (const auto& x : v) l << QString::fromStdString(x);
    return l.join("; ");
}

QToolButton* headButton(const char* name, const QString& text, const QString& tip) {
    auto* b = new QToolButton;
    b->setObjectName(name);
    b->setText(text);
    b->setToolTip(tip);
    QFont bf = b->font();
    bf.setBold(true);
    b->setFont(bf);
    b->setAutoRaise(true);
    b->setFixedSize(28, 28);
    return b;
}

QLabel* boldTitle(const QString& text) {
    auto* title = new QLabel(text);
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    return title;
}

}  // namespace

VariablesPanel::VariablesPanel(AppModel& model, QWidget* parent) : QWidget(parent), model_(model) {
    setObjectName("variablesPanel");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    table_ = new QTableWidget(0, ColCount);
    table_->setObjectName("variablesTable");
    table_->setHorizontalHeaderLabels({txt("ui.common.name3"), txt("ui.common.expression"), txt("ui.common.value"), txt("ui.common.type")});
    // All columns fit the panel (FX7-UI-004): name, expression and value share the width, the type takes what it needs.
    table_->horizontalHeader()->setStretchLastSection(false);
    for (int c : {int(ColName), int(ColExpr), int(ColValue)}) table_->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(ColType, QHeaderView::ResizeToContents);
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    table_->installEventFilter(this);
    // Header row: "Variables", "+", "−" (§8, FX1-UI-006) and the close cross (§2.1в, FX8-UI-008).
    auto* head = new PanelHeader(txt("ui.common.variables"), "variablesClose");
    add_ = headButton("addVariable", "+", txt("ui.variables.addVariable"));
    del_ = headButton("deleteVariable", QString::fromUtf8("−"), txt("ui.variables.deleteVariable"));
    del_->setEnabled(false);
    head->addButton(add_);
    head->addButton(del_);
    connect(head, &PanelHeader::closeRequested, this, &QWidget::hide);
    v->addWidget(head);
    v->addWidget(table_, 3);
    // Properties of the current variable (§8, FX7-UI-004).
    propsTitle_ = new QLabel;
    propsTitle_->setObjectName("variablePropsTitle");
    v->addWidget(propsTitle_);
    auto* props = new QFormLayout;
    props->setContentsMargins(0, 0, 0, 0);
    valuesEdit_ = new QLineEdit;
    valuesEdit_->setObjectName("variableValuesEdit");
    valuesEdit_->setPlaceholderText(txt("ui.variables.valuesSeparatedByForThe"));
    helpEdit_ = new QLineEdit;
    helpEdit_->setObjectName("variableHelpEdit");
    helpEdit_->setPlaceholderText(txt("ui.variables.explanationShownInTheVariable"));
    props->addRow(txt("ui.variables.values"), valuesEdit_);
    props->addRow(txt("ui.common.help"), helpEdit_);
    v->addLayout(props);
    // Variant tables (§8, S6-PARAM-003).
    auto* thead = new QHBoxLayout;
    thead->addWidget(boldTitle(txt("ui.variables.variantTables")));
    tableSelect_ = new QComboBox;
    tableSelect_->setObjectName("variantTableSelect");
    thead->addWidget(tableSelect_, 1);
    addTable_ = headButton("addVariantTable", "+", txt("ui.variables.newVariantTable"));
    delTable_ = headButton("deleteVariantTable", QString::fromUtf8("−"), txt("ui.variables.deleteVariantTable"));
    thead->addWidget(addTable_);
    thead->addWidget(delTable_);
    v->addLayout(thead);
    auto* arow = new QHBoxLayout;
    arow->addWidget(new QLabel(txt("ui.variables.activeVariant")));
    activeVariant_ = new QComboBox;
    activeVariant_->setObjectName("variantActive");
    arow->addWidget(activeVariant_, 1);
    v->addLayout(arow);
    grid_ = new QTableWidget(0, 1);
    grid_->setObjectName("variantGrid");
    grid_->verticalHeader()->hide();
    grid_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    v->addWidget(grid_, 2);
    auto* brow = new QHBoxLayout;
    auto btn = [&](const char* name, const QString& text) {
        auto* b = new QPushButton(text);
        b->setObjectName(name);
        brow->addWidget(b);
        return b;
    };
    addRow_ = btn("variantAddRow", txt("ui.variables.row"));
    delRow_ = btn("variantDeleteRow", QString::fromUtf8(txt("ui.variables.row2")));
    paste_ = btn("variantPaste", txt("ui.common.paste"));
    paste_->setToolTip(txt("ui.variables.pasteATableFromThe"));
    importCsv_ = btn("variantImportCsv", txt("ui.variables.importCsv"));
    exportCsv_ = btn("variantExportCsv", txt("ui.variables.exportCsv"));
    v->addLayout(brow);

    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] { del_->setEnabled(table_->currentRow() >= 0 && !table_->selectedItems().isEmpty()); });
    connect(table_, &QTableWidget::currentCellChanged, this, [this] { loadProperties(); });
    connect(valuesEdit_, &QLineEdit::returnPressed, this, [this] { propertyEdited(true); });
    connect(helpEdit_, &QLineEdit::returnPressed, this, [this] { propertyEdited(false); });
    connect(del_, &QToolButton::clicked, this, [this] { deleteCurrent(); });
    connect(table_, &QTableWidget::cellChanged, this, &VariablesPanel::cellEdited);
    connect(add_, &QToolButton::clicked, this, [this] {
        const auto d = model_.session().document().document;
        int k = 1;
        auto taken = [&](const std::string& n) {
            for (const auto& x : d->variables)
                if (x.name == n) return true;
            return false;
        };
        while (taken("Var" + std::to_string(k))) ++k;
        // A new variable is dimensionless (FX7-UI-003): a unit only where the user types it (a length or an angle).
        auto r = model_.session().addVariable("Var" + std::to_string(k), "0");
        if (!r) {
            emit message(errorText(r.error()), true);
            return;
        }
        rebuild();
        const int row = rowOf(QString::fromStdString("Var" + std::to_string(k)));  // its alphabetical place (S7F-VAR-001)
        table_->setCurrentCell(row, 0);
        table_->scrollToItem(table_->item(row, 0));  // the new row may be below the visible part of the table
        table_->editItem(table_->item(row, 0));
    });
    connect(tableSelect_, &QComboBox::activated, this, [this](int i) {
        currentTable_ = tableSelect_->itemData(i).toString().toStdString();
        rebuildTables();
    });
    connect(activeVariant_, &QComboBox::activated, this, [this](int i) {
        if (rebuilding_ || currentTable_.empty()) return;
        if (auto s = model_.session().setActiveVariant(currentTable_, activeVariant_->itemText(i).toStdString()); !s) error(s.error());
    });
    connect(addTable_, &QToolButton::clicked, this, [this] {
        // Columns — the selected variables, otherwise all not yet in tables; one row "1" of their expressions (§8).
        const auto d = model_.session().document().document;
        std::set<std::string> rows;  // IDs of the selected rows' variables
        for (auto* it : table_->selectedItems())
            if (auto* n = table_->item(it->row(), ColName)) rows.insert(n->data(Qt::UserRole).toString().toStdString());
        api::Session::VariantTableDef def;
        std::vector<std::string> cols;
        doc::VariantRow row{"1", {}};
        for (int r = 0; r < int(d->variables.size()); ++r) {
            const auto& var = d->variables[size_t(r)];
            if (!rows.empty() ? !rows.count(var.id) : d->tableOfVariable(var.id) != nullptr) continue;
            cols.push_back(var.id);
            row.values.push_back(d->effectiveExpr(var));
        }
        def.columns = cols;
        def.variants = std::vector<doc::VariantRow>{row};
        auto r = model_.session().addVariantTable(def);
        if (!r) return error(r.error());
        currentTable_ = *r;
        rebuild();
    });
    connect(delTable_, &QToolButton::clicked, this, [this] {
        if (currentTable_.empty()) return;
        if (auto s = model_.session().deleteVariantTable(currentTable_); !s) return error(s.error());
        currentTable_.clear();
        rebuild();
    });
    connect(grid_, &QTableWidget::cellChanged, this, &VariablesPanel::gridEdited);
    connect(addRow_, &QPushButton::clicked, this, [this] {
        const auto d = model_.session().document().document;
        const doc::VariantTable* t = d->variantTable(currentTable_);
        if (!t) return;
        int k = 1;
        while (t->row(std::to_string(k))) ++k;
        auto rows = t->variants;
        doc::VariantRow nr = *t->row(t->active);
        nr.name = std::to_string(k);
        rows.push_back(nr);
        if (auto s = model_.session().updateVariantTable(currentTable_, {std::nullopt, std::nullopt, rows, std::nullopt}); !s) error(s.error());
        rebuild();
    });
    connect(delRow_, &QPushButton::clicked, this, [this] {
        const auto d = model_.session().document().document;
        const doc::VariantTable* t = d->variantTable(currentTable_);
        const int r = grid_->currentRow();
        if (!t || r < 0 || r >= int(t->variants.size()) || t->variants.size() < 2) return;
        auto rows = t->variants;
        rows.erase(rows.begin() + r);
        if (auto s = model_.session().updateVariantTable(currentTable_, {std::nullopt, std::nullopt, rows, std::nullopt}); !s) error(s.error());
        rebuild();
    });
    connect(paste_, &QPushButton::clicked, this, [this] { applyTableText(QApplication::clipboard()->text()); });
    connect(importCsv_, &QPushButton::clicked, this, [this] { importCsv(); });
    connect(exportCsv_, &QPushButton::clicked, this, [this] { exportCsv(); });
    rebuildTables();
}

std::vector<size_t> variableOrder(const std::vector<doc::Variable>& vars) {
    // Natural order: a run of digits compares by its number, other characters case-insensitively.
    auto natural = [](const QString& a, const QString& b) {
        int i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            if (a[i].isDigit() && b[j].isDigit()) {
                int ie = i, je = j;
                while (ie < a.size() && a[ie].isDigit()) ++ie;
                while (je < b.size() && b[je].isDigit()) ++je;
                QString na = a.mid(i, ie - i), nb = b.mid(j, je - j);
                while (na.size() > 1 && na[0] == '0') na.remove(0, 1);
                while (nb.size() > 1 && nb[0] == '0') nb.remove(0, 1);
                if (na.size() != nb.size()) return na.size() < nb.size() ? -1 : 1;
                if (const int c = na.compare(nb); c != 0) return c < 0 ? -1 : 1;
                i = ie, j = je;
                continue;
            }
            const QChar ca = a[i].toCaseFolded(), cb = b[j].toCaseFolded();
            if (ca != cb) return ca < cb ? -1 : 1;
            ++i, ++j;
        }
        return i < a.size() ? 1 : j < b.size() ? -1 : 0;
    };
    std::vector<size_t> out(vars.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = i;
    std::sort(out.begin(), out.end(), [&](size_t x, size_t y) {
        const QString a = QString::fromStdString(vars[x].name), b = QString::fromStdString(vars[y].name);
        if (const int c = natural(a, b); c != 0) return c < 0;
        if (const int c = a.compare(b); c != 0) return c < 0;
        return vars[x].id < vars[y].id;
    });
    return out;
}

int VariablesPanel::rowOf(const QString& name) const {
    for (int r = 0; r < table_->rowCount(); ++r)
        if (table_->item(r, 0) && table_->item(r, 0)->text() == name) return r;
    return -1;
}

void VariablesPanel::error(const construe::Error& e) {
    const auto d = model_.session().document().document;
    emit message(errorText(e, d.get()), true);
    QTimer::singleShot(0, this, [this] { rebuild(); });
}

void VariablesPanel::rebuild() {
    const auto d = model_.session().document().document;
    const auto units = model_.session().units();
    const auto vars = model_.session().variableValues();
    auto valueOf = [&](const doc::Variable& v, QTableWidgetItem* val) {
        const expr::VariableEntry* e = vars.find(v.name);
        val->setForeground(palette().color(QPalette::Text));
        QString tip = v.comment ? QString::fromStdString(*v.comment) : QString();
        if (e && e->isString()) {
            val->setText(QString::fromStdString(e->text));
        } else if (e && e->value.ok()) {
            val->setText(valueText(*e->value, units));
        } else if (e) {
            val->setText(QString::fromUtf8("ⓧ"));
            val->setForeground(COLOR_ERROR_TEXT.color);
            const QString err = errorText(e->value.error(), d.get());
            tip = tip.isEmpty() ? err : err + "\n" + tip;
        }
        val->setToolTip(tip);
    };
    // While a cell is being edited the rows are not recreated (otherwise the editor would close):
    // only the "Value" column is updated.
    bool editing = false;
    for (auto* ed : table_->viewport()->findChildren<QLineEdit*>())
        if (ed->isVisible()) editing = true;
    if (editing && table_->rowCount() == int(d->variables.size()) && order_.size() == d->variables.size()) {
        rebuilding_ = true;
        for (int r = 0; r < table_->rowCount(); ++r)
            if (auto* val = table_->item(r, ColValue)) valueOf(d->variables[order_[size_t(r)]], val);
        rebuilding_ = false;
        rebuildTables();
        return;
    }
    rebuilding_ = true;
    order_ = variableOrder(d->variables);  // always alphabetical (S7F-VAR-001)
    for (int r = int(d->variables.size()); r < table_->rowCount(); ++r)  // removed rows — without their widgets
        for (int c : {int(ColExpr), int(ColType)}) table_->removeCellWidget(r, c);
    table_->setRowCount(int(d->variables.size()));
    for (int r = 0; r < int(d->variables.size()); ++r) {
        const auto& v = d->variables[order_[size_t(r)]];
        const QString help = v.comment ? QString::fromStdString(*v.comment) : QString();
        auto* name = new QTableWidgetItem(QString::fromStdString(v.name));
        name->setData(Qt::UserRole, QString::fromStdString(v.id));
        QString nameTip = help;
        if (const doc::VariantTable* t = d->tableOfVariable(v.id)) {
            const QString from = text("ui.variant.fromTable", {{"name", QString::fromStdString(t->name)}});
            nameTip = nameTip.isEmpty() ? from : nameTip + "\n" + from;
        }
        name->setToolTip(nameTip);
        const std::string eff = d->effectiveExpr(v);
        auto* ex = new QTableWidgetItem(QString::fromStdString(eff));
        auto* val = new QTableWidgetItem;
        val->setFlags(val->flags() & ~Qt::ItemIsEditable);
        valueOf(v, val);
        auto* typeItem = new QTableWidgetItem;
        typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(r, ColName, name);
        table_->setItem(r, ColExpr, ex);
        table_->setItem(r, ColValue, val);
        table_->setItem(r, ColType, typeItem);
        // «Type» — a drop-down list (§8). Cell widgets are reused: a list being chosen in must not be deleted.
        const QString id = QString::fromStdString(v.id);
        auto* type = qobject_cast<QComboBox*>(table_->cellWidget(r, ColType));
        if (!type) {
            type = new QComboBox;
            type->setObjectName("variableType");
            for (const char* l : kTypeLabels) type->addItem(txt(l));
            connect(type, &QComboBox::activated, this, [this, type](int i) { typeChosen(type->property("variableId").toString().toStdString(), i); });
            table_->setCellWidget(r, ColType, type);
        }
        type->setProperty("variableId", id);
        type->setCurrentIndex(typeIndex(v.type));
        // List value — a drop-down list of the list values (§8, S6-PARAM-001).
        auto* lv = qobject_cast<QComboBox*>(table_->cellWidget(r, ColExpr));
        if (v.type == "list") {
            if (!lv) {
                lv = new QComboBox;
                lv->setObjectName("variableListValue");
                connect(lv, &QComboBox::activated, this, [this, lv](int i) {
                    const std::string vid = lv->property("variableId").toString().toStdString();
                    if (auto s = model_.session().updateVariable(vid, std::nullopt, lv->itemText(i).toStdString()); !s) error(s.error());
                });
                table_->setCellWidget(r, ColExpr, lv);
            }
            lv->setProperty("variableId", id);
            lv->clear();
            for (const auto& x : v.values) lv->addItem(QString::fromStdString(x));
            lv->setCurrentText(QString::fromStdString(eff));
        } else if (lv) {
            table_->removeCellWidget(r, ColExpr);
        }
    }
    rebuilding_ = false;
    loadProperties();
    rebuildTables();
}

std::string VariablesPanel::currentVariable() const {
    const int r = table_->currentRow();
    const auto* it = r >= 0 ? table_->item(r, ColName) : nullptr;
    return it ? it->data(Qt::UserRole).toString().toStdString() : std::string();
}

void VariablesPanel::loadProperties() {
    const auto d = model_.session().document().document;
    const doc::Variable* v = d->variable(currentVariable());
    propsTitle_->setText(v ? text("ui.variable.props", {{"name", QString::fromStdString(v->name)}}) : text("ui.variable.propsNone"));
    for (QLineEdit* e : {valuesEdit_, helpEdit_}) e->setEnabled(v != nullptr);
    if (!valuesEdit_->hasFocus()) valuesEdit_->setText(v ? joinValues(v->values) : QString());
    if (!helpEdit_->hasFocus()) helpEdit_->setText(v && v->comment ? QString::fromStdString(*v->comment) : QString());
}

void VariablesPanel::propertyEdited(bool values) {
    const std::string id = currentVariable();
    const auto d = model_.session().document().document;
    const doc::Variable* var = d->variable(id);
    if (!var) return;
    Status s = okStatus();
    if (values) {
        // The list values apply to a «List» variable; for another type they wait for the type to be chosen.
        if (var->type != "list") return;
        s = model_.session().setVariableType(id, "list", splitValues(valuesEdit_->text()));
    } else {
        const QString raw = helpEdit_->text();
        std::optional<std::string> c;
        if (!raw.trimmed().isEmpty()) c = raw.toStdString();
        s = model_.session().updateVariable(id, std::nullopt, std::nullopt, std::optional<std::optional<std::string>>(std::in_place, c));
    }
    if (!s) return error(s.error());
    QTimer::singleShot(0, this, [this] { rebuild(); });
}

void VariablesPanel::typeChosen(const std::string& id, int index) {
    const auto d = model_.session().document().document;
    const doc::Variable* v = d->variable(id);
    if (!v) return;
    const std::string type = kTypes[index];
    std::vector<std::string> values;
    if (type == "list") {
        if (currentVariable() == id) values = splitValues(valuesEdit_->text());
        if (values.empty()) values = {d->effectiveExpr(*v)};
    }
    if (auto s = model_.session().setVariableType(id, type, values); !s) return error(s.error());
    QTimer::singleShot(0, this, [this] { rebuild(); });
}

void VariablesPanel::cellEdited(int row, int col) {
    if (rebuilding_ || col == ColValue || col == ColType) return;
    const auto* nameItem = table_->item(row, ColName);
    if (!nameItem || !table_->item(row, col)) return;
    const std::string id = nameItem->data(Qt::UserRole).toString().toStdString();
    const auto d = model_.session().document().document;
    const doc::Variable* var = d->variable(id);
    if (!var) return;
    const QString raw = table_->item(row, col)->text();
    const QString t = raw.trimmed();
    Status s = okStatus();
    if (col == ColName) {
        // Used in formulas — the question «rename everywhere?» (app-ux §8, FX9-VAR-001): «Yes» — the variable and all
        // its references in one undo step; «No» — the renaming is cancelled.
        if (t.toStdString() != var->name && model_.session().variableReferenceCount(id) > 0) {
            QMessageBox box(QMessageBox::Question, "construe", text("ui.variable.renameEverywhere"), QMessageBox::Yes | QMessageBox::No, this);
            box.setObjectName("variableRenameQuestion");
            box.button(QMessageBox::Yes)->setText(txt("ui.variables.yes"));
            box.button(QMessageBox::No)->setText(txt("ui.variables.no"));
            box.setDefaultButton(QMessageBox::Yes);
            if (box.exec() != QMessageBox::Yes) {
                QTimer::singleShot(0, this, [this] { rebuild(); });
                return;
            }
            s = model_.session().renameVariableEverywhere(id, t.toStdString());
        } else {
            s = model_.session().updateVariable(id, t.toStdString(), std::nullopt);
        }
    } else if (col == ColExpr) {
        // A string is taken as is; an expression — normalized like every expression field (§8, §5.6).
        s = model_.session().updateVariable(id, std::nullopt, var->type == "string" ? raw.toStdString() : normalizeExprInput(t));
    }
    if (!s) {
        QString msg = s.error().code == "API_INVALID_NAME" ? text("ui.variable.badName") : errorText(s.error(), d.get());
        emit message(msg, true);
        QTimer::singleShot(0, this, [this] { rebuild(); });
        return;
    }
    if (col == ColName) {
        // Enter in "Name" moves to "Expression" (§8).
        QTimer::singleShot(0, this, [this, row, id] {
            rebuild();
            int at = row;  // the renamed variable takes its alphabetical place (S7F-VAR-001)
            for (int r = 0; r < table_->rowCount(); ++r)
                if (table_->item(r, ColName) && table_->item(r, ColName)->data(Qt::UserRole).toString().toStdString() == id) at = r;
            table_->setCurrentCell(at, ColExpr);
            if (!table_->cellWidget(at, ColExpr)) table_->editItem(table_->item(at, ColExpr));
        });
    }
}

void VariablesPanel::deleteCurrent() {
    const int row = table_->currentRow();
    if (row < 0 || !table_->item(row, 0)) return;
    const std::string id = table_->item(row, 0)->data(Qt::UserRole).toString().toStdString();
    // References are counted before deletion: after it their expressions would yield EXPR_UNKNOWN_NAME (§8).
    const auto docPtr = model_.session().document().document;
    std::string name;
    for (const auto& v : docPtr->variables)
        if (v.id == id) name = v.name;
    const int refs = model_.session().variableReferenceCount(id);
    auto s = model_.session().deleteVariable(id);
    if (!s) emit message(errorText(s.error()), true);
    else if (refs > 0)
        emit undoableDone(text("ui.variable.deletedReferenced", {{"name", QString::fromStdString(name)}, {"count", QString::number(refs)}}));
}

// ---------------------------------------------------------------- variant tables (§8, S6-PARAM-003)

void VariablesPanel::rebuildTables() {
    const auto d = model_.session().document().document;
    if (!d->variantTable(currentTable_)) currentTable_ = d->variantTables.empty() ? std::string() : d->variantTables.front().id;
    const doc::VariantTable* t = d->variantTable(currentTable_);
    rebuilding_ = true;
    tableSelect_->clear();
    for (const auto& x : d->variantTables) tableSelect_->addItem(QString::fromStdString(x.name), QString::fromStdString(x.id));
    tableSelect_->setCurrentIndex(tableSelect_->findData(QString::fromStdString(currentTable_)));
    activeVariant_->clear();
    const bool any = t != nullptr;
    for (QWidget* w : std::initializer_list<QWidget*>{tableSelect_, delTable_, activeVariant_, grid_, addRow_, exportCsv_}) w->setEnabled(any);
    delRow_->setEnabled(any && t->variants.size() > 1);
    // Rows are not recreated while a grid cell is being edited.
    bool editing = false;
    for (auto* ed : grid_->viewport()->findChildren<QLineEdit*>())
        if (ed->isVisible()) editing = true;
    if (!t) {
        grid_->setRowCount(0);
        grid_->setColumnCount(1);
        grid_->setHorizontalHeaderLabels({txt("ui.variables.variant")});
        rebuilding_ = false;
        return;
    }
    for (const auto& r : t->variants) activeVariant_->addItem(QString::fromStdString(r.name));
    activeVariant_->setCurrentText(QString::fromStdString(t->active));
    if (editing && grid_->rowCount() == int(t->variants.size()) && grid_->columnCount() == int(t->columns.size()) + 1) {
        rebuilding_ = false;
        return;
    }
    grid_->setColumnCount(int(t->columns.size()) + 1);
    grid_->setRowCount(int(t->variants.size()));
    auto* h0 = new QTableWidgetItem(txt("ui.variables.variant"));
    grid_->setHorizontalHeaderItem(0, h0);
    for (size_t c = 0; c < t->columns.size(); ++c) {
        const doc::Variable* v = d->variable(t->columns[c]);
        auto* h = new QTableWidgetItem(v ? QString::fromStdString(v->name) : QString::fromStdString(t->columns[c]));
        if (v && v->comment) h->setToolTip(QString::fromStdString(*v->comment));
        grid_->setHorizontalHeaderItem(int(c) + 1, h);
    }
    for (size_t r = 0; r < t->variants.size(); ++r) {
        grid_->setItem(int(r), 0, new QTableWidgetItem(QString::fromStdString(t->variants[r].name)));
        for (size_t c = 0; c < t->columns.size(); ++c)
            grid_->setItem(int(r), int(c) + 1, new QTableWidgetItem(QString::fromStdString(t->variants[r].values[c])));
    }
    rebuilding_ = false;
}

void VariablesPanel::gridEdited(int row, int col) {
    if (rebuilding_) return;
    const auto d = model_.session().document().document;
    const doc::VariantTable* t = d->variantTable(currentTable_);
    if (!t || row >= int(t->variants.size()) || !grid_->item(row, col)) return;
    auto rows = t->variants;
    std::optional<std::string> active;
    const std::string v = grid_->item(row, col)->text().trimmed().toStdString();
    if (col == 0) {
        if (rows[size_t(row)].name == t->active) active = v;  // renaming the active row keeps it active
        rows[size_t(row)].name = v;
    } else {
        rows[size_t(row)].values[size_t(col - 1)] = v;
    }
    if (auto s = model_.session().updateVariantTable(currentTable_, {std::nullopt, std::nullopt, rows, active}); !s) error(s.error());
}

void VariablesPanel::applyTableText(const QString& text) {
    auto& s = model_.session();
    auto p = s.parseVariantTableText(text.toStdString());
    if (!p) return error(p.error());
    if (s.document().document->variantTable(currentTable_)) {
        if (auto r = s.updateVariantTable(currentTable_, {std::nullopt, p->columns, p->variants, std::nullopt}); !r) return error(r.error());
    } else {
        auto r = s.addVariantTable({std::nullopt, p->columns, p->variants, std::nullopt});
        if (!r) return error(r.error());
        currentTable_ = *r;
    }
    rebuild();
}

void VariablesPanel::importCsv() {
    const QFileDialog::Options opt = testMode_ ? QFileDialog::DontUseNativeDialog : QFileDialog::Options();
    const QString path = QFileDialog::getOpenFileName(this, txt("ui.variables.importVariantTable"), {}, "CSV (*.csv)", nullptr, opt);
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return error(Error("IO_READ").with("file", path.toStdString()));
    QByteArray bytes = f.readAll();
    if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);  // BOM (§8)
    applyTableText(QString::fromUtf8(bytes));
}

void VariablesPanel::exportCsv() {
    if (currentTable_.empty()) return;
    auto t = model_.session().variantTableText(currentTable_, ',');
    if (!t) return error(t.error());
    const QFileDialog::Options opt = testMode_ ? (QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite) : QFileDialog::Options();
    QString path = QFileDialog::getSaveFileName(this, txt("ui.variables.exportVariantTable"), tableSelect_->currentText() + ".csv", "CSV (*.csv)", nullptr, opt);
    if (path.isEmpty()) return;
    if (!path.endsWith(".csv")) path += ".csv";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QByteArray::fromStdString(*t)) < 0)
        return error(Error("IO_WRITE").with("file", path.toStdString()));
}

bool VariablesPanel::eventFilter(QObject* o, QEvent* e) {
    if (o == table_ && e->type() == QEvent::KeyPress && isDeleteKey(static_cast<QKeyEvent*>(e))) {
        deleteCurrent();
        return true;
    }
    return QWidget::eventFilter(o, e);
}

}  // namespace construe::app
