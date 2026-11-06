// The main window at first start inside the available area of its screen (spec/app-ux.md §2.1, FX14-UI-002): the
// default size when it fits with the window frame, centred; a smaller screen — maximized, the normal geometry filling
// the area, so that the status bar is never under the taskbar.
#pragma once

#include <QMargins>
#include <QPoint>
#include <QRect>
#include <QSize>

namespace construe::app {

struct WindowPlacement {
    QPoint pos;        // top-left of the frame (QWidget::move of a top-level window)
    QSize size;        // client size (QWidget::resize)
    QSize minimum;     // minimum client size: WINDOW_MIN_W × WINDOW_MIN_H, at most what fits
    bool maximized = false;
};

// available — QScreen::availableGeometry (without the taskbar); frame — the frame of the window around the client area
// (before the window is shown — WINDOW_FRAME_ALLOWANCE).
WindowPlacement initialWindowPlacement(const QRect& available, const QSize& defaultSize, const QSize& minimumSize,
                                       const QMargins& frame);

}  // namespace construe::app
