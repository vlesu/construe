#include "app/messages.h"
#include "app/primitive_box.h"

#include "app/dialog_buttons.h"
#include "app/expr_input.h"

#include <QSignalBlocker>
#include <QVBoxLayout>

#include <map>

namespace construe::app {

PrimitiveBox::PrimitiveBox(QWidget* parent) : PopupBox("primitiveEditor", parent) {
    auto* col = new QVBoxLayout(this);
    title_ = new QLabel;
    title_->setObjectName("primitiveTitle");
    col->addWidget(title_);
    form_ = new QFormLayout;
    col->addLayout(form_);
    count_ = new QSpinBox;
    count_->setObjectName("primitiveCount");
    count_->setRange(3, 40);  // PRIMITIVE_MAX_SIDES (sketch.md §7)
    kind_ = new QComboBox;
    kind_->setObjectName("primitiveKind");
    kind_->addItems({txt("ui.primitive.inscribed"), txt("ui.primitive.circumscribed")});
    d1_ = new ExprField;  // errors come back from the command (markError)
    d1_->setObjectName("primitiveD1");
    d2_ = new ExprField;
    d2_->setObjectName("primitiveD2");
    for (ExprField* l : {d1_, d2_}) l->setEscapeReverts(false);  // Esc closes the box (§5.6)
    form_->addRow(txt("ui.common.count"), count_);
    form_->addRow(txt("ui.primitive.type"), kind_);
    form_->addRow(txt("ui.primitive.diameter1"), d1_);
    form_->addRow(txt("ui.primitive.diameter2"), d2_);
    auto* buttons = new DialogButtons("primitiveOk", "primitiveCancel");
    auto* unlink = buttons->addAction(txt("ui.common.unlink"), "primitiveUnlink", txt("ui.primitive.turnThePrimitiveIntoRegular"));
    col->addWidget(buttons);
    connect(buttons, &DialogButtons::accepted, this, [this] { emit accepted(); });
    connect(buttons, &DialogButtons::cancelled, this, [this] { emit cancelled(); });
    connect(unlink, &QPushButton::clicked, this, [this] { emit unlinkRequested(); });
    for (ExprField* l : {d1_, d2_}) connect(l, &QLineEdit::textEdited, this, [this] { markError({}); });  // Enter — PopupBox
    hide();
}

void PrimitiveBox::open(const QPoint& at, const Values& v) {
    values_ = v;
    const QSignalBlocker b(count_);
    count_->setValue(v.count);
    kind_->setCurrentIndex(v.inscribed ? 0 : 1);
    d1_->setText(v.d1), d2_->setText(v.d2);
    if (auto* l = qobject_cast<QLabel*>(form_->labelForField(count_))) l->setText(v.star ? txt("ui.primitive.rays") : txt("ui.primitive.sides"));
    form_->setRowVisible(kind_, !v.star);
    form_->setRowVisible(d1_, v.star);
    form_->setRowVisible(d2_, v.star);
    title_->setText(v.star ? txt("ui.common.star") : txt("ui.primitive.nGon"));
    markError({});
    openAt(at + QPoint(12, 12));
    count_->setFocus();
}

PrimitiveBox::Values PrimitiveBox::values() const {
    Values v = values_;
    v.count = count_->value();
    v.inscribed = kind_->currentIndex() == 0;
    v.d1 = QString::fromStdString(normalizeExprInput(d1_->text()));
    v.d2 = QString::fromStdString(normalizeExprInput(d2_->text()));
    return v;
}

void PrimitiveBox::markError(const std::string& name) {
    const std::map<std::string, QWidget*> fields = {{"sides", count_}, {"rays", count_}, {"d1", d1_}, {"d2", d2_}};
    for (const auto& [n, w] : fields) markFieldError(w, false);
    if (auto it = fields.find(name); it != fields.end()) markFieldError(it->second, true);
}

}  // namespace construe::app
