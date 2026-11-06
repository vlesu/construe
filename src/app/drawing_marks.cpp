// Sheet marks on a drawing (spec/app-ux.md §8б «Annotations», drawing.md §6.25, FX9-DRW-008).
#include "app/drawing_marks.h"

#include "app/messages.h"
#include "app/theme.h"
#include "base/geom2.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace construe::app {

namespace {

// Distance from p to the primitive (texts — to the box from the baseline to the height, 0.7·h per character).
double primDist(const drawing::Primitive& pr, const Point2& p) {
    switch (pr.kind) {
        case drawing::PrimitiveKind::Line:
        case drawing::PrimitiveKind::Polyline: {
            double best = 1e300;
            const size_t n = pr.points.size();
            for (size_t i = 0; i + 1 < n; ++i) best = std::min(best, geom2::distToSegment(p, pr.points[i], pr.points[i + 1]));
            if (pr.closed && n > 2) best = std::min(best, geom2::distToSegment(p, pr.points[n - 1], pr.points[0]));
            if (n == 1) best = (p - pr.points[0]).norm();
            return best;
        }
        case drawing::PrimitiveKind::Circle:
        case drawing::PrimitiveKind::Arc: return std::abs((p - pr.center).norm() - pr.radius);
        case drawing::PrimitiveKind::Text: {
            size_t chars = 0;
            for (unsigned char ch : pr.text) chars += (ch & 0xC0) != 0x80;
            const double w = 0.7 * pr.height * double(chars);
            const Point2 q = p - pr.position;
            const double dx = std::max({-q.x() - w, q.x() - w, 0.0}), dy = std::max({-q.y() - pr.height, q.y() - pr.height, 0.0});
            return std::hypot(dx, dy);
        }
        case drawing::PrimitiveKind::Image: return 1e300;
    }
    return 1e300;
}

}  // namespace

SheetMarkTool::SheetMarkTool(DrawingView* view, Hooks hooks, QObject* parent) : QObject(parent), view_(view), h_(std::move(hooks)) {}

const drawing::MarkPlacement* SheetMarkTool::placement(const std::string& id) const {
    for (const auto& m : view_->graphics().marks)
        if (m.mark == id) return &m;
    return nullptr;
}

void SheetMarkTool::select(const std::string& id) {
    if (id == selected_) return;
    selected_ = id;
    if (!id.empty() && h_.selected) h_.selected();
    view_->update();
}

std::string SheetMarkTool::markAt(const Point2& at) const {
    const double tol = safeDiv(PICK_CURVE_PX, view_->zoom());
    const auto& g = view_->graphics();
    std::string best;
    double bestD = tol;
    for (const auto& m : g.marks)
        for (const auto& pr : g.primitives) {
            if (std::find(m.roles.begin(), m.roles.end(), pr.role) == m.roles.end()) continue;
            const double d = primDist(pr, at);
            if (d <= bestD) {  // the later one wins at a tie (drawn on top)
                bestD = d;
                best = m.mark;
            }
        }
    return best;
}

void SheetMarkTool::refresh() {
    if (!selected_.empty() && !placement(selected_)) selected_.clear();
    view_->update();
}

bool SheetMarkTool::escape() {
    if (selected_.empty()) return false;
    select({});
    return true;
}

bool SheetMarkTool::press(const Point2& at) {
    if (view_->toolActive()) return false;  // a sheet tool picks under the sign (FX11-DRW-001: the line of a face)
    const std::string id = markAt(at);
    if (id.empty()) return false;
    select(id);
    pressed_ = true;
    dragging_ = false;
    pressAt_ = at;
    dragDelta_ = Point2(0, 0);
    return true;
}

void SheetMarkTool::move(const Point2& at, bool leftDown) {
    if (!leftDown || !pressed_) return;
    if (!dragging_ && (at - pressAt_).norm() * view_->zoom() > DRAG_THRESHOLD_PX) dragging_ = true;
    if (!dragging_) return;
    dragDelta_ = at - pressAt_;
    view_->update();
}

void SheetMarkTool::release(const Point2&) {
    pressed_ = false;
    if (!dragging_) return;
    dragging_ = false;
    const Point2 d = dragDelta_;
    dragDelta_ = Point2(0, 0);
    view_->update();
    const drawing::MarkPlacement* m = placement(selected_);
    if (!m) return;
    // The sign follows the cursor; the leader end is computed again at the build (drawing.md §6.25).
    const Point2 pos = m->anchor + d;
    if (auto s = h_.session().updateMark(selected_, api::Json{{"position", {pos.x(), pos.y()}}}); !s) h_.toast(errorText(s.error()), true);
}

bool SheetMarkTool::key(int k) {
    if (selected_.empty() || k != Qt::Key_Delete) return false;
    const std::string id = selected_;
    select({});
    if (auto s = h_.session().deleteMark(id); !s) h_.toast(errorText(s.error()), true);
    return true;
}

void SheetMarkTool::paint(QPainter& p) {
    const drawing::MarkPlacement* m = placement(selected_);
    if (!m) return;
    // The sign of the selected mark in the selection color, shifted while dragging (the weld image stays in place).
    QPen pen(COLOR_SELECTED.color, 2);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    for (const auto& pr : view_->graphics().primitives) {
        const auto it = std::find(m->roles.begin(), m->roles.end(), pr.role);
        if (it == m->roles.end()) continue;
        const Point2 off = dragging_ && it == m->roles.begin() ? dragDelta_ : Point2(0, 0);
        auto w = [&](const Point2& q) { return view_->sheetToWidget(q + off); };
        switch (pr.kind) {
            case drawing::PrimitiveKind::Line:
            case drawing::PrimitiveKind::Polyline: {
                QPainterPath path;
                for (size_t i = 0; i < pr.points.size(); ++i) i == 0 ? path.moveTo(w(pr.points[i])) : path.lineTo(w(pr.points[i]));
                if (pr.closed) path.closeSubpath();
                p.drawPath(path);
                break;
            }
            case drawing::PrimitiveKind::Circle:
            case drawing::PrimitiveKind::Arc: {
                const double r = pr.radius * view_->zoom();
                p.drawEllipse(w(pr.center), r, r);
                break;
            }
            case drawing::PrimitiveKind::Text: {
                size_t chars = 0;
                for (unsigned char ch : pr.text) chars += (ch & 0xC0) != 0x80;
                const QPointF a = w(pr.position), b = w(pr.position + Point2(0.7 * pr.height * double(chars), pr.height));
                p.drawLine(a, QPointF(b.x(), a.y()));  // underline of the label
                break;
            }
            case drawing::PrimitiveKind::Image: break;
        }
    }
}

std::string SheetMarkTool::hiddenRole() const {
    if (!dragging_) return {};
    const drawing::MarkPlacement* m = placement(selected_);
    return m && !m->roles.empty() ? m->roles.front() : std::string();
}

}  // namespace construe::app
