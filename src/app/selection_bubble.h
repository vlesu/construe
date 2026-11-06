// Hint at the selection set (spec/selection.md §5.3, FX7-SEL-001): the candidate label «<label> (k of n)» and the
// «Yes» / «Other» / «Reset» buttons next to the last click in the 3D view.
#pragma once

#include <QFrame>
#include <QLabel>
#include <QPushButton>

namespace construe::app {

class SelectionBubble : public QFrame {
    Q_OBJECT
public:
    explicit SelectionBubble(QWidget* parent);
    // pending — a candidate waits for «Yes»; variants — number of candidates (> 1 shows «Other»).
    void showAt(const QPoint& at, const QString& text, bool pending, int variants);
    QLabel* label() const { return label_; }
    QPushButton* yesButton() const { return yes_; }
    QPushButton* nextButton() const { return next_; }
    QPushButton* resetButton() const { return reset_; }

signals:
    void yes();
    void next();
    void reset();

private:
    QLabel* label_;
    QPushButton *yes_, *next_, *reset_;
};

}  // namespace construe::app
