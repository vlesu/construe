#include "app/flip_button.h"

#include "app/messages.h"

namespace construe::app {

FlipButton::FlipButton(const QString& objectName, const QString& tip, QWidget* parent) : QToolButton(parent) {
    setObjectName(objectName);
    setText(FLIP_GLYPH);
    setToolTip(tip.isEmpty() ? QString(txt("ui.common.flip")) : tip);
    setCheckable(true);
}

}  // namespace construe::app
