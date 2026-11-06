// Pop-up box at the cursor (spec/app-ux.md §5.13 «Pop-up box at the cursor», S7F-RF-007): the base of the sketch
// pattern, primitive, tolerance and text boxes — a non-modal frame over the view, placed inside its parent, Enter accepts,
// Esc cancels, an error field gets a COLOR_ERROR border.
#pragma once

#include <QFrame>
#include <QPoint>

class QWidget;

namespace construe::app {

// Top-left position for w at p, clamped so that w fits inside its parent (no parent — p unchanged).
QPoint placeInside(const QWidget* w, QPoint p);
// Moves w to placeInside(w, p).
void moveInside(QWidget* w, QPoint p);
// Error highlight of an input field: a COLOR_ERROR border, or none.
void markFieldError(QWidget* field, bool error);

class PopupBox : public QFrame {
    Q_OBJECT
public:
    PopupBox(const QString& objectName, QWidget* parent);

protected:
    // Fits the box to its contents, places it inside the parent at p, shows and raises it.
    void openAt(QPoint p);
    // Enter (not taken by the focused widget) and Esc.
    virtual void acceptBox() = 0;
    virtual void cancelBox() = 0;
    void keyPressEvent(QKeyEvent* e) override;
};

}  // namespace construe::app
