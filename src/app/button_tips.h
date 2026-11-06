// Tooltips of buttons (spec/app-ux.md §2.1 «Button tooltips», FX9-UI-001): shown by the program
// after BUTTON_TOOLTIP_DELAY_MS of a still cursor — also in an inactive window, held while the cursor is on the button,
// shown again after a click, at once on a neighbour button.
#pragma once

#include <QBasicTimer>
#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QPointer>

class QAbstractButton;

namespace construe::app {

class ButtonTips : public QObject {
public:
    // Installs the filter on the application once (repeated calls do nothing).
    static void install();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;
    void timerEvent(QTimerEvent* e) override;

private:
    explicit ButtonTips(QObject* parent) : QObject(parent) {}
    void arm(QAbstractButton* b, const QPoint& global, bool immediate);
    void show();
    QBasicTimer timer_;
    QPointer<QAbstractButton> button_;  // the button under the cursor
    QPoint global_;                    // last cursor position over it (global)
    QPointer<QAbstractButton> shown_;   // the button whose tooltip we show
    QElapsedTimer hiddenAgo_;           // since the shown tooltip was left (neighbour — at once)
};

}  // namespace construe::app
