#include "app/snap.h"

#include "base/geom2.h"

#include <algorithm>
#include <cmath>

namespace construe::app::snap {

namespace {
using geom2::angleOf;
}  // namespace

Point2 nearestOnSegment(const Point2& a, const Point2& b, const Point2& p) {
    const Point2 ab = b - a;
    const double l2 = ab.squaredNorm();
    if (l2 == 0) return a;
    return a + std::clamp((p - a).dot(ab) / l2, 0.0, 1.0) * ab;
}

std::optional<Point2> nearestOnCircle(const Point2& c, double r, const Point2& p) {
    const Point2 v = p - c;
    if (geom2::isZeroVec(v)) return std::nullopt;
    return Point2(c + r * v.normalized());
}

Point2 nearestOnEllipse(const Point2& c, double rx, double ry, double angle, const Point2& p) {
    constexpr int SNAP_ELLIPSE_SAMPLES = 128;
    const double cs = std::cos(angle), sn = std::sin(angle);
    auto at = [&](double t) {
        const double x = rx * std::cos(t), y = ry * std::sin(t);
        return Point2(c + Point2(x * cs - y * sn, x * sn + y * cs));
    };
    auto d2 = [&](double t) { return (at(t) - p).squaredNorm(); };
    int best = 0;
    for (int i = 1; i < SNAP_ELLIPSE_SAMPLES; ++i)
        if (d2(2 * PI * i / SNAP_ELLIPSE_SAMPLES) < d2(2 * PI * best / SNAP_ELLIPSE_SAMPLES)) best = i;
    double lo = 2 * PI * (best - 1) / SNAP_ELLIPSE_SAMPLES, hi = 2 * PI * (best + 1) / SNAP_ELLIPSE_SAMPLES;
    const double gr = (std::sqrt(5.0) - 1) / 2;
    for (int k = 0; k < 60; ++k) {
        const double x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
        (d2(x1) < d2(x2) ? hi : lo) = d2(x1) < d2(x2) ? x2 : x1;
    }
    return at((lo + hi) / 2);
}

std::optional<Point2> nearestOnArc(const Point2& c, const Point2& start, const Point2& end, const Point2& p) {
    const auto q = nearestOnCircle(c, (start - c).norm(), p);
    if (!q) return std::nullopt;
    const double a0 = angleOf(start - c);
    if (geom2::ccwSweep(a0, angleOf(*q - c)) > geom2::ccwSweep(a0, angleOf(end - c))) return std::nullopt;
    return q;
}

namespace {

// Point q of the curve's circle lies on the curve itself (segment: between the ends; arc: within its span).
bool onCurve(const Curve& k, const Point2& q) {
    if (k.kind == Curve::Kind::Segment) {
        const Point2 d = k.b - k.a;
        const double t = (q - k.a).dot(d) / std::max(d.squaredNorm(), 1e-300);
        return t >= -1e-9 && t <= 1 + 1e-9;
    }
    if (k.kind == Curve::Kind::Arc) {
        const auto n = nearestOnArc(k.c, k.start, k.end, q);
        return n && (*n - q).norm() <= 1e-9 * std::max(1.0, k.r);
    }
    return true;
}

}  // namespace

std::vector<Point2> intersect(const Curve& u, const Curve& v) {
    std::vector<Point2> cand;
    const bool us = u.kind == Curve::Kind::Segment, vs = v.kind == Curve::Kind::Segment;
    if (us && vs) {
        const Point2 d1 = u.b - u.a, d2 = v.b - v.a;
        const double den = d1.x() * d2.y() - d1.y() * d2.x();
        if (std::abs(den) > 1e-12 * d1.norm() * d2.norm()) {
            const Point2 w = v.a - u.a;
            cand.push_back(u.a + (w.x() * d2.y() - w.y() * d2.x()) / den * d1);
        }
    } else if (us || vs) {
        const Curve& s = us ? u : v;
        const Curve& c = us ? v : u;
        const Point2 d = s.b - s.a, f = s.a - c.c;
        const double A = d.squaredNorm(), B = 2 * f.dot(d), C = f.squaredNorm() - c.r * c.r;
        const double disc = B * B - 4 * A * C;
        if (A > 0 && disc >= -1e-12 * B * B) {
            const double sq = std::sqrt(std::max(0.0, disc));
            cand.push_back(s.a + (-B - sq) / (2 * A) * d);
            if (sq > 0) cand.push_back(s.a + (-B + sq) / (2 * A) * d);
        }
    } else {
        const Point2 d = v.c - u.c;
        const double dd = d.norm();
        if (dd > 1e-12 && dd <= u.r + v.r + 1e-12 && dd >= std::abs(u.r - v.r) - 1e-12) {
            const double a = (u.r * u.r - v.r * v.r + dd * dd) / (2 * dd);
            const double h = std::sqrt(std::max(0.0, u.r * u.r - a * a));
            const Point2 m = u.c + a / dd * d, n(-d.y() / dd, d.x() / dd);
            cand.push_back(m + h * n);
            if (h > 0) cand.push_back(m - h * n);
        }
    }
    std::vector<Point2> out;
    for (const auto& q : cand)
        if (onCurve(u, q) && onCurve(v, q)) out.push_back(q);
    return out;
}

DirectionResult snapDirection(const Point2& a, const Point2& b, double tolRad,
                              const std::optional<Point2>& woken, const std::optional<Point2>& tangent) {
    DirectionResult r;
    r.end = b;
    const Point2 v = b - a;
    const double len = v.norm();
    if (len < 1e-12) return r;
    const Point2 u = v / len;
    // Angle between directions regardless of sign, [0, π/2].
    auto lineAngle = [&](const Point2& d) { return std::acos(std::clamp(std::abs(u.dot(d.normalized())), 0.0, 1.0)); };
    // Rotate onto line d preserving length and side.
    auto rotateTo = [&](const Point2& d) {
        const Point2 n = d.normalized();
        return Point2(a + n * (u.dot(n) >= 0 ? len : -len));
    };
    const double ang = std::atan2(std::abs(v.y()), std::abs(v.x()));
    if (ang <= tolRad) {
        r.kind = Direction::Horizontal;
        r.end = Point2(b.x(), a.y());
        return r;
    }
    if (PI / 2 - ang <= tolRad) {
        r.kind = Direction::Vertical;
        r.end = Point2(a.x(), b.y());
        return r;
    }
    if (woken && woken->norm() > 1e-12) {
        if (lineAngle(*woken) <= tolRad) {
            r.kind = Direction::Parallel;
            r.end = rotateTo(*woken);
            return r;
        }
        if (PI / 2 - lineAngle(*woken) <= tolRad) {
            r.kind = Direction::Perpendicular;
            r.end = rotateTo(Point2(-woken->y(), woken->x()));
            return r;
        }
    }
    // Tangent only in the continuation direction (the reverse direction is a kink).
    if (tangent && tangent->norm() > 1e-12 && std::acos(std::clamp(u.dot(tangent->normalized()), -1.0, 1.0)) <= tolRad) {
        r.kind = Direction::Tangent;
        r.end = a + tangent->normalized() * len;
    }
    return r;
}

Point2 arcContinuation(const Point2& c, const Point2& q, bool atStart) {
    const Point2 v = q - c;
    const Point2 t(-v.y(), v.x());
    return atStart ? Point2(-t) : t;
}

bool alongTangent(const Point2& dir, const Point2& t, double tolRad) {
    const double n = dir.norm() * t.norm();
    if (n <= 0 || dir.dot(t) <= 0) return false;
    return std::abs(dir.x() * t.y() - dir.y() * t.x()) <= std::sin(tolRad) * n;
}

namespace {
bool inBox(const Point2& p, const Point2& lo, const Point2& hi) {
    return p.x() >= lo.x() && p.x() <= hi.x() && p.y() >= lo.y() && p.y() <= hi.y();
}
using geom2::cross2;
// Intersection of segments ab and cd (including touching at ends).
bool segmentsIntersect(const Point2& a, const Point2& b, const Point2& c, const Point2& d) {
    const double d1 = cross2(b - a, c - a), d2 = cross2(b - a, d - a), d3 = cross2(d - c, a - c), d4 = cross2(d - c, b - c);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) return true;
    auto onSeg = [](const Point2& p, const Point2& q, const Point2& r) {
        return std::min(p.x(), q.x()) <= r.x() && r.x() <= std::max(p.x(), q.x()) && std::min(p.y(), q.y()) <= r.y() &&
               r.y() <= std::max(p.y(), q.y());
    };
    return (d1 == 0 && onSeg(a, b, c)) || (d2 == 0 && onSeg(a, b, d)) || (d3 == 0 && onSeg(c, d, a)) || (d4 == 0 && onSeg(c, d, b));
}
}  // namespace

bool polylineInside(const std::vector<Point2>& pts, const Point2& c0, const Point2& c1) {
    const Point2 lo = c0.cwiseMin(c1), hi = c0.cwiseMax(c1);
    if (pts.empty()) return false;
    return std::all_of(pts.begin(), pts.end(), [&](const Point2& p) { return inBox(p, lo, hi); });
}

bool polylineTouches(const std::vector<Point2>& pts, const Point2& c0, const Point2& c1) {
    const Point2 lo = c0.cwiseMin(c1), hi = c0.cwiseMax(c1);
    for (const auto& p : pts)
        if (inBox(p, lo, hi)) return true;
    const Point2 corners[4] = {lo, Point2(hi.x(), lo.y()), hi, Point2(lo.x(), hi.y())};
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        for (int k = 0; k < 4; ++k)
            if (segmentsIntersect(pts[i], pts[i + 1], corners[k], corners[(k + 1) % 4])) return true;
    return false;
}

bool pointInPlane(const Plane& plane, const Point3& p, double tol) { return std::abs((p - plane.origin).dot(plane.normal)) <= tol; }

bool polylineInPlane(const Plane& plane, const std::vector<Point3>& pts, double tol) {
    if (pts.empty()) return false;
    for (const auto& p : pts)
        if (!pointInPlane(plane, p, tol)) return false;
    return true;
}

namespace {
std::optional<std::pair<Point2, double>> circleThrough(const Point2& a, const Point2& b, const Point2& c) {
    const double d = 2 * (a.x() * (b.y() - c.y()) + b.x() * (c.y() - a.y()) + c.x() * (a.y() - b.y()));
    if (std::abs(d) < 1e-12) return std::nullopt;
    const double a2 = a.squaredNorm(), b2 = b.squaredNorm(), c2 = c.squaredNorm();
    const Point2 o((a2 * (b.y() - c.y()) + b2 * (c.y() - a.y()) + c2 * (a.y() - b.y())) / d,
                   (a2 * (c.x() - b.x()) + b2 * (a.x() - c.x()) + c2 * (b.x() - a.x())) / d);
    return std::make_pair(o, (a - o).norm());
}
}  // namespace

double arc3RadiusSensitivity(const Point2& a, const Point2& b, const Point2& m) {
    const auto c = circleThrough(a, b, m);
    if (!c || (m - c->first).norm() <= 0) return 0;
    const double eps = 1e-4 * std::max(1.0, c->second);
    const Point2 u = (m - c->first).normalized();
    const auto p = circleThrough(a, b, m + eps * u), q = circleThrough(a, b, m - eps * u);
    if (!p || !q) return 0;
    return std::abs(p->second - q->second) / (2 * eps);
}

std::string directionHintKey(const std::string& type, const std::string& other) {
    if (type.empty()) return {};
    return type == "tangent" ? "tangent:" + other : "dir:" + type + ":" + other;
}

bool DismissedHints::dismiss(const std::vector<std::string>& shown) {
    bool any = false;
    for (const auto& k : shown)
        if (!k.empty()) keys_.insert(k), any = true;
    return any;
}

void DismissedHints::retain(const std::vector<std::string>& offered) {
    for (auto it = keys_.begin(); it != keys_.end();)
        it = std::find(offered.begin(), offered.end(), *it) == offered.end() ? keys_.erase(it) : std::next(it);
}

std::optional<Point2> nearestOnPolyline(const std::vector<Point2>& pts, const Point2& p) {
    if (pts.empty()) return std::nullopt;
    if (pts.size() == 1) return pts.front();
    std::optional<Point2> best;
    double bd = 0;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const Point2 q = nearestOnSegment(pts[i], pts[i + 1], p);
        const double d = (q - p).squaredNorm();
        if (!best || d < bd) best = q, bd = d;
    }
    return best;
}

}  // namespace construe::app::snap
