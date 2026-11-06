// Button row of a box or a confirming panel (spec/app-ux.md §5.13 «Window button row», S7F-RF-007): extra actions on
// the left, a stretch, «OK» and «Cancel» on the right, in this order, with the same captions everywhere.
#pragma once

#include <QWidget>

class QHBoxLayout;
class QPushButton;

namespace construe::app {

class DialogButtons : public QWidget {
    Q_OBJECT
public:
    // okName, cancelName — objectNames of the buttons (e2e and button coverage find them); empty cancelName — no «Cancel».
    DialogButtons(const QString& okName, const QString& cancelName, QWidget* parent = nullptr);
    QPushButton* ok() const { return ok_; }
    QPushButton* cancel() const { return cancel_; }
    // An extra action button on the left (e.g. «Unlink», «Remove»); its clicked signal is connected by the caller.
    QPushButton* addAction(const QString& text, const QString& objectName, const QString& tip = {});

signals:
    void accepted();
    void cancelled();

private:
    QHBoxLayout* row_ = nullptr;
    int actions_ = 0;
    QPushButton* ok_ = nullptr;
    QPushButton* cancel_ = nullptr;
};

}  // namespace construe::app
