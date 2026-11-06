#include "app/dialog_buttons.h"

#include "app/messages.h"

#include <QHBoxLayout>
#include <QPushButton>

namespace construe::app {

DialogButtons::DialogButtons(const QString& okName, const QString& cancelName, QWidget* parent) : QWidget(parent) {
    row_ = new QHBoxLayout(this);
    row_->setContentsMargins(0, 0, 0, 0);
    row_->addStretch(1);
    ok_ = new QPushButton("OK");
    ok_->setObjectName(okName);
    row_->addWidget(ok_);
    connect(ok_, &QPushButton::clicked, this, &DialogButtons::accepted);
    if (!cancelName.isEmpty()) {
        cancel_ = new QPushButton(txt("ui.common.cancel"));
        cancel_->setObjectName(cancelName);
        row_->addWidget(cancel_);
        connect(cancel_, &QPushButton::clicked, this, &DialogButtons::cancelled);
    }
}

QPushButton* DialogButtons::addAction(const QString& text, const QString& objectName, const QString& tip) {
    auto* b = new QPushButton(text);
    b->setObjectName(objectName);
    if (!tip.isEmpty()) b->setToolTip(tip);
    row_->insertWidget(actions_++, b);  // before the stretch
    return b;
}

}  // namespace construe::app
