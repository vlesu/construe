// Part selection (app-ux §3.5), picking a plane for a sketch, the extrude depth manipulator (§5.8) and
// drawing visible sketches outside sketch mode (§3.4): the view tool outside the sketcher.
#pragma once

#include "app/viewport.h"

#include <QPoint>
#include <QPointF>
#include <QRectF>

#include <optional>

namespace construe::app {

class MainWindow;

class PartTool : public ViewportTool {
public:
    explicit PartTool(MainWindow& w) : w_(w) {}

    bool mousePress(QMouseEvent* e) override;
    bool mouseMove(QMouseEvent* e) override;
    bool mouseRelease(QMouseEvent* e) override;
    bool mouseDoubleClick(QMouseEvent* e) override;
    void paint(QPainter& p) override;
    // The selection box being drawn (FX13-ASM-002), screen coordinates of the view; empty — none.
    std::optional<QRectF> selectionBox() const;

private:
    bool overManipulator(const QPointF& p) const;
    void paintSelection(QPainter& p);  // FX7-SEL-001

    MainWindow& w_;
    QPoint press_;
    std::string dragInstance_;  // assembly: instance under the cursor at press time (§8а)
    bool dragging_ = false;
    bool overFreeManip(const QPointF& p) const;  // FX8-ASM-003
    void paintFreeManip(QPainter& p);
    bool pulling_ = false, hoverFree_ = false;
    // Selection box over an assembly (FX13-ASM-002): armed by a press on an empty place, drawn past the drag threshold.
    bool boxArmed_ = false, boxing_ = false;
    QPoint boxCur_;
    // Annotation drag (FX12-PMI-002): ID under the cursor at press time, by the anchor or by the label; moving — past
    // the drag threshold.
    std::string pmiDrag_;
    bool pmiByAnchor_ = false, pmiMoving_ = false;
    QPointF pmiL0_, pmiA0_;  // drawn label point and anchor at press time
    bool manip_ = false, hoverManip_ = false, rev0_ = false;
    double d0_ = 0, mmPerAxisPx_ = 1;
    QPointF axis_;
};

}  // namespace construe::app
