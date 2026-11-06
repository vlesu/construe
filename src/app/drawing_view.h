// Drawing sheet window (spec/app-ux.md §8б): drawing sheet primitives, zoom with the wheel, pan with the middle button,
// selecting and dragging views.
#pragma once
#include "drawing/sheet.h"

#include <QImage>
#include <QPainterPath>
#include <QTransform>
#include <QWidget>

#include <Eigen/Geometry>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace construe::app {

class DrawingView : public QWidget {
    Q_OBJECT
public:
    // Tool over the sheet (dimensions, §8б): gets the left-button press first; true — the gesture is its own (the view
    // is not selected/moved), further motion and release go to it, so does buttonless motion (highlight); draws on top.
    class Tool {
    public:
        virtual ~Tool() = default;
        virtual bool press(const Point2& at) = 0;
        virtual void move(const Point2& at, bool leftDown) = 0;
        virtual void release(const Point2& at) = 0;
        virtual bool key(int key) = 0;  // true — the key was handled
        virtual void paint(QPainter& p) = 0;
        virtual std::string hiddenRole() const = 0;  // primitives with this role are not drawn (dragging); empty — all
    };

    // Several tools over the sheet (texts, dimensions — FX7-DRW-006): the press goes to them in order, the first one that
    // takes it owns the gesture; buttonless motion, painting go to all; keys — to the first that handles them.
    class ToolGroup : public Tool {
    public:
        explicit ToolGroup(std::vector<Tool*> tools) : tools_(std::move(tools)) {}
        bool press(const Point2& at) override {
            for (Tool* t : tools_)
                if (t->press(at)) {
                    owner_ = t;
                    return true;
                }
            return false;
        }
        void move(const Point2& at, bool leftDown) override {
            if (leftDown) {
                if (owner_) owner_->move(at, true);
                return;
            }
            for (Tool* t : tools_) t->move(at, false);
        }
        void release(const Point2& at) override {
            if (Tool* t = std::exchange(owner_, nullptr)) t->release(at);
        }
        bool key(int k) override {
            for (Tool* t : tools_)
                if (t->key(k)) return true;
            return false;
        }
        void paint(QPainter& p) override {
            for (Tool* t : tools_) t->paint(p);
        }
        std::string hiddenRole() const override {
            for (Tool* t : tools_)
                if (auto r = t->hiddenRole(); !r.empty()) return r;
            return {};
        }

    private:
        std::vector<Tool*> tools_;
        Tool* owner_ = nullptr;
    };

    explicit DrawingView(QWidget* parent = nullptr);
    void setTool(Tool* t) { tool_ = t; }

    void setGraphics(drawing::SheetGraphics g);
    const drawing::SheetGraphics& graphics() const { return g_; }
    // Whole sheet in the window with margins (§8б).
    void fitSheet();
    // Pixels per sheet millimeter (for tests and drawing scale).
    double zoom() const { return zoom_; }
    QPointF sheetToWidget(const Point2& p) const;
    QTransform sheetTransform() const;  // sheetToWidget as a transform (for paths in sheet mm)
    Point2 widgetToSheet(const QPointF& p) const;

    // Views (§8б): a view rectangle is its primitives' extent with VIEW_PICK_MARGIN_MM margin; empty — no primitives.
    Eigen::AlignedBox2d viewRect(const std::string& viewId) const;
    // View under a sheet point (when overlapping — the one drawn last); empty — none.
    std::string viewAt(const Point2& p) const;
    void setSelectedView(const std::string& viewId);
    const std::string& selectedView() const { return selected_; }
    // Drag constraint: (view, offset mm) → allowed offset (a projected view — along its own axis).
    void setDragConstraint(std::function<Point2(const std::string&, const Point2&)> f) { constrain_ = std::move(f); }
    // Views moving with the dragged one as one whole (FX12-DRW-003: a main view — with its projected views); the dragged
    // view itself is always included. Not set — only it.
    void setDragGroup(std::function<std::vector<std::string>(const std::string&)> f) { dragGroup_ = std::move(f); }
    // Tool preview (section line, detail-view circle) — in the selection color on top of the sheet.
    void setOverlay(std::vector<drawing::Primitive> prims);
    const std::vector<drawing::Primitive>& overlay() const { return overlay_; }
    // Left double click (FX7-DRW-003, FX7-DRW-006): true — handled; unset or false — as a press.
    void setDoubleClickHandler(std::function<bool(const Point2&)> f) { doubleClick_ = std::move(f); }
    // Tool input in progress: pressing on a view does not start dragging.
    void setToolActive(bool on) { toolActive_ = on; }
    bool toolActive() const { return toolActive_; }

signals:
    void sheetClicked(const construe::Point2& at);                          // left-button click without dragging
    void viewDragged(const QString& viewId, const construe::Point2& delta);  // view dragging finished
    void contextRequested(const construe::Point2& at, const QPoint& globalPos);
    void escapePressed();
    void sheetHovered(const construe::Point2& at);  // cursor over the sheet with no buttons pressed

protected:
    void paintEvent(QPaintEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    drawing::SheetGraphics g_;
    std::vector<QPainterPath> textPaths_;  // text outlines in sheet mm (by primitive index)
    std::vector<QImage> images_;           // images of shaded views (by primitive index, drawing.md §6.22)
    double zoom_ = 1.0;
    QPointF origin_;  // position of the sheet's lower-left corner in the window, pixels
    QPoint panFrom_;
    bool panning_ = false, fitted_ = false;
    // Views: primitive extents (without margin) in order of first appearance.
    std::vector<std::pair<std::string, Eigen::AlignedBox2d>> viewBoxes_;
    std::string selected_;
    std::function<Point2(const std::string&, const Point2&)> constrain_;
    std::vector<drawing::Primitive> overlay_;
    bool toolActive_ = false;
    // Left-button press: the point, the view under it, whether dragging is in progress and its offset.
    bool leftDown_ = false, dragging_ = false;
    QPoint pressAt_;
    std::string pressView_;
    std::vector<std::string> dragGroupNow_;  // views shifted while dragging (FX12-DRW-003)
    std::function<std::vector<std::string>(const std::string&)> dragGroup_;
    Point2 dragDelta_{0, 0};
    Tool* tool_ = nullptr;
    bool toolGesture_ = false;  // the left-button press was taken by the tool
    std::function<bool(const Point2&)> doubleClick_;
};

}  // namespace construe::app
