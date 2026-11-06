// Drawing sheet window (spec/app-ux.md §8б).
#include "app/drawing_view.h"
#include "app/session_log.h"

#include "app/keys.h"
#include "app/sheet_print.h"
#include "app/theme.h"

#include "api/session.h"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace construe::app {

namespace {
constexpr double FIT_MARGIN_PX = 24;       // margin around the sheet for «Whole sheet»
constexpr double WHEEL_ZOOM_STEP = 1.2;    // zoom factor per wheel click
constexpr double ZOOM_MIN = 0.05, ZOOM_MAX = 200;  // pixels per mm
constexpr double MIN_PEN_PX = 1.0;         // thin lines are no thinner than a pixel

// View ID from a primitive role «view:<id>:<class>»; empty — not a view.
std::string viewOfRole(const std::string& role) {
    if (role.rfind("view:", 0) != 0) return {};
    const auto e = role.find(':', 5);
    return role.substr(5, e == std::string::npos ? std::string::npos : e - 5);
}
}  // namespace

DrawingView::DrawingView(QWidget* parent) : QWidget(parent) {
    setObjectName("drawingView");
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void DrawingView::setGraphics(drawing::SheetGraphics g) {
    const bool sizeChanged = g.width != g_.width || g.height != g_.height;
    g_ = std::move(g);
    textPaths_.assign(g_.primitives.size(), QPainterPath());
    images_.assign(g_.primitives.size(), QImage());
    for (size_t i = 0; i < g_.primitives.size(); ++i) {
        if (g_.primitives[i].kind == drawing::PrimitiveKind::Text) textPaths_[i] = sheetTextPath(g_.primitives[i]);
        if (g_.primitives[i].kind == drawing::PrimitiveKind::Image && g_.primitives[i].image) images_[i] = rasterQImage(*g_.primitives[i].image);
    }
    viewBoxes_.clear();
    for (const auto& p : g_.primitives) {
        const std::string v = viewOfRole(p.role);
        if (v.empty()) continue;
        auto it = std::find_if(viewBoxes_.begin(), viewBoxes_.end(), [&](const auto& b) { return b.first == v; });
        if (it == viewBoxes_.end()) it = viewBoxes_.insert(viewBoxes_.end(), {v, Eigen::AlignedBox2d()});
        for (const auto& q : p.points) it->second.extend(q);
        if (p.kind == drawing::PrimitiveKind::Circle || p.kind == drawing::PrimitiveKind::Arc) {
            it->second.extend(p.center + Point2(p.radius, p.radius));
            it->second.extend(p.center - Point2(p.radius, p.radius));
        }
    }
    if (sizeChanged || !fitted_) fitSheet();
    update();
}

Eigen::AlignedBox2d DrawingView::viewRect(const std::string& viewId) const {
    for (const auto& [id, b] : viewBoxes_)
        if (id == viewId && !b.isEmpty())
            return Eigen::AlignedBox2d(b.min() - Point2(VIEW_PICK_MARGIN_MM, VIEW_PICK_MARGIN_MM),
                                       b.max() + Point2(VIEW_PICK_MARGIN_MM, VIEW_PICK_MARGIN_MM));
    return {};
}

std::string DrawingView::viewAt(const Point2& p) const {
    for (auto it = viewBoxes_.rbegin(); it != viewBoxes_.rend(); ++it)
        if (const auto r = viewRect(it->first); !r.isEmpty() && r.contains(p)) return it->first;
    return {};
}

void DrawingView::setOverlay(std::vector<drawing::Primitive> prims) {
    overlay_ = std::move(prims);
    update();
}

void DrawingView::setSelectedView(const std::string& viewId) {
    if (selected_ == viewId) return;
    selected_ = viewId;
    // The action trace (FX14-UI-003): what is selected on the sheet — by ID.
    traceAction(viewId.empty() ? QString("sheet view selection cleared") : "sheet view selected " + QString::fromStdString(viewId));  // i18n-skip: log
    update();
}

void DrawingView::fitSheet() {
    if (g_.width <= 0 || g_.height <= 0 || width() <= 0 || height() <= 0) return;
    zoom_ = std::min((width() - 2 * FIT_MARGIN_PX) / g_.width, (height() - 2 * FIT_MARGIN_PX) / g_.height);
    zoom_ = std::clamp(zoom_, ZOOM_MIN, ZOOM_MAX);
    origin_ = QPointF((width() - g_.width * zoom_) / 2, (height() + g_.height * zoom_) / 2);
    fitted_ = true;
    update();
}

QPointF DrawingView::sheetToWidget(const Point2& p) const { return QPointF(origin_.x() + p.x() * zoom_, origin_.y() - p.y() * zoom_); }

QTransform DrawingView::sheetTransform() const { return app::sheetTransform(zoom_, origin_); }

Point2 DrawingView::widgetToSheet(const QPointF& p) const { return Point2((p.x() - origin_.x()) / zoom_, (origin_.y() - p.y()) / zoom_); }

void DrawingView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x90, 0x94, 0x9a));  // margin around the sheet
    if (g_.width <= 0) return;
    const QRectF sheet(sheetToWidget(Point2(0, g_.height)), sheetToWidget(Point2(g_.width, 0)));
    p.fillRect(sheet, Qt::white);
    const QTransform toWidget = sheetTransform();  // sheet mm → window pixels
    // The dragged view is drawn shifted (§8б).
    // With it — the views of its group (FX12-DRW-003).
    auto dragged = [&](const std::string& role) {
        if (!dragging_ || role.rfind("view:", 0) != 0) return false;
        for (const auto& v : dragGroupNow_)
            if (role.size() > v.size() + 5 && role.compare(5, v.size(), v) == 0 && role[5 + v.size()] == ':') return true;
        return false;
    };
    const std::string hidden = tool_ ? tool_->hiddenRole() : std::string();  // dimension hidden while being dragged
    for (size_t i = 0; i < g_.primitives.size(); ++i) {
        const auto& pr = g_.primitives[i];
        if (!hidden.empty() && pr.role == hidden) continue;
        if (pr.penOnly) continue;  // the view "as on the drawing" for pen outputs (drawing.md §3, §6.22)
        const Point2 off = dragged(pr.role) ? dragDelta_ : Point2(0, 0);
        auto W = [&](const Point2& q) { return sheetToWidget(q + off); };
        // Set in the drawing (FX9-DRW-009); a cut plan's colors (drawing.md §2.7, FX11-CUT-002).
        const QColor ink = pr.local ? COLOR_DRAWING_LOCAL.color : primitiveColor(pr);
        if (paintSheetPrimitive(p, pr, zoom_, W, SheetInk{ink, MIN_PEN_PX, Qt::BevelJoin})) continue;
        switch (pr.kind) {
            case drawing::PrimitiveKind::Image: {  // shaded view (drawing.md §6.22)
                if (images_[i].isNull() || pr.points.size() < 2) break;
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.drawImage(QRectF(W(Point2(pr.points[0].x(), pr.points[1].y())), W(Point2(pr.points[1].x(), pr.points[0].y()))), images_[i]);
                p.restore();
                break;
            }
            case drawing::PrimitiveKind::Text: {
                if (!textPaths_[i].isEmpty()) {  // with font outlines, as in PDF and SVG (§8б)
                    p.fillPath(toWidget.map(textPaths_[i].translated(off.x(), off.y())), primitiveColor(pr));  // view dragging — with an offset
                    break;
                }
                QFont f = font();
                f.setPixelSize(std::max(1, int(std::lround(pr.height * zoom_))));
                p.setFont(f);
                p.setPen(ink);
                p.save();
                p.translate(W(pr.position));
                p.rotate(-pr.angle * 180 / std::numbers::pi);
                p.drawText(QPointF(0, 0), QString::fromStdString(pr.text));
                p.restore();
                break;
            }
            default: break;
        }
    }
    // Tool preview (§8б): lines and circles in the selection color.
    for (const auto& pr : overlay_) {
        QPen pen(pr.role == "hover" ? COLOR_HOVER.color : COLOR_SELECTED.color);  // hover — the element under the cursor
        pen.setWidthF(1.5);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        if (pr.kind == drawing::PrimitiveKind::Line && pr.points.size() == 2) p.drawLine(sheetToWidget(pr.points[0]), sheetToWidget(pr.points[1]));
        if (pr.kind == drawing::PrimitiveKind::Circle) p.drawEllipse(sheetToWidget(pr.center), pr.radius * zoom_, pr.radius * zoom_);
    }
    // Frame of the selected view (§8б): dashed, selection color.
    if (const auto r = viewRect(selected_); !r.isEmpty()) {
        const Point2 off = dragging_ && std::find(dragGroupNow_.begin(), dragGroupNow_.end(), selected_) != dragGroupNow_.end() ? dragDelta_ : Point2(0, 0);
        QPen pen(COLOR_SELECTED.color);
        pen.setWidthF(1.5);
        pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(sheetToWidget(Point2(r.min().x(), r.max().y()) + off), sheetToWidget(Point2(r.max().x(), r.min().y()) + off)));
    }
    if (tool_) tool_->paint(p);
}

void DrawingView::wheelEvent(QWheelEvent* e) {
    const double k = e->angleDelta().y() > 0 ? WHEEL_ZOOM_STEP : 1 / WHEEL_ZOOM_STEP;
    const double nz = std::clamp(zoom_ * k, ZOOM_MIN, ZOOM_MAX);
    const Point2 at = widgetToSheet(e->position());  // the point under the cursor stays in place
    zoom_ = nz;
    const QPointF w = sheetToWidget(at);
    origin_ += e->position() - w;
    update();
    e->accept();
}

void DrawingView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton) {
        panning_ = true;
        panFrom_ = e->position().toPoint();
        e->accept();
        return;
    }
    if (e->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        if (tool_ && tool_->press(widgetToSheet(e->position()))) {
            toolGesture_ = true;
            e->accept();
            return;
        }
        leftDown_ = true;
        dragging_ = false;
        pressAt_ = e->position().toPoint();
        pressView_ = toolActive_ ? std::string() : viewAt(widgetToSheet(e->position()));
        dragDelta_ = Point2(0, 0);
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void DrawingView::mouseMoveEvent(QMouseEvent* e) {
    if (panning_) {
        const QPoint p = e->position().toPoint();
        origin_ += QPointF(p - panFrom_);
        panFrom_ = p;
        update();
        return;
    }
    if (!leftDown_ && !toolGesture_) emit sheetHovered(widgetToSheet(e->position()));
    if (tool_ && (toolGesture_ || !leftDown_)) {
        tool_->move(widgetToSheet(e->position()), toolGesture_);
        return;
    }
    if (leftDown_ && !pressView_.empty()) {
        const QPoint p = e->position().toPoint();
        if (!dragging_ && (p - pressAt_).manhattanLength() > DRAG_THRESHOLD_PX) {
            dragging_ = true;
            dragGroupNow_ = dragGroup_ ? dragGroup_(pressView_) : std::vector<std::string>{};
            if (std::find(dragGroupNow_.begin(), dragGroupNow_.end(), pressView_) == dragGroupNow_.end()) dragGroupNow_.push_back(pressView_);
        }
        if (dragging_) {
            const Point2 d = widgetToSheet(p) - widgetToSheet(pressAt_);
            dragDelta_ = constrain_ ? constrain_(pressView_, d) : d;
            update();
        }
    }
}

void DrawingView::mouseDoubleClickEvent(QMouseEvent* e) {
    // Handled: no press is simulated, so the release that may follow is not a click (leftDown_ stays false).
    if (e->button() == Qt::LeftButton && doubleClick_ && doubleClick_(widgetToSheet(e->position()))) {
        e->accept();
        return;
    }
    mousePressEvent(e);
}

void DrawingView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton) panning_ = false;
    if (e->button() == Qt::LeftButton && toolGesture_) {
        toolGesture_ = false;
        if (tool_) tool_->release(widgetToSheet(e->position()));
        return;
    }
    if (e->button() != Qt::LeftButton || !leftDown_) return;
    leftDown_ = false;
    if (dragging_) {
        dragging_ = false;
        const Point2 d = dragDelta_;
        dragDelta_ = Point2(0, 0);
        update();
        emit viewDragged(QString::fromStdString(pressView_), d);
        return;
    }
    emit sheetClicked(widgetToSheet(e->position()));
}

void DrawingView::contextMenuEvent(QContextMenuEvent* e) { emit contextRequested(widgetToSheet(e->pos()), e->globalPos()); }

void DrawingView::keyPressEvent(QKeyEvent* e) {
    if (tool_ && tool_->key(isDeleteKey(e) ? int(Qt::Key_Delete) : e->key())) {  // macOS «delete» (keys.h)
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        emit escapePressed();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

void DrawingView::resizeEvent(QResizeEvent*) { fitSheet(); }

}  // namespace construe::app
