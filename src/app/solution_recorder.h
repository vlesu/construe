// Recording a solution (spec/app-ux.md §8е, solutions.md §8а; S6-SOL-009): inputs picked on the example assembly,
// tables, formulas and row choices, cuts by a tool in the connection frame and library parts; the description and the
// plan on the example after every change. Saving belongs to the main window.
#pragma once

#include "api/session.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace construe::app {

class SolutionRecorder : public QWidget {
    Q_OBJECT
public:
    explicit SolutionRecorder(api::Session& session, QWidget* parent = nullptr);
    void reset();  // an empty recording
    // The recording (id from the title, solutions.md §8а.1) and its description; nullopt — not built (the error shown).
    solutions::Recording recording() const;
    const std::optional<api::Json>& definition() const { return definition_; }
    bool canSave() const;
    QString title() const;
    QString folder() const;  // the title without characters unsafe in file names
    // A face picked in the view (as in the connection wizard): the selected input gets its example.
    void picked(const std::string& ref, std::optional<kernel::SurfaceInfo::Kind> kind, const std::optional<std::array<double, 3>>& at);
    std::vector<std::string> chosenFaces() const;
    void setMessage(const QString& m);
    void setRefLabeler(std::function<QString(const std::string&)> f) { labeler_ = std::move(f); }

signals:
    void accepted();
    void cancelled();
    void changed();  // the description, the plan or the examples changed (availability of ✓, highlighting)

private:
    struct Table {
        solutions::TableDef def;
        QString title;
        std::vector<std::string> columns;
    };
    void addInput();
    void addTable();
    void addRule();
    void addOutput();
    void syncFields();   // field visibility by kinds, combos of tables, columns, parts, rows
    void fillLists();
    void rebuild();      // description, plan, result
    const Table* table(const std::string& id) const;

    api::Session& s_;
    std::vector<solutions::InputDef> inputs_;
    api::Json examples_ = api::Json::object();
    std::vector<Table> tables_;
    std::vector<solutions::RecordedRule> rules_;
    std::vector<solutions::RecordedOutput> outputs_;
    std::optional<api::Json> definition_;
    std::optional<api::Session::SolutionPlan> plan_;
    std::function<QString(const std::string&)> labeler_;
    bool syncing_ = false;

    QLineEdit* title_ = nullptr;
    QLineEdit* section_ = nullptr;
    QLineEdit* description_ = nullptr;
    QListWidget* inputsList_ = nullptr;
    QComboBox* inputKind_ = nullptr;
    QLineEdit* inputTitle_ = nullptr;
    QCheckBox* pack_ = nullptr;
    QLineEdit* probe_ = nullptr;
    QListWidget* tablesList_ = nullptr;
    QComboBox* tableSource_ = nullptr;
    QListWidget* rulesList_ = nullptr;
    QLabel* names_ = nullptr;
    QComboBox* ruleKind_ = nullptr;
    QLineEdit* ruleName_ = nullptr;
    QLineEdit* ruleExpr_ = nullptr;
    QComboBox* ruleTable_ = nullptr;
    QComboBox* ruleColumn_ = nullptr;
    QComboBox* ruleOp_ = nullptr;
    QComboBox* ruleOrder_ = nullptr;
    std::vector<QWidget*> selectFields_;
    QFormLayout* ruleForm_ = nullptr;
    QListWidget* outputsList_ = nullptr;
    QComboBox* outputKind_ = nullptr;
    QLineEdit* outputTitle_ = nullptr;
    QComboBox* outputParts_ = nullptr;
    QPlainTextEdit* outputGeometry_ = nullptr;
    QComboBox* outputTable_ = nullptr;
    QComboBox* outputRow_ = nullptr;
    QLineEdit* outputZ_ = nullptr;
    QComboBox* outputDirection_ = nullptr;
    std::vector<QWidget*> cutFields_, partFields_;
    QFormLayout* outputForm_ = nullptr;
    QLabel* result_ = nullptr;
    QLabel* message_ = nullptr;
    QPushButton* save_ = nullptr;
};

}  // namespace construe::app
