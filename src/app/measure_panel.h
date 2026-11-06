// The «Measure» tool (app-ux §3.8, FX10-MEAS-001): a panel over the 3D view with the measurements of the entities
// selected in the view — one (a click) or two (Ctrl+click).
#pragma once

#include "base/math.h"

#include <QFrame>

#include <array>
#include <optional>

class QLabel;
class QToolButton;

namespace construe::app {

inline constexpr int MEASURE_DIGITS = 3;            // decimals of the shown values (app-ux §11)
inline constexpr int MEASURE_PANEL_MARGIN_PX = 8;   // offset of the panel from the view's top left corner
inline constexpr int MEASURE_ARROW_HEAD_PX = 10;    // distance arrow: head length (app-ux §3.8, FX12-MEAS-001)
inline constexpr int MEASURE_ARROW_WIDTH_PX = 2;    // distance arrow: line width

class AppModel;
class Viewport;

class MeasurePanel : public QFrame {
    Q_OBJECT
public:
    MeasurePanel(AppModel& model, Viewport* view);
    // Shows the panel in the top left corner of the view and measures the current selection.
    void open();
    void refresh();
    QLabel* resultLabel() const { return text_; }
    // The ends of the shown distance arrow (FX12-MEAS-001), world; none — no arrow.
    std::optional<std::array<Point3, 2>> arrowSpan() const;

protected:
    void hideEvent(QHideEvent*) override;

private:
    AppModel& model_;
    Viewport* view_;
    QLabel* text_;
    QToolButton* close_;
    QWidget* arrow_;  // the layer over the view with the distance arrow (measureArrow)
    std::optional<std::array<Point3, 2>> span_;
};

}  // namespace construe::app
