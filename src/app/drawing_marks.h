// Sheet marks on a drawing (spec/app-ux.md §8б «Marks», drawing.md §6.25, FX9-DRW-008): selection of a
// roughness, tolerance frame, datum, weld or rest roughness sign, dragging the sign (the leader goes to the nearest point
// of its surface), deletion.
#pragma once
#include "app/drawing_view.h"

#include "api/session.h"

#include <QObject>

#include <functional>
#include <string>

namespace construe::app {

class SheetMarkTool : public QObject, public DrawingView::Tool {
    Q_OBJECT
public:
    struct Hooks {
        std::function<api::Session&()> session;
        std::function<void(const QString&, bool)> toast;  // notification (error — true)
        std::function<void()> selected;                   // a mark selected: the window drops other selections
    };
    SheetMarkTool(DrawingView* view, Hooks hooks, QObject* parent = nullptr);

    const std::string& selectedMark() const { return selected_; }
    void select(const std::string& id);
    // Mark under a sheet point (its primitives within PICK_CURVE_PX); empty — none.
    std::string markAt(const Point2& at) const;
    void refresh();  // the document or the sheet changed
    bool escape();   // Esc: clear the selection; false — nothing to cancel

    // DrawingView::Tool
    bool press(const Point2& at) override;
    void move(const Point2& at, bool leftDown) override;
    void release(const Point2& at) override;
    bool key(int key) override;
    void paint(QPainter& p) override;
    std::string hiddenRole() const override;

private:
    DrawingView* view_;
    Hooks h_;
    std::string selected_;
    bool pressed_ = false, dragging_ = false;
    Point2 pressAt_{0, 0}, dragDelta_{0, 0};
    const drawing::MarkPlacement* placement(const std::string& id) const;
};

}  // namespace construe::app
