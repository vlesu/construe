// Tooltips of buttons (spec/app-ux.md §2.1 «Button tooltips», FX9-UI-001).
#include "app/button_tips.h"

#include "app/theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QToolTip>

namespace construe::app {

namespace {

constexpr int HOLD_MS = 24 * 3600 * 1000;  // "without a timeout": the tooltip is hidden by leaving the button

QAbstractButton* managed(QObject* o) {
    auto* b = qobject_cast<QAbstractButton*>(o);
    return b && b->isEnabled() && !b->toolTip().isEmpty() ? b : nullptr;
}

}  // namespace

void ButtonTips::install() {
    static ButtonTips* tips = nullptr;
    if (tips || !qApp) return;
    tips = new ButtonTips(qApp);
    qApp->installEventFilter(tips);
}

void ButtonTips::arm(QAbstractButton* b, const QPoint& global, bool immediate) {
    button_ = b;
    global_ = global;
    if (immediate) {
        timer_.stop();
        show();
        return;
    }
    timer_.start(BUTTON_TOOLTIP_DELAY_MS, this);
}

void ButtonTips::show() {
    QAbstractButton* b = button_;
    if (!b || !b->isVisible() || !b->isEnabled() || b->toolTip().isEmpty()) return;
    if (QApplication::activePopupWidget() || QApplication::mouseButtons() != Qt::NoButton) return;  // a menu over it
    if (!b->rect().contains(b->mapFromGlobal(global_))) return;
    shown_ = b;
    QToolTip::showText(global_, b->toolTip(), b, b->rect(), HOLD_MS);
}

bool ButtonTips::eventFilter(QObject* o, QEvent* e) {
    switch (e->type()) {
        case QEvent::ToolTip:
            return managed(o) != nullptr;  // ours, not Qt's (Qt's does not show in an inactive window)
        case QEvent::Enter:
            if (QAbstractButton* b = managed(o)) {
                const bool warm = (shown_ && QToolTip::isVisible()) || (hiddenAgo_.isValid() && hiddenAgo_.elapsed() <= BUTTON_TOOLTIP_DELAY_MS);
                arm(b, static_cast<QEnterEvent*>(e)->globalPosition().toPoint(), warm && shown_ != b);
            }
            break;
        case QEvent::MouseMove:
            if (QAbstractButton* b = managed(o); b && static_cast<QMouseEvent*>(e)->buttons() == Qt::NoButton) {
                const QPoint g = static_cast<QMouseEvent*>(e)->globalPosition().toPoint();
                if (b == shown_ && QToolTip::isVisible()) {
                    global_ = g;  // the tooltip stays while the cursor is on the button
                } else if (b != button_ || g != global_ || !timer_.isActive()) {
                    const bool warm = shown_ && shown_ != b && QToolTip::isVisible();
                    arm(b, g, warm);
                }
            }
            break;
        case QEvent::Leave:
            if (o == button_) {
                timer_.stop();
                button_ = nullptr;
            }
            if (o == shown_) {
                hiddenAgo_.start();  // Qt may have hidden the label on this Leave already
                shown_ = nullptr;
                QToolTip::hideText();
            }
            break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            if (o == button_) timer_.stop();
            break;
        case QEvent::MouseButtonRelease:
            // After a click the tooltip comes back if the cursor stayed on the button (the command may have hidden it).
            if (QAbstractButton* b = managed(o)) {
                const QPoint g = static_cast<QMouseEvent*>(e)->globalPosition().toPoint();
                if (b->rect().contains(b->mapFromGlobal(g))) {
                    shown_ = nullptr;
                    arm(b, g, false);
                }
            }
            break;
        case QEvent::Hide:
        case QEvent::Destroy:
            if (o == button_) {
                timer_.stop();
                button_ = nullptr;
            }
            break;
        default: break;
    }
    return QObject::eventFilter(o, e);
}

void ButtonTips::timerEvent(QTimerEvent* e) {
    if (e->timerId() != timer_.timerId()) return QObject::timerEvent(e);
    timer_.stop();
    show();
}

}  // namespace construe::app
