// Document variables panel (spec/app-ux.md §8): variables with types and help, variant tables (S6-PARAM-001…003).
#pragma once

#include "base/error.h"
#include "doc/document.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QPushButton>
#include <QTableWidget>
#include <QWidget>

#include <string>
#include <vector>

namespace construe::app {

class AppModel;

// Order of the variables in the panel (S7F-VAR-001): by name alphabetically, case-insensitive, digit runs by their
// number (Var2 before Var10); equal — case-sensitive, then by ID. Returns indices into vars.
std::vector<size_t> variableOrder(const std::vector<doc::Variable>& vars);

class VariablesPanel : public QWidget {
    Q_OBJECT
public:
    explicit VariablesPanel(AppModel& model, QWidget* parent = nullptr);
    void rebuild();
    QTableWidget* table() const { return table_; }
    QToolButton* addButton() const { return add_; }
    QToolButton* deleteButton() const { return del_; }
    int rowOf(const QString& name) const;
    // Variant tables (§8, S6-PARAM-003).
    QComboBox* tableSelect() const { return tableSelect_; }
    QComboBox* activeVariant() const { return activeVariant_; }
    QTableWidget* variantGrid() const { return grid_; }
    const std::string& currentTable() const { return currentTable_; }
    // File dialogs without the native ones (e2e, as MainWindow in test mode).
    void setTestMode(bool on) { testMode_ = on; }

    // Table columns (§8).
    enum Column { ColName = 0, ColExpr, ColValue, ColType, ColCount };
    // Properties of the current variable under the table (§8, FX7-UI-004): the list values and the help.
    QLineEdit* valuesEdit() const { return valuesEdit_; }
    QLineEdit* helpEdit() const { return helpEdit_; }

signals:
    void message(const QString& text, bool error);
    void undoableDone(const QString& text);  // notification with an "Undo" action (§8)

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void cellEdited(int row, int col);
    void deleteCurrent();  // Del and "−" (§8)
    void loadProperties();                      // the current row → the properties box (FX7-UI-004)
    void propertyEdited(bool values);           // Enter in «Values» or «Help»
    std::string currentVariable() const;        // ID of the variable of the current row, empty — none
    void typeChosen(const std::string& id, int index);
    void rebuildTables();
    void gridEdited(int row, int col);
    void applyTableText(const QString& text);  // paste / CSV import (§8)
    void exportCsv();
    void importCsv();
    void error(const construe::Error& e);

    AppModel& model_;
    QTableWidget* table_;
    QToolButton* add_;
    QToolButton* del_;
    QComboBox* tableSelect_;
    QToolButton* addTable_;
    QToolButton* delTable_;
    QComboBox* activeVariant_;
    QTableWidget* grid_;
    QLabel* propsTitle_;
    QLineEdit* valuesEdit_;
    QLineEdit* helpEdit_;
    QPushButton *addRow_, *delRow_, *paste_, *importCsv_, *exportCsv_;
    std::string currentTable_;
    bool rebuilding_ = false;
    std::vector<size_t> order_;  // table row → index in the document's variables (S7F-VAR-001)
    bool testMode_ = false;
};

}  // namespace construe::app
