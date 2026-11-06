// Screen geometry of the open sketch (app/sketch_geometry.h).
#include "app/sketch_geometry.h"
#include "app/text_box.h"

#include "app/theme.h"
#include "base/ids.h"
#include "base/geom2.h"

#include <QVector3D>

#include <algorithm>
#include <cmath>

namespace construe::app {

namespace {
}  // namespace

double segDist(const QPointF& p, const QPointF& a, const QPointF& b) {
    const QPointF ab = b - a, ap = p - a;
    const double l2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = l2 > 0 ? (ap.x() * ab.x() + ap.y() * ab.y()) / l2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF c = a + ab * t;
    return std::hypot(p.x() - c.x(), p.y() - c.y());
}

Point2 SketchGeometry::pointPos(const std::string& id) const {
    if (id == "origin" || !active_) return Point2::Zero();
    auto it = active_->result.state.points.find(id);
    return it != active_->result.state.points.end() ? it->second : Point2::Zero();
}

double SketchGeometry::radius(const sketch::Entity& e) const {
    if (e.type == sketch::EntityType::Arc) return (pointPos(e.start) - pointPos(e.center)).norm();
    auto it = active_->result.state.radii.find(e.id);
    return it != active_->result.state.radii.end() ? it->second : e.r;
}

std::optional<std::pair<Point2, Point2>> SketchGeometry::lineEnds(const std::string& id) const {
    const auto* e = id == "origin" || !data_ ? nullptr : data_->findEntity(id);
    if (!e) return std::nullopt;
    if (e->type == sketch::EntityType::Line) return std::pair{pointPos(e->p1), pointPos(e->p2)};
    if (e->type == sketch::EntityType::Projected && base_)
        if (auto it = base_->projections.find(id); it != base_->projections.end() && it->second.kind == sketch::Projection::Kind::Line)
            return std::pair{Point2(it->second.x1, it->second.y1), Point2(it->second.x2, it->second.y2)};
    return std::nullopt;
}

std::optional<std::pair<Point2, double>> SketchGeometry::circleOf(const std::string& id) const {
    const auto* e = id == "origin" || !data_ ? nullptr : data_->findEntity(id);
    if (!e) return std::nullopt;
    if (e->type == sketch::EntityType::Circle || e->type == sketch::EntityType::Arc) return std::pair{pointPos(e->center), radius(*e)};
    if (e->type == sketch::EntityType::Projected && base_)
        if (auto it = base_->projections.find(id); it != base_->projections.end()) {
            const auto& g = it->second;
            if (g.kind == sketch::Projection::Kind::Circle) return std::pair{Point2(g.cx, g.cy), g.r};
            if (g.kind == sketch::Projection::Kind::Arc) return std::pair{Point2(g.cx, g.cy), std::hypot(g.sx - g.cx, g.sy - g.cy)};
        }
    return std::nullopt;
}

std::optional<std::pair<Point2, sketch::SolvedState::EllipseShape>> SketchGeometry::ellipseOf(const std::string& id) const {
    const auto* e = id == "origin" || !data_ ? nullptr : data_->findEntity(id);
    if (!e || !sketch::isEllipseType(e->type)) return std::nullopt;
    auto it = active_->result.state.ellipses.find(id);
    return std::pair{pointPos(e->center), it != active_->result.state.ellipses.end() ? it->second
                                                                                   : sketch::SolvedState::EllipseShape{e->rx, e->ry, e->angle}};
}

Point2 ellipsePoint(const Point2& c, const sketch::SolvedState::EllipseShape& sh, double t) {
    const double x = sh.rx * std::cos(t), y = sh.ry * std::sin(t), cs = std::cos(sh.angle), sn = std::sin(sh.angle);
    return c + Point2(x * cs - y * sn, x * sn + y * cs);
}

double ellipseParam(const Point2& c, const sketch::SolvedState::EllipseShape& sh, const Point2& p) {
    const double cs = std::cos(sh.angle), sn = std::sin(sh.angle);
    const Point2 w = p - c;
    return std::atan2((-w.x() * sn + w.y() * cs) / sh.ry, (w.x() * cs + w.y() * sn) / sh.rx);
}

std::vector<Point2> ellipseArcPolyline(const Point2& c, const sketch::SolvedState::EllipseShape& sh, const Point2& start,
                                       const Point2& end, int n) {
    const double t0 = ellipseParam(c, sh, start);
    double sw = geom2::ccwSweepPositive(t0, ellipseParam(c, sh, end));
    std::vector<Point2> out;
    for (int i = 0; i <= n; ++i) out.push_back(ellipsePoint(c, sh, t0 + sw * i / n));
    return out;
}

std::vector<Point2> splinePolyline(const sketch::Entity& e, const std::function<Point2(const std::string&)>& pos) {
    sketch::SolvedState st;
    for (const auto& q : e.poles) st.points[q] = pos(q);
    const spline::Curve c = sketch::splineCurve(e, st);
    if (!spline::validate(c).empty()) return {};
    Point2 lo = c.poles[0], hi = c.poles[0];
    for (const auto& q : c.poles) lo = lo.cwiseMin(q), hi = hi.cwiseMax(q);
    const spline::Flat f = spline::expand(c);
    std::vector<Point2> out;
    for (double t : spline::flattenParams(f, std::max(1e-6, 1e-3 * (hi - lo).norm()))) out.push_back(spline::eval(f, t));
    return out;
}

std::vector<Point2> SketchGeometry::splineEntityPolyline(const sketch::Entity& e) const {
    return splinePolyline(e, [this](const std::string& id) { return pointPos(id); });
}

std::vector<Point2> SketchGeometry::ellipseEntityPolyline(const sketch::Entity& e, int n) const {
    const auto el = ellipseOf(e.id);
    if (!el) return {};
    if (e.type == sketch::EntityType::EllipseArc) return ellipseArcPolyline(el->first, el->second, pointPos(e.start), pointPos(e.end), n);
    return ellipsePolyline(el->first, el->second, n);
}

std::vector<Point2> ellipsePolyline(const Point2& c, const sketch::SolvedState::EllipseShape& sh, int n) {
    std::vector<Point2> out;
    for (int i = 0; i <= n; ++i) out.push_back(ellipsePoint(c, sh, 2 * PI * i / n));
    return out;
}

QPointF SketchGeometry::toScreen(const Point2& local) const {
    if (!base_) return {};
    const Point3 w = base_->frame.toWorld(local);
    return camera_.project(QVector3D(float(w.x()), float(w.y()), float(w.z())));
}

std::optional<Point2> SketchGeometry::toLocal(const QPointF& s) const {
    if (!base_) return std::nullopt;
    QVector3D o, dir;
    camera_.ray(s, o, dir);
    const Plane& f = base_->frame;
    const QVector3D n(float(f.normal.x()), float(f.normal.y()), float(f.normal.z()));
    const QVector3D po(float(f.origin.x()), float(f.origin.y()), float(f.origin.z()));
    const float den = QVector3D::dotProduct(n, dir);
    if (std::abs(den) < 1e-9f) return std::nullopt;
    const float t = QVector3D::dotProduct(n, po - o) / den;
    const QVector3D w = o + dir * t;
    return f.toLocal(Point3(w.x(), w.y(), w.z()));
}

std::vector<QPointF> SketchGeometry::projectedScreen(const std::string& id) const {
    std::vector<QPointF> out;
    if (!base_) return out;
    const auto it = base_->projections.find(id);
    if (it == base_->projections.end()) return out;
    const sketch::Projection& g = it->second;
    switch (g.kind) {
        case sketch::Projection::Kind::Point: out.push_back(toScreen(Point2(g.px, g.py))); break;
        case sketch::Projection::Kind::Line: out = {toScreen(Point2(g.x1, g.y1)), toScreen(Point2(g.x2, g.y2))}; break;
        case sketch::Projection::Kind::Arc:
        case sketch::Projection::Kind::Circle: {
            const Point2 c(g.cx, g.cy);
            double a0 = 0, sw = 2 * PI, r = g.r;
            if (g.kind == sketch::Projection::Kind::Arc) {
                r = std::hypot(g.sx - g.cx, g.sy - g.cy);
                a0 = std::atan2(g.sy - g.cy, g.sx - g.cx);
                sw = geom2::ccwSweepPositive(a0, std::atan2(g.ey - g.cy, g.ex - g.cx));
            }
            const int n = 96;
            for (int i = 0; i <= n; ++i) out.push_back(toScreen(c + r * Point2(std::cos(a0 + sw * i / n), std::sin(a0 + sw * i / n))));
            break;
        }
    }
    return out;
}

double SketchGeometry::distToEntity(const sketch::Entity& e, const QPointF& s) const {
    switch (e.type) {
        case sketch::EntityType::Line: return segDist(s, toScreen(pointPos(e.p1)), toScreen(pointPos(e.p2)));
        case sketch::EntityType::Circle:
        case sketch::EntityType::Arc: {
            const Point2 c = pointPos(e.center);
            const double r = radius(e);
            // Discretization in screen coordinates.
            const int N = 96;
            double a0 = 0, a1 = 2 * PI;
            if (e.type == sketch::EntityType::Arc) {
                const Point2 s0 = pointPos(e.start) - c, s1 = pointPos(e.end) - c;
                a0 = std::atan2(s0.y(), s0.x());
                a1 = std::atan2(s1.y(), s1.x());
                if (a1 <= a0) a1 += 2 * PI;
            }
            double best = 1e18;
            QPointF prev = toScreen(c + Point2(r * std::cos(a0), r * std::sin(a0)));
            for (int i = 1; i <= N; ++i) {
                const double a = a0 + (a1 - a0) * i / N;
                const QPointF cur = toScreen(c + Point2(r * std::cos(a), r * std::sin(a)));
                best = std::min(best, segDist(s, prev, cur));
                prev = cur;
            }
            return best;
        }
        case sketch::EntityType::Bezier:
        case sketch::EntityType::BSpline: {  // S4A-SPL-001
            const auto pl = splineEntityPolyline(e);
            double best = 1e18;
            for (size_t i = 1; i < pl.size(); ++i) best = std::min(best, segDist(s, toScreen(pl[i - 1]), toScreen(pl[i])));
            return best;
        }
        case sketch::EntityType::Ellipse:
        case sketch::EntityType::EllipseArc: {  // S4-SKT-004, -007
            const auto pl = ellipseEntityPolyline(e, 128);
            if (pl.empty()) return 1e18;
            double best = 1e18;
            for (size_t i = 1; i < pl.size(); ++i) best = std::min(best, segDist(s, toScreen(pl[i - 1]), toScreen(pl[i])));
            return best;
        }
        case sketch::EntityType::Text: {  // text outlines (§6.11в)
            double best = 1e18;
            for (const auto& pl : textPolylines(e, pointPos(e.center)))
                for (size_t i = 1; i < pl.size(); ++i) best = std::min(best, segDist(s, toScreen(pl[i - 1]), toScreen(pl[i])));
            return best;
        }
        case sketch::EntityType::Projected: {
            const auto pl = projectedScreen(e.id);
            if (pl.empty()) return 1e18;
            if (pl.size() == 1) return std::hypot(pl[0].x() - s.x(), pl[0].y() - s.y());
            double best = 1e18;
            for (size_t i = 1; i < pl.size(); ++i) best = std::min(best, segDist(s, pl[i - 1], pl[i]));
            return best;
        }
        default: return 1e18;
    }
}

std::vector<Point2> SketchGeometry::screenPolyline(const sketch::Entity& e) const {
    std::vector<Point2> out;
    auto S = [&](const Point2& q) {
        const QPointF p = toScreen(q);
        return Point2(p.x(), p.y());
    };
    switch (e.type) {
        case sketch::EntityType::Point: {
            // Curve ends and centers are not selectable by box selection (§6.3).
            for (const auto& o : data_->entities)
                for (const auto* r : {&o.p1, &o.p2, &o.center, &o.start, &o.end})
                    if (*r == e.id) return out;
            for (const auto& o : data_->entities)
                if (std::find(o.poles.begin(), o.poles.end(), e.id) != o.poles.end()) return out;
            out.push_back(S(pointPos(e.id)));
            break;
        }
        case sketch::EntityType::Line: out = {S(pointPos(e.p1)), S(pointPos(e.p2))}; break;
        case sketch::EntityType::Projected:
            for (const auto& q : projectedScreen(e.id)) out.push_back(Point2(q.x(), q.y()));
            break;
        case sketch::EntityType::Text:
            for (const auto& pl : textPolylines(e, pointPos(e.center)))
                for (const auto& q : pl) out.push_back(S(q));
            break;
        case sketch::EntityType::Bezier:
        case sketch::EntityType::BSpline:
            for (const auto& q : splineEntityPolyline(e)) out.push_back(S(q));
            break;
        case sketch::EntityType::Ellipse:
        case sketch::EntityType::EllipseArc:
            for (const auto& q : ellipseEntityPolyline(e, 64)) out.push_back(S(q));
            break;
        case sketch::EntityType::Circle:
        case sketch::EntityType::Arc: {
            const Point2 c = pointPos(e.center);
            const double r = radius(e);
            double a0 = 0, sw = 2 * PI;
            if (e.type == sketch::EntityType::Arc) {
                const Point2 s0 = pointPos(e.start) - c, s1 = pointPos(e.end) - c;
                a0 = std::atan2(s0.y(), s0.x());
                sw = geom2::ccwSweepPositive(a0, std::atan2(s1.y(), s1.x()));
            }
            const int n = 64;
            for (int i = 0; i <= n; ++i) out.push_back(S(c + r * Point2(std::cos(a0 + sw * i / n), std::sin(a0 + sw * i / n))));
            break;
        }
        default: break;
    }
    return out;
}

void markNewConstruction(doc::Json& items, bool curves, bool texts) {
    for (auto& it : items) {
        if (!it.is_object() || !it.contains("type")) continue;
        const bool curve = it["type"] == "line" || it["type"] == "circle" || it["type"] == "arc" || it["type"] == "ellipse" ||
                           it["type"] == "ellipseArc";
        if ((curves && curve) || (texts && it["type"] == "text")) it["construction"] = true;
    }
}

std::vector<std::string> addDimensionPick(std::vector<std::string> picks, const std::string& pick) {
    if (std::find(picks.begin(), picks.end(), pick) != picks.end()) return picks;
    if (picks.size() >= 2) picks.clear();
    picks.push_back(pick);
    return picks;
}

bool dimensionPlaceable(const sketch::SketchData& d, const std::vector<std::string>& picks) {
    if (picks.size() == 2) return true;
    if (picks.size() != 1) return false;
    const std::string& id = picks[0];
    if (id.rfind("srce:", 0) == 0) return true;
    if (id == "origin" || id.rfind("srcv:", 0) == 0) return false;
    const auto* e = d.findEntity(id);
    if (!e) return false;
    using T = sketch::EntityType;
    if (e->type == T::Line || e->type == T::Circle || e->type == T::Arc || e->type == T::Ellipse) return true;
    if (e->type == T::Projected)
        if (auto it = d.projections.find(id); it != d.projections.end()) return it->second.kind != sketch::Projection::Kind::Point;
    return false;
}

std::string SketchGeometry::trimCurveAt(const QPointF& screen) const {
    if (!data_) return {};
    std::string best;
    double bd = PICK_CURVE_PX;
    for (const auto& e : data_->entities) {
        if (e.type != sketch::EntityType::Line && e.type != sketch::EntityType::Circle && e.type != sketch::EntityType::Arc &&
            !sketch::isEllipseType(e.type) && !sketch::isSplineType(e.type))  // ellipses (S4-SKT-007), splines (S4A-SPL-003)
            continue;
        const double dd = distToEntity(e, screen);
        if (dd < bd || (dd == bd && !best.empty() && compareIds(e.id, best) < 0)) bd = dd, best = e.id;
    }
    return best;
}

Snap arcThroughPointSnap(const Snap& sn, const Point2& cursor) {
    if (sn.kind == Snap::Kind::Point) return sn;
    Snap raw;
    raw.at = cursor;
    return raw;
}

}  // namespace construe::app
