// Recording a solution (app/solution_recorder.h, spec/app-ux.md §8е).
#include "app/solution_recorder.h"

#include "app/messages.h"
#include "app/solution_wizard.h"
#include "app/theme.h"

#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

namespace construe::app {

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

QString number(double v) { return QString::number(v, 'g', 6); }

QPushButton* button(const QString& label, const char* name) {
    auto* b = new QPushButton(label);
    b->setObjectName(name);
    return b;
}

QListWidget* list(const char* name, int height = 80) {
    auto* l = new QListWidget;
    l->setObjectName(name);
    l->setMaximumHeight(height);
    return l;
}

// The smallest free id <prefix><n>, n from 1 (app-ux §8е).
template <class C, class F>
std::string freeId(const std::string& prefix, const C& items, F idOf) {
    for (int n = 1;; ++n) {
        const std::string id = prefix + std::to_string(n);
        if (std::none_of(items.begin(), items.end(), [&](const auto& x) { return idOf(x) == id; })) return id;
    }
}

const char* HOLES_SOURCE = "builtin:holes";

}  // namespace

SolutionRecorder::SolutionRecorder(api::Session& session, QWidget* parent) : QWidget(parent), s_(session) {
    setObjectName("solutionRecorder");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setObjectName("recScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* body = new QWidget;
    scroll->setWidget(body);
    auto* v = new QVBoxLayout(body);
    v->addWidget(new QLabel("<b>" + text("ui.record.title") + "</b>"));
    // The solution.
    auto* head = new QFormLayout;
    title_ = new QLineEdit;
    title_->setObjectName("recTitle");
    section_ = new QLineEdit;
    section_->setObjectName("recSection");
    description_ = new QLineEdit;
    description_->setObjectName("recDescription");
    head->addRow(txt("ui.common.name"), title_);
    head->addRow(txt("ui.common.section2"), section_);
    head->addRow(txt("ui.record.description"), description_);
    v->addLayout(head);
    // Inputs.
    v->addWidget(new QLabel(text("ui.record.inputs")));
    inputsList_ = list("recInputs");
    v->addWidget(inputsList_);
    auto* inForm = new QFormLayout;
    inputKind_ = new QComboBox;
    inputKind_->setObjectName("recInputKind");
    inputKind_->addItem(txt("ui.record.planarFace"), "face");
    inputKind_->addItem(txt("ui.record.cylindricalFace"), "cylinder");
    inputKind_->addItem(txt("ui.record.pointOnFace"), "point");
    inputTitle_ = new QLineEdit;
    inputTitle_->setObjectName("recInputTitle");
    inForm->addRow(txt("ui.record.inputType"), inputKind_);
    inForm->addRow(txt("ui.record.inputName"), inputTitle_);
    v->addLayout(inForm);
    auto* inButtons = new QHBoxLayout;
    auto* addInputB = button(txt("ui.record.addInput"), "recAddInput");
    auto* removeInputB = button(txt("ui.record.removeInput"), "recRemoveInput");
    inButtons->addWidget(addInputB);
    inButtons->addWidget(removeInputB);
    inButtons->addStretch(1);
    v->addLayout(inButtons);
    auto* packRow = new QHBoxLayout;
    pack_ = new QCheckBox(txt("ui.record.measureThePartStackProbe"));
    pack_->setObjectName("recPack");
    probe_ = new QLineEdit("1 mm");
    probe_->setObjectName("recProbe");
    packRow->addWidget(pack_);
    packRow->addWidget(probe_, 1);
    v->addLayout(packRow);
    // Tables.
    v->addWidget(new QLabel(text("ui.record.tables")));
    tablesList_ = list("recTables", 60);
    v->addWidget(tablesList_);
    auto* tRow = new QHBoxLayout;
    tableSource_ = new QComboBox;
    tableSource_->setObjectName("recTableSource");
    auto* addTableB = button(txt("ui.record.addTable"), "recAddTable");
    auto* removeTableB = button(txt("ui.record.removeTable"), "recRemoveTable");
    tRow->addWidget(tableSource_, 1);
    tRow->addWidget(addTableB);
    tRow->addWidget(removeTableB);
    v->addLayout(tRow);
    // Rules.
    v->addWidget(new QLabel(text("ui.record.rules")));
    rulesList_ = list("recRules");
    v->addWidget(rulesList_);
    names_ = new QLabel;
    names_->setObjectName("recNames");
    names_->setWordWrap(true);
    names_->setTextFormat(Qt::PlainText);
    v->addWidget(names_);
    ruleForm_ = new QFormLayout;
    ruleKind_ = new QComboBox;
    ruleKind_->setObjectName("recRuleKind");
    ruleKind_->addItem(txt("ui.record.formula"), "let");
    ruleKind_->addItem(txt("ui.record.tableRowLookup"), "select");
    ruleName_ = new QLineEdit;
    ruleName_->setObjectName("recRuleName");
    ruleTable_ = new QComboBox;
    ruleTable_->setObjectName("recRuleTable");
    ruleColumn_ = new QComboBox;
    ruleColumn_->setObjectName("recRuleColumn");
    ruleOp_ = new QComboBox;
    ruleOp_->setObjectName("recRuleOp");
    for (const char* op : {"=", "!=", "<", "<=", ">", ">="}) ruleOp_->addItem(op, op);
    ruleExpr_ = new QLineEdit;
    ruleExpr_->setObjectName("recRuleExpr");
    ruleOrder_ = new QComboBox;
    ruleOrder_->setObjectName("recRuleOrder");
    ruleOrder_->addItem(txt("ui.record.asInTable"), "");
    ruleOrder_->addItem(txt("ui.record.ascending"), "asc");
    ruleOrder_->addItem(txt("ui.record.descending"), "desc");
    ruleForm_->addRow(txt("ui.record.ruleType"), ruleKind_);
    ruleForm_->addRow(txt("ui.common.name3"), ruleName_);
    ruleForm_->addRow(txt("ui.record.table"), ruleTable_);
    ruleForm_->addRow(txt("ui.record.column"), ruleColumn_);
    ruleForm_->addRow(txt("ui.record.condition"), ruleOp_);
    ruleForm_->addRow(txt("ui.common.expression"), ruleExpr_);
    ruleForm_->addRow(txt("ui.record.order"), ruleOrder_);
    selectFields_ = {ruleTable_, ruleColumn_, ruleOp_, ruleOrder_};
    v->addLayout(ruleForm_);
    auto* rButtons = new QHBoxLayout;
    auto* addRuleB = button(txt("ui.record.addRule"), "recAddRule");
    auto* removeRuleB = button(txt("ui.record.removeRule"), "recRemoveRule");
    rButtons->addWidget(addRuleB);
    rButtons->addWidget(removeRuleB);
    rButtons->addStretch(1);
    v->addLayout(rButtons);
    // Outputs.
    v->addWidget(new QLabel(text("ui.record.outputs")));
    outputsList_ = list("recOutputs");
    v->addWidget(outputsList_);
    outputForm_ = new QFormLayout;
    outputKind_ = new QComboBox;
    outputKind_->setObjectName("recOutputKind");
    outputKind_->addItem(txt("ui.record.cutByRevolvedBody"), "revolve");
    outputKind_->addItem(txt("ui.record.cutByBlock"), "box");
    outputKind_->addItem(txt("ui.record.libraryPart"), "part");
    outputTitle_ = new QLineEdit;
    outputTitle_->setObjectName("recOutputTitle");
    outputParts_ = new QComboBox;
    outputParts_->setObjectName("recOutputParts");
    outputGeometry_ = new QPlainTextEdit;
    outputGeometry_->setObjectName("recOutputGeometry");
    outputGeometry_->setMaximumHeight(90);
    outputGeometry_->setPlaceholderText(txt("ui.record.rZOneLinePer"));
    outputTable_ = new QComboBox;
    outputTable_->setObjectName("recOutputTable");
    outputRow_ = new QComboBox;
    outputRow_->setObjectName("recOutputRow");
    outputZ_ = new QLineEdit("0 mm");
    outputZ_->setObjectName("recOutputZ");
    outputDirection_ = new QComboBox;
    outputDirection_->setObjectName("recOutputDirection");
    outputDirection_->addItem(txt("ui.record.intoMaterial"), "in");
    outputDirection_->addItem(txt("ui.record.outOfMaterial"), "out");
    outputForm_->addRow(txt("ui.record.outputType"), outputKind_);
    outputForm_->addRow(txt("ui.record.outputName"), outputTitle_);
    outputForm_->addRow(txt("ui.record.parts"), outputParts_);
    outputForm_->addRow(txt("ui.common.profile"), outputGeometry_);
    outputForm_->addRow(txt("ui.record.partTable"), outputTable_);
    outputForm_->addRow(txt("ui.record.row"), outputRow_);
    outputForm_->addRow(txt("ui.record.axialOffset"), outputZ_);
    outputForm_->addRow(txt("ui.common.direction"), outputDirection_);
    cutFields_ = {outputParts_, outputGeometry_};
    partFields_ = {outputTable_, outputRow_, outputZ_, outputDirection_};
    v->addLayout(outputForm_);
    auto* oButtons = new QHBoxLayout;
    auto* addOutputB = button(txt("ui.record.addOutput"), "recAddOutput");
    auto* removeOutputB = button(txt("ui.record.removeOutput"), "recRemoveOutput");
    oButtons->addWidget(addOutputB);
    oButtons->addWidget(removeOutputB);
    oButtons->addStretch(1);
    v->addLayout(oButtons);
    // Result.
    result_ = new QLabel;
    result_->setObjectName("recResult");
    result_->setWordWrap(true);
    result_->setTextFormat(Qt::PlainText);
    v->addWidget(result_);
    message_ = new QLabel;
    message_->setObjectName("recMessage");
    message_->setWordWrap(true);
    message_->setTextFormat(Qt::PlainText);
    message_->setStyleSheet(QString("color: %1").arg(COLOR_ERROR_TEXT.color.name()));
    v->addWidget(message_);
    v->addStretch(1);
    auto* buttons = new QHBoxLayout;
    save_ = button(txt("ui.record.saveAsSolution"), "recSave");
    auto* cancel = button(txt("ui.common.cancel"), "recCancel");
    buttons->addWidget(save_);
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    outer->addLayout(buttons);

    connect(addInputB, &QPushButton::clicked, this, [this] { addInput(); });
    connect(removeInputB, &QPushButton::clicked, this, [this] {
        const int r = inputsList_->currentRow();
        if (r < 0 || r >= int(inputs_.size())) return setMessage(text("ui.record.needInput"));
        examples_.erase(inputs_[size_t(r)].id);
        inputs_.erase(inputs_.begin() + r);
        rebuild();
    });
    connect(pack_, &QCheckBox::clicked, this, [this] { rebuild(); });
    connect(probe_, &QLineEdit::editingFinished, this, [this] {
        if (pack_->isChecked()) rebuild();
    });
    connect(addTableB, &QPushButton::clicked, this, [this] { addTable(); });
    connect(removeTableB, &QPushButton::clicked, this, [this] {
        const int r = tablesList_->currentRow();
        if (r < 0 || r >= int(tables_.size())) return;
        tables_.erase(tables_.begin() + r);
        rebuild();
    });
    connect(ruleKind_, &QComboBox::currentIndexChanged, this, [this] { syncFields(); });
    connect(ruleTable_, &QComboBox::currentIndexChanged, this, [this] {
        if (syncing_) return;
        ruleColumn_->clear();
        if (const Table* t = table(ruleTable_->currentData().toString().toStdString()))
            for (const auto& c : t->columns) ruleColumn_->addItem(q(c), q(c));
    });
    connect(addRuleB, &QPushButton::clicked, this, [this] { addRule(); });
    connect(removeRuleB, &QPushButton::clicked, this, [this] {
        const int r = rulesList_->currentRow();
        if (r < 0 || r >= int(rules_.size())) return;
        rules_.erase(rules_.begin() + r);
        rebuild();
    });
    connect(outputKind_, &QComboBox::currentIndexChanged, this, [this] { syncFields(); });
    connect(addOutputB, &QPushButton::clicked, this, [this] { addOutput(); });
    connect(removeOutputB, &QPushButton::clicked, this, [this] {
        const int r = outputsList_->currentRow();
        if (r < 0 || r >= int(outputs_.size())) return;
        outputs_.erase(outputs_.begin() + r);
        rebuild();
    });
    for (QLineEdit* e : {title_, section_, description_})
        connect(e, &QLineEdit::editingFinished, this, [this] { rebuild(); });
    connect(save_, &QPushButton::clicked, this, [this] {
        if (canSave()) emit accepted();
    });
    connect(cancel, &QPushButton::clicked, this, &SolutionRecorder::cancelled);
}

void SolutionRecorder::reset() {
    inputs_.clear();
    examples_ = api::Json::object();
    tables_.clear();
    rules_.clear();
    outputs_.clear();
    title_->setText(txt("ui.record.mySolution"));
    section_->setText(txt("ui.record.mySolutions"));
    description_->clear();
    inputTitle_->clear();
    pack_->setChecked(false);
    probe_->setText("1 mm");
    ruleName_->clear();
    ruleExpr_->clear();
    outputTitle_->clear();
    outputGeometry_->clear();
    outputZ_->setText("0 mm");
    message_->clear();
    // Table sources: effective library elements by title, then the built-in hole table.
    tableSource_->clear();
    std::vector<std::pair<QString, QString>> items;
    for (const auto& it : s_.libraryCatalog()->items)
        if (it.shadowedBy.empty()) items.emplace_back(q(it.package.description.title), q(it.package.description.id));
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [title, id] : items) tableSource_->addItem(title, id);
    tableSource_->addItem(txt("ui.record.holeTableBuiltIn"), HOLES_SOURCE);
    rebuild();
}

QString SolutionRecorder::title() const { return title_->text().trimmed(); }

QString SolutionRecorder::folder() const {
    QString f = title();
    for (const QChar c : QString("/\\:*?\"<>|")) f.replace(c, '_');
    return f.isEmpty() ? q(recording().id) : f;
}

const SolutionRecorder::Table* SolutionRecorder::table(const std::string& id) const {
    for (const auto& t : tables_)
        if (t.def.id == id) return &t;
    return nullptr;
}

solutions::Recording SolutionRecorder::recording() const {
    solutions::Recording r;
    r.title = title().toStdString();
    // Solution id: «user-sol-» + hash of the title (deterministic, without typing by the engineer).
    r.id = "user-sol-" + QCryptographicHash::hash(title().toUtf8(), QCryptographicHash::Sha1).toHex().left(16).toStdString();
    r.description = description_->text().trimmed().toStdString();
    for (const auto& part : section_->text().split('/', Qt::SkipEmptyParts))
        if (!part.trimmed().isEmpty()) r.section.push_back(part.trimmed().toStdString());
    if (r.section.empty()) r.section.push_back(txt("ui.record.mySolutions"));  // not the module's Russian default
    r.inputs = inputs_;
    if (pack_->isChecked()) r.probe = probe_->text().trimmed().toStdString();
    for (const auto& t : tables_) r.tables.push_back(t.def);
    r.rules = rules_;
    r.outputs = outputs_;
    return r;
}

void SolutionRecorder::addInput() {
    solutions::InputDef in;
    in.type = inputKind_->currentData().toString().toStdString();
    if (in.type == "point" && std::none_of(inputs_.begin(), inputs_.end(), [](const auto& x) { return x.type == "face"; }))
        return setMessage(text("ui.record.needFace"));
    const std::string prefix = in.type == "face" ? "face" : in.type == "cylinder" ? "cyl" : "point";
    in.id = freeId(prefix, inputs_, [](const solutions::InputDef& x) { return x.id; });
    const QString t = inputTitle_->text().trimmed();
    in.title = !t.isEmpty() ? t.toStdString() : in.type == "face" ? txt("ui.common.face") : in.type == "cylinder" ? txt("ui.record.cylinder") : txt("ui.common.point2");
    inputs_.push_back(in);
    inputTitle_->clear();
    rebuild();
    inputsList_->setCurrentRow(int(inputs_.size()) - 1);
}

void SolutionRecorder::addTable() {
    const std::string source = tableSource_->currentData().toString().toStdString();
    if (source.empty()) return;
    Table t;
    if (source == HOLES_SOURCE) {
        if (table("holes")) return;
        t.def.id = "holes";
        t.def.builtin = "holes";
        t.title = tableSource_->currentText();
        t.columns = solutions::holesTable(api::Json::object()).columns;
    } else {
        const library::CatalogItem* it = s_.libraryCatalog()->find(source);
        if (!it) return;
        t.def.id = freeId("t", tables_, [](const Table& x) { return x.def.id; });
        t.def.library = source;
        t.title = q(it->package.description.title);
        t.columns = it->package.sizes.columns;
    }
    tables_.push_back(std::move(t));
    rebuild();
}

void SolutionRecorder::addRule() {
    solutions::RecordedRule r;
    r.kind = ruleKind_->currentData().toString().toStdString();
    r.name = ruleName_->text().trimmed().toStdString();
    r.expr = ruleExpr_->text().trimmed().toStdString();
    if (r.kind == "select") {
        r.table = ruleTable_->currentData().toString().toStdString();
        r.column = ruleColumn_->currentData().toString().toStdString();
        r.op = ruleOp_->currentData().toString().toStdString();
        r.order = ruleOrder_->currentData().toString().toStdString();
    }
    if (r.name.empty() || r.expr.empty() || (r.kind == "select" && (r.table.empty() || r.column.empty())))
        return setMessage(text("ui.record.needRule"));
    rules_.push_back(r);
    ruleName_->clear();
    ruleExpr_->clear();
    rebuild();
}

void SolutionRecorder::addOutput() {
    solutions::RecordedOutput o;
    const std::string kind = outputKind_->currentData().toString().toStdString();
    o.kind = kind == "part" ? "part" : "cut";
    const QString t = outputTitle_->text().trimmed();
    o.title = !t.isEmpty() ? t.toStdString() : o.kind == "part" ? txt("ui.common.part") : txt("ui.common.cut");
    if (o.kind == "cut") {
        o.tool = kind;
        o.parts = outputParts_->currentData().toString().toStdString();
        o.geometry = outputGeometry_->toPlainText().toStdString();
        if (o.parts.empty() || outputGeometry_->toPlainText().trimmed().isEmpty()) return setMessage(text("ui.record.needOutput"));
    } else {
        o.table = outputTable_->currentData().toString().toStdString();
        o.row = outputRow_->currentData().toString().toStdString();
        o.z = outputZ_->text().trimmed().toStdString();
        o.direction = outputDirection_->currentData().toString().toStdString();
        if (o.table.empty() || o.row.empty() || o.z.empty()) return setMessage(text("ui.record.needOutput"));
    }
    outputs_.push_back(o);
    outputTitle_->clear();
    outputGeometry_->clear();
    rebuild();
}

void SolutionRecorder::syncFields() {
    const bool select = ruleKind_->currentData().toString() == "select";
    for (QWidget* w : selectFields_) ruleForm_->setRowVisible(w, select);
    const bool part = outputKind_->currentData().toString() == "part";
    for (QWidget* w : cutFields_) outputForm_->setRowVisible(w, !part);
    for (QWidget* w : partFields_) outputForm_->setRowVisible(w, part);
    outputGeometry_->setPlaceholderText(outputKind_->currentData().toString() == "box" ? txt("ui.record.fromToLinesXY") : txt("ui.record.rZOneLinePer"));
    // Combos built from the recording, keeping the current choice.
    auto refill = [](QComboBox* c, const std::vector<std::pair<QString, QString>>& items) {
        const QString keep = c->currentData().toString();
        c->clear();
        for (const auto& [label, value] : items) c->addItem(label, value);
        const int i = c->findData(keep);
        if (i >= 0) c->setCurrentIndex(i);
    };
    syncing_ = true;
    std::vector<std::pair<QString, QString>> tables, libTables, parts, rows;
    for (const auto& t : tables_) {
        tables.emplace_back(q(t.def.id) + " — " + t.title, q(t.def.id));
        if (!t.def.library.empty()) libTables.emplace_back(q(t.def.id) + " — " + t.title, q(t.def.id));
    }
    for (const auto& in : inputs_)
        if (in.type != "point") parts.emplace_back(txt("ui.record.inputPart") + q(in.id), q(in.id));
    if (pack_->isChecked()) {
        parts.emplace_back(txt("ui.record.allPartsOfTheStack"), "pack");
        parts.emplace_back(txt("ui.record.firstPartOfTheStack"), "packFirst");
        parts.emplace_back(txt("ui.record.lastPartOfTheStack"), "packLast");
        parts.emplace_back(txt("ui.record.allButTheLast"), "packButLast");
    }
    for (const auto& r : rules_)
        if (r.kind == "select" && std::none_of(rows.begin(), rows.end(), [&](const auto& x) { return x.second == q(r.name); }))
            rows.emplace_back(q(r.name), q(r.name));
    const QString keepTable = ruleTable_->currentData().toString();
    refill(ruleTable_, tables);
    refill(outputTable_, libTables);
    refill(outputParts_, parts);
    refill(outputRow_, rows);
    syncing_ = false;
    if (ruleTable_->currentData().toString() != keepTable || ruleColumn_->count() == 0) {
        const QString keepColumn = ruleColumn_->currentData().toString();
        ruleColumn_->clear();
        if (const Table* t = table(ruleTable_->currentData().toString().toStdString()))
            for (const auto& c : t->columns) ruleColumn_->addItem(q(c), q(c));
        const int i = ruleColumn_->findData(keepColumn);
        if (i >= 0) ruleColumn_->setCurrentIndex(i);
    }
}

void SolutionRecorder::fillLists() {
    const int keepIn = inputsList_->currentRow();
    inputsList_->clear();
    for (const auto& in : inputs_) {
        QString value = text("ui.solution.notSet");
        if (examples_.contains(in.id)) {
            const auto& j = examples_[in.id];
            if (j.is_array() && j.size() == 3)
                value = text("ui.solution.point", {{"x", number(j[0].get<double>())}, {"y", number(j[1].get<double>())}, {"z", number(j[2].get<double>())}});
            else if (j.is_string())
                value = labeler_ ? labeler_(j.get<std::string>()) : q(j.get<std::string>());
        }
        inputsList_->addItem(q(in.id) + " — " + q(in.title) + ": " + value);
    }
    inputsList_->setCurrentRow(keepIn >= 0 && keepIn < inputsList_->count() ? keepIn : inputsList_->count() - 1);
    tablesList_->clear();
    for (const auto& t : tables_) {
        QStringList cols;
        for (const auto& c : t.columns) cols << q(c);
        tablesList_->addItem(q(t.def.id) + " — " + t.title + ": " + cols.join(", "));
    }
    rulesList_->clear();
    for (const auto& r : rules_) {
        if (r.kind == "let") {
            rulesList_->addItem(q(r.name) + " = " + q(r.expr));
            continue;
        }
        QString line = q(r.name) + txt("ui.record.row2") + q(r.table) + txt("ui.record.where") + q(r.column) + " " + q(r.op) + " " + q(r.expr);
        if (r.order == "asc") line += txt("ui.record.ascending2");
        if (r.order == "desc") line += txt("ui.record.descending2");
        rulesList_->addItem(line);
    }
    outputsList_->clear();
    for (const auto& o : outputs_)
        outputsList_->addItem(o.kind == "cut" ? q(o.title) + txt("ui.record.cutIn") + q(o.parts) : q(o.title) + txt("ui.record.part") + q(o.table) + txt("ui.record.row3") + q(o.row));
    // Names for expressions (app-ux §8е).
    QStringList names;
    for (const auto& in : inputs_) {
        if (in.type == "cylinder") names << q(in.id) + ".diameter" << q(in.id) + ".length";
        if (in.type == "face") names << q(in.id) + ".depth";
    }
    if (pack_->isChecked()) names << "pack.thickness" << "pack.count" << "pack.firstThickness" << "pack.lastThickness" << "pack.lastStart";
    for (const auto& r : rules_) {
        if (r.kind == "let") names << q(r.name);
        else if (const Table* t = table(r.table))
            for (const auto& c : t->columns) names << q(r.name) + "." + q(c);
    }
    names.removeDuplicates();
    names_->setText(text("ui.record.names", {{"names", names.join(", ")}}));
}

void SolutionRecorder::rebuild() {
    fillLists();
    syncFields();
    definition_.reset();
    plan_.reset();
    message_->clear();
    QStringList lines;
    auto d = solutions::recordDefinition(recording());
    if (!d) {
        Error e = d.error();
        if (!e.stringParam("dir")) e.with("dir", title().toStdString());
        if (!e.stringParam("file")) e.with("file", std::string(solutions::SOL_DESCRIPTION_FILE));
        message_->setText(errorText(e));
    } else {
        definition_ = *d;
        auto p = s_.solutionDraftPlan(*d, examples_, api::Json::object());
        if (!p) message_->setText(errorText(p.error()));
        else {
            plan_ = std::move(*p);
            if (plan_->error) message_->setText(errorText(*plan_->error));
            else lines = solutionPlanLines(plan_->report);
        }
    }
    result_->setText(lines.join("\n"));
    save_->setEnabled(canSave());
    emit changed();
}

bool SolutionRecorder::canSave() const { return definition_ && plan_ && !plan_->error; }

void SolutionRecorder::picked(const std::string& ref, std::optional<kernel::SurfaceInfo::Kind> kind,
                              const std::optional<std::array<double, 3>>& at) {
    const int row = inputsList_->currentRow();
    if (row < 0 || row >= int(inputs_.size())) return setMessage(text("ui.record.needInput"));
    const solutions::InputDef in = inputs_[size_t(row)];
    const bool plane = kind == kernel::SurfaceInfo::Kind::Plane, cyl = kind == kernel::SurfaceInfo::Kind::Cylinder;
    if (in.type == "cylinder") {
        if (!cyl) return setMessage(text("ui.solution.needCylinder"));
        examples_[in.id] = ref;
    } else {
        if (!plane) return setMessage(text("ui.solution.needPlane"));
        // A point lies on the nearest preceding face input (solutions.md §8а.2); its click point is the point example.
        std::string faceId = in.id;
        if (in.type == "point") {
            for (int k = row - 1; k >= 0; --k)
                if (inputs_[size_t(k)].type == "face") {
                    faceId = inputs_[size_t(k)].id;
                    break;
                }
        }
        examples_[faceId] = ref;
        for (size_t k = 0; k < inputs_.size(); ++k) {
            const auto& p = inputs_[k];
            if (p.type != "point" || !at || (p.id != in.id && examples_.contains(p.id))) continue;
            std::string pf;  // the face of this point
            for (size_t m = k; m-- > 0;)
                if (inputs_[m].type == "face") {
                    pf = inputs_[m].id;
                    break;
                }
            if (pf == faceId) examples_[p.id] = api::Json::array({(*at)[0], (*at)[1], (*at)[2]});
        }
    }
    // The next input without an example.
    int next = row;
    for (int k = 1; k <= int(inputs_.size()); ++k) {
        const int r = (row + k) % int(inputs_.size());
        if (!examples_.contains(inputs_[size_t(r)].id)) {
            next = r;
            break;
        }
    }
    rebuild();
    inputsList_->setCurrentRow(next);
}

void SolutionRecorder::setMessage(const QString& m) { message_->setText(m); }

std::vector<std::string> SolutionRecorder::chosenFaces() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : examples_.items())
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

}  // namespace construe::app
