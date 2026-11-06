// Hint at the selection set (app/selection_bubble.h).
#include "app/selection_bubble.h"

#include "app/messages.h"
#include "app/popup_box.h"
#include "app/theme.h"

#include <QHBoxLayout>

namespace construe::app {

SelectionBubble::SelectionBubble(QWidget* parent) : QFrame(parent) {
    setObjectName("selBubble");
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setStyleSheet(QString("#selBubble { background: %1; border: 1px solid %2; }").arg(COLOR_FIELD_ACTIVE_BG.color.name(), COLOR_DIM_WEAK.color.name()));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 3, 6, 3);
    label_ = new QLabel;
    label_->setObjectName("selBubbleText");
    yes_ = new QPushButton(text("ui.sel.yes"));
    yes_->setObjectName("selYes");
    next_ = new QPushButton(text("ui.sel.next"));
    next_->setObjectName("selNext");
    reset_ = new QPushButton(text("ui.sel.reset"));
    reset_->setObjectName("selReset");
    for (auto* b : {yes_, next_, reset_}) b->setFocusPolicy(Qt::NoFocus);  // keys stay with the view (§5.3)
    row->addWidget(label_);
    row->addWidget(yes_);
    row->addWidget(next_);
    row->addWidget(reset_);
    connect(yes_, &QPushButton::clicked, this, &SelectionBubble::yes);
    connect(next_, &QPushButton::clicked, this, &SelectionBubble::next);
    connect(reset_, &QPushButton::clicked, this, &SelectionBubble::reset);
    hide();
}

void SelectionBubble::showAt(const QPoint& at, const QString& t, bool pending, int variants) {
    label_->setText(t);
    yes_->setVisible(pending);
    next_->setVisible(pending && variants > 1);
    adjustSize();
    // Next to the click (CANDIDATE_BUBBLE_OFFSET_PX = CONTEXT_BAR_OFFSET_PX), kept inside the view.
    moveInside(this, at + QPoint(CONTEXT_BAR_OFFSET_X_PX, CONTEXT_BAR_OFFSET_X_PX));  // below-right: the context panel goes above
    show();
    raise();
}

}  // namespace construe::app
