// «Flip» button (spec/app-ux.md §5.13 «Кнопка «Обратить»», S7F-RF-007): everywhere a checkable «⇅» button to
// the right of the field it belongs to (pressed — flipped), with a tooltip by meaning (direction, side).
#pragma once

#include <QToolButton>

namespace construe::app {

// The flip glyph, also put before the caption of the «Flip» action of a mate.
inline const QString FLIP_GLYPH = QString::fromUtf8("⇅");

class FlipButton : public QToolButton {
    Q_OBJECT
public:
    // tip — the tooltip text; empty — «Flip» (ui.common.flip).
    explicit FlipButton(const QString& objectName, const QString& tip = {}, QWidget* parent = nullptr);
};

}  // namespace construe::app
