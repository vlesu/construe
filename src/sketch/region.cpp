// Interior point and triangulation of a region (spec/sketch.md §3.5).
#include "sketch/region_internal.h"

#include "base/geom2.h"

#include <algorithm>
#include <cmath>

namespace construe::sketch {

namespace {

constexpr double TOL = PROFILE_MERGE_TOL;

using geom2::cross2;

double sweepOf(const Segment& s) { return geom2::arcSweep(s.center, s.start, s.end); }

// Point X strictly on the arc (angle from start in (0, α)).
bool onArcStrict(const Segment& s, const Point2& X) { return geom2::arcSweep(s.center, s.start, X) < sweepOf(s); }

std::vector<const Segment*> allSegments(const Region& r) {
    std::vector<const Segment*> v;
    for (const auto& s : r.outer.segments) v.push_back(&s);
    for (const auto& h : r.holes)
        for (const auto& s : h.segments) v.push_back(&s);
    return v;
}

// Ellipse point at parameter t (§3.5.2, S4-SKT-004).
Point2 ellipseAt(const Segment& s, double t) {
    const double c = std::cos(s.angle), sn = std::sin(s.angle);
    const double x = s.radius * std::cos(t), y = s.minorRadius * std::sin(t);
    return s.center + Point2(x * c - y * sn, x * sn + y * c);
}

// Ellipse arc (S4-SKT-007): parameter of a point, sweep from start to end in (0, 2π], point on the arc.
double ellipseParam(const Segment& s, const Point2& X) {
    const double c = std::cos(s.angle), sn = std::sin(s.angle);
    const Point2 w = X - s.center;
    return std::atan2((-w.x() * sn + w.y() * c) / s.minorRadius, (w.x() * c + w.y() * sn) / s.radius);
}
double ellipseSweep(const Segment& s) {
    return geom2::ccwSweepPositive(ellipseParam(s, s.start), ellipseParam(s, s.end));
}
bool onEllipseArc(const Segment& s, const Point2& X) {
    return geom2::ccwSweep(ellipseParam(s, s.start), ellipseParam(s, X)) < ellipseSweep(s);
}

int arcSegments(double alpha, double R, double chordTol) {
    const double theta = chordTol < R ? 2 * std::acos(1 - chordTol / R) : REGION_ARC_MAX_STEP;
    const int byStep = static_cast<int>(std::ceil(alpha / REGION_ARC_MAX_STEP));
    const int byChord = theta > 0 ? static_cast<int>(std::ceil(alpha / theta)) : REGION_ARC_MAX_SEGMENTS;
    return std::min(REGION_ARC_MAX_SEGMENTS, std::max({1, byStep, byChord}));
}

std::vector<Point2> loopPolygon(const Loop& loop, double chordTol) {
    std::vector<Point2> pts;
    for (const Segment& s : loop.segments) {
        switch (s.kind) {
            case Segment::Kind::Line: pts.push_back(s.reversed ? s.p2 : s.p1); break;
            case Segment::Kind::Arc: {
                const double R = (s.start - s.center).norm();
                const double alpha = sweepOf(s);
                const int n = arcSegments(alpha, R, chordTol);
                const double a0 = std::atan2((s.start - s.center).y(), (s.start - s.center).x());
                auto at = [&](int i) {
                    const double a = a0 + alpha * i / n;
                    return Point2(s.center + R * Point2(std::cos(a), std::sin(a)));
                };
                if (!s.reversed) {
                    pts.push_back(s.start);
                    for (int i = 1; i < n; ++i) pts.push_back(at(i));
                } else {
                    pts.push_back(s.end);
                    for (int i = n - 1; i >= 1; --i) pts.push_back(at(i));
                }
                break;
            }
            case Segment::Kind::EllipseArc: {  // like an arc by the parameter (§3.5.2, S4-SKT-007)
                const double R = std::pow(std::max(s.radius, s.minorRadius), 2) / std::min(s.radius, s.minorRadius);
                const double alpha = ellipseSweep(s), t0 = ellipseParam(s, s.start);
                const int n = arcSegments(alpha, R, chordTol);
                if (!s.reversed) {
                    pts.push_back(s.start);
                    for (int i = 1; i < n; ++i) pts.push_back(ellipseAt(s, t0 + alpha * i / n));
                } else {
                    pts.push_back(s.end);
                    for (int i = n - 1; i >= 1; --i) pts.push_back(ellipseAt(s, t0 + alpha * i / n));
                }
                break;
            }
            case Segment::Kind::Spline: {  // flattening §2.7.6 with chordTol (S4A-SPL-003)
                const spline::Flat f = spline::expand(s.curve);
                const std::vector<double> ts = spline::flattenParams(f, chordTol);
                if (!s.reversed)
                    for (size_t i = 0; i + 1 < ts.size(); ++i) pts.push_back(spline::eval(f, ts[i]));
                else
                    for (size_t i = ts.size() - 1; i >= 1; --i) pts.push_back(spline::eval(f, ts[i]));
                break;
            }
            case Segment::Kind::Circle:
            case Segment::Kind::Ellipse: {
                // Ellipse — by parameter t with the largest radius of curvature max(rx, ry)²/min(rx, ry) (§3.5.2).
                const bool el = s.kind == Segment::Kind::Ellipse;
                const double R = el ? std::pow(std::max(s.radius, s.minorRadius), 2) / std::min(s.radius, s.minorRadius) : s.radius;
                const int n = arcSegments(2 * PI, R, chordTol);
                auto at = [&](int i) {
                    const double a = 2 * PI * i / n;
                    return el ? ellipseAt(s, a) : Point2(s.center + s.radius * Point2(std::cos(a), std::sin(a)));
                };
                pts.push_back(at(0));
                if (!s.reversed)
                    for (int i = 1; i < n; ++i) pts.push_back(at(i));
                else
                    for (int i = n - 1; i >= 1; --i) pts.push_back(at(i));
                break;
            }
        }
    }
    // Merging adjacent close points (including the last and the first).
    std::vector<Point2> out;
    for (const Point2& p : pts)
        if (out.empty() || (p - out.back()).norm() > TOL) out.push_back(p);
    while (out.size() > 1 && (out.back() - out.front()).norm() <= TOL) out.pop_back();
    return out;
}

bool pointInTriangle(const Point2& p, const Point2& a, const Point2& b, const Point2& c) {
    const double d1 = cross2(b - a, p - a), d2 = cross2(c - b, p - b), d3 = cross2(a - c, p - c);
    const bool hasNeg = d1 < 0 || d2 < 0 || d3 < 0;
    const bool hasPos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(hasNeg && hasPos);
}

}  // namespace

RegionTriangles triangulateRegion(const Region& region, double chordTol) {
    RegionTriangles out;
    chordTol = std::clamp(chordTol, REGION_MIN_CHORD, REGION_MAX_CHORD);
    // 1. Polygons.
    std::vector<int> P;
    for (const Point2& p : loopPolygon(region.outer, chordTol)) {
        P.push_back(static_cast<int>(out.vertices.size()));
        out.vertices.push_back(p);
    }
    struct Hole {
        std::vector<int> idx;
        size_t order;
    };
    std::vector<Hole> holes;
    for (size_t h = 0; h < region.holes.size(); ++h) {
        Hole hole{{}, h};
        for (const Point2& p : loopPolygon(region.holes[h], chordTol)) {
            hole.idx.push_back(static_cast<int>(out.vertices.size()));
            out.vertices.push_back(p);
        }
        if (!hole.idx.empty()) holes.push_back(hole);
    }
    const auto& V = out.vertices;
    auto maxX = [&](const Hole& h) {
        double m = -1e300;
        for (int i : h.idx) m = std::max(m, V[i].x());
        return m;
    };
    std::stable_sort(holes.begin(), holes.end(), [&](const Hole& a, const Hole& b) { return maxX(a) > maxX(b); });

    // 2. Bridges to holes (Eberly).
    for (const Hole& H : holes) {
        size_t mi = 0;
        for (size_t i = 1; i < H.idx.size(); ++i) {
            const Point2 &a = V[H.idx[i]], &b = V[H.idx[mi]];
            if (a.x() > b.x() || (a.x() == b.x() && a.y() < b.y())) mi = i;
        }
        const Point2 M = V[H.idx[mi]];
        double bestX = 1e300;
        int bestEdge = -1;
        const int n = static_cast<int>(P.size());
        for (int i = 0; i < n; ++i) {
            const Point2 &a = V[P[i]], &b = V[P[(i + 1) % n]];
            if (!(std::min(a.y(), b.y()) <= M.y() && M.y() < std::max(a.y(), b.y()))) continue;
            const double x = a.x() + (M.y() - a.y()) * (b.x() - a.x()) / (b.y() - a.y());
            if (x < M.x()) continue;
            if (x < bestX) {
                bestX = x;
                bestEdge = i;
            }
        }
        if (bestEdge < 0) continue;  // numerical degeneracy: the hole is skipped
        const Point2 I(bestX, M.y());
        int vPos = -1;
        for (int i = 0; i < n; ++i)
            if ((V[P[i]] - I).norm() <= TOL) {
                vPos = i;
                break;
            }
        if (vPos < 0) {
            const int ia = bestEdge, ib = (bestEdge + 1) % n;
            const int qPos = V[P[ia]].x() > V[P[ib]].x() ? ia : ib;
            const Point2 Q = V[P[qPos]];
            int best = -1;
            double bestAng = 1e300, bestDist = 1e300;
            for (int i = 0; i < n; ++i) {
                if (i == qPos) continue;
                const Point2 &u = V[P[(i + n - 1) % n]], &v = V[P[i]], &w = V[P[(i + 1) % n]];
                if (cross2(v - u, w - v) > 0) continue;  // not concave
                if (!pointInTriangle(v, M, I, Q)) continue;
                const double ang = std::abs(std::atan2(v.y() - M.y(), v.x() - M.x()));
                const double dist = (v - M).norm();
                if (ang < bestAng || (ang == bestAng && dist < bestDist)) {
                    bestAng = ang;
                    bestDist = dist;
                    best = i;
                }
            }
            vPos = best >= 0 ? best : qPos;
        }
        std::vector<int> merged(P.begin(), P.begin() + vPos + 1);
        for (size_t k = 0; k < H.idx.size(); ++k) merged.push_back(H.idx[(mi + k) % H.idx.size()]);
        merged.push_back(H.idx[mi]);
        merged.push_back(P[vPos]);
        merged.insert(merged.end(), P.begin() + vPos + 1, P.end());
        P = std::move(merged);
    }

    // 3. Ear clipping.
    size_t cursor = 0;
    while (P.size() > 3) {
        const size_t n = P.size();
        bool clipped = false;
        for (size_t step = 0; step < n; ++step) {
            const size_t iv = (cursor + step) % n;
            const int u = P[(iv + n - 1) % n], v = P[iv], w = P[(iv + 1) % n];
            if (cross2(V[v] - V[u], V[w] - V[v]) <= REGION_EAR_EPS) continue;
            bool empty = true;
            for (size_t k = 0; k < n && empty; ++k) {
                const int q = P[k];
                if ((V[q] - V[u]).norm() <= TOL || (V[q] - V[v]).norm() <= TOL || (V[q] - V[w]).norm() <= TOL) continue;
                if (pointInTriangle(V[q], V[u], V[v], V[w])) empty = false;
            }
            if (!empty) continue;
            out.triangles.push_back({u, v, w});
            P.erase(P.begin() + static_cast<long>(iv));
            cursor = iv % P.size();  // cursor at w
            clipped = true;
            break;
        }
        if (clipped) continue;
        // Fallback branches.
        bool removed = false;
        for (size_t step = 0; step < n && !removed; ++step) {
            const size_t iv = (cursor + step) % n;
            const int u = P[(iv + n - 1) % n], v = P[iv], w = P[(iv + 1) % n];
            if (std::abs(cross2(V[v] - V[u], V[w] - V[v])) <= REGION_EAR_EPS) {
                P.erase(P.begin() + static_cast<long>(iv));
                cursor = iv % P.size();
                removed = true;
            }
        }
        for (size_t step = 0; step < n && !removed; ++step) {
            const size_t iv = (cursor + step) % n;
            const int u = P[(iv + n - 1) % n], v = P[iv], w = P[(iv + 1) % n];
            if (cross2(V[v] - V[u], V[w] - V[v]) > REGION_EAR_EPS) {
                out.triangles.push_back({u, v, w});
                P.erase(P.begin() + static_cast<long>(iv));
                cursor = iv % P.size();
                removed = true;
            }
        }
        if (!removed) {
            P.clear();
            break;
        }
    }
    if (P.size() == 3 && cross2(V[P[1]] - V[P[0]], V[P[2]] - V[P[1]]) > REGION_EAR_EPS)
        out.triangles.push_back({P[0], P[1], P[2]});
    return out;
}

Point2 computeInteriorPoint(const Region& region) {
    const auto segs = allSegments(region);
    // 1. y range of the outer loop.
    double ymin = 1e300, ymax = -1e300;
    auto addY = [&](double y) {
        ymin = std::min(ymin, y);
        ymax = std::max(ymax, y);
    };
    for (const Segment& s : region.outer.segments) {
        switch (s.kind) {
            case Segment::Kind::Line:
                addY(s.p1.y());
                addY(s.p2.y());
                break;
            case Segment::Kind::Arc: {
                addY(s.start.y());
                addY(s.end.y());
                const double R = (s.start - s.center).norm();
                for (double sg : {1.0, -1.0}) {
                    const Point2 X = s.center + Point2(0, sg * R);
                    if (onArcStrict(s, X)) addY(X.y());
                }
                break;
            }
            case Segment::Kind::Circle:
                addY(s.center.y() - s.radius);
                addY(s.center.y() + s.radius);
                break;
            case Segment::Kind::Ellipse: {
                const double hy = std::hypot(s.radius * std::sin(s.angle), s.minorRadius * std::cos(s.angle));
                addY(s.center.y() - hy);
                addY(s.center.y() + hy);
                break;
            }
            case Segment::Kind::Spline: {  // ends and the extrema of y (roots of y'(t)) (S4A-SPL-003)
                const spline::Flat f = spline::expand(s.curve);
                addY(spline::eval(f, f.a).y());
                addY(spline::eval(f, f.b).y());
                for (double t : spline::flattenParams(f, REGION_MIN_CHORD)) addY(spline::eval(f, t).y());
                break;
            }
            case Segment::Kind::EllipseArc: {  // ends and the extreme points of the ellipse lying on the arc (S4-SKT-007)
                addY(s.start.y());
                addY(s.end.y());
                const double ts = std::atan2(s.minorRadius * std::cos(s.angle), s.radius * std::sin(s.angle));
                for (double t : {ts, ts + PI})
                    if (onEllipseArc(s, ellipseAt(s, t))) addY(ellipseAt(s, t).y());
                break;
            }
        }
    }
    // 2–4. Candidate rows.
    for (int d = 1; d <= REGION_ROW_DEPTH; ++d) {
        const int den = 1 << d;
        for (int num = 1; num < den; num += 2) {
            const double y = ymin + (ymax - ymin) * num / den;
            bool invalid = false;
            std::vector<double> xs;
            for (const Segment* s : segs) {
                if (s->kind == Segment::Kind::Line) {
                    const Point2 &p = s->p1, &q = s->p2;
                    if (std::abs(p.y() - y) <= TOL || std::abs(q.y() - y) <= TOL) {
                        invalid = true;
                        break;
                    }
                    if ((p.y() < y && y < q.y()) || (q.y() < y && y < p.y()))
                        xs.push_back(p.x() + (y - p.y()) * (q.x() - p.x()) / (q.y() - p.y()));
                    continue;
                }
                if (s->kind == Segment::Kind::Spline) {
                    // Roots of y(t) − y (§2.7.5); a row through an end or tangent to the curve is invalid (S4A-SPL-003).
                    const spline::Flat f = spline::expand(s->curve);
                    const Point2 e0 = spline::eval(f, f.a), e1 = spline::eval(f, f.b);
                    if (std::abs(e0.y() - y) <= TOL || std::abs(e1.y() - y) <= TOL) {
                        invalid = true;
                        break;
                    }
                    for (double t : spline::roots(f, [y](const Point2& X) { return X.y() - y; }, TOL)) {
                        Point2 d[3];
                        spline::derivs(f, t, d);
                        if (std::abs(d[1].y()) <= TOL * std::max(1.0, d[1].norm())) invalid = true;
                        xs.push_back(d[0].x());
                    }
                    if (invalid) break;
                    continue;
                }
                if (s->kind == Segment::Kind::Ellipse || s->kind == Segment::Kind::EllipseArc) {
                    // Intersection of a row with an ellipse: quadratic equation in dx (§3.5.1, S4-SKT-004); an ellipse
                    // arc — roots lying on the arc, rows through its ends are invalid (S4-SKT-007).
                    const double cs = std::cos(s->angle), sn = std::sin(s->angle);
                    const double hy = std::hypot(s->radius * sn, s->minorRadius * cs);
                    const double dy = y - s->center.y();
                    const bool arc = s->kind == Segment::Kind::EllipseArc;
                    if (std::abs(std::abs(dy) - hy) <= TOL ||
                        (arc && (std::abs(s->start.y() - y) <= TOL || std::abs(s->end.y() - y) <= TOL))) {
                        invalid = true;
                        break;
                    }
                    if (std::abs(dy) >= hy) continue;
                    const double ia = 1 / (s->radius * s->radius), ib = 1 / (s->minorRadius * s->minorRadius);
                    const double A = cs * cs * ia + sn * sn * ib;
                    const double B = 2 * dy * cs * sn * (ia - ib);
                    const double C = dy * dy * (sn * sn * ia + cs * cs * ib) - 1;
                    const double disc = std::sqrt(std::max(0.0, B * B - 4 * A * C));
                    for (double x : {s->center.x() + (-B - disc) / (2 * A), s->center.x() + (-B + disc) / (2 * A)})
                        if (!arc || onEllipseArc(*s, Point2(x, y))) xs.push_back(x);
                    continue;
                }
                const Point2 c = s->center;
                const double R = s->kind == Segment::Kind::Circle ? s->radius : (s->start - s->center).norm();
                if (s->kind == Segment::Kind::Arc && (std::abs(s->start.y() - y) <= TOL || std::abs(s->end.y() - y) <= TOL)) {
                    invalid = true;
                    break;
                }
                if (std::abs(y - (c.y() + R)) <= TOL || std::abs(y - (c.y() - R)) <= TOL) {
                    invalid = true;
                    break;
                }
                if (std::abs(y - c.y()) >= R) continue;
                const double h = std::sqrt(R * R - (y - c.y()) * (y - c.y()));
                for (double x : {c.x() - h, c.x() + h}) {
                    const Point2 X(x, y);
                    if (s->kind == Segment::Kind::Circle || onArcStrict(*s, X)) xs.push_back(x);
                }
            }
            if (invalid || xs.size() % 2 != 0 || xs.empty()) continue;
            std::sort(xs.begin(), xs.end());
            double bestLen = -1, bestMid = 0;
            for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                const double len = xs[i + 1] - xs[i];
                if (len > bestLen + TOL) {
                    bestLen = len;
                    bestMid = (xs[i] + xs[i + 1]) / 2;
                }
            }
            if (bestLen <= 2 * TOL) continue;
            return {bestMid, y};
        }
    }
    // 5. Fallback: centroid of the largest triangle.
    const RegionTriangles t = triangulateRegion(region, REGION_MIN_CHORD);
    double bestA = -1;
    Point2 best = Point2::Zero();
    for (const auto& tri : t.triangles) {
        const Point2 &a = t.vertices[tri[0]], &b = t.vertices[tri[1]], &c = t.vertices[tri[2]];
        const double A = cross2(b - a, c - a) / 2;
        if (A > bestA) {
            bestA = A;
            best = (a + b + c) / 3;
        }
    }
    return best;
}

}  // namespace construe::sketch
