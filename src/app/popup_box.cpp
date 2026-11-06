#include "app/popup_box.h"

#include "app/theme.h"

#include <QKeyEvent>

#include <algorithm>

namespace construe::app {

QPoint placeInside(const QWidget* w, QPoint p) {
    if (const QWidget* pw = w->parentWidget()) {
        p.setX(std::clamp(p.x(), 0, std::max(0, pw->width() - w->width())));
        p.setY(std::clamp(p.y(), 0, std::max(0, pw->height() - w->height())));
    }
    return p;
}

void moveInside(QWidget* w, QPoint p) { w->move(placeInside(w, p)); }

void markFieldError(QWidget* field, bool error) {
    field->setStyleSheet(error ? QString("border: 1px solid %1").arg(COLOR_ERROR.color.name()) : QString());
}

PopupBox::PopupBox(const QString& objectName, QWidget* parent) : QFrame(parent) {
    setObjectName(objectName);
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
}

void PopupBox::openAt(QPoint p) {
    adjustSize();
    moveInside(this, p);
    show();
    raise();
}

void PopupBox::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        cancelBox();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        // Fields do not accept the box themselves (no returnPressed handlers): Enter comes here once.
        if (isVisible()) acceptBox();
        return;
    }
    QFrame::keyPressEvent(e);
}

}  // namespace construe::app
