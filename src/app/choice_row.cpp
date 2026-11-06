#include "app/choice_row.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QToolButton>
#include <QVBoxLayout>

namespace construe::app {

ChoiceRow::ChoiceRow(const std::vector<Option>& options, QWidget* parent, QButtonGroup* group, bool vertical) : QWidget(parent) {
    QBoxLayout* row = vertical ? static_cast<QBoxLayout*>(new QVBoxLayout(this)) : new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    group_ = group ? group : new QButtonGroup(this);
    group_->setExclusive(true);
    for (const auto& o : options) {
        auto* b = new QToolButton;
        b->setObjectName(o.objectName);
        b->setText(o.label);
        if (!o.tip.isEmpty()) b->setToolTip(o.tip);
        b->setProperty("choice", o.id);
        b->setCheckable(true);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        group_->addButton(b);
        row->addWidget(b, 1);
    }
    if (!group_->buttons().isEmpty()) group_->buttons().front()->setChecked(true);
    connect(group_, &QButtonGroup::buttonClicked, this, [this](QAbstractButton* b) { emit chosen(b->property("choice").toString()); });
}

QString ChoiceRow::current() const {
    auto* b = group_->checkedButton();
    return b ? b->property("choice").toString() : QString();
}

void ChoiceRow::setCurrent(const QString& id) {
    if (auto* b = button(id)) b->setChecked(true);
}

QAbstractButton* ChoiceRow::button(const QString& id) const {
    for (auto* b : group_->buttons())
        if (b->property("choice").toString() == id) return b;
    return nullptr;
}

}  // namespace construe::app
