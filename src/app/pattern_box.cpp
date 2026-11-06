#include "app/messages.h"
#include "app/pattern_box.h"

#include "app/expr_input.h"
#include "app/dialog_buttons.h"

#include <QVBoxLayout>

namespace construe::app {

PatternBox::PatternBox(QWidget* parent) : PopupBox("patternEditor", parent) {
    auto* col = new QVBoxLayout(this);
    title_ = new QLabel;
    title_->setObjectName("patternTitle");
    col->addWidget(title_);
    form_ = new QFormLayout;
    col->addLayout(form_);
    auto line = [](const char* name) {
        auto* l = new ExprField;  // no document check here: errors come back from the command (markError)
        l->setEscapeReverts(false);  // Esc closes the box (§5.6)
        l->setObjectName(name);
        l->setMinimumWidth(90);
        return l;
    };
    // Counts are expressions too (an integer or a variable, S4-SKT-006).
    count1_ = line("patternCount1");
    step1_ = line("patternStep1");
    angle1_ = line("patternAngle1");
    count2_ = line("patternCount2");
    step2_ = line("patternStep2");
    angle2_ = line("patternAngle2");
    count_ = line("patternCount");
    angle_ = line("patternAngle");
    form_->addRow(txt("ui.sketchPattern.count1"), count1_);
    form_->addRow(txt("ui.sketchPattern.spacing1"), step1_);
    form_->addRow(txt("ui.common.direction1"), angle1_);
    line_ = new QPushButton(txt("ui.sketchPattern.byLine"));  // axis along a sketch segment (S4-SKT-006)
    line_->setObjectName("patternLine");
    line_->setCheckable(true);
    line_->setToolTip(txt("ui.sketchPattern.axisDirectionsFollowASketch"));
    form_->addRow("", line_);
    form_->addRow(txt("ui.sketchPattern.count2"), count2_);
    form_->addRow(txt("ui.sketchPattern.spacing2"), step2_);
    form_->addRow(txt("ui.common.direction2"), angle2_);
    line2_ = new QPushButton(txt("ui.sketchPattern.byLine"));  // the second axis along its own segment (FX12-FEAT-001)
    line2_->setObjectName("patternLine2");
    line2_->setCheckable(true);
    line2_->setToolTip(txt("ui.sketchPattern.secondAxisFollowsASketch"));
    form_->addRow("", line2_);
    form_->addRow(txt("ui.common.count"), count_);
    form_->addRow(txt("ui.common.angle"), angle_);
    auto* buttons = new DialogButtons("patternOk", "patternCancel");
    ok_ = buttons->ok();
    cancel_ = buttons->cancel();
    unlink_ = buttons->addAction(txt("ui.common.unlink"), "patternUnlink", txt("ui.sketchPattern.turnThePatternIntoRegular"));
    col->addWidget(buttons);
    connect(buttons, &DialogButtons::accepted, this, [this] { emit accepted(); });
    connect(buttons, &DialogButtons::cancelled, this, [this] { emit cancelled(); });
    connect(unlink_, &QPushButton::clicked, this, [this] { emit unlinkRequested(); });
    connect(line_, &QPushButton::clicked, this, [this](bool on) {
        if (!on && !values_.line.empty()) {  // pressed with a segment chosen — remove it
            values_.line.clear();
            updateLineButton();
            emit changed();
            return;
        }
        if (on && line2_->isChecked() && values_.line2.empty()) line2_->setChecked(false);  // one pick at a time
        emit linePickRequested(on);
    });
    connect(line2_, &QPushButton::clicked, this, [this](bool on) {
        if (!on && !values_.line2.empty()) {
            values_.line2.clear();
            updateLineButton();
            emit changed();
            return;
        }
        if (on && line_->isChecked() && values_.line.empty()) line_->setChecked(false);
        emit linePickRequested(on);
    });
    for (ExprField* l : {count1_, count2_, count_, step1_, angle1_, step2_, angle2_, angle_}) {
        connect(l, &QLineEdit::textEdited, this, [this] {
            markError({});
            emit changed();
        });
    }  // Enter in a field reaches the box (PopupBox): accepted once
    hide();
}

void PatternBox::open(const QPoint& at, const Values& v, bool editing) {
    values_ = v;
    count1_->setText(v.count1);
    count2_->setText(v.count2);
    count_->setText(v.count);
    step1_->setText(v.step1), angle1_->setText(v.angle1);
    step2_->setText(v.step2), angle2_->setText(v.angle2);
    angle_->setText(v.angle);
    for (QWidget* w : {static_cast<QWidget*>(count1_), static_cast<QWidget*>(step1_), static_cast<QWidget*>(angle1_),
                       static_cast<QWidget*>(count2_), static_cast<QWidget*>(step2_), static_cast<QWidget*>(angle2_),
                       static_cast<QWidget*>(line_), static_cast<QWidget*>(line2_)})
        form_->setRowVisible(w, !v.circular);
    updateLineButton();
    form_->setRowVisible(count_, v.circular);
    form_->setRowVisible(angle_, v.circular);
    title_->setText(v.circular ? txt("ui.common.circularPattern") : txt("ui.common.linearPattern"));
    unlink_->setVisible(editing);
    markError({});
    openAt(at + QPoint(12, 12));
    (v.circular ? static_cast<QWidget*>(count_) : static_cast<QWidget*>(count1_))->setFocus();
}

PatternBox::Values PatternBox::values() const {
    Values v = values_;
    v.count1 = count1_->text(), v.count2 = count2_->text(), v.count = count_->text();
    v.step1 = step1_->text(), v.angle1 = angle1_->text(), v.step2 = step2_->text(), v.angle2 = angle2_->text();
    v.angle = angle_->text();
    return v;
}

doc::Json PatternBox::params() const {
    const Values v = values();
    auto t = [](const QString& s) { return normalizeExprInput(s); };
    // Count: only digits — an integer, otherwise an expression (sketch.md §2.6.3, S4-SKT-006).
    auto n = [&](const QString& s) -> doc::Json {
        bool ok = false;
        const int k = s.trimmed().toInt(&ok);
        if (ok && !s.trimmed().isEmpty() && s.trimmed().at(0).isDigit()) return k;
        return t(s);
    };
    if (v.circular) return {{"center", v.center}, {"count", n(v.count)}, {"angle", t(v.angle)}};
    doc::Json p = {{"count1", n(v.count1)}, {"step1", t(v.step1)}, {"angle1", t(v.angle1)},
                   {"count2", n(v.count2)}, {"step2", t(v.step2)}, {"angle2", t(v.angle2)}};
    if (!v.line.empty()) p["line"] = v.line;
    if (!v.line2.empty()) p["line2"] = v.line2;
    return p;
}

void PatternBox::setLine(const std::string& line) {
    values_.line = line;
    updateLineButton();
    emit changed();
}

bool PatternBox::pickingLine() const { return line_->isChecked() && values_.line.empty(); }

void PatternBox::setLine2(const std::string& line) {
    values_.line2 = line;
    if (!line.empty()) angle2_->setText("0");  // along the segment itself (the 90° default is for the first axis' frame)
    updateLineButton();
    emit changed();
}

bool PatternBox::pickingLine2() const { return line2_->isChecked() && values_.line2.empty(); }

void PatternBox::updateLineButton() {
    const QSignalBlocker b(line_);
    line_->setChecked(!values_.line.empty());
    line_->setText(values_.line.empty() ? txt("ui.sketchPattern.byLine") : txt("ui.sketchPattern.line"));
    const QSignalBlocker b2(line2_);
    line2_->setChecked(!values_.line2.empty());
    line2_->setText(values_.line2.empty() ? txt("ui.sketchPattern.byLine") : txt("ui.sketchPattern.line"));
}

void PatternBox::markError(const std::string& name) {
    const std::map<std::string, QWidget*> fields = {{"count1", count1_}, {"step1", step1_}, {"angle1", angle1_}, {"count2", count2_},
                                                    {"step2", step2_},   {"angle2", angle2_}, {"count", count_},  {"angle", angle_},
                                                    {"line", line_}, {"line2", line2_}};
    for (const auto& [n, w] : fields) markFieldError(w, n == name);
}

}  // namespace construe::app
