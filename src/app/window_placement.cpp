// The main window at first start inside the available area of its screen (app/window_placement.h, FX14-UI-002).
#include "app/window_placement.h"

#include <algorithm>

namespace construe::app {

WindowPlacement initialWindowPlacement(const QRect& available, const QSize& defaultSize, const QSize& minimumSize,
                                       const QMargins& frame) {
    WindowPlacement p;
    const int frameW = frame.left() + frame.right(), frameH = frame.top() + frame.bottom();
    // The largest client size whose frame is inside the area.
    const QSize room(std::max(1, available.width() - frameW), std::max(1, available.height() - frameH));
    p.minimum = minimumSize.boundedTo(room);
    if (defaultSize.width() <= room.width() && defaultSize.height() <= room.height()) {
        p.size = defaultSize;
        p.pos = QPoint(available.x() + (available.width() - (defaultSize.width() + frameW)) / 2,
                       available.y() + (available.height() - (defaultSize.height() + frameH)) / 2);
        return p;
    }
    // The screen is smaller than the default size: maximized; the normal geometry fills the area (without a window
    // manager that honours the maximized state the window still fits).
    p.maximized = true;
    p.size = room;
    p.pos = available.topLeft();
    return p;
}

}  // namespace construe::app
