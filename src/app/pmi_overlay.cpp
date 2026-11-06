#include "app/messages.h"
#include "app/pmi_overlay.h"

#include "app/theme.h"

#include <QFontMetricsF>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <map>

namespace construe::app {

namespace {

QPointF unit(const QPointF& v) {
    const double l = std::hypot(v.x(), v.y());
    return l > 0 ? v / l : QPointF(1, 0);
}

QVector3D q3(const Point3& p) { return QVector3D(float(p.x()), float(p.y()), float(p.z())); }

// Leader arrow with its tip at tip, pointing away from from.
void arrow(QPainter& p, const QPointF& from, const QPointF& tip, const QColor& c) {
    const QPointF u = unit(tip - from), n(-u.y(), u.x());
    QPolygonF head{tip, tip - u * PMI_ARROW_PX + n * (PMI_ARROW_PX * 0.35), tip - u * PMI_ARROW_PX - n * (PMI_ARROW_PX * 0.35)};
    p.setBrush(c);
    p.drawPolygon(head);
    p.setBrush(Qt::NoBrush);
}

// Symbol (pmi.md §6) at scale s with the symbol origin at o; symbol y axis points up (screen y points down).
void drawGlyph(QPainter& p, const pmi::Glyph& g, const QPointF& o, double s, const QColor& c) {
    auto S = [&](const Point2& q) { return QPointF(o.x() + q.x() * s, o.y() - q.y() * s); };
    for (const auto& pl : g.polylines) {
        QPolygonF poly;
        for (const auto& q : pl) poly << S(q);
        p.drawPolyline(poly);
    }
    for (const auto& ci : g.circles) p.drawEllipse(S(ci.center), ci.r * s, ci.r * s);
    p.setBrush(c);
    for (const auto& f : g.filled) {
        QPolygonF poly;
        for (const auto& q : f) poly << S(q);
        p.drawPolygon(poly);
    }
    p.setBrush(Qt::NoBrush);
}

}  // namespace

QString annotationTitle(const api::Session::AnnotationInfo& a) {
    const std::string type = a.def.value("type", std::string());
    const auto Q = [](const std::string& s) { return QString::fromStdString(s); };
    if (type == "roughness") {
        const std::string t = pmi::roughnessText(a.def);
        return t.empty() ? QString(txt("ui.common.roughness")) : txt("ui.pmi.roughness") + Q(t);
    }
    if (type == "fit") {
        std::optional<pmi::Deviations> dv;
        if (a.upper && a.lower) dv = pmi::Deviations{*a.upper, *a.lower};
        const QString suffix = Q(pmi::toleranceSuffix(a.def, dv)).trimmed();
        if (a.def.contains("dimension"))
            return txt("ui.pmi.sizeToleranceD") + Q(a.def["dimension"]["constraint"].get<std::string>().substr(1)) + " " + suffix;
        if (a.nominal) {
            const QString n = "⌀" + Q(pmi::formatNumber(*a.nominal, pmi::SIZE_DECIMALS));
            return txt("ui.pmi.tolerance") + n + (a.def.contains("field") ? suffix : " " + suffix);
        }
        return txt("ui.pmi.tolerance") + suffix;
    }
    if (type == "geometric") {
        QString t = Q(pmi::characteristicName(a.def.value("characteristic", std::string()))) + " " + Q(pmi::frameValueText(a.def));
        const std::string m = a.def.value("material", std::string());
        if (m == "M") t += " Ⓜ";
        if (m == "L") t += " Ⓛ";
        if (a.def.contains("datums"))
            for (const auto& l : a.def["datums"]) t += " " + Q(l.get<std::string>());
        return t;
    }
    if (type == "datum") return txt("ui.pmi.datum") + Q(a.def.value("letter", std::string()));
    return Q(a.id);
}

QString fitLabelText(const api::Session::AnnotationInfo& a) {
    std::optional<pmi::Deviations> dv;
    if (a.upper && a.lower) dv = pmi::Deviations{*a.upper, *a.lower};
    const QString t = a.nominal ? "⌀" + QString::fromStdString(pmi::formatNumber(*a.nominal, pmi::SIZE_DECIMALS)) : QString();
    return (t + QString::fromStdString(api::Session::toleranceLabel(a.def, dv))).trimmed();
}

QVector3D AnnotationOverlay::depthProbe(const api::Session::AnnotationInfo& a, const Camera& cam) {
    Point3 at = *a.anchor;
    if (a.normal) at += *a.normal * std::max(PMI_DEPTH_LIFT_PX * cam.mmPerPx(), PMI_DEPTH_LIFT_MM);
    return q3(at);
}

void AnnotationOverlay::paint(QPainter& p, const Camera& cam, const std::string& selected, const VisibleFn& visible) {
    rects_.clear();
    drawn_.clear();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont font = p.font();
    font.setPixelSize(PMI_TEXT_PX);
    p.setFont(font);
    const QFontMetricsF fm(font);
    const double h = PMI_TEXT_PX;
    std::map<std::string, int> stack;  // annotation index among annotations of the same face
    for (const auto& a : items_) {
        if (!a.anchor || hidden_.count(a.id)) continue;
        if (visible && !visible(depthProbe(a, cam))) continue;  // behind the model (FX12-PMI-001)
        QPointF A = cam.project(q3(*a.anchor));
        QPointF d(1 / std::sqrt(2.0), -1 / std::sqrt(2.0));
        if (a.normal) {
            const double probe = 30 * cam.mmPerPx();
            const QPointF dn = cam.project(q3(*a.anchor + *a.normal * probe)) - A;
            if (std::hypot(dn.x(), dn.y()) >= PMI_NORMAL_MIN * 30) d = unit(dn);
        }
        const int k = stack[a.face]++;
        QPointF L = A + d * (PMI_LEADER_PX + k * PMI_STACK_PX);
        // Position set by dragging (FX12-PMI-002): the label point is anchor + labelOffset; the leader direction — to it.
        if (const auto off = a.def.find("labelOffset"); off != a.def.end() && off->is_array() && off->size() == 3) {
            // On the exploded view the offset turns with the instance (FX14-PMI-001: `move`, identity otherwise).
            const Point3 w = *a.anchor + Point3(a.move.applyDir(Vec3((*off)[0].get<double>(), (*off)[1].get<double>(), (*off)[2].get<double>())));
            L = cam.project(q3(w));
        }
        if (preview_ && preview_->id == a.id) {
            L = preview_->label;
            if (preview_->anchor) A = *preview_->anchor;
        }
        if (std::hypot((L - A).x(), (L - A).y()) > 1e-6) d = unit(L - A);
        drawn_.push_back({a.id, {A, L}});
        const QColor c = a.error ? COLOR_ERROR.color : a.id == selected ? COLOR_SELECTED.color : COLOR_PMI.color;
        p.setPen(QPen(c, 1.4));
        const std::string type = a.def.value("type", std::string());
        QRectF box;
        if (type == "roughness") {
            // GOST 2.309 (FX12-PMI-006): the sign stands with its vertex on the shelf of a leader that ends with an
            // arrow at the surface — as on the drawing (drawing.md §6.18, view_pmi roughness).
            const QString param = QString::fromStdString(pmi::roughnessText(a.def));
            const QString method = QString::fromStdString(a.def.value("method", std::string()));
            const QString lay = QString::fromStdString(pmi::laySymbol(a.def.value("lay", std::string())));
            const double tw = std::max(fm.horizontalAdvance(param), fm.horizontalAdvance(method)) / h;
            const bool shelf = !param.isEmpty() || !method.isEmpty() || !lay.isEmpty();
            const double xl = pmi::ROUGHNESS_SHORT_LEG_X, xr = pmi::ROUGHNESS_LONG_LEG_X + (shelf ? tw + pmi::ROUGHNESS_SHELF_PAD : 0);
            QPointF V;  // sign vertex on the leader shelf
            if (d.x() >= 0) {
                V = L + QPointF(h * (pmi::ROUGHNESS_LEADER_GAP - xl), 0);
                p.drawLine(L, QPointF(V.x() + h * xr, L.y()));
            } else {
                V = L - QPointF(h * (pmi::ROUGHNESS_LEADER_GAP + xr), 0);
                p.drawLine(QPointF(V.x() + h * xl, L.y()), L);
            }
            p.drawLine(L, A);
            arrow(p, L, A, c);
            drawGlyph(p, pmi::roughnessGlyph(a.def.value("sign", std::string("any")), tw, shelf), V, h, c);
            auto textAt = [&](const Point2& at, const QString& t) {
                const QRectF r(V.x() + at.x() * h, V.y() - (at.y() + 1) * h, fm.horizontalAdvance(t) + 2, h);
                p.drawText(r, Qt::AlignLeft | Qt::AlignBottom, t);
            };
            if (!param.isEmpty()) textAt(pmi::ROUGHNESS_PARAMETER_AT, param);
            if (!method.isEmpty()) textAt(pmi::ROUGHNESS_METHOD_AT, method);
            if (!lay.isEmpty()) textAt(pmi::ROUGHNESS_LAY_AT, lay);
            box = QRectF(V.x() + (xl - 0.1) * h, V.y() - 4.3 * h, (xr - xl + 0.2) * h, 4.3 * h);
        } else if (type == "fit") {
            const QString t = fitLabelText(a);
            const double w = fm.horizontalAdvance(t) + 4;
            box = QRectF(d.x() >= 0 ? L.x() : L.x() - w, L.y() - h * 1.2, w, h * 1.3);
            p.drawLine(L, A);
            arrow(p, L, A, c);
            p.drawLine(QPointF(box.left(), L.y()), QPointF(box.right(), L.y()));  // text shelf (leader landing line)
            p.drawText(box.adjusted(2, 0, 0, -h * 0.15), Qt::AlignLeft | Qt::AlignBottom, t);
        } else if (type == "geometric") {
            const std::string ch = a.def.value("characteristic", std::string());
            const QString val = QString::fromStdString(pmi::frameValueText(a.def));
            const QString mat = QString::fromStdString(a.def.value("material", std::string()));
            std::vector<QString> datums;
            if (a.def.contains("datums"))
                for (const auto& l : a.def["datums"]) datums.push_back(QString::fromStdString(l.get<std::string>()));
            const double cell = 2 * h, valW = fm.horizontalAdvance(val) + h + (mat.isEmpty() ? 0 : 1.4 * h);
            const double w = cell + valW + cell * double(datums.size());
            const double x0 = d.x() >= 0 ? L.x() : L.x() - w;
            box = QRectF(x0, L.y() - h, w, cell);
            p.drawLine(QPointF(d.x() >= 0 ? box.left() : box.right(), L.y()), A);
            arrow(p, L, A, c);
            p.drawRect(box);
            drawGlyph(p, pmi::characteristicGlyph(ch), QPointF(x0, box.bottom()), cell, c);
            double x = x0 + cell;
            p.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
            p.drawText(QRectF(x + h / 2, box.top(), fm.horizontalAdvance(val) + 2, cell), Qt::AlignLeft | Qt::AlignVCenter, val);
            if (!mat.isEmpty()) {
                const QPointF mc(x + h / 2 + fm.horizontalAdvance(val) + 0.8 * h, L.y());
                p.drawEllipse(mc, 0.6 * h, 0.6 * h);
                p.drawText(QRectF(mc.x() - 0.6 * h, mc.y() - 0.6 * h, 1.2 * h, 1.2 * h), Qt::AlignCenter, mat);
            }
            x += valW;
            for (const auto& l : datums) {
                p.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
                p.drawText(QRectF(x, box.top(), cell, cell), Qt::AlignCenter, l);
                x += cell;
            }
        } else if (type == "datum") {
            const QPointF n(-d.y(), d.x());
            const QPointF apex = A + d * (1.04 * h);
            p.setBrush(c);
            p.drawPolygon(QPolygonF{A + n * (0.6 * h), A - n * (0.6 * h), apex});
            p.setBrush(Qt::NoBrush);
            box = QRectF(L.x() - h, L.y() - h, 2 * h, 2 * h);
            p.drawLine(apex, QPointF(L.x(), d.y() <= 0 ? box.bottom() : box.top()));
            p.drawRect(box);
            p.drawText(box, Qt::AlignCenter, QString::fromStdString(a.def.value("letter", std::string())));
        }
        if (!box.isNull()) rects_.push_back({a.id, box});
    }
    p.restore();
}

std::optional<std::string> AnnotationOverlay::hit(const QPointF& at) const {
    for (auto it = rects_.rbegin(); it != rects_.rend(); ++it)
        if (it->second.adjusted(-3, -3, 3, 3).contains(at)) return it->first;
    return std::nullopt;
}

std::optional<QPointF> AnnotationOverlay::anchorScreen(const std::string& id) const {
    for (const auto& [i, d] : drawn_)
        if (i == id) return d.anchor;
    return std::nullopt;
}

std::optional<QPointF> AnnotationOverlay::labelScreen(const std::string& id) const {
    for (const auto& [i, d] : drawn_)
        if (i == id) return d.label;
    return std::nullopt;
}

std::optional<std::string> AnnotationOverlay::anchorHit(const QPointF& at) const {
    std::optional<std::string> best;
    double bd = PICK_POINT_PX;
    for (const auto& [i, d] : drawn_)
        if (const double r = std::hypot(d.anchor.x() - at.x(), d.anchor.y() - at.y()); r <= bd) bd = r, best = i;
    return best;
}

void AnnotationOverlay::setDragPreview(const std::string& id, const QPointF& label, std::optional<QPointF> anchor) {
    preview_ = Preview{id, label, anchor};
}

std::optional<QVector3D> AnnotationOverlay::labelWorld(const api::Session::AnnotationInfo& a) {
    if (!a.anchor) return std::nullopt;
    if (const auto off = a.def.find("labelOffset"); off != a.def.end() && off->is_array() && off->size() == 3)
        return q3(*a.anchor + Point3(a.move.applyDir(Vec3((*off)[0].get<double>(), (*off)[1].get<double>(), (*off)[2].get<double>()))));
    return std::nullopt;
}

std::optional<QRectF> AnnotationOverlay::labelRect(const std::string& id) const {
    for (const auto& [i, r] : rects_)
        if (i == id) return r;
    return std::nullopt;
}

}  // namespace construe::app
