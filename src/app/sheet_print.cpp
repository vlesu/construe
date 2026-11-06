// Painting drawing sheets for printing (spec/app-ux.md §8б «Печать» ("Print"), FX7-DRW-004).
#include "app/sheet_print.h"

#include "app/messages.h"

#include "api/session.h"
#include "drawing/render.h"

#include <QPageSize>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace construe::app {

QImage rasterQImage(const drawing::RasterImage& img) {
    if (img.width <= 0 || img.height <= 0 || img.rgba.size() < size_t(img.width) * size_t(img.height) * 4) return {};
    return QImage(img.rgba.data(), img.width, img.height, img.width * 4, QImage::Format_RGBA8888).copy();
}

QPainterPath sheetTextPath(const drawing::Primitive& pr) {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    auto o = api::Session::textOutline(pr);
    if (!o) return path;
    auto Q = [](const Point2& q) { return QPointF(q.x(), q.y()); };
    for (const auto& c : o->contours) {
        if (c.empty()) continue;
        const auto oc = drawing::orientContour(c);  // arcs follow the traversal direction (drawing.md §3.1)
        path.moveTo(Q(oc.front().from));
        for (const auto& cv : oc) {
            if (!cv.arc) {
                path.lineTo(Q(cv.to));
            } else {
                for (const auto& b : drawing::arcBeziers(cv)) path.cubicTo(Q(b[1]), Q(b[2]), Q(b[3]));
            }
        }
        path.closeSubpath();
    }
    return path;
}

Point2 textLocal(const drawing::Primitive& pr, const Point2& p) {
    const double c = std::cos(-pr.angle), s = std::sin(-pr.angle);
    const Point2 d = p - pr.position;
    return Point2(c * d.x() - s * d.y(), s * d.x() + c * d.y());
}

std::optional<std::pair<double, double>> textExtentAlong(const drawing::Primitive& pr) {
    auto o = api::Session::textOutline(pr);
    if (!o) return std::nullopt;
    std::optional<std::pair<double, double>> r;
    for (const auto& cont : o->contours)
        for (const auto& cv : cont)
            for (const Point2& v : {cv.a, cv.b}) {
                const double x = textLocal(pr, v).x();
                r = r ? std::pair(std::min(r->first, x), std::max(r->second, x)) : std::pair(x, x);
            }
    return r;
}

bool drawSheetGeometry(QPainter& p, const drawing::Primitive& pr, double pxPerMm, const std::function<QPointF(const Point2&)>& W) {
    using K = drawing::PrimitiveKind;
    switch (pr.kind) {
        case K::Line:
            if (pr.points.size() == 2) p.drawLine(W(pr.points[0]), W(pr.points[1]));
            return true;
        case K::Polyline: {
            QPolygonF poly;
            for (const auto& q : pr.points) poly << W(q);
            if (pr.closed) p.drawPolygon(poly);
            else p.drawPolyline(poly);
            return true;
        }
        case K::Circle: {
            const double r = pr.radius * pxPerMm;
            p.drawEllipse(W(pr.center), r, r);
            return true;
        }
        case K::Arc: {
            const double r = pr.radius * pxPerMm;
            const QPointF c = W(pr.center);
            double span = (pr.end - pr.start) * 180 / std::numbers::pi;
            while (span <= 0) span += 360;
            p.drawArc(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), int(std::lround(pr.start * 180 / std::numbers::pi * 16)),
                      int(std::lround(span * 16)));
            return true;
        }
        default: return false;
    }
}

bool paintSheetPrimitive(QPainter& p, const drawing::Primitive& pr, double pxPerMm, const std::function<QPointF(const Point2&)>& W,
                         const SheetInk& ink) {
    using K = drawing::PrimitiveKind;
    if (pr.kind != K::Line && pr.kind != K::Polyline && pr.kind != K::Circle && pr.kind != K::Arc) return false;
    const double wMm = pr.weight == drawing::Weight::Thick ? drawing::LINE_THICK_MM : drawing::LINE_THIN_MM;
    QPen pen(ink.color);
    pen.setWidthF(std::max(ink.minPenPx, wMm * pxPerMm));
    pen.setCapStyle(Qt::FlatCap);
    pen.setJoinStyle(ink.join);
    if (const auto d = drawing::dashPattern(pr.lineType); !d.empty()) {
        QVector<qreal> dp;
        for (double mm : d) dp << mm / wMm;  // QPen dashes — in line widths
        pen.setDashPattern(dp);
    }
    p.setPen(pen);
    p.setBrush(pr.filled ? QBrush(ink.color) : QBrush(Qt::NoBrush));
    return drawSheetGeometry(p, pr, pxPerMm, W);
}

void applyCutLabelFormat(api::Session& s) {
    const std::map<std::string, QString> keep{{"material", QStringLiteral("{material}")}, {"thickness", QStringLiteral("{thickness}")}};
    s.setCutLabelFormat(text("ui.drawing.cutLabel", keep).toStdString(), text("ui.drawing.cutLabelNoMaterial", keep).toStdString(),
                               language() == "ru" ? "," : ".");
}

QColor primitiveColor(const drawing::Primitive& pr) {
    const QColor c(QString::fromStdString(pr.color));
    return pr.color.empty() || !c.isValid() ? QColor(Qt::black) : c;
}

void paintSheet(QPainter& p, const drawing::SheetGraphics& g, double pxPerMm, const QPointF& origin) {
    auto W = [&](const Point2& q) { return QPointF(origin.x() + q.x() * pxPerMm, origin.y() - q.y() * pxPerMm); };
    const QTransform toDevice = sheetTransform(pxPerMm, origin);  // the same mapping as W
    p.setRenderHint(QPainter::Antialiasing);
    for (const auto& pr : g.primitives) {
        const SheetInk ink{primitiveColor(pr), 0, Qt::MiterJoin};  // a cut plan's colors (drawing.md §2.7)
        if (pr.kind == drawing::PrimitiveKind::Text) {
            if (const QPainterPath path = sheetTextPath(pr); !path.isEmpty()) p.fillPath(toDevice.map(path), ink.color);
            continue;
        }
        if (pr.penOnly) continue;  // the view "as on the drawing" for pen outputs (drawing.md §3, §6.22)
        if (pr.kind == drawing::PrimitiveKind::Image) {  // shaded view (drawing.md §6.22)
            drawImagePrimitive(p, pr, W);
            continue;
        }
        paintSheetPrimitive(p, pr, pxPerMm, W, ink);
    }
}

bool printSheets(QPrinter& printer, const std::vector<drawing::SheetGraphics>& sheets) {
    if (sheets.empty()) return false;
    printer.setFullPage(true);
    auto pageOf = [](const drawing::SheetGraphics& g) {
        return QPageSize(QSizeF(g.width, g.height), QPageSize::Millimeter, QString(), QPageSize::ExactMatch);
    };
    printer.setPageOrientation(QPageLayout::Portrait);
    printer.setPageSize(pageOf(sheets.front()));
    printer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter);
    QPainter p;
    if (!p.begin(&printer)) return false;
    for (size_t i = 0; i < sheets.size(); ++i) {
        const auto& g = sheets[i];
        if (i > 0) {
            printer.setPageSize(pageOf(g));
            if (!printer.newPage()) return false;
        }
        const double pxPerMm = printer.resolution() / 25.4;
        paintSheet(p, g, pxPerMm, QPointF(0, g.height * pxPerMm));
    }
    return p.end();
}

}  // namespace construe::app
