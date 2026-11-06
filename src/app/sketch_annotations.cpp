// Sketch labels (app/sketch_annotations.h).
#include "app/sketch_annotations.h"

#include "api/sketch_ops.h"

#include "app/icons.h"
#include "app/messages.h"
#include "app/theme.h"
#include "base/geom2.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPolygonF>
#include <QRegularExpression>

#include <cmath>
#include <map>

namespace construe::app {

namespace {
}  // namespace

std::optional<double> SketchAnnotations::dimValue(const sketch::Constraint& c) const {
    if (c.driving.value_or(true)) {
        auto q = session_.checkExpression(c.value.value_or(""), sketch::isAngleDimension(c.type)    ? expr::ANGLE
                                                                : sketch::isNumberDimension(c.type) ? expr::DIMLESS
                                                                                                    : expr::LENGTH);
        if (!q) return std::nullopt;
        return q->value;
    }
    const auto& ref = g_.active().result.reference;
    auto it = ref.find(c.id);
    if (it == ref.end()) return std::nullopt;
    return it->second;
}

std::optional<doc::Json> SketchAnnotations::dimTolerance(const sketch::Constraint& c) const {
    const auto d = session_.document().document;
    for (const auto& a : d->active().annotations)
        if (a.value("type", std::string()) == "fit" && a.contains("dimension") && a["dimension"]["constraint"] == c.id) {
            std::optional<doc::Json> r;
            r.emplace(a);
            return r;
        }
    return std::nullopt;
}

// Tolerance suffix for the dimension text (app-ux §6.6, pmi.md §5.2 toleranceLabel): zone deviations at the current
// dimension value.
QString SketchAnnotations::toleranceSuffix(const sketch::Constraint& c) const {
    const auto t = dimTolerance(c);
    if (!t) return {};
    std::optional<pmi::Deviations> dv;
    if (t->contains("field"))
        if (const auto v = dimValue(c))
            if (auto r = api::Session::fitDeviations((*t)["field"].get<std::string>(), std::abs(*v))) dv = *r;
    return QString::fromStdString(api::Session::toleranceLabel(*t, dv));  // field with deviations (FX13-TOL-002)
}

QString SketchAnnotations::dimText(const sketch::Constraint& c) const {
    const QString base = dimBaseText(c);
    const QString suffix = toleranceSuffix(c);
    if (suffix.isEmpty()) return base;
    if (base.startsWith('(') && base.endsWith(')')) return base.chopped(1) + suffix + ")";  // reference: in parentheses
    return base + suffix;
}

QString SketchAnnotations::dimBaseText(const sketch::Constraint& c) const {
    const auto u = session_.units();
    const auto val = dimValue(c);
    const bool ok = val.has_value();
    const double v = val.value_or(0);
    QString t;
    if (!ok) t = "?";
    else if (sketch::isAngleDimension(c.type)) t = formatNumber(v / expr::angleFactor(u.angle)) + (u.angle == expr::AngleUnit::deg ? "°" : " rad");
    else if (sketch::isNumberDimension(c.type)) t = "w " + formatNumber(v);  // pole weight (S4A-SPL-002, app-ux §6.9)
    else t = formatNumber(std::abs(v) / expr::lengthFactor(u.length));
    if (c.type == "radius") t = "R" + t;
    if (c.diametral) t = QString::fromUtf8("Ø") + t;  // diametral dimension from an axis (S2-SK-003)
    if (c.type == "diameter") t = QString::fromUtf8("⌀") + t;
    if (c.type == "arcLength") t = QString::fromUtf8("⌒") + t;
    if (c.type == "arcAngle") t = QString::fromUtf8("∠") + t;   // central angle of an arc (S2-SK-005)
    if (c.type == "pathLength") t = QString::fromUtf8("Σ") + t;  // path length (S2-SK-005)
    if (c.type == "semiMajor") t = "a " + t;                      // ellipse semi-axes (S4-SKT-004, app-ux §6.9)
    if (c.type == "semiMinor") t = "b " + t;
    if (c.type == "ellipseAngle") t = QString::fromUtf8("∠") + t;
    // Expression (not just a number with a unit) — prefix ƒ.
    if (c.value && c.driving.value_or(true)) {
        static const QRegularExpression plain("^\\s*[+-]?[0-9.]+(e[+-]?[0-9]+)?\\s*(mm|cm|m|in|ft|deg|rad)?\\s*$");
        if (!plain.match(QString::fromStdString(*c.value)).hasMatch()) t = QString::fromUtf8("ƒ ") + t;
    }
    if (!c.driving.value_or(true) && !sketch::isWeakId(c.id)) t = "(" + t + ")";  // weak: no parentheses (§6.6а)
    return t;
}

std::optional<Point2> SketchAnnotations::anchor(const sketch::Constraint& c) const {
    // sketch.md §2.2 «labelOffset» (FX8-SK-004), in the active state: the label follows its points while dragging.
    if (c.refs.empty()) return std::nullopt;
    const auto* d = g_.data();
    auto refAnchor = [&](const std::string& r) -> Point2 {
        if (const auto le = g_.lineEnds(r)) return (le->first + le->second) / 2;
        if (const auto ci = g_.circleOf(r)) return ci->first;
        if (const auto el = g_.ellipseOf(r)) return el->first;
        if (const auto* e = d ? d->findEntity(r) : nullptr) {
            if (e->type == sketch::EntityType::Text) return g_.pointPos(e->center);
            if (sketch::isSplineType(e->type) && !e->poles.empty()) {
                Point2 s = Point2::Zero();
                for (const auto& q : e->poles) s += g_.pointPos(q);
                return s / double(e->poles.size());
            }
        }
        return g_.pointPos(r);
    };
    if (c.type == "weight") return refAnchor(c.refs[0]);
    Point2 m = Point2::Zero();
    for (const auto& r : c.refs) m += refAnchor(r);
    return m / double(c.refs.size());
}

QPointF SketchAnnotations::labelPos(const sketch::Constraint& c) const {
    if (labelOverride_ && labelOverride_->first == c.id) return g_.toScreen(labelOverride_->second);  // being dragged (§6.6)
    if (c.labelOffset)  // relative to the dimension anchor (FX8-SK-004)
        if (const auto a = anchor(c)) return g_.toScreen(*a + Point2((*c.labelOffset)[0], (*c.labelOffset)[1]));
    // Without a saved position — near the middle of the geometry (projections — by their geometry).
    Point2 m = Point2::Zero();
    int n = 0;
    if (c.type == "weight" && !c.refs.empty()) return g_.toScreen(g_.pointPos(c.refs[0])) + QPointF(12, -12);  // at the pole
    for (const auto& r : c.refs) {
        if (const auto le = g_.lineEnds(r)) m += (le->first + le->second) / 2;
        else if (const auto ci = g_.circleOf(r)) m += ci->first;
        else if (const auto el = g_.ellipseOf(r)) m += el->first;  // ellipse: from the center (S4-SKT-004)
        else m += g_.pointPos(r);
        ++n;
    }
    if (n) m /= n;
    return g_.toScreen(m) + QPointF(12, -12);
}

namespace {
// A dimension of an offset contour other than the first: one label per contour (sketch.md §2.6.5, S7F-SK-005).
bool hiddenOffsetDimension(const sketch::SketchData& d, const std::string& id) {
    const auto* g = d.groupOfMember(id);
    if (!g || g->type != "offset") return false;
    for (const auto& m : g->members)
        if (m.id == id) return m.role != "dim0";
    return false;
}
}  // namespace

std::optional<QRectF> SketchAnnotations::labelRect(const std::string& id) const {
    if (!g_.valid()) return std::nullopt;
    const auto* c = g_.data()->findConstraint(id);
    if (!c || !sketch::isDimensionType(c->type) || hiddenOffsetDimension(*g_.data(), id)) return std::nullopt;
    QFont f;
    f.setPixelSize(DIM_TEXT_PX);
    const QFontMetricsF fm(f);
    const QString t = dimText(*c);
    const QPointF p = labelPos(*c);
    const double w = fm.horizontalAdvance(t) + 6, h = fm.height() + 2;
    return QRectF(p.x() - w / 2, p.y() - h / 2, w, h);
}

std::optional<QRectF> SketchAnnotations::glyphRect(const std::string& constraintId) const {
    for (const auto& g : glyphs())
        if (g.constraint == constraintId) return g.rect;
    return std::nullopt;
}

std::vector<Glyph> SketchAnnotations::glyphs() const {
    std::vector<Glyph> out;
    if (!g_.valid() || !glyphsShown_) return out;
    const auto* d = g_.data();
    auto S = [&](const Point2& q) { return g_.toScreen(q); };
    auto P = [&](const std::string& id) { return g_.pointPos(id); };
    // Entity anchor: a point — itself; a segment — midpoint (normal — perpendicular); a curve — rim point at 45°.
    struct Anchor {
        QPointF at, normal;
    };
    auto anchorOf = [&](const std::string& id) -> std::optional<Anchor> {
        if (id == "origin") return Anchor{S(Point2::Zero()), QPointF(0.7071, 0.7071)};
        const auto* e = d->findEntity(id);
        if (!e) return std::nullopt;
        switch (e->type) {
            case sketch::EntityType::Point: return Anchor{S(P(id)), QPointF(0.7071, 0.7071)};
            case sketch::EntityType::Line: {
                const QPointF a = S(P(e->p1)), b = S(P(e->p2)), v = b - a;
                const double l = std::hypot(v.x(), v.y());
                const QPointF n = l > 0 ? QPointF(-v.y() / l, v.x() / l) : QPointF(0, 1);
                return Anchor{(a + b) / 2, n.y() < 0 ? -n : n};  // along the normal, downwards on screen
            }
            case sketch::EntityType::Ellipse:
            case sketch::EntityType::EllipseArc: {  // ellipse point at parameter 45° (S4-SKT-004); arc — its middle (S4-SKT-007)
                const auto el = g_.ellipseOf(id);
                if (!el) return std::nullopt;
                const Point2 mid = e->type == sketch::EntityType::EllipseArc ? g_.ellipseEntityPolyline(*e, 2)[1]
                                                                             : ellipsePoint(el->first, el->second, PI / 4);
                const QPointF at = S(mid), cs = S(el->first), v = at - cs;
                const double l = std::hypot(v.x(), v.y());
                return Anchor{at, l > 0 ? v / l : QPointF(0, 1)};
            }
            case sketch::EntityType::Circle:
            case sketch::EntityType::Arc: {
                const Point2 c = P(e->center);
                const double r = g_.radius(*e);
                double ang = PI / 4;
                if (e->type == sketch::EntityType::Arc) {
                    const Point2 s0 = P(e->start) - c, s1 = P(e->end) - c;
                    const double sw = geom2::ccwSweepPositive(std::atan2(s0.y(), s0.x()), std::atan2(s1.y(), s1.x()));
                    ang = std::atan2(s0.y(), s0.x()) + sw / 2;
                }
                const Point2 dir(std::cos(ang), std::sin(ang));
                const QPointF at = S(c + r * dir), cs = S(c), v = at - cs;
                const double l = std::hypot(v.x(), v.y());
                return Anchor{at, l > 0 ? v / l : QPointF(0, 1)};
            }
            case sketch::EntityType::Bezier:
            case sketch::EntityType::BSpline: {  // the middle of its polyline (S4A-SPL-001)
                const auto pl = g_.splineEntityPolyline(*e);
                if (pl.size() < 2) return std::nullopt;
                const size_t i = pl.size() / 2;
                const QPointF at = S(pl[i]), v = S(pl[i]) - S(pl[i - 1]);
                const double l = std::hypot(v.x(), v.y());
                const QPointF n = l > 0 ? QPointF(-v.y() / l, v.x() / l) : QPointF(0, 1);
                return Anchor{at, n.y() < 0 ? -n : n};
            }
            default: return std::nullopt;
        }
    };
    // Tangency point (§6.6: the tangency icon is at the tangency point).
    auto tangentPoint = [&](const sketch::Constraint& c) -> std::optional<Anchor> {
        const auto* a = d->findEntity(c.refs[0]);
        const auto* b = d->findEntity(c.refs[1]);
        if (!a || !b) return std::nullopt;
        Point2 q;
        if (sketch::isSplineType(b->type)) {  // at the joint with a spline end (S4A-SPL-002): its end nearest to the other curve
            std::vector<std::string> ends = sketch::isSplineType(a->type) ? sketch::splineEnds(*a)
                                            : a->type == sketch::EntityType::Line ? std::vector<std::string>{a->p1, a->p2}
                                                                                  : std::vector<std::string>{a->start, a->end};
            double best = 1e300;
            for (const auto& se : sketch::splineEnds(*b))
                for (const auto& oe : ends)
                    if (const double dd = (P(se) - P(oe)).norm(); dd < best) best = dd, q = P(se);
            if (best == 1e300) return std::nullopt;
            return Anchor{S(q), QPointF(0.7071, 0.7071)};
        }
        if (a->type == sketch::EntityType::Line) {
            const Point2 p1 = P(a->p1), dv = P(a->p2) - p1, c0 = P(b->center);
            q = dv.squaredNorm() > 0 ? Point2(p1 + (c0 - p1).dot(dv) / dv.squaredNorm() * dv) : p1;
        } else {
            const Point2 c1 = P(a->center), c2 = P(b->center), v = c2 - c1;
            q = v.norm() > 0 ? Point2(c1 + g_.radius(*a) * v.normalized()) : c1;
        }
        return Anchor{S(q), QPointF(0.7071, 0.7071)};
    };
    // Points related to the cursor/selection — for coincident/pointOnObject.
    auto involved = [&](const sketch::Constraint& c) {
        for (const auto& r : c.refs)
            if (isSelected_(r) || (hover_.kind == Hit::Kind::Point && hover_.id == r)) return true;
        return false;
    };
    std::map<std::string, int> perAnchor;  // number of icons at an anchor
    auto place = [&](const std::string& key, const Anchor& an, const sketch::Constraint& c, const QString& sym) {
        const int k = perAnchor[key]++;
        const QPointF along(-an.normal.y(), an.normal.x());
        const QPointF centre = an.at + an.normal * GLYPH_OFFSET_PX + along * (k * (GLYPH_PX + 2));
        out.push_back({c.id, QRectF(centre.x() - GLYPH_PX / 2.0, centre.y() - GLYPH_PX / 2.0, GLYPH_PX, GLYPH_PX), sym});
    };
    auto at = [&](const std::string& ref, const sketch::Constraint& c, const QString& sym) {
        if (auto an = anchorOf(ref)) place(ref, *an, c, sym);
    };
    for (const auto& c : d->constraints) {
        const auto& t = c.type;
        // Relations of a linked group are not shown while it is linked (§6.6, sketch.md §2.6.1).
        if (d->groupOfMember(c.id)) continue;
        if (t == "horizontal" || t == "vertical") {
            const QString sym = constraintSymbol(QString::fromStdString(t));
            if (c.refs.size() == 1) at(c.refs[0], c, sym);
            else if (c.refs.size() == 2) {
                auto a = anchorOf(c.refs[0]), b = anchorOf(c.refs[1]);
                if (a && b) {
                    place(c.refs[0] + "|" + c.refs[1], Anchor{(a->at + b->at) / 2, QPointF(0.7071, 0.7071)}, c, sym);
                    // Point aligned with a segment midpoint (FX8-SK-003): the icon shows "|·" and a dashed leader to the
                    // midpoint, so it is not mistaken for an ordinary H / V.
                    const auto* le = d->findEntity(c.refs[1]);
                    if (le && le->type != sketch::EntityType::Point) {
                        out.back().leader = QLineF(a->at, b->at);
                        out.back().symbol = sym + QString::fromUtf8("½");
                    }
                }
            }
        } else if ((t == "parallel" || t == "perpendicular" || t == "equal" || t == "collinear") && c.refs.size() == 2) {
            const QString sym = constraintSymbol(QString::fromStdString(t));
            at(c.refs[0], c, sym);
            at(c.refs[1], c, sym);
        } else if ((t == "tangent" || t == "curvature") && c.refs.size() == 2) {
            // Icons of one joint (tangency and curvature) are placed side by side.
            if (auto an = tangentPoint(c))
                place("joint:" + std::to_string(std::lround(an->at.x())) + ":" + std::to_string(std::lround(an->at.y())), *an, c,
                      constraintSymbol(QString::fromStdString(t)));
        } else if (t == "concentric" && c.refs.size() == 2) {
            if (const auto* e = d->findEntity(c.refs[0]); e && !e->center.empty()) at(e->center, c, QString::fromUtf8("◎"));
        } else if (t == "midpoint" && !c.refs.empty()) {
            at(c.refs[0], c, QString::fromUtf8("◆"));
        } else if (t == "symmetric" && c.refs.size() == 3) {
            at(c.refs[2], c, QString::fromUtf8("⇋"));
        } else if ((t == "translate" || t == "rotate") && c.refs.size() >= 2) {  // copy relation: at the copy (S4-SKT-005)
            at(c.refs[1], c, constraintSymbol(QString::fromStdString(t)));
        } else if (t == "fix" && !c.refs.empty()) {
            const auto* e = d->findEntity(c.refs[0]);
            at(e && e->type == sketch::EntityType::Circle ? e->center : c.refs[0], c, QString::fromUtf8("⚓"));
        } else if ((t == "coincident" || t == "pointOnObject") && !c.refs.empty() && involved(c)) {
            at(c.refs[0], c, QString::fromUtf8(t == "coincident" ? "●" : "⌒"));
        }
    }
    return out;
}

std::optional<DimPrims> SketchAnnotations::dimensionPrims(const sketch::Constraint& c, const QPointF& L) const {
    // The dimension lies in the sketch plane (FX7-SK-016): its geometry is built in sketch coordinates from the label
    // point on the plane and projected, so under any view rotation it turns with the plane. Only the arrow heads and
    // the overshoot of extension lines are in pixels — along the projected lines.
    // References to projections — by their geometry (app-ux §6.9, FX1-SKG-004): segment — lineEnds, curve — circleOf.
    const auto Lp = g_.toLocal(L);
    if (!Lp) return std::nullopt;  // the plane is seen edge-on
    auto S = [&](const Point2& q) { return g_.toScreen(q); };
    auto P = [&](const std::string& id) { return g_.pointPos(id); };
    auto lenS = [](const QPointF& v) { return std::hypot(v.x(), v.y()); };
    auto unitS = [&](const QPointF& v) { const double l = lenS(v); return l > 0 ? v / l : QPointF(1, 0); };
    auto unit = [](const Point2& v) { return v.squaredNorm() > 0 ? Point2(v.normalized()) : Point2(1, 0); };
    auto foot = [](const Point2& q, const std::pair<Point2, Point2>& le) {
        const Point2 a = le.first, dv = le.second - le.first;
        return dv.squaredNorm() > 0 ? Point2(a + (q - a).dot(dv) / dv.squaredNorm() * dv) : a;
    };
    DimPrims out;
    // Arrows at both ends of a dimension line a–b: inside, or outside for a short line, with the line continued beyond
    // them (app-ux §6.6, FX10-IO-007).
    auto addArrows = [&](const QPointF& a, const QPointF& b) {
        const double l = lenS(b - a);
        if (l <= 1) return;
        if (l >= DIM_ARROWS_INSIDE_MIN_PX) {
            out.arrows.push_back({a, a - b});
            out.arrows.push_back({b, b - a});
            return;
        }
        const QPointF u = unitS(b - a);
        out.arrows.push_back({a, b - a});
        out.arrows.push_back({b, a - b});
        out.dimension.push_back(QLineF(a, a - u * DIM_ARROW_STUB_PX));
        out.dimension.push_back(QLineF(b, b + u * DIM_ARROW_STUB_PX));
    };
    const auto& t = c.type;
    if (t == "weight") {  // pole weight (S4A-SPL-002): a leader from the pole to the label
        if (c.refs.empty()) return std::nullopt;
        const QPointF A = S(P(c.refs[0]));
        if (lenS(L - A) > 1) out.extension.push_back(QLineF(A, L));
        return out;
    }
    if (t == "length" || t == "distance" || t == "distanceX" || t == "distanceY") {
        Point2 A, B;
        if (t == "length") {
            const auto le = c.refs.empty() ? std::nullopt : g_.lineEnds(c.refs[0]);
            if (!le) return std::nullopt;
            A = le->first, B = le->second;
        } else if (c.refs.size() == 2 && c.arc) {
            // To a circle/arc (§6.9): nearest or farthest points along the line of centers / perpendicular.
            const bool far = *c.arc == "far";
            const auto cc = g_.circleOf(c.refs[1]);
            if (!cc) return std::nullopt;
            const Point2 o = cc->first;
            const double r = cc->second;
            if (const auto c1 = g_.circleOf(c.refs[0])) {  // curve — curve
                const Point2 o1 = c1->first, u12 = unit(o - o1);
                const double r1 = c1->second;
                A = far ? Point2(o1 - u12 * r1) : Point2(o1 + u12 * r1);
                B = far ? Point2(o + u12 * r) : Point2(o - u12 * r);
            } else if (const auto le = g_.lineEnds(c.refs[0])) {  // line — curve: along the line normal (FX1-SKG-002)
                const Point2 f = foot(o, *le), u = unit(o - f);
                A = f;
                if (c.branch && c.branch->kind == sketch::Branch::Kind::Side) {
                    // Tangent by equation (solver.md §4.2.15а): nearest — sd(T) = sd(o) − side·R, farthest — + side·R.
                    const Point2 dv = le->second - le->first;
                    const Point2 nl = dv.squaredNorm() > 0 ? Point2(Point2(-dv.y(), dv.x()).normalized()) : Point2(0, 1);
                    B = o + nl * ((far ? 1.0 : -1.0) * c.branch->side * r);
                } else {
                    B = far ? Point2(o + u * r) : Point2(o - u * r);
                }
            } else {  // point — curve
                const Point2 q = P(c.refs[0]), u = unit(q - o);
                A = q;
                B = far ? Point2(o - u * r) : Point2(o + u * r);
            }
        } else if (c.refs.size() == 2 && g_.lineEnds(c.refs[1])) {
            // Point — line: to the foot of the perpendicular.
            const Point2 q = P(c.refs[0]);
            A = q, B = foot(q, *g_.lineEnds(c.refs[1]));
            // Diametral (S2-SK-003, FX8-SK-001): symmetric about the axis — the second extension line is the mirror
            // image of the point, the dimension line spans the whole diameter.
            if (c.diametral) {
                sketch::SketchData sd = *g_.data();
                sd.projections = g_.active().projections;
                // The point lies on the axis and the segment is the radius side (FX13-DIM-001): symmetric about the
                // axis through the point — from the segment to its mirror image.
                if (api::sketchops::diametralAxisAtPoint(sd, c, &g_.active().result.state)) A = 2 * A - B, std::swap(A, B);
                else B = 2 * B - A;
            }
        } else if (c.refs.size() == 2) {
            A = P(c.refs[0]), B = P(c.refs[1]);
            // Diametral from an axis-equivalent point (FX9-SK-001): the other point and its mirror image about the
            // point on the axis — the dimension line spans the whole diameter.
            if (c.diametral) {
                sketch::SketchData sd = *g_.data();
                sd.projections = g_.active().projections;
                const int i = api::sketchops::diametralAxisRef(sd, g_.active().result.state, c).value_or(0);
                const Point2 axis = i == 0 ? A : B, other = i == 0 ? B : A;
                A = other, B = 2 * axis - other;
            }
        } else {
            return std::nullopt;
        }
        Point2 u = unit(B - A);
        if (t == "distanceX") u = Point2(1, 0);
        if (t == "distanceY") u = Point2(0, 1);
        const Point2 n(-u.y(), u.x());
        const Point2 A2 = A + n * (*Lp - A).dot(n), B2 = B + n * (*Lp - B).dot(n);
        // Extension lines (with a small overshoot beyond the dimension line, along the projected line).
        for (const auto& [from, to] : {std::pair{A, A2}, std::pair{B, B2}}) {
            const QPointF f = S(from), e = S(to);
            if (lenS(e - f) > 1) out.extension.push_back(QLineF(f, e + unitS(e - f) * 3));
        }
        out.dimension.push_back(QLineF(S(A2), S(B2)));
        // Text outside the dimension line — extend it to the label.
        const Point2 ud = unit(B2 - A2);
        const double sl = (*Lp - A2).dot(ud), total = (B2 - A2).norm();
        if (sl < 0) out.dimension.push_back(QLineF(S(A2), S(A2 + ud * sl)));
        if (sl > total) out.dimension.push_back(QLineF(S(B2), S(A2 + ud * sl)));
        addArrows(S(A2), S(B2));
        return out;
    }
    if (t == "radius" || t == "diameter") {
        const auto cc = c.refs.empty() ? std::nullopt : g_.circleOf(c.refs[0]);
        if (!cc) return std::nullopt;
        const Point2 c0 = cc->first;
        const double r = cc->second;
        const Point2 v = unit(*Lp - c0);
        const QPointF C = S(c0), Q = S(c0 + v * r);
        if (t == "radius") {
            out.dimension.push_back(QLineF(C, L));
            out.arrows.push_back({Q, Q - C});
        } else {
            const QPointF Q2 = S(c0 - v * r);
            out.dimension.push_back(QLineF(Q2, (*Lp - c0).norm() > r ? L : Q));
            addArrows(Q2, Q);
        }
        return out;
    }
    if (t == "semiMajor" || t == "semiMinor" || t == "ellipseAngle") {
        // Ellipse (§6.9, S4-SKT-004): semi-axis — from the center to its end; rotation — arc from axis x to semi-axis rx.
        const auto el = c.refs.empty() ? std::nullopt : g_.ellipseOf(c.refs[0]);
        if (!el) return std::nullopt;
        const Point2 c0 = el->first;
        const auto& sh = el->second;
        const Point2 u(std::cos(sh.angle), std::sin(sh.angle)), v(-u.y(), u.x());
        const QPointF C = S(c0);
        if (t != "ellipseAngle") {
            const QPointF E = S(t == "semiMajor" ? Point2(c0 + sh.rx * u) : Point2(c0 + sh.ry * v));
            out.dimension.push_back(QLineF(C, E));
            out.arrows.push_back({E, E - C});
            if (lenS(L - E) > 1) out.extension.push_back(QLineF(E, L));
            return out;
        }
        const double rs = (*Lp - c0).norm();  // arc radius in sketch coordinates
        if (lenS(L - C) < 2 || rs <= 0) return std::nullopt;
        double a = std::fmod(sh.angle, PI);
        if (a < 0) a += PI;
        auto ptAt = [&](double ang) { return S(c0 + rs * Point2(std::cos(ang), std::sin(ang))); };
        out.extension.push_back(QLineF(C, ptAt(0)));
        out.extension.push_back(QLineF(C, ptAt(a)));
        const int n = 32;
        for (int i = 0; i < n; ++i) out.dimension.push_back(QLineF(ptAt(a * i / n), ptAt(a * (i + 1) / n)));
        if (a > 1e-9) out.arrows.push_back({ptAt(a), ptAt(a) - ptAt(a * (n - 1) / n)});
        return out;
    }
    if (t == "angle" && c.refs.size() == 2) {
        const auto l1 = g_.lineEnds(c.refs[0]), l2 = g_.lineEnds(c.refs[1]);
        if (!l1 || !l2) return std::nullopt;
        const Point2 a1 = l1->first, d1 = l1->second - l1->first;
        const Point2 a2 = l2->first, d2 = l2->second - l2->first;
        const double den = d1.x() * d2.y() - d1.y() * d2.x();
        if (std::abs(den) < 1e-12 * std::max(1.0, d1.norm() * d2.norm())) return std::nullopt;
        const double tt = ((a2 - a1).x() * d2.y() - (a2 - a1).y() * d2.x()) / den;
        const Point2 O = a1 + d1 * tt;
        const double R = (*Lp - O).norm();
        if (lenS(L - S(O)) < 2 || R <= 0) return std::nullopt;
        // The sector between rays along the lines that contains the label (sketch angles, counterclockwise).
        const double angL = std::atan2((*Lp - O).y(), (*Lp - O).x());
        double best = -1, s0 = 0, sw0 = 0;
        for (double ang1 : {std::atan2(d1.y(), d1.x()), std::atan2(-d1.y(), -d1.x())})
            for (double ang2 : {std::atan2(d2.y(), d2.x()), std::atan2(-d2.y(), -d2.x())}) {
                const double sw = geom2::normalizeAngleSym(ang2 - ang1);
                const double rel = geom2::normalizeAngleSym(angL - ang1);
                const bool inside = sw >= 0 ? (rel >= 0 && rel <= sw) : (rel <= 0 && rel >= sw);
                if (inside && (best < 0 || std::abs(sw) < best)) best = std::abs(sw), s0 = ang1, sw0 = sw;
            }
        if (best < 0) return std::nullopt;
        auto ptAt = [&](double a) { return S(O + R * Point2(std::cos(a), std::sin(a))); };
        const int n = 32;
        for (int i = 0; i < n; ++i) out.dimension.push_back(QLineF(ptAt(s0 + sw0 * i / n), ptAt(s0 + sw0 * (i + 1) / n)));
        out.arrows.push_back({ptAt(s0), ptAt(s0) - ptAt(s0 + sw0 / n)});
        out.arrows.push_back({ptAt(s0 + sw0), ptAt(s0 + sw0) - ptAt(s0 + sw0 * (n - 1) / n)});
        return out;
    }
    return std::nullopt;
}

bool SketchAnnotations::drawDimensionGeometry(QPainter& p, const sketch::Constraint& c, const QPointF& L, const QColor& col) const {
    const auto prims = dimensionPrims(c, L);
    if (!prims) return false;
    p.setPen(QPen(col, 1));
    for (const auto& l : prims->extension) p.drawLine(l);
    for (const auto& l : prims->dimension) p.drawLine(l);
    // Arrow with its tip at tip, pointing along dir.
    for (const auto& [tip, dir] : prims->arrows) {
        const double l = std::hypot(dir.x(), dir.y());
        const QPointF u = l > 0 ? dir / l : QPointF(1, 0), n(-u.y(), u.x());
        const QPointF base = tip - u * DIM_ARROW_PX;
        p.setBrush(col);
        p.setPen(Qt::NoPen);
        p.drawPolygon(QPolygonF{tip, base + n * 3, base - n * 3});
        p.setBrush(Qt::NoBrush);
    }
    return true;
}

double SketchAnnotations::dimensionLineDistance(const sketch::Constraint& c, const QPointF& s) const {
    if (!g_.valid() || !sketch::isDimensionType(c.type) || hiddenOffsetDimension(*g_.data(), c.id)) return 1e18;
    const auto prims = dimensionPrims(c, labelPos(c));
    if (!prims) return 1e18;
    double best = 1e18;
    for (const auto& l : prims->dimension) best = std::min(best, segDist(s, l.p1(), l.p2()));
    return best;
}

}  // namespace construe::app
