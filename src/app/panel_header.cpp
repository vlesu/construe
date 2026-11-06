// Header row of a hideable panel (app/panel_header.h, spec/app-ux.md §2.1в, FX8-UI-008).
#include "app/messages.h"
#include "app/panel_header.h"

#include "app/theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>

namespace construe::app {

void PanelHeader::makeRow(const QString& title) {
    row_ = new QHBoxLayout(this);
    row_->setContentsMargins(0, 0, 0, 0);
    title_ = new QLabel(title);
    QFont f = title_->font();
    f.setBold(true);
    title_->setFont(f);
    row_->addWidget(title_, 1);
}

PanelHeader::PanelHeader(const QString& title, const QString& closeName, QWidget* parent) : QWidget(parent) {
    setObjectName(closeName + "Header");
    makeRow(title);
    close_ = new QToolButton;
    close_->setObjectName(closeName);
    close_->setText(QString::fromUtf8("✕"));  // ✕
    close_->setToolTip(txt("ui.common.close"));
    close_->setAutoRaise(true);
    close_->setFixedSize(PANEL_CLOSE_PX, PANEL_CLOSE_PX);
    row_->addWidget(close_, 0, Qt::AlignRight | Qt::AlignTop);
    connect(close_, &QToolButton::clicked, this, &PanelHeader::closeRequested);
}

void PanelHeader::addButton(QWidget* w) {
    if (close_) row_->insertWidget(row_->indexOf(close_), w);
    else row_->insertWidget(row_->indexOf(ok_), w);
}

PanelHeader::PanelHeader(const QString& title, const CommandButtons& names, QWidget* parent) : QWidget(parent) {
    setObjectName(names.ok + "Header");
    makeRow(title);
    ok_ = new QPushButton(QString::fromUtf8("✓"));
    ok_->setObjectName(names.ok);
    ok_->setToolTip("OK (Ctrl+Enter)");
    cancel_ = new QPushButton(QString::fromUtf8("✗"));
    cancel_->setObjectName(names.cancel);
    cancel_->setToolTip(txt("ui.feature.cancelEsc"));
    for (auto* b : {ok_, cancel_}) {
        b->setFixedSize(CONFIRM_CORNER_BTN_PX, CONFIRM_CORNER_BTN_PX);
        b->setAutoDefault(false);  // Enter is handled by acceptOnEnter, not by a default button
        row_->addWidget(b);
    }
    ok_->setStyleSheet(QString("color: %1; font-weight: bold;").arg(COLOR_OK.color.name()));
    cancel_->setStyleSheet(QString("color: %1; font-weight: bold;").arg(COLOR_CANCEL.color.name()));
    connect(ok_, &QPushButton::clicked, this, [this] {
        if (ok_->isEnabled()) emit accepted();
    });
    connect(cancel_, &QPushButton::clicked, this, &PanelHeader::cancelled);
}

void PanelHeader::acceptOnEnter(QWidget* panel) {
    if (!ok_ || !panel) return;
    for (QLineEdit* e : panel->findChildren<QLineEdit*>())
        connect(e, &QLineEdit::returnPressed, this, [this] {
            if (ok_->isEnabled()) emit accepted();
        });
}

}  // namespace construe::app
