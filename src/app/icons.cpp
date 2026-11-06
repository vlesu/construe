#include "app/icons.h"

#include "app/theme.h"

#include <QFile>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <tuple>

namespace construe::app {

QIcon appIcon() {
    QIcon icon;
    for (int s : APP_ICON_SIZES) {
        const QString path = QString(":/appicon/construe-%1.png").arg(s);
        if (QFile::exists(path)) icon.addFile(path, QSize(s, s));
    }
    return icon;
}

namespace {

// Drawing in a 24×24 coordinate system (scaled to the pixmap size).
using Draw = std::function<void(QPainter&, const QPen& ink, const QPen& accent)>;

QPixmap render(const Draw& draw, int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(size / 24.0, size / 24.0);
    QPen ink(COLOR_ICON.color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPen accent(COLOR_ICON_ACCENT.color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setBrush(Qt::NoBrush);
    draw(p, ink, accent);
    return pm;
}

void dot(QPainter& p, const QPen& pen, QPointF c, double r = 1.6) {
    p.setPen(Qt::NoPen);
    p.setBrush(pen.color());
    p.drawEllipse(c, r, r);
    p.setBrush(Qt::NoBrush);
}

// Display style cube (app-ux §3.4а): face fill, visible edges, hidden edges dashed or solid.
void styleCube(QPainter& p, const QPen& ink, const QColor* fill, bool edges, int hidden) {
    const QPolygonF top{{12, 3}, {20, 7}, {12, 11}, {4, 7}}, left{{4, 7}, {12, 11}, {12, 20}, {4, 16}},
        right{{12, 11}, {20, 7}, {20, 16}, {12, 20}};
    if (hidden) {  // 1 — dashed, 2 — solid
        QPen h = ink;
        h.setWidthF(1.0);
        if (hidden == 1) h.setStyle(Qt::DashLine);
        p.setPen(h);
        p.drawLine(QPointF(12, 12), QPointF(4, 16)), p.drawLine(QPointF(12, 12), QPointF(20, 16)), p.drawLine(QPointF(12, 12), QPointF(12, 3));
    }
    p.setPen(edges ? ink : QPen(Qt::NoPen));
    for (const auto* f : {&top, &left, &right}) {
        if (fill) p.setBrush(f == &top ? fill->lighter(115) : f == &left ? *fill : fill->darker(115));
        p.drawPolygon(*f);
    }
    p.setBrush(Qt::NoBrush);
}

// A part as a solid on an icon (app-ux §2.1 «Icons», S7F-UI-005): an isometric block with gray faces — the top lighter,
// the right darker — and edges of `edge`. (cx, top) — the top corner; a — the half width; h — the height. Returns the
// outline of the block (for overlaps).
QPolygonF solidBlock(QPainter& p, const QPen& edge, double cx, double top, double a, double h, int alpha = 255) {
    const double b = a / 2;
    const QPolygonF up{{cx, top}, {cx + a, top + b}, {cx, top + 2 * b}, {cx - a, top + b}},
        left{{cx - a, top + b}, {cx, top + 2 * b}, {cx, top + 2 * b + h}, {cx - a, top + b + h}},
        right{{cx, top + 2 * b}, {cx + a, top + b}, {cx + a, top + b + h}, {cx, top + 2 * b + h}};
    QColor c = COLOR_ICON_SOLID.color;
    c.setAlpha(alpha);
    QPen pen = edge;
    pen.setWidthF(std::min(edge.widthF(), a < 6 ? 1.1 : 1.3));
    p.setPen(pen);
    for (const auto* f : {&up, &left, &right}) {
        p.setBrush(f == &up ? c.lighter(125) : f == &left ? c : c.darker(125));
        p.drawPolygon(*f);
    }
    p.setBrush(Qt::NoBrush);
    return QPolygonF{{cx, top}, {cx + a, top + b}, {cx + a, top + b + h}, {cx, top + 2 * b + h}, {cx - a, top + b + h}, {cx - a, top + b}};
}

// The «+» of the «new …» icons at the lower right corner.
void plusMark(QPainter& p, const QPen& ac) {
    QPen plus = ac;
    plus.setWidthF(2.2);
    p.setPen(plus);
    p.drawLine(QPointF(19, 15), QPointF(19, 23)), p.drawLine(QPointF(15, 19), QPointF(23, 19));
}

const std::map<QString, Draw>& drawers() {
    static const std::map<QString, Draw> m = {
        {"style_shadedEdges", [](QPainter& p, const QPen& ink, const QPen&) {
             const QColor c("#9AA6B2");
             styleCube(p, ink, &c, true, 0);
         }},
        {"style_shaded", [](QPainter& p, const QPen& ink, const QPen&) {
             const QColor c("#9AA6B2");
             styleCube(p, ink, &c, false, 0);
         }},
        {"style_hiddenRemoved", [](QPainter& p, const QPen& ink, const QPen&) {
             const QColor c(Qt::white);
             styleCube(p, ink, &c, true, 0);
         }},
        {"style_hiddenDashed", [](QPainter& p, const QPen& ink, const QPen&) { styleCube(p, ink, nullptr, true, 1); }},
        {"style_wireframe", [](QPainter& p, const QPen& ink, const QPen&) { styleCube(p, ink, nullptr, true, 2); }},
        {"interference", [](QPainter& p, const QPen& ink, const QPen&) {  // two overlapping solid blocks, the common part red
             const QPolygonF back = solidBlock(p, ink, 14, 2, 7, 8);
             const QPolygonF front = solidBlock(p, ink, 9, 8, 7, 8);
             QPainterPath a, b;
             a.addPolygon(back), b.addPolygon(front);
             p.setPen(Qt::NoPen);
             p.setBrush(QColor(214, 40, 40, 150));
             p.drawPath(a.intersected(b));
             p.setBrush(Qt::NoBrush);
         }},
        {"sectionAsm", [](QPainter& p, const QPen& ink, const QPen& ac) {  // same as «Section»
             p.setPen(ink);
             p.setBrush(COLOR_ICON_SOLID.color);  // the kept part of the solid (S7F-UI-005)
             p.drawPolygon(QPolygonF{{4, 9}, {10, 6}, {10, 18}, {4, 21}});
             p.setBrush(Qt::NoBrush);
             p.drawLine(QPointF(10, 6), QPointF(14, 8));
             p.setPen(ac);
             p.setBrush(QColor(242, 140, 40, 90));
             p.drawPolygon(QPolygonF{{10, 6}, {20, 11}, {20, 21}, {10, 18}});
             p.setBrush(Qt::NoBrush);
         }},
        {"asmProperties", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawRect(QRectF(4, 3, 16, 18));
             p.setPen(ac);
             for (int y = 8; y <= 16; y += 4) p.drawLine(QPointF(7, y), QPointF(17, y));
         }},
        {"mate", [](QPainter& p, const QPen& ink, const QPen& ac) {  // two solid parts pressed together face to face
             solidBlock(p, ink, 7, 6, 5, 9);
             solidBlock(p, ink, 17, 6, 5, 9);
             p.setPen(ac);
             p.drawLine(QPointF(12, 3), QPointF(12, 22));
         }},
        {"weld", [](QPainter& p, const QPen& ink, const QPen& ac) {  // tee joint with a fillet weld (S5-WELD-001)
             p.setPen(ink);
             p.drawRect(QRectF(3, 15, 18, 5));
             p.drawRect(QRectF(9, 4, 4, 11));
             p.setPen(ac);
             p.setBrush(ac.color());
             p.drawPolygon(QPolygonF({QPointF(13, 15), QPointF(18, 15), QPointF(13, 10)}));
             p.drawPolygon(QPolygonF({QPointF(9, 15), QPointF(4, 15), QPointF(9, 10)}));
             p.setBrush(Qt::NoBrush);
         }},
        {"assembly", [](QPainter& p, const QPen& ink, const QPen&) {  // two solid blocks, one before the other
             solidBlock(p, ink, 15, 2, 6.5, 7);
             solidBlock(p, ink, 9, 9, 6.5, 7);
         }},
        {"newAssembly", [](QPainter& p, const QPen& ink, const QPen& ac) {
             solidBlock(p, ink, 12, 1, 5.5, 6);
             solidBlock(p, ink, 7, 7, 5.5, 6);
             plusMark(p, ac);
         }},
        {"drawing", [](QPainter& p, const QPen& ink, const QPen& ac) {  // sheet with a title block (stage 3)
             p.setPen(ink);
             p.drawRect(QRectF(3, 4, 18, 16));
             p.setPen(ac);
             p.drawRect(QRectF(12, 15, 9, 5));
         }},
        {"newDrawing", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawRect(QRectF(3, 3, 14, 13));
             p.drawRect(QRectF(10, 12, 7, 4));
             p.setPen(ac);
             p.drawLine(QPointF(19, 15), QPointF(19, 23)), p.drawLine(QPointF(15, 19), QPointF(23, 19));
         }},
        {"specification", [](QPainter& p, const QPen& ink, const QPen& ac) {  // sheet with table rows (S3-SPEC-001)
             p.setPen(ink);
             p.drawRect(QRectF(5, 3, 14, 18));
             p.drawLine(QPointF(9, 3), QPointF(9, 21));
             p.setPen(ac);
             for (double y : {8.0, 12.0, 16.0}) p.drawLine(QPointF(5, y), QPointF(19, y));
         }},
        {"newSpecification", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawRect(QRectF(3, 3, 13, 15));
             p.drawLine(QPointF(7, 3), QPointF(7, 18));
             for (double y : {7.0, 11.0, 15.0}) p.drawLine(QPointF(3, y), QPointF(16, y));
             p.setPen(ac);
             p.drawLine(QPointF(19, 15), QPointF(19, 23)), p.drawLine(QPointF(15, 19), QPointF(23, 19));
         }},
        {"sheet", [](QPainter& p, const QPen& ink, const QPen&) {  // sheet
             p.setPen(ink);
             p.drawRect(QRectF(5, 3, 14, 18));
         }},
        {"addSheet", [](QPainter& p, const QPen& ink, const QPen& ac) {  // sheet with a plus
             p.setPen(ink);
             p.drawRect(QRectF(3, 2, 12, 16));
             p.setPen(ac);
             p.drawLine(QPointF(19, 15), QPointF(19, 23)), p.drawLine(QPointF(15, 19), QPointF(23, 19));
         }},
        {"titleBlock", [](QPainter& p, const QPen& ink, const QPen& ac) {  // sheet with a highlighted title block (§8б)
             p.setPen(ink);
             p.drawRect(QRectF(3, 3, 18, 18));
             p.setPen(ac);
             p.drawRect(QRectF(9, 14, 12, 7));
             p.drawLine(QPointF(9, 17.5), QPointF(21, 17.5)), p.drawLine(QPointF(15, 14), QPointF(15, 21));
         }},
        {"view", [](QPainter& p, const QPen& ink, const QPen& ac) {  // drawing view: part outline in a frame (S3-DRW-003)
             p.setPen(ink);
             p.drawRect(QRectF(3, 4, 18, 16));
             p.setPen(ac);
             p.drawRect(QRectF(7, 9, 10, 6));
         }},
        {"baseView", [](QPainter& p, const QPen& ink, const QPen& ac) {  // base view: a block seen "from the front" on a sheet
             p.setPen(ink);
             p.drawRect(QRectF(2, 3, 20, 18));
             p.setPen(ac);
             p.drawRect(QRectF(6, 8, 12, 8));
             p.drawLine(QPointF(10, 8), QPointF(10, 16));
         }},
        {"projectedView", [](QPainter& p, const QPen& ink, const QPen& ac) {  // projected: the base view and a view below it
             p.setPen(ink);
             p.drawRect(QRectF(4, 2, 12, 7));
             p.setPen(ac);
             p.drawRect(QRectF(4, 15, 12, 7));
             p.drawLine(QPointF(10, 10), QPointF(10, 14));
             p.drawLine(QPointF(8, 12), QPointF(10, 14)), p.drawLine(QPointF(12, 12), QPointF(10, 14));
         }},
        {"sectionView", [](QPainter& p, const QPen& ink, const QPen& ac) {  // section: section line with arrows and hatching (S3-DRW-005)
             p.setPen(ink);
             p.drawRect(QRectF(5, 7, 14, 10));
             p.drawLine(QPointF(8, 17), QPointF(15, 10)), p.drawLine(QPointF(12, 17), QPointF(19, 10));
             p.setPen(ac);
             p.drawLine(QPointF(2, 3), QPointF(2, 21));
             p.drawLine(QPointF(2, 3), QPointF(5, 3)), p.drawLine(QPointF(2, 21), QPointF(5, 21));
             p.drawLine(QPointF(22, 3), QPointF(22, 21));
             p.drawLine(QPointF(22, 3), QPointF(19, 3)), p.drawLine(QPointF(22, 21), QPointF(19, 21));
         }},
        {"detailView", [](QPainter& p, const QPen& ink, const QPen& ac) {  // detail view: a circle with a leader shelf and a magnified view
             p.setPen(ink);
             p.drawEllipse(QPointF(7, 15), 5, 5);
             p.drawLine(QPointF(10.5, 11.5), QPointF(14, 8)), p.drawLine(QPointF(14, 8), QPointF(20, 8));
             p.setPen(ac);
             p.drawEllipse(QPointF(17, 17), 5, 5);
         }},
        {"drawingDimension", [](QPainter& p, const QPen& ink, const QPen& ac) {  // drawing dimension: extension lines, dimension line with arrows (S3-DRW-006)
             p.setPen(ink);
             p.drawLine(QPointF(4, 22), QPointF(4, 8)), p.drawLine(QPointF(20, 22), QPointF(20, 8));
             p.setPen(ac);
             p.drawLine(QPointF(4, 12), QPointF(20, 12));
             p.drawLine(QPointF(4, 12), QPointF(8, 10)), p.drawLine(QPointF(4, 12), QPointF(8, 14));
             p.drawLine(QPointF(20, 12), QPointF(16, 10)), p.drawLine(QPointF(20, 12), QPointF(16, 14));
             p.drawLine(QPointF(10, 3), QPointF(14, 3)), p.drawLine(QPointF(12, 3), QPointF(12, 8));
         }},
        {"threeViews", [](QPainter& p, const QPen& ink, const QPen& ac) {  // three projections: front, top (below it), left (FX1-DRW-001)
             p.setPen(ink);
             p.drawRect(QRectF(2, 2, 10, 9));
             p.setPen(ac);
             p.drawRect(QRectF(2, 14, 10, 8));
             p.drawRect(QRectF(15, 2, 7, 9));
         }},
        {"modelDimensions", [](QPainter& p, const QPen& ink, const QPen& ac) {  // model dimensions: a part and a dimension above it (FX1-DRW-003)
             solidBlock(p, ink, 12, 10, 7, 6);
             p.setPen(ac);
             p.drawLine(QPointF(5, 6), QPointF(19, 6));
             p.drawLine(QPointF(5, 6), QPointF(8, 4)), p.drawLine(QPointF(5, 6), QPointF(8, 8));
             p.drawLine(QPointF(19, 6), QPointF(16, 4)), p.drawLine(QPointF(19, 6), QPointF(16, 8));
             p.drawLine(QPointF(5, 3), QPointF(5, 13)), p.drawLine(QPointF(19, 3), QPointF(19, 13));
         }},
        {"modelMarks", [](QPainter& p, const QPen& ink, const QPen& ac) {  // a part and a roughness sign with a leader (FX9-DRW-008)
             solidBlock(p, ink, 9, 11, 6.5, 6);
             p.setPen(ac);
             p.drawLine(QPointF(13, 14), QPointF(19, 6));
             p.drawLine(QPointF(19, 6), QPointF(13, 6)), p.drawLine(QPointF(13, 6), QPointF(11, 2));
             p.drawLine(QPointF(19, 6), QPointF(21, 3)), p.drawLine(QPointF(21, 3), QPointF(23, 3));
         }},
        {"showUndimensioned", [](QPainter& p, const QPen& ink, const QPen& ac) {  // a dimension with an eye: show the unplaced (FX12-DRW-004)
             p.setPen(ink);  // the dimension: extension lines, the line with arrows
             p.drawLine(QPointF(4, 3), QPointF(4, 10)), p.drawLine(QPointF(20, 3), QPointF(20, 10));
             p.drawLine(QPointF(4, 6), QPointF(20, 6));
             p.drawLine(QPointF(4, 6), QPointF(7, 4.5)), p.drawLine(QPointF(4, 6), QPointF(7, 7.5));
             p.drawLine(QPointF(20, 6), QPointF(17, 4.5)), p.drawLine(QPointF(20, 6), QPointF(17, 7.5));
             p.setPen(ac);  // the eye
             QPainterPath eye;
             eye.moveTo(4, 16.5);
             eye.quadTo(12, 9.5, 20, 16.5);
             eye.quadTo(12, 23.5, 4, 16.5);
             p.setBrush(Qt::NoBrush);
             p.drawPath(eye);
             p.setBrush(ac.color());
             p.drawEllipse(QPointF(12, 16.5), 2.2, 2.2);
             p.setBrush(Qt::NoBrush);
         }},
        {"sheetText", [](QPainter& p, const QPen& ink, const QPen& ac) {  // text block on a sheet: letter T in a frame (FX7-DRW-006)
             p.setPen(ink);
             p.drawRect(QRectF(3, 3, 18, 18));
             p.setPen(ac);
             p.drawLine(QPointF(7, 7), QPointF(17, 7)), p.drawLine(QPointF(12, 7), QPointF(12, 18));
         }},
        {"consPitch", [](QPainter& p, const QPen& ink, const QPen& ac) {  // pitch circle through hole centers (FX14-DRW-006)
             QPen d = ac;
             d.setDashPattern({4, 1.5, 1, 1.5});
             p.setPen(d);
             p.drawEllipse(QPointF(12, 12), 7, 7);
             p.setPen(ink);
             for (const QPointF c : {QPointF(12, 5), QPointF(18.06, 15.5), QPointF(5.94, 15.5)}) p.drawEllipse(c, 2.2, 2.2);
         }},
        {"drawingBalloon", [](QPainter& p, const QPen& ink, const QPen& ac) {  // item number: a dot, a leader and a shelf with "1" (FX14-DRW-002)
             p.setPen(ink);
             p.setBrush(ink.color());
             p.drawEllipse(QPointF(5, 19), 1.6, 1.6);
             p.setBrush(Qt::NoBrush);
             p.drawLine(QPointF(5, 19), QPointF(12, 11)), p.drawLine(QPointF(12, 11), QPointF(21, 11));
             p.setPen(ac);
             p.drawLine(QPointF(16.5, 3), QPointF(16.5, 9)), p.drawLine(QPointF(15, 4.5), QPointF(16.5, 3));
         }},
        {"partsList", [](QPainter& p, const QPen& ink, const QPen& ac) {  // parts list: a table with a header (FX14-DRW-003)
             p.setPen(ink);
             p.drawRect(QRectF(3, 4, 18, 16));
             p.drawLine(QPointF(7, 4), QPointF(7, 20)), p.drawLine(QPointF(17, 4), QPointF(17, 20));
             p.setPen(ac);
             p.drawLine(QPointF(3, 9), QPointF(21, 9));
             p.setPen(ink);
             p.drawLine(QPointF(3, 14.5), QPointF(21, 14.5));
         }},
        {"print", [](QPainter& p, const QPen& ink, const QPen& ac) {  // printer with a sheet (FX7-DRW-004)
             p.setPen(ink);
             p.drawRect(QRectF(3, 9, 18, 8));
             p.drawRect(QRectF(7, 3, 10, 6));
             p.setPen(ac);
             p.drawRect(QRectF(7, 14, 10, 7));
         }},
        {"sheetProperties", [](QPainter& p, const QPen& ink, const QPen& ac) {  // sheet with format dimensions
             p.setPen(ink);
             p.drawRect(QRectF(6, 3, 14, 18));
             p.setPen(ac);
             p.drawLine(QPointF(2, 3), QPointF(2, 21)), p.drawLine(QPointF(6, 23), QPointF(20, 23));
         }},
        {"insert", [](QPainter& p, const QPen& ink, const QPen& ac) {  // solid block with an inward arrow
             solidBlock(p, ink, 15, 8, 7, 8);
             p.setPen(ac);
             p.drawLine(QPointF(2, 3), QPointF(10, 11)), p.drawLine(QPointF(10, 11), QPointF(10, 6)), p.drawLine(QPointF(10, 11), QPointF(5, 11));
         }},
        {"text", [](QPainter& p, const QPen& ink, const QPen& ac) {  // filled letter "A" and a text cursor (FX1-UI-004)
             // The letter is a font glyph, not lines: two unfilled "legs" with a crossbar read as a compass.
             QFont f("Arial");
             f.setBold(true);
             f.setPixelSize(19);
             QPainterPath glyph;
             glyph.addText(QPointF(1, 19), f, QString::fromUtf8("А"));  // i18n-skip: icon glyph
             p.setPen(Qt::NoPen);
             p.setBrush(ink.color());
             p.drawPath(glyph);
             p.setBrush(Qt::NoBrush);
             QPen caret = ac;
             caret.setWidthF(1.8);
             p.setPen(caret);
             p.drawLine(QPointF(19.5, 4), QPointF(19.5, 20));
             p.drawLine(QPointF(17, 4), QPointF(22, 4)), p.drawLine(QPointF(17, 20), QPointF(22, 20));
         }},
        {"reconstruct", [](QPainter& p, const QPen& ink, const QPen& ac) {  // solid block and a "back through history" arrow (S4-REC-002)
             solidBlock(p, ink, 11, 6, 8, 9);
             p.setPen(ac);
             p.drawArc(QRectF(12, 1, 10, 10), 30 * 16, 260 * 16);
             p.drawLine(QPointF(12.5, 3), QPointF(13.5, 7));
             p.drawLine(QPointF(13.5, 7), QPointF(17, 5));
         }},
        {"section", [](QPainter& p, const QPen& ink, const QPen& ac) {  // block cut by a plane, the section filled
             p.setPen(ink);
             p.setBrush(COLOR_ICON_SOLID.color);  // the kept part of the solid (S7F-UI-005)
             p.drawPolygon(QPolygonF{{4, 9}, {10, 6}, {10, 18}, {4, 21}});
             p.setBrush(Qt::NoBrush);
             p.drawLine(QPointF(10, 6), QPointF(14, 8));
             p.setPen(ac);
             p.setBrush(QColor(242, 140, 40, 90));
             p.drawPolygon(QPolygonF{{10, 6}, {20, 11}, {20, 21}, {10, 18}});
             p.setBrush(Qt::NoBrush);
         }},
        {"displayStyle", [](QPainter& p, const QPen& ink, const QPen&) {
             const QColor c("#9AA6B2");
             styleCube(p, ink, &c, true, 0);
         }},
        {"sketch", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{3, 17}, {9, 21}, {21, 15}, {15, 11}});  // plane
             p.setPen(ac);
             p.drawLine(QPointF(6, 6), QPointF(15, 13));  // pencil
             p.drawLine(QPointF(15, 13), QPointF(13, 14.5));
         }},
        {"extrude", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawRect(QRectF(4, 12, 10, 8));
             p.drawLine(QPointF(4, 12), QPointF(9, 7)), p.drawLine(QPointF(14, 12), QPointF(19, 7));
             p.drawLine(QPointF(9, 7), QPointF(19, 7)), p.drawLine(QPointF(19, 7), QPointF(19, 15)), p.drawLine(QPointF(14, 20), QPointF(19, 15));
             p.setPen(ac);
             p.drawLine(QPointF(9, 16), QPointF(9, 3)), p.drawLine(QPointF(9, 3), QPointF(6.5, 5.5)), p.drawLine(QPointF(9, 3), QPointF(11.5, 5.5));
         }},
        {"revolve", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawLine(QPointF(12, 2), QPointF(12, 22));  // axis
             p.drawEllipse(QRectF(4, 14, 16, 6));
             p.drawEllipse(QRectF(4, 4, 16, 6));
             p.setPen(ac);
             p.drawArc(QRectF(3, 8, 18, 8), 200 * 16, 140 * 16);
             p.drawLine(QPointF(20, 13), QPointF(21, 10.5));
         }},
        {"fillet", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawLine(QPointF(4, 20), QPointF(4, 10)), p.drawLine(QPointF(14, 4), QPointF(20, 4));
             p.setPen(ac);
             p.drawArc(QRectF(4, 4, 20, 12), 90 * 16, 90 * 16);
         }},
        {"chamfer", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawLine(QPointF(4, 20), QPointF(4, 11)), p.drawLine(QPointF(12, 4), QPointF(20, 4));
             p.setPen(ac);
             p.drawLine(QPointF(4, 11), QPointF(12, 4));
         }},
        {"variables", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             QFont f;
             f.setPixelSize(13);
             f.setItalic(true);
             f.setBold(true);
             p.setFont(f);
             p.drawText(QRectF(2, 3, 20, 18), Qt::AlignCenter, "x=");
             p.setPen(ac);
             p.drawLine(QPointF(4, 21), QPointF(20, 21));
         }},
        {"point", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawLine(QPointF(12, 5), QPointF(12, 9)), p.drawLine(QPointF(12, 15), QPointF(12, 19));
             p.drawLine(QPointF(5, 12), QPointF(9, 12)), p.drawLine(QPointF(15, 12), QPointF(19, 12));
             dot(p, ac, {12, 12}, 2.2);
         }},
        {"line", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawLine(QPointF(5, 19), QPointF(19, 5));
             dot(p, ac, {5, 19}), dot(p, ac, {19, 5});
         }},
        {"centerline", [](QPainter& p, const QPen& ink, const QPen&) {
             QPen d = ink;
             d.setDashPattern({4, 1.5, 1, 1.5});
             p.setPen(d);
             p.drawLine(QPointF(4, 20), QPointF(20, 4));
         }},
        {"rectangle", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawRect(QRectF(4, 7, 16, 11));
             dot(p, ac, {4, 18}), dot(p, ac, {20, 7});
         }},
        {"centerRectangle", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawRect(QRectF(4, 7, 16, 11));
             dot(p, ac, {12, 12.5}), dot(p, ac, {20, 7});
         }},
        {"circle", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawEllipse(QPointF(12, 12), 8, 8);
             dot(p, ac, {12, 12});
         }},
        {"arc3", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawArc(QRectF(4, 6, 16, 16), 0, 180 * 16);
             dot(p, ac, {4, 14}), dot(p, ac, {20, 14}), dot(p, ac, {12, 6});
         }},
        {"arcCenter", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawArc(QRectF(4, 6, 16, 16), 0, 135 * 16);
             dot(p, ac, {12, 14}), dot(p, ac, {20, 14});
         }},
        {"tangentArc", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawLine(QPointF(3, 18), QPointF(11, 18));
             p.setPen(ac);
             p.drawArc(QRectF(5, 6, 12, 12), 270 * 16, 180 * 16);
         }},
        {"trim", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawEllipse(QPointF(7, 17), 3, 3), p.drawEllipse(QPointF(17, 17), 3, 3);
             p.drawLine(QPointF(9, 15), QPointF(18, 4)), p.drawLine(QPointF(15, 15), QPointF(6, 4));
             QPen d = ac;
             d.setDashPattern({2, 2});
             p.setPen(d);
             p.drawLine(QPointF(2, 9), QPointF(22, 9));
         }},
        {"project", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawRect(QRectF(5, 3, 14, 8));  // body edge
             QPen d = ac;
             d.setDashPattern({4, 1.5, 1, 1.5});
             p.setPen(d);
             p.drawLine(QPointF(5, 19), QPointF(19, 19));  // projection
             p.setPen(ink);
             p.drawLine(QPointF(12, 12), QPointF(12, 17)), p.drawLine(QPointF(12, 17), QPointF(10, 15)), p.drawLine(QPointF(12, 17), QPointF(14, 15));
         }},
        {"construction", [](QPainter& p, const QPen& ink, const QPen&) {
             QPen d = ink;
             d.setDashPattern({3, 2});
             p.setPen(d);
             p.drawEllipse(QPointF(12, 12), 8, 8);
             p.drawLine(QPointF(4, 20), QPointF(20, 4));
         }},
        // «Draw as construction», FX2-SK-001: a pencil drawing a dashed line.
        {"drawConstruction", [](QPainter& p, const QPen& ink, const QPen& ac) {
             QPen d = ac;
             d.setDashPattern({3, 2});
             p.setPen(d);
             p.drawLine(QPointF(3, 20), QPointF(12, 20));
             p.setPen(ink);
             const QPointF body[] = {QPointF(12, 18), QPointF(20, 6), QPointF(22, 8), QPointF(14, 20)};
             p.drawPolygon(body, 4);
             p.drawLine(QPointF(12, 18), QPointF(12, 21)), p.drawLine(QPointF(12, 21), QPointF(14, 20));
         }},
        {"dimension", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawLine(QPointF(4, 6), QPointF(4, 18)), p.drawLine(QPointF(20, 6), QPointF(20, 18));
             p.setPen(ac);
             p.drawLine(QPointF(5, 12), QPointF(19, 12));
             p.drawLine(QPointF(5, 12), QPointF(8, 10)), p.drawLine(QPointF(5, 12), QPointF(8, 14));
             p.drawLine(QPointF(19, 12), QPointF(16, 10)), p.drawLine(QPointF(19, 12), QPointF(16, 14));
         }},
        {"new", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{6, 3}, {14, 3}, {18, 7}, {18, 21}, {6, 21}});  // sheet
             p.setPen(ac);
             p.drawLine(QPointF(12, 10), QPointF(12, 16)), p.drawLine(QPointF(9, 13), QPointF(15, 13));  // plus
         }},
        {"open", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{3, 7}, {9, 7}, {11, 9}, {21, 9}, {21, 19}, {3, 19}});  // folder
             p.setPen(ac);
             p.drawLine(QPointF(12, 17), QPointF(12, 11)), p.drawLine(QPointF(12, 11), QPointF(9.5, 13.5)), p.drawLine(QPointF(12, 11), QPointF(14.5, 13.5));
         }},
        {"measure", [](QPainter& p, const QPen& ink, const QPen& ac) {  // FX10-MEAS-001: a ruler and a dimension arrow
             p.setPen(ink);
             p.drawRect(QRectF(3, 13, 18, 6));
             for (int x = 6; x <= 18; x += 3) p.drawLine(QPointF(x, 13), QPointF(x, x % 2 ? 15.5 : 16.5));
             p.setPen(ac);
             p.drawLine(QPointF(4, 7), QPointF(20, 7));
             p.drawLine(QPointF(4, 7), QPointF(7, 5)), p.drawLine(QPointF(4, 7), QPointF(7, 9));
             p.drawLine(QPointF(20, 7), QPointF(17, 5)), p.drawLine(QPointF(20, 7), QPointF(17, 9));
         }},
        {"elementGroup", [](QPainter& p, const QPen& ink, const QPen& ac) {  // FX10-TREE-001
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{3, 7}, {9, 7}, {11, 9}, {21, 9}, {21, 19}, {3, 19}});  // folder
             p.setPen(ac);
             p.drawRect(QRectF(7, 12, 4, 4)), p.drawRect(QRectF(13, 12, 4, 4));  // two members
         }},
        {"save", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawRect(QRectF(4, 4, 16, 16));  // floppy disk
             p.drawRect(QRectF(8, 4, 8, 5));
             p.setPen(ac);
             p.drawRect(QRectF(7, 13, 10, 7));
         }},
        {"undo", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             QPainterPath path(QPointF(7, 10));
             path.cubicTo(QPointF(12, 4), QPointF(21, 8), QPointF(19, 16));
             p.drawPath(path);
             p.setPen(ac);
             p.drawLine(QPointF(7, 10), QPointF(7, 4)), p.drawLine(QPointF(7, 10), QPointF(13, 10));
         }},
        {"redo", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             QPainterPath path(QPointF(17, 10));
             path.cubicTo(QPointF(12, 4), QPointF(3, 8), QPointF(5, 16));
             p.drawPath(path);
             p.setPen(ac);
             p.drawLine(QPointF(17, 10), QPointF(17, 4)), p.drawLine(QPointF(17, 10), QPointF(11, 10));
         }},
        {"polygonInscribed", [](QPainter& p, const QPen& ink, const QPen& ac) {  // N-gon (pentagon) inscribed in a circle
             QPolygonF h;
             for (int i = 0; i < 5; ++i) h << QPointF(12 + 9 * std::sin(i * 2 * 3.14159265 / 5), 12 - 9 * std::cos(i * 2 * 3.14159265 / 5));
             p.setPen(ac);
             p.drawEllipse(QPointF(12, 12), 9, 9);
             p.setPen(ink);
             p.drawPolygon(h);
         }},
        {"polygonCircumscribed", [](QPainter& p, const QPen& ink, const QPen& ac) {  // N-gon (pentagon) circumscribed around a circle
             QPolygonF h;
             for (int i = 0; i < 5; ++i) h << QPointF(12 + 10.6 * std::sin(i * 2 * 3.14159265 / 5), 12 - 10.6 * std::cos(i * 2 * 3.14159265 / 5));
             p.setPen(ink);
             p.drawPolygon(h);
             p.setPen(ac);
             p.drawEllipse(QPointF(12, 12), 8.6, 8.6);
         }},
        {"ellipse", [](QPainter& p, const QPen& ink, const QPen& ac) {  // ellipse with axes (S4-SKT-004)
             p.setPen(ink);
             p.drawEllipse(QPointF(12, 12), 10, 6);
             p.setPen(ac);
             p.drawLine(QPointF(12, 12), QPointF(22, 12));
             p.drawLine(QPointF(12, 12), QPointF(12, 6));
             p.drawEllipse(QPointF(12, 12), 1.4, 1.4);
         }},
        {"splineControl", [](QPainter& p, const QPen& ink, const QPen& ac) {  // spline with its control polygon (S4A-SPL-001)
             const QPointF P[4] = {QPointF(3, 19), QPointF(7, 4), QPointF(17, 4), QPointF(21, 19)};
             QPen dash = ac;
             dash.setStyle(Qt::DashLine);
             p.setPen(dash);
             p.drawPolyline(P, 4);
             p.setPen(ink);
             QPainterPath path(P[0]);
             path.cubicTo(P[1], P[2], P[3]);
             p.drawPath(path);
             p.setPen(ac);
             for (const QPointF& q : P) p.drawRect(QRectF(q.x() - 1.3, q.y() - 1.3, 2.6, 2.6));
         }},
        {"splineThrough", [](QPainter& p, const QPen& ink, const QPen& ac) {  // spline through points (S4A-SPL-001)
             p.setPen(ink);
             QPainterPath path(QPointF(3, 18));
             path.cubicTo(QPointF(7, 2), QPointF(12, 22), QPointF(21, 6));
             p.drawPath(path);
             p.setPen(ac);
             for (const QPointF& q : {QPointF(3, 18), QPointF(9.5, 11.6), QPointF(21, 6)}) p.drawEllipse(q, 1.8, 1.8);
         }},
        {"star", [](QPainter& p, const QPen& ink, const QPen& ac) {  // five-pointed star (S4-SKT-003)
             QPolygonF h;
             for (int i = 0; i < 10; ++i) {
                 const double r = i % 2 ? 4.2 : 10, a = i * 3.14159265 / 5;
                 h << QPointF(12 + r * std::sin(a), 12.5 - r * std::cos(a));
             }
             p.setPen(ink);
             p.drawPolygon(h);
             p.setPen(ac);
             p.drawEllipse(QPointF(12, 12.5), 1.4, 1.4);
         }},
        {"circle3", [](QPainter& p, const QPen& ink, const QPen& ac) {  // circle through three points
             p.setPen(ink);
             p.drawEllipse(QPointF(12, 12), 8, 8);
             p.setPen(ac);
             for (const QPointF& q : {QPointF(4, 12), QPointF(17.7, 6.3), QPointF(17.7, 17.7)}) p.drawEllipse(q, 1.6, 1.6);
         }},
        {"mirror", [](QPainter& p, const QPen& ink, const QPen& ac) {  // triangle and its mirror image
             QPen d = ink;
             d.setStyle(Qt::DashDotLine);
             p.setPen(d);
             p.drawLine(QPointF(12, 2), QPointF(12, 22));
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{10, 6}, {10, 18}, {3, 18}});
             p.setPen(ac);
             p.drawPolygon(QPolygonF{{14, 6}, {14, 18}, {21, 18}});
         }},
        {"sketchFillet", [](QPainter& p, const QPen& ink, const QPen& ac) {  // corner with a fillet
             p.setPen(ink);
             p.drawLine(QPointF(4, 20), QPointF(4, 12)), p.drawLine(QPointF(12, 4), QPointF(20, 4));
             p.setPen(ac);
             p.drawArc(QRectF(4, 4, 16, 16), 90 * 16, 90 * 16);
         }},
        {"sketchChamfer", [](QPainter& p, const QPen& ink, const QPen& ac) {  // corner with a chamfer
             p.setPen(ink);
             p.drawLine(QPointF(4, 20), QPointF(4, 11)), p.drawLine(QPointF(11, 4), QPointF(20, 4));
             p.setPen(ac);
             p.drawLine(QPointF(4, 11), QPointF(11, 4));
         }},
        {"sketchPatternLinear", [](QPainter& p, const QPen& ink, const QPen& ac) {  // square and its copies in a row and a column
             p.setPen(ink);
             p.drawRect(QRectF(2, 14, 7, 7));
             p.setPen(ac);
             p.drawRect(QRectF(14, 14, 7, 7)), p.drawRect(QRectF(2, 3, 7, 7)), p.drawRect(QRectF(14, 3, 7, 7));
         }},
        {"sketchPatternCircular", [](QPainter& p, const QPen& ink, const QPen& ac) {  // small circles arranged on a circle around a center
             p.setPen(ink);
             p.drawEllipse(QPointF(20, 12), 2.5, 2.5);
             p.setPen(ac);
             for (int i = 1; i < 6; ++i)
                 p.drawEllipse(QPointF(12 + 8 * std::cos(i * 3.14159265 / 3), 12 + 8 * std::sin(i * 3.14159265 / 3)), 2.5, 2.5);
             dot(p, ink, QPointF(12, 12), 1.2);
         }},
        {"sketchUnlink", [](QPainter& p, const QPen& ink, const QPen& ac) {  // broken chain link
             p.setPen(ink);
             p.drawRoundedRect(QRectF(2, 8, 9, 8), 4, 4);
             p.setPen(ac);
             p.drawRoundedRect(QRectF(13, 8, 9, 8), 4, 4);
             p.setPen(ink);
             p.drawLine(QPointF(11, 4), QPointF(13, 2)), p.drawLine(QPointF(11, 20), QPointF(13, 22));
         }},
        {"offset", [](QPainter& p, const QPen& ink, const QPen& ac) {  // contour and an offset contour
             p.setPen(ink);
             p.drawPolyline(QPolygonF{{4, 20}, {4, 9}, {13, 9}, {13, 4}});
             p.setPen(ac);
             p.drawPolyline(QPolygonF{{9, 20}, {9, 14}, {18, 14}, {18, 4}});
         }},
        // Reference geometry (§5.3а): plane is a parallelogram, axis is dash-dot; accent shows the construction method.
        {"planeOffset", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{2, 18}, {8, 22}, {22, 16}, {16, 12}});
             p.setPen(ac);
             p.drawPolygon(QPolygonF{{2, 9}, {8, 13}, {22, 7}, {16, 3}});
         }},
        {"planeAngle", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{2, 18}, {8, 22}, {22, 16}, {16, 12}});
             p.setPen(ac);
             p.drawPolygon(QPolygonF{{8, 22}, {22, 16}, {18, 3}, {4, 9}});
         }},
        {"planeThreePoints", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{2, 16}, {8, 20}, {22, 14}, {16, 10}});
             dot(p, ac, QPointF(6, 16), 2), dot(p, ac, QPointF(16, 16), 2), dot(p, ac, QPointF(14, 11.5), 2);
         }},
        {"planeMid", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{2, 20}, {8, 23}, {22, 18}, {16, 15}});
             p.drawPolygon(QPolygonF{{2, 6}, {8, 9}, {22, 4}, {16, 1}});
             p.setPen(ac);
             p.drawPolygon(QPolygonF{{2, 13}, {8, 16}, {22, 11}, {16, 8}});
         }},
        {"fullyDefine", [](QPainter& p, const QPen& ink, const QPen& ac) {  // rectangle with dimensions along the axes
             p.setPen(ink);
             p.drawRect(QRectF(7, 7, 14, 10));
             p.setPen(ac);
             p.drawLine(QPointF(7, 21), QPointF(21, 21)), p.drawLine(QPointF(3, 7), QPointF(3, 17));
             p.drawLine(QPointF(7, 19), QPointF(7, 23)), p.drawLine(QPointF(21, 19), QPointF(21, 23));
         }},
        {"properties", [](QPainter& p, const QPen& ink, const QPen& ac) {  // card with property rows
             p.setPen(ink);
             p.drawRect(QRectF(4, 3, 16, 18));
             p.setPen(ac);
             for (int y = 8; y <= 16; y += 4) p.drawLine(QPointF(7, y), QPointF(17, y));
         }},
        {"gear", [](QPainter& p, const QPen& ink, const QPen& ac) {  // spur wheel: hub circle and teeth
             p.setPen(ink);
             p.drawEllipse(QPointF(12, 12), 3, 3);
             p.setPen(ac);
             QPolygonF teeth;
             for (int i = 0; i < 32; ++i) {  // 8 teeth: tip and root radius alternate by quarter pitch
                 const double a = i * 3.14159265358979323846 / 16;
                 const double r = (i % 4 == 0 || i % 4 == 1) ? 10 : 7;
                 teeth << QPointF(12 + r * std::cos(a), 12 + r * std::sin(a));
             }
             p.drawPolygon(teeth);
         }},
        {"coil", [](QPainter& p, const QPen& ink, const QPen& ac) {  // spring: axis and helical turns (FX7-PT-004)
             p.setPen(ink);
             p.drawLine(QPointF(12, 2), QPointF(12, 22));
             p.setPen(ac);
             QPainterPath path;
             for (int i = 0; i <= 80; ++i) {
                 const double t = i / 80.0;
                 const QPointF q(12 + 8 * std::cos(t * 4 * 2 * 3.14159265358979323846), 3 + 18 * t);
                 if (i == 0) path.moveTo(q);
                 else path.lineTo(q);
             }
             p.drawPath(path);
         }},
        {"thread", [](QPainter& p, const QPen& ink, const QPen& ac) {  // rod with thread turns
             p.setPen(ink);
             p.drawRect(QRectF(7, 2, 10, 20));
             p.setPen(ac);
             for (int y = 5; y <= 19; y += 4) p.drawLine(QPointF(7, y + 2), QPointF(17, y));
         }},
        {"asmHole", [](QPainter& p, const QPen& ink, const QPen& ac) {  // two stacked solid plates, a hole axis through both (§8а)
             solidBlock(p, ink, 12, 8, 9, 3);
             solidBlock(p, ink, 12, 4, 9, 3);
             p.setPen(ac);
             p.drawEllipse(QPointF(12, 8.5), 2.6, 1.3);
             p.drawLine(QPointF(12, 1), QPointF(12, 23));
         }},
        {"asmSolution", [](QPainter& p, const QPen& ink, const QPen& ac) {  // a bolt with a nut through two solid plates (§8д)
             solidBlock(p, ink, 12, 10, 9, 2.5);
             solidBlock(p, ink, 12, 7, 9, 2.5);
             p.setPen(ac);
             p.drawRect(QRectF(8, 3, 8, 4));     // head
             p.drawLine(QPointF(10, 7), QPointF(10, 21)), p.drawLine(QPointF(14, 7), QPointF(14, 21));
             p.drawRect(QRectF(8, 18, 8, 3));    // nut
         }},
        {"asmPattern", [](QPainter& p, const QPen& ink, const QPen& ac) {  // a solid part and its copies in a row (§8а, FX7-ASM-007)
             solidBlock(p, ink, 4.5, 8, 3.5, 5);
             solidBlock(p, ac, 12, 8, 3.5, 5);
             solidBlock(p, ac, 19.5, 8, 3.5, 5);
         }},
        {"exploded", [](QPainter& p, const QPen& ink, const QPen& ac) {  // two solid parts pulled apart along an axis (FX7-ASM-005)
             solidBlock(p, ink, 12, 1, 4.5, 3);
             solidBlock(p, ink, 12, 13, 9, 4);
             p.setPen(ac);
             p.drawLine(QPointF(12, 8), QPointF(12, 14));
             p.drawLine(QPointF(10, 10), QPointF(12, 8)), p.drawLine(QPointF(14, 10), QPointF(12, 8));
         }},
        {"explodeReset", [](QPainter& p, const QPen& ink, const QPen& ac) {  // pulled-apart solid parts and a circular arrow
             solidBlock(p, ink, 7, 2, 3.5, 3);
             solidBlock(p, ink, 8, 13, 6, 4);
             p.setPen(ac);
             p.drawArc(QRectF(13, 6, 9, 9), 30 * 16, 270 * 16);
             p.drawLine(QPointF(21, 6), QPointF(21, 10)), p.drawLine(QPointF(21, 10), QPointF(17, 10));
         }},
        {"hole", [](QPainter& p, const QPen& ink, const QPen& ac) {  // section of a counterbored hole
             p.setPen(ink);
             p.drawLine(QPointF(2, 6), QPointF(22, 6));
             p.setPen(ac);
             p.drawPolyline(QPolygonF({QPointF(6, 6), QPointF(6, 11), QPointF(9, 11), QPointF(9, 21), QPointF(15, 21), QPointF(15, 11),
                                       QPointF(18, 11), QPointF(18, 6)}));
         }},
        {"loft", [](QPainter& p, const QPen& ink, const QPen& ac) {  // square at the bottom, circle at the top, transition
             p.setPen(ink);
             p.drawRect(QRectF(4, 16, 16, 5));
             p.drawEllipse(QRectF(7, 2, 10, 4));
             p.setPen(ac);
             p.drawLine(QPointF(4, 16), QPointF(7, 4)), p.drawLine(QPointF(20, 16), QPointF(17, 4));
         }},
        {"pierce", [](QPainter& p, const QPen& ink, const QPen& ac) {  // curve piercing a plane, and the point
             p.setPen(ink);
             p.drawPolygon(QPolygonF({QPointF(3, 15), QPointF(13, 19), QPointF(21, 9), QPointF(11, 5)}));
             p.setPen(ac);
             p.drawLine(QPointF(12, 2), QPointF(12, 22));
             p.setBrush(ac.color());
             p.drawEllipse(QPointF(12, 12), 2, 2);
         }},
        {"sweep", [](QPainter& p, const QPen& ink, const QPen& ac) {  // profile and a curved path
             p.setPen(ink);
             p.drawEllipse(QPointF(5, 19), 3, 3);
             p.setPen(ac);
             QPainterPath path(QPointF(5, 19));
             path.cubicTo(QPointF(5, 6), QPointF(12, 4), QPointF(21, 4));
             p.drawPath(path);
         }},
        {"draft", [](QPainter& p, const QPen& ink, const QPen& ac) {  // trapezoid: tapered walls
             p.setPen(ink);
             p.drawLine(QPointF(2, 21), QPointF(22, 21));
             p.setPen(ac);
             p.drawPolyline(QPolygonF({QPointF(4, 21), QPointF(8, 4), QPointF(16, 4), QPointF(20, 21)}));
         }},
        // Sheet metal (§2.3, §5.3к): sheet with a bend, base wall, flange, bend line, cut, unfold.
        {"sheetMetal", [](QPainter& p, const QPen& ink, const QPen& ac) {  // L-shaped sheet with a rounded bend
             p.setPen(ink);
             p.drawPolyline(QPolygonF({QPointF(3, 4), QPointF(3, 17)}));
             p.drawArc(QRectF(3, 13, 8, 8), 180 * 16, 90 * 16);
             p.drawLine(QPointF(7, 21), QPointF(21, 21));
             p.setPen(ac);
             p.drawText(QRectF(9, 3, 13, 13), Qt::AlignCenter, "T");
         }},
        {"sheetWall", [](QPainter& p, const QPen& ink, const QPen& ac) {  // thin plate in perspective
             p.setPen(ink);
             p.drawPolygon(QPolygonF({QPointF(3, 15), QPointF(14, 19), QPointF(21, 11), QPointF(10, 7)}));
             p.setPen(ac);
             p.drawLine(QPointF(3, 15), QPointF(3, 17)), p.drawLine(QPointF(3, 17), QPointF(14, 21)), p.drawLine(QPointF(14, 21), QPointF(21, 13));
         }},
        {"sheetFlange", [](QPainter& p, const QPen& ink, const QPen& ac) {  // plate with an edge flange up and a "+"
             p.setPen(ink);
             p.drawLine(QPointF(2, 20), QPointF(16, 20));
             p.setPen(ac);
             p.drawArc(QRectF(12, 12, 8, 8), 270 * 16, 90 * 16);
             p.drawLine(QPointF(20, 16), QPointF(20, 3));
             // The flange adds material (FX10-SH-002): a "+" in the free top-left corner.
             QPen plus = ac;
             plus.setWidthF(2.2);
             p.setPen(plus);
             p.drawLine(QPointF(7, 3), QPointF(7, 11)), p.drawLine(QPointF(3, 7), QPointF(11, 7));
         }},
        {"sheetBend", [](QPainter& p, const QPen& ink, const QPen& ac) {  // plate bent along a dashed line
             p.setPen(ink);
             p.drawLine(QPointF(2, 20), QPointF(12, 20));
             p.drawLine(QPointF(12, 20), QPointF(20, 6));
             QPen d = ac;
             d.setStyle(Qt::DashLine);
             p.setPen(d);
             p.drawLine(QPointF(12, 3), QPointF(12, 22));
         }},
        {"sheetCut", [](QPainter& p, const QPen& ink, const QPen& ac) {  // plate with a rectangular cut-out
             p.setPen(ink);
             p.drawRect(QRectF(3, 6, 18, 12));
             p.setPen(ac);
             p.drawRect(QRectF(9, 9, 6, 6));
         }},
        {"sheetUnfold", [](QPainter& p, const QPen& ink, const QPen& ac) {  // flat pattern with bend lines
             p.setPen(ink);
             p.drawRect(QRectF(2, 8, 20, 8));
             QPen d = ac;
             d.setStyle(Qt::DashLine);
             p.setPen(d);
             p.drawLine(QPointF(8, 8), QPointF(8, 16)), p.drawLine(QPointF(16, 8), QPointF(16, 16));
         }},
        {"shell", [](QPainter& p, const QPen& ink, const QPen& ac) {  // open box with a wall
             p.setPen(ink);
             p.drawPolyline(QPolygonF({QPointF(3, 5), QPointF(3, 20), QPointF(21, 20), QPointF(21, 5)}));
             p.setPen(ac);
             p.drawPolyline(QPolygonF({QPointF(6, 5), QPointF(6, 17), QPointF(18, 17), QPointF(18, 5)}));
         }},
        {"mirrorFeature", [](QPainter& p, const QPen& ink, const QPen& ac) {  // solid body and its mirror image beyond a plane
             solidBlock(p, ink, 6, 6, 4, 8);
             p.setPen(ink);
             p.drawLine(QPointF(12, 2), QPointF(12, 22));
             solidBlock(p, ac, 18, 6, 4, 8);
         }},
        {"patternLinear", [](QPainter& p, const QPen& ink, const QPen& ac) {  // row of squares
             p.setPen(ink);
             p.drawRect(QRectF(2, 9, 5, 5));
             p.setPen(ac);
             p.drawRect(QRectF(9.5, 9, 5, 5)), p.drawRect(QRectF(17, 9, 5, 5));
         }},
        {"patternCircular", [](QPainter& p, const QPen& ink, const QPen& ac) {  // small circles arranged on a circle
             p.setPen(ink);
             p.drawEllipse(QPointF(12, 4), 2.5, 2.5);
             p.setPen(ac);
             for (const QPointF c : {QPointF(19, 12), QPointF(12, 20), QPointF(5, 12)}) p.drawEllipse(c, 2.5, 2.5);
         }},
        {"axisTwoPoints", [](QPainter& p, const QPen& ink, const QPen& ac) {
             QPen d = ink;
             d.setStyle(Qt::DashDotLine);
             p.setPen(d);
             p.drawLine(QPointF(3, 21), QPointF(21, 3));
             dot(p, ac, QPointF(7, 17), 2.2), dot(p, ac, QPointF(17, 7), 2.2);
         }},
        {"axisEdge", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawRect(QRectF(5, 9, 12, 10));
             QPen d = ac;
             d.setStyle(Qt::DashDotLine);
             p.setPen(d);
             p.drawLine(QPointF(2, 9), QPointF(22, 9));
         }},
        {"axisFace", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawEllipse(QRectF(6, 3, 12, 5));
             p.drawLine(QPointF(6, 5.5), QPointF(6, 18.5)), p.drawLine(QPointF(18, 5.5), QPointF(18, 18.5));
             p.drawArc(QRectF(6, 16, 12, 5), 180 * 16, 180 * 16);
             QPen d = ac;
             d.setStyle(Qt::DashDotLine);
             p.setPen(d);
             p.drawLine(QPointF(12, 1), QPointF(12, 23));
         }},
        {"axisPlanes", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{2, 12}, {12, 7}, {22, 12}, {12, 17}});
             p.drawLine(QPointF(12, 2), QPointF(12, 22));
             QPen d = ac;
             d.setStyle(Qt::DashDotLine);
             p.setPen(d);
             p.drawLine(QPointF(12, 2), QPointF(12, 22));
         }},
        {"rebuild", [](QPainter& p, const QPen& ink, const QPen& ac) {  // circular arrows
             p.setPen(ink);
             p.drawArc(QRectF(5, 5, 14, 14), 30 * 16, 150 * 16);
             p.drawArc(QRectF(5, 5, 14, 14), 210 * 16, 150 * 16);
             p.setPen(ac);
             p.drawLine(QPointF(18, 8), QPointF(19, 3)), p.drawLine(QPointF(18, 8), QPointF(13, 7));
             p.drawLine(QPointF(6, 16), QPointF(5, 21)), p.drawLine(QPointF(6, 16), QPointF(11, 17));
         }},
        {"views", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{12, 3}, {20, 7}, {12, 11}, {4, 7}});  // view cube
             p.drawLine(QPointF(4, 7), QPointF(4, 16)), p.drawLine(QPointF(20, 7), QPointF(20, 16)), p.drawLine(QPointF(12, 11), QPointF(12, 20));
             p.drawLine(QPointF(4, 16), QPointF(12, 20)), p.drawLine(QPointF(20, 16), QPointF(12, 20));
             p.setPen(ac);
             p.drawPolygon(QPolygonF{{12, 3}, {20, 7}, {12, 11}, {4, 7}});
         }},
        {"fit", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             for (const auto& [a, b, c] : {std::tuple{QPointF(3, 8), QPointF(3, 3), QPointF(8, 3)}, std::tuple{QPointF(16, 3), QPointF(21, 3), QPointF(21, 8)},
                                           std::tuple{QPointF(21, 16), QPointF(21, 21), QPointF(16, 21)}, std::tuple{QPointF(8, 21), QPointF(3, 21), QPointF(3, 16)}})
                p.drawLine(a, b), p.drawLine(b, c);  // zoom-to-fit frame
             p.setPen(ac);
             p.drawRect(QRectF(8, 8, 8, 8));
         }},
        {"select", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.setBrush(QColor(255, 255, 255));
             p.drawPolygon(QPolygonF{{6, 3}, {6, 19}, {10, 15}, {13, 21}, {15, 20}, {12, 14}, {17, 14}});  // cursor arrow
             p.setBrush(Qt::NoBrush);
             (void)ac;
         }},
        {"selectAll", [](QPainter& p, const QPen& ink, const QPen& ac) {
             QPen dash = ink;
             dash.setStyle(Qt::DashLine);
             p.setPen(dash);
             p.drawRect(QRectF(3, 3, 18, 18));  // selection box
             p.setPen(ac);
             p.drawLine(QPointF(7, 16), QPointF(17, 8));
             dot(p, ac, QPointF(7, 16)), dot(p, ac, QPointF(17, 8));
         }},
        {"normalTo", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawRect(QRectF(5, 5, 14, 14));  // plane seen face-on
             p.setPen(ac);
             dot(p, ac, QPointF(12, 12), 2.2);  // looking along the normal
         }},
        {"showConstraints", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawLine(QPointF(3, 18), QPointF(21, 18));
             p.setPen(ac);
             p.drawRect(QRectF(8, 6, 8, 8));  // constraint badge
             p.drawLine(QPointF(10, 10), QPointF(14, 10));
         }},
        {"part", [](QPainter& p, const QPen& ink, const QPen&) {  // a solid block in isometric view
             solidBlock(p, ink, 12, 3, 8.5, 9.5);
         }},
        {"newPart", [](QPainter& p, const QPen& ink, const QPen& ac) {  // a solid block and a "+"
             solidBlock(p, ink, 9.5, 2, 7, 7);
             plusMark(p, ac);
         }},
        {"eye", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             QPainterPath e;
             e.moveTo(3, 12), e.quadTo(12, 3, 21, 12), e.quadTo(12, 21, 3, 12);
             p.drawPath(e);
             dot(p, ac, QPointF(12, 12), 3);
         }},
        {"eyeOff", [](QPainter& p, const QPen& ink, const QPen&) {
             QPen light = ink;
             light.setColor(QColor(150, 155, 162));
             p.setPen(light);
             QPainterPath e;
             e.moveTo(3, 12), e.quadTo(12, 3, 21, 12), e.quadTo(12, 21, 3, 12);
             p.drawPath(e);
             p.drawLine(QPointF(4, 20), QPointF(20, 4));
         }},
        // Technical requirements / PMI (app-ux §2.3, §5.12, S3-PMI-001…003).
        {"pmiRoughness", [](QPainter& p, const QPen& ink, const QPen& ac) {  // surface roughness symbol with a leader shelf
             p.setPen(ink);
             p.drawPolyline(QPolygonF({QPointF(4, 14), QPointF(8, 21), QPointF(15, 5)}));
             p.drawLine(QPointF(4, 14), QPointF(12, 14));
             p.setPen(ac);
             p.drawLine(QPointF(15, 5), QPointF(22, 5));
         }},
        {"pmiRest", [](QPainter& p, const QPen& ink, const QPen& ac) {  // roughness of the remaining surfaces: symbol and (√)
             p.setPen(ink);
             p.drawPolyline(QPolygonF({QPointF(1, 13), QPointF(4, 19), QPointF(9, 6)}));
             p.drawLine(QPointF(9, 6), QPointF(12, 6));
             p.setPen(ac);
             p.drawArc(QRectF(12, 6, 6, 14), 100 * 16, 160 * 16);
             p.drawPolyline(QPolygonF({QPointF(15, 14), QPointF(17, 18), QPointF(20, 9)}));
             p.drawArc(QRectF(17, 6, 6, 14), -80 * 16, 160 * 16);
         }},
        {"pmiFit", [](QPainter& p, const QPen& ink, const QPen& ac) {  // diameter symbol and "±"
             p.setPen(ink);
             p.drawEllipse(QPointF(8, 13), 5, 5);
             p.drawLine(QPointF(2, 19), QPointF(14, 7));
             p.setPen(ac);
             p.drawLine(QPointF(16, 9), QPointF(22, 9)), p.drawLine(QPointF(19, 6), QPointF(19, 12));
             p.drawLine(QPointF(16, 16), QPointF(22, 16));
         }},
        {"pmiGeometric", [](QPainter& p, const QPen& ink, const QPen& ac) {  // tolerance frame with perpendicularity
             p.setPen(ink);
             p.drawRect(QRectF(2, 7, 20, 10));
             p.drawLine(QPointF(9, 7), QPointF(9, 17)), p.drawLine(QPointF(16, 7), QPointF(16, 17));
             p.setPen(ac);
             p.drawLine(QPointF(5.5, 9), QPointF(5.5, 15)), p.drawLine(QPointF(3.5, 15), QPointF(7.5, 15));
         }},
        {"pmiDatum", [](QPainter& p, const QPen& ink, const QPen& ac) {  // datum: filled triangle and a frame with a letter
             p.setPen(ink);
             p.drawRect(QRectF(7, 2, 10, 9));
             p.drawPolyline(QPolygonF({QPointF(10, 9), QPointF(12, 4), QPointF(14, 9)}));
             p.drawLine(QPointF(12, 11), QPointF(12, 15));
             p.setPen(ac);
             p.setBrush(ac.color());
             p.drawPolygon(QPolygonF({QPointF(7, 21), QPointF(17, 21), QPointF(12, 15)}));
             p.setBrush(Qt::NoBrush);
         }},
        {"exitSketch", [](QPainter& p, const QPen& ink, const QPen& ac) {
             p.setPen(ink);
             p.drawPolygon(QPolygonF{{3, 17}, {9, 21}, {21, 15}, {15, 11}});
             QPen ok = ac;
             ok.setWidthF(2.2);
             p.setPen(ok);
             p.drawLine(QPointF(7, 8), QPointF(10, 11)), p.drawLine(QPointF(10, 11), QPointF(17, 3));
         }},
    };
    return m;
}

}  // namespace

// Constraint badge (the same symbol as the constraint badges in the sketch, §6.6).
QString constraintSymbol(const QString& type) {
    static const std::map<QString, QString> s = {
        {"coincident", "●"}, {"horizontal", "H"}, {"vertical", "V"}, {"parallel", "∥"}, {"perpendicular", "⊥"},
        {"tangent", "◠"}, {"curvature", "κ"}, {"equal", "="}, {"concentric", "◎"}, {"midpoint", "◆"}, {"symmetric", "⇋"}, {"fix", "⚓"}, {"collinear", "≡"}, {"coradial", "⊚"},
        {"intersection", "✕"}, {"pathLength", "Σ"},  // FX7-SK-004: every suggested constraint has a symbol
        {"translate", "⇉"}, {"rotate", "↻"}};
    const auto it = s.find(type);
    return it != s.end() ? it->second : QString();
}

QIcon commandIcon(const QString& name) {
    const auto& m = drawers();
    // Buttons of the «Assembly» tab that repeat part commands (§8а) use the same icon.
    static const std::map<QString, QString> alias = {{"asmSketch", "sketch"}, {"asmPlane", "planeOffset"}, {"asmAxis", "axisTwoPoints"},
                                                     {"asmCut", "extrude"},  // the extrude panel in the assembly mode (FX7-ASM-001)
                                                     {"asmRoughness", "pmiRoughness"}, {"asmPmiGeometric", "pmiGeometric"},  // FX12-PMI-003
                                                     {"asmPmiDatum", "pmiDatum"},
                                                     {"body", "part"},  // body uses the part icon (S2-DOC-001)
                                                     // marks set in the drawing (FX11-DRW-001): the annotation icons
                                                     {"drawingRoughness", "pmiRoughness"}, {"drawingGeometric", "pmiGeometric"},
                                                     {"drawingDatum", "pmiDatum"},
                                                     // constructions on a sheet (FX14-DRW-006): the sketch icons
                                                     {"consLine", "line"}, {"consCircle", "circle"}, {"consArc", "arcCenter"},
                                                     {"consAxis", "centerline"}, {"drawingConsGroup", "consPitch"}};
    const auto al = alias.find(name);
    auto it = m.find(al != alias.end() ? al->second : name);
    // Constraints use their constraint symbol; «Constraints ▾» uses the perpendicularity symbol.
    QString sym;
    if (name.startsWith("constraint_")) sym = constraintSymbol(name.mid(int(sizeof("constraint_")) - 1));
    if (name == "constraints") sym = QString::fromUtf8("⊥");
    if (!sym.isEmpty()) {
        const Draw d = [sym](QPainter& p, const QPen& ink, const QPen&) {
            QFont f = p.font();
            f.setPixelSize(18);
            p.setFont(f);
            p.setPen(ink);
            p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, sym);
        };
        QIcon icon;
        for (int sz : {RIBBON_ICON_PX, RIBBON_ICON_PX * 2}) icon.addPixmap(render(d, sz));
        return icon;
    }
    if (it == m.end()) return {};
    QIcon icon;
    for (int sz : {RIBBON_ICON_PX, RIBBON_ICON_PX * 2}) icon.addPixmap(render(it->second, sz));
    return icon;
}

}  // namespace construe::app
