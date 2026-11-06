// Painting drawing sheets with QPainter for printing (spec/app-ux.md §8б «Печать» ("Print"), FX7-DRW-004): the same
// primitives as PDF export (io.md §2в) — lines with GOST 2.303 widths and dashes, texts as glyph outlines, images of
// shaded views (pen-only primitives are skipped, drawing.md §3).
#pragma once
#include "drawing/sheet.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPrinter>

#include <QTransform>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace construe::api {
class Session;
}

namespace construe::app {

// Image of a shaded view (drawing.md §6.22) as a QImage (a deep copy).
QImage rasterQImage(const drawing::RasterImage& img);
// Draws an image primitive into the sheet rectangle of its corners; W — sheet mm → device.
template <class ToDevice>
void drawImagePrimitive(QPainter& p, const drawing::Primitive& pr, const ToDevice& W) {
    if (!pr.image || pr.points.size() < 2) return;
    const QRectF r(W(Point2(pr.points[0].x(), pr.points[1].y())), W(Point2(pr.points[1].x(), pr.points[0].y())));
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(r, rasterQImage(*pr.image));
    p.restore();
}

// Text outlines of a text primitive in sheet mm (drawing.md §3.1), the winding fill rule; the font is not loaded — an
// empty path.
QPainterPath sheetTextPath(const drawing::Primitive& pr);

// Sheet mm → device as a transform: (x, y) → origin + (x, −y)·pxPerMm (for paths in sheet mm).
inline QTransform sheetTransform(double pxPerMm, const QPointF& origin) { return QTransform(pxPerMm, 0, 0, -pxPerMm, origin.x(), origin.y()); }
// A sheet point in the axes of a text primitive: origin — its position, x — along the baseline (angle).
Point2 textLocal(const drawing::Primitive& pr, const Point2& p);
// Extent [x0, x1] of a text primitive along its baseline by glyph outlines (drawing.md §3.1), in textLocal axes;
// nullopt — no outlines (the font is not loaded, no glyphs). Across the baseline a text spans [0, height] (as a dimension label).
std::optional<std::pair<double, double>> textExtentAlong(const drawing::Primitive& pr);

// Draws the geometry of a line, polyline, circle or arc primitive with the painter's pen and brush; W — sheet mm →
// device, pxPerMm — for radii. Other kinds — false, nothing drawn.
bool drawSheetGeometry(QPainter& p, const drawing::Primitive& pr, double pxPerMm, const std::function<QPointF(const Point2&)>& W);

// How a sheet primitive is inked (S7F-RF-013): color, the thinnest pen in device pixels, the line join.
struct SheetInk {
    QColor color = Qt::black;
    double minPenPx = 0;
    Qt::PenJoinStyle join = Qt::BevelJoin;
};
// Draws a line, polyline, circle or arc primitive (drawing.md §3) — the common part of the screen and the print: pen of
// the GOST 2.303 width of its weight (pxPerMm pixels per mm, not thinner than ink.minPenPx), dashes of its line type,
// flat caps; filled primitives (dimension arrows) are filled with the ink; then drawSheetGeometry. Texts and images are
// not drawn (false): their callers draw them.
bool paintSheetPrimitive(QPainter& p, const drawing::Primitive& pr, double pxPerMm, const std::function<QPointF(const Point2&)>& W,
                         const SheetInk& ink);

// Label format of cut plan parts in the interface language (drawing.md §2.7, FX11-CUT-002): templates and the decimal
// separator — Russian: comma, otherwise a point.
void applyCutLabelFormat(api::Session& s);
// Default check of a drawing or specification in the choice of drawings (app-ux §8б, FX11-CUT-006): a cut plan — checked
// only for DXF; others — the active element only.
inline bool drawingPickChecked(bool cutPlan, bool active, bool forDxf) { return cutPlan ? forDxf : active; }
// Color of a primitive (drawing.md §3 color, §2.7): its "#RRGGBB", none — black.
QColor primitiveColor(const drawing::Primitive& pr);
// Sheet mm → device: a point (x, y) of the sheet goes to origin + (x, −y)·pxPerMm (origin — the lower-left corner).
void paintSheet(QPainter& p, const drawing::SheetGraphics& g, double pxPerMm, const QPointF& origin);
// Each sheet on its own page of the sheet size (full page, no margins); false — the printer did not start.
bool printSheets(QPrinter& printer, const std::vector<drawing::SheetGraphics>& sheets);

}  // namespace construe::app
