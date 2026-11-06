// Profile check (spec/kernel-interface.md §4.1). Independent of the kernel implementation.
#include "kernel/kernel.h"

#include "base/geom2.h"
#include "base/ids.h"
#include "base/spline.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>

namespace construe::kernel {

namespace {

constexpr double GAP = KERNEL_PROFILE_GAP_TOL;

using geom2::cross2;

// Segment in the plane's local coordinates.
struct Seg2 {
    const Segment3* src = nullptr;
    Segment3::Kind kind = Segment3::Kind::Line;
    Point2 a = Point2::Zero(), b = Point2::Zero();  // natural start/end (line p1,p2; arc start,end)
    Point2 c = Point2::Zero();
    double r = 0, alpha = 0;
    double rb = 0;                // Ellipse, EllipseArc: second semi-axis (S4-SKT-004)
    Point2 u = Point2(1, 0);      // Ellipse, EllipseArc: unit direction of semi-axis r
    double t0 = 0;                // EllipseArc: start parameter; alpha — parameter sweep (S4-SKT-007)
    std::shared_ptr<const spline::Flat> sp;  // Spline in local coordinates (S4A-SPL-003)
    mutable std::shared_ptr<const std::vector<struct Seg2>> chordCache;  // chords of a curved segment, computed once
    bool reversed = false;
    Point2 from() const { return reversed ? b : a; }
    Point2 to() const { return reversed ? a : b; }
};

Error err(const char* code) { return Error(code); }

std::vector<std::string> idsOf(const std::vector<Seg2>& loop) {
    std::vector<std::string> v;
    for (const auto& s : loop) v.push_back(s.src->id);
    return v;
}

bool finite3(const Point3& p) {
    return std::isfinite(p.x()) && std::isfinite(p.y()) && std::isfinite(p.z()) && std::abs(p.x()) <= KERNEL_MAX_COORD &&
           std::abs(p.y()) <= KERNEL_MAX_COORD && std::abs(p.z()) <= KERNEL_MAX_COORD;
}


bool onArc(const Seg2& s, const Point2& X) {
    if (s.kind == Segment3::Kind::Circle) return true;
    return geom2::onArc(s.c, s.a, s.alpha, X, GAP / std::max(s.r, GAP));
}

bool onLine(const Seg2& s, const Point2& X) {
    const Point2 d = s.b - s.a;
    const double len2 = d.squaredNorm();
    const double t = (X - s.a).dot(d) / len2;
    const double eps = GAP / std::sqrt(len2);
    return t >= -eps && t <= 1 + eps && std::abs(cross2(d.normalized(), X - s.a)) <= GAP;
}

bool onSeg(const Seg2& s, const Point2& X) { return s.kind == Segment3::Kind::Line ? onLine(s, X) : onArc(s, X); }

double pointSegDistance(const Point2& p, const Point2& a, const Point2& b) {
    const Point2 d = b - a;
    double t = (p - a).dot(d) / d.squaredNorm();
    t = std::clamp(t, 0.0, 1.0);
    return (p - (a + t * d)).norm();
}

bool closedKind(Segment3::Kind k) { return k == Segment3::Kind::Circle || k == Segment3::Kind::Ellipse; }

// Ellipse parameter of a point and the Sampson distance to the ellipse (S4-SKT-007).
double ellipseParamOf(const Seg2& s, const Point2& P) {
    const Point2 w = P - s.c, v(-s.u.y(), s.u.x());
    return std::atan2(w.dot(v) / s.rb, w.dot(s.u) / s.r);
}
double ellipseSampson(const Seg2& s, const Point2& P) {
    const Point2 w = P - s.c, v(-s.u.y(), s.u.x());
    const double a = w.dot(s.u), b = w.dot(v);
    const double F = a * a / (s.r * s.r) + b * b / (s.rb * s.rb) - 1;
    return F / std::max(2 * std::hypot(a / (s.r * s.r), b / (s.rb * s.rb)), 1e-300);
}

bool insideEllipse(const Seg2& s, const Point2& P) {
    const Point2 w = P - s.c, v(-s.u.y(), s.u.x());
    const double a = w.dot(s.u) / s.r, b = w.dot(v) / s.rb;
    return a * a + b * b < 1;
}

// Ellipse — a closed polyline of KERNEL_ELLIPSE_CHECK_CHORDS chords for intersection checks (§4.1 items 5–6).
std::vector<Seg2> ellipseChords(const Seg2& s) {
    std::vector<Seg2> out;
    const Point2 v(-s.u.y(), s.u.x());
    auto at = [&](int i) {
        const double t = 2 * PI * i / KERNEL_ELLIPSE_CHECK_CHORDS;
        return Point2(s.c + s.r * std::cos(t) * s.u + s.rb * std::sin(t) * v);
    };
    for (int i = 0; i < KERNEL_ELLIPSE_CHECK_CHORDS; ++i) {
        Seg2 g;
        g.src = s.src;
        g.kind = Segment3::Kind::Line;
        g.a = at(i);
        g.b = at(i + 1);
        out.push_back(g);
    }
    return out;
}

// Ellipse arc — a polyline of ⌈KERNEL_ELLIPSE_CHECK_CHORDS·α/2π⌉ chords from start to end (§4.1 item 5, S4-SKT-007);
// the ends are exactly the arc ends.
std::vector<Seg2> ellipseArcChords(const Seg2& s) {
    std::vector<Seg2> out;
    const Point2 v(-s.u.y(), s.u.x());
    const int n = std::max(1, static_cast<int>(std::ceil(KERNEL_ELLIPSE_CHECK_CHORDS * s.alpha / (2 * PI))));
    auto at = [&](int i) {
        if (i == 0) return s.a;
        if (i == n) return s.b;
        const double t = s.t0 + s.alpha * i / n;
        return Point2(s.c + s.r * std::cos(t) * s.u + s.rb * std::sin(t) * v);
    };
    for (int i = 0; i < n; ++i) {
        Seg2 g;
        g.src = s.src;
        g.kind = Segment3::Kind::Line;
        g.a = at(i);
        g.b = at(i + 1);
        out.push_back(g);
    }
    return out;
}

// Common points of two segments (candidates + overlap flag).
struct Contact {
    std::vector<Point2> points;
    bool overlap = false;
};

Contact contact(const Seg2& p, const Seg2& q) {
    Contact c;
    const bool pl = p.kind == Segment3::Kind::Line, ql = q.kind == Segment3::Kind::Line;
    if (pl && ql) {
        const Point2 d = (p.b - p.a).normalized();
        const double da = std::abs(cross2(d, q.a - p.a)), db = std::abs(cross2(d, q.b - p.a));
        if (da <= GAP && db <= GAP) {  // collinear
            const double len = (p.b - p.a).norm();
            double t0 = (q.a - p.a).dot(d), t1 = (q.b - p.a).dot(d);
            if (t0 > t1) std::swap(t0, t1);
            const double lo = std::max(0.0, t0), hi = std::min(len, t1);
            if (hi - lo > GAP) c.overlap = true;
            else if (hi - lo >= -GAP) c.points.push_back(p.a + 0.5 * (lo + hi) * d);
            return c;
        }
        // Closest point: intersection or ends.
        const Point2 r = p.b - p.a, s = q.b - q.a;
        const double den = cross2(r, s);
        if (std::abs(den) > 0) {
            const double t = cross2(q.a - p.a, s) / den, u = cross2(q.a - p.a, r) / den;
            const double et = GAP / r.norm(), eu = GAP / s.norm();
            if (t >= -et && t <= 1 + et && u >= -eu && u <= 1 + eu) {
                c.points.push_back(p.a + t * r);
                return c;
            }
        }
        for (const Point2& x : {q.a, q.b})
            if (pointSegDistance(x, p.a, p.b) <= GAP) c.points.push_back(x);
        for (const Point2& x : {p.a, p.b})
            if (pointSegDistance(x, q.a, q.b) <= GAP) c.points.push_back(x);
        return c;
    }
    auto lineCircle = [&](const Seg2& l, const Point2& cc, double R) {
        std::vector<Point2> out;
        const Point2 d = (l.b - l.a).normalized();
        const Point2 f = l.a + (cc - l.a).dot(d) * d;
        const double dist = (cc - f).norm();
        if (std::abs(dist - R) <= GAP) return std::vector<Point2>{f};
        if (dist > R) return out;
        const double h = std::sqrt(R * R - dist * dist);
        return std::vector<Point2>{f - h * d, f + h * d};
    };
    std::vector<Point2> cand;
    if (pl || ql) {
        const Seg2& l = pl ? p : q;
        const Seg2& k = pl ? q : p;
        cand = lineCircle(l, k.c, k.r);
    } else {
        const Point2 v = q.c - p.c;
        const double d = v.norm();
        if (d <= GAP && std::abs(p.r - q.r) <= GAP) {
            // Same carrier: overlap if the common part is not a single point.
            const int N = 360;
            int common = 0;
            const Seg2& k = p.kind == Segment3::Kind::Circle ? q : p;
            const Seg2& m = p.kind == Segment3::Kind::Circle ? p : q;
            const double alpha = k.kind == Segment3::Kind::Circle ? 2 * PI : k.alpha;
            const double a0 = std::atan2((k.a - k.c).y(), (k.a - k.c).x());
            for (int i = 0; i <= N; ++i) {
                const double a = a0 + alpha * i / N;
                if (onArc(m, k.c + k.r * Point2(std::cos(a), std::sin(a)))) ++common;
            }
            if (common > 2) c.overlap = true;
            else
                for (const Point2& x : {k.a, k.b})
                    if (onArc(m, x)) c.points.push_back(x);
            return c;
        }
        if (d > GAP) {
            if (std::abs(d - (p.r + q.r)) <= GAP) cand.push_back(p.c + p.r * v / d);
            else if (std::abs(d - std::abs(p.r - q.r)) <= GAP) cand.push_back(p.c + (p.r >= q.r ? 1.0 : -1.0) * p.r * v / d);
            else if (d < p.r + q.r && d > std::abs(p.r - q.r)) {
                const double a = (p.r * p.r - q.r * q.r + d * d) / (2 * d);
                const double h = std::sqrt(std::max(0.0, p.r * p.r - a * a));
                const Point2 mpt = p.c + a * v / d, n(-v.y() / d, v.x() / d);
                cand = {mpt + h * n, mpt - h * n};
            }
        }
    }
    for (const Point2& x : cand)
        if (onSeg(p, x) && onSeg(q, x)) c.points.push_back(x);
    return c;
}

// Spline — the polyline of its flattening with KERNEL_SPLINE_CHECK_TOL (§4.1 item 5, S4A-SPL-003); ends exact.
std::vector<Seg2> splineChords(const Seg2& s) {
    std::vector<Seg2> out;
    const std::vector<double> ts = spline::flattenParams(*s.sp, KERNEL_SPLINE_CHECK_TOL);
    for (size_t i = 0; i + 1 < ts.size(); ++i) {
        Seg2 g;
        g.src = s.src;
        g.kind = Segment3::Kind::Line;
        g.a = i == 0 ? s.a : spline::eval(*s.sp, ts[i]);
        g.b = i + 2 == ts.size() ? s.b : spline::eval(*s.sp, ts[i + 1]);
        out.push_back(g);
    }
    return out;
}

// Bounding box of a segment, enlarged by 2·GAP: arcs — ends and axis extremes, circles and ellipses — the full curve,
// splines — the poles (convex hull property). Disjoint boxes — no common points (pruning of §4.1 items 5, 6; the result does not change).
Eigen::AlignedBox2d segBox(const Seg2& s) {
    Eigen::AlignedBox2d b;
    switch (s.kind) {
        case Segment3::Kind::Line:
            b.extend(s.a), b.extend(s.b);
            break;
        case Segment3::Kind::Spline:
            for (const auto& q : s.sp->P) b.extend(q);
            break;
        case Segment3::Kind::Arc:  // ends and the axis extremes within the sweep
            b.extend(s.a), b.extend(s.b);
            for (const Point2& d : {Point2(1, 0), Point2(0, 1), Point2(-1, 0), Point2(0, -1)})
                if (onArc(s, s.c + s.r * d)) b.extend(Point2(s.c + s.r * d));
            break;
        default: {
            const double r = std::max(s.r, s.rb);
            b.extend(Point2(s.c - Point2(r, r))), b.extend(Point2(s.c + Point2(r, r)));
            break;
        }
    }
    b.min() -= Point2(2 * GAP, 2 * GAP);
    b.max() += Point2(2 * GAP, 2 * GAP);
    return b;
}

// Whether there are common points other than the allowed ones (shared ends of neighbors).
bool intersects(const Seg2& p, const Seg2& q, const std::vector<Point2>& allowed) {
    const Eigen::AlignedBox2d pb = segBox(p), qb = segBox(q);
    if (!pb.intersects(qb)) return false;
    auto curved = [](const Seg2& s) {
        return s.kind == Segment3::Kind::Ellipse || s.kind == Segment3::Kind::EllipseArc || s.kind == Segment3::Kind::Spline;
    };
    auto chords = [&](const Seg2& s) -> const std::vector<Seg2>& {
        if (!s.chordCache)
            s.chordCache = std::make_shared<const std::vector<Seg2>>(s.kind == Segment3::Kind::Ellipse      ? ellipseChords(s)
                                                                     : s.kind == Segment3::Kind::EllipseArc ? ellipseArcChords(s)
                                                                                                             : splineChords(s));
        return *s.chordCache;
    };
    if (curved(p) || curved(q)) {
        const std::vector<Seg2> single{p}, singleQ{q};
        const std::vector<Seg2>& ps = curved(p) ? chords(p) : single;
        const std::vector<Seg2>& qs = curved(q) ? chords(q) : singleQ;
        for (const Seg2& a : ps) {
            if (!segBox(a).intersects(qb)) continue;
            for (const Seg2& b : qs)
                if (intersects(a, b, allowed)) return true;
        }
        return false;
    }
    const Contact c = contact(p, q);
    if (c.overlap) return true;
    for (const Point2& x : c.points) {
        bool ok = false;
        for (const Point2& a : allowed) ok = ok || (x - a).norm() <= 2 * GAP;
        if (!ok) return true;
    }
    return false;
}

double signedArea(const std::vector<Seg2>& loop) {
    double sum = 0;
    for (const Seg2& s : loop) {
        if (s.kind == Segment3::Kind::Circle) {
            sum += (s.reversed ? -1.0 : 1.0) * 2 * PI * s.r * s.r;
            continue;
        }
        if (s.kind == Segment3::Kind::Ellipse) {
            sum += (s.reversed ? -1.0 : 1.0) * 2 * PI * s.r * s.rb;
            continue;
        }
        if (s.kind == Segment3::Kind::Line) {
            const Point2 a = s.from(), b = s.to();
            sum += a.x() * b.y() - b.x() * a.y();
            continue;
        }
        if (s.kind == Segment3::Kind::Spline) {  // 2·area: Green over the curve (S4A-SPL-003)
            sum += (s.reversed ? -2.0 : 2.0) * spline::greenArea(*s.sp, s.sp->a, s.sp->b);
            continue;
        }
        if (s.kind == Segment3::Kind::EllipseArc) {  // 2·area: cross(c, e − s) + r·rb·α (S4-SKT-007)
            sum += (s.reversed ? -1.0 : 1.0) * (cross2(s.c, s.b - s.a) + s.r * s.rb * s.alpha);
            continue;
        }
        const double a0s = std::atan2((s.a - s.c).y(), (s.a - s.c).x());
        double a0 = a0s, delta = s.alpha;
        if (s.reversed) {
            a0 = a0s + s.alpha;
            delta = -s.alpha;
        }
        const double a1 = a0 + delta;
        sum += s.r * s.r * delta + s.r * s.c.x() * (std::sin(a1) - std::sin(a0)) - s.r * s.c.y() * (std::cos(a1) - std::cos(a0));
    }
    return sum / 2;
}

int winding(const std::vector<Seg2>& loop, const Point2& P) {
    double sum = 0;
    for (const Seg2& s : loop) {
        if (s.kind == Segment3::Kind::Circle) {
            if ((P - s.c).norm() < s.r) sum += (s.reversed ? -2 : 2) * PI;
            continue;
        }
        if (s.kind == Segment3::Kind::Ellipse) {
            if (insideEllipse(s, P)) sum += (s.reversed ? -2 : 2) * PI;
            continue;
        }
        if (s.kind == Segment3::Kind::Spline) {  // by its polyline (S4A-SPL-003)
            double w = 0;
            for (const Seg2& c : splineChords(s)) w += std::atan2(cross2(c.a - P, c.b - P), (c.a - P).dot(c.b - P));
            sum += s.reversed ? -w : w;
            continue;
        }
        const Point2 a = s.from(), b = s.to();
        double contrib = std::atan2(cross2(a - P, b - P), (a - P).dot(b - P));
        if (s.kind == Segment3::Kind::Arc || s.kind == Segment3::Kind::EllipseArc) {
            const bool inside = s.kind == Segment3::Kind::Arc ? (P - s.c).norm() < s.r : insideEllipse(s, P);
            const bool inSeg = inside && cross2(s.b - s.a, P - s.a) < 0;
            if (inSeg) contrib += s.reversed ? -2 * PI : 2 * PI;
        }
        sum += contrib;
    }
    return static_cast<int>(std::lround(sum / (2 * PI)));
}

void reverseLoop(std::vector<Seg2>& loop) {
    std::reverse(loop.begin(), loop.end());
    for (auto& s : loop) s.reversed = !s.reversed;
}

void rotateToMin(std::vector<Seg2>& loop) {
    if (loop.empty()) return;
    size_t m = 0;
    for (size_t i = 1; i < loop.size(); ++i)
        if (compareIds(loop[i].src->id, loop[m].src->id) < 0) m = i;
    std::rotate(loop.begin(), loop.begin() + static_cast<long>(m), loop.end());
}

std::string minId(const std::vector<Seg2>& loop) {
    std::string m = loop.front().src->id;
    for (const auto& s : loop)
        if (compareIds(s.src->id, m) < 0) m = s.src->id;
    return m;
}

}  // namespace

Result<ProfileInfo> validateProfile(const Profile& p, Profile* normalized) {
    const Plane& pl = p.plane;
    // 0. ID format and canonical order.
    std::vector<const Loop3*> loops3{&p.outer};
    for (const auto& h : p.holes) loops3.push_back(&h);
    for (const Loop3* l : loops3)
        for (const auto& s : l->segments)
            if (!isSegmentId(s.id)) {
                Error e("KERNEL_PROFILE_INVALID_LOOP");
                std::vector<std::string> ids;
                for (const auto& x : l->segments) ids.push_back(x.id);
                e.with("ids", ids).with("detail", std::string("badSegmentId")).with("id", s.id);
                return e;
            }
    // 1. Numbers and plane.
    for (const Point3* v : {&pl.origin})
        if (!finite3(*v)) return err("KERNEL_INVALID_ARGUMENT").with("name", std::string("coordinate"));
    if (std::abs(pl.x.norm() - 1) > 1e-9 || std::abs(pl.y.norm() - 1) > 1e-9 || std::abs(pl.x.dot(pl.y)) > 1e-9 ||
        (pl.x.cross(pl.y) - pl.normal).norm() > 1e-9)
        return err("KERNEL_INVALID_ARGUMENT").with("name", std::string("plane"));
    auto toLocal = [&](const Point3& q) { return Point2((q - pl.origin).dot(pl.x), (q - pl.origin).dot(pl.y)); };
    auto planeDist = [&](const Point3& q) { return std::abs((q - pl.origin).dot(pl.normal)); };

    std::vector<std::vector<Seg2>> loops;
    for (const Loop3* l : loops3) {
        std::vector<Seg2> loop;
        for (const auto& s : l->segments) {
            std::vector<const Point3*> pts;
            if (s.kind == Segment3::Kind::Line) pts = {&s.p1, &s.p2};
            else if (s.kind == Segment3::Kind::Spline)
                for (const auto& q : s.poles) pts.push_back(&q);
            else if (s.kind == Segment3::Kind::Arc || s.kind == Segment3::Kind::EllipseArc) pts = {&s.center, &s.start, &s.end};
            else pts = {&s.center};
            if ((s.kind == Segment3::Kind::Ellipse || s.kind == Segment3::Kind::EllipseArc) &&
                (!std::isfinite(s.minorRadius) || !s.xDir.allFinite() || std::abs(s.xDir.norm() - 1) > 1e-9 || std::abs(s.xDir.dot(pl.normal)) > 1e-9))
                return err("KERNEL_INVALID_ARGUMENT").with("name", std::string("xDir")).with("id", s.id);
            for (const Point3* q : pts) {
                if (!finite3(*q) || !std::isfinite(s.radius))
                    return err("KERNEL_INVALID_ARGUMENT").with("name", std::string("coordinate")).with("id", s.id);
                if (planeDist(*q) > GAP)
                    return err("KERNEL_PROFILE_NOT_PLANAR").with("id", s.id).with("actual", planeDist(*q));
            }
            Seg2 g;
            g.src = &s;
            g.kind = s.kind;
            g.reversed = s.reversed;
            if (s.kind == Segment3::Kind::Line) {
                g.a = toLocal(s.p1);
                g.b = toLocal(s.p2);
            } else if (s.kind == Segment3::Kind::Spline) {  // open clamped NURBS (§2.5, S4A-SPL-003)
                spline::Curve c;
                c.degree = s.degree;
                for (const auto& q : s.poles) c.poles.push_back(toLocal(q));
                c.weights = s.weights;
                c.knots = s.knots;
                c.mults = s.mults;
                if (!spline::validate(c).empty() || c.poles.size() < 2)
                    return err("KERNEL_INVALID_ARGUMENT").with("name", std::string("spline")).with("id", s.id);
                g.sp = std::make_shared<const spline::Flat>(spline::expand(c));
                g.a = c.poles.front();
                g.b = c.poles.back();
            } else if (s.kind == Segment3::Kind::Ellipse) {
                g.c = toLocal(s.center);
                g.r = s.radius;
                g.rb = s.minorRadius;
                g.u = Point2(s.xDir.dot(pl.x), s.xDir.dot(pl.y)).normalized();
                g.a = g.b = g.c + g.r * g.u;
            } else if (s.kind == Segment3::Kind::EllipseArc) {  // S4-SKT-007
                g.c = toLocal(s.center);
                g.r = s.radius;
                g.rb = s.minorRadius;
                g.u = Point2(s.xDir.dot(pl.x), s.xDir.dot(pl.y)).normalized();
                g.a = toLocal(s.start);
                g.b = toLocal(s.end);
                if (g.r > 0 && g.rb > 0) {
                    g.t0 = ellipseParamOf(g, g.a);
                    g.alpha = geom2::ccwSweepPositive(g.t0, ellipseParamOf(g, g.b));
                }
            } else {
                g.c = toLocal(s.center);
                g.r = s.radius;
                if (s.kind == Segment3::Kind::Arc) {
                    g.a = toLocal(s.start);
                    g.b = toLocal(s.end);
                    g.alpha = geom2::arcSweep(g.c, g.a, g.b);
                } else {
                    g.a = g.b = g.c + Point2(g.r, 0);
                }
            }
            loop.push_back(g);
        }
        rotateToMin(loop);
        loops.push_back(loop);
    }
    // Holes — by the smallest SegmentId (non-empty; empty ones are checked by item 2).
    std::stable_sort(loops.begin() + 1, loops.end(), [](const auto& a, const auto& b) {
        if (a.empty() || b.empty()) return !a.empty() && b.empty();
        return compareIds(minId(a), minId(b)) < 0;
    });

    // 2. Loop composition.
    std::set<std::string> seen;
    for (const auto& loop : loops) {
        Error e("KERNEL_PROFILE_INVALID_LOOP");
        e.with("ids", idsOf(loop));
        if (loop.empty()) return e.with("detail", std::string("empty"));
        const bool hasCircle = std::any_of(loop.begin(), loop.end(), [](const Seg2& s) { return closedKind(s.kind); });
        if (loop.size() == 1 && !hasCircle) return e.with("detail", std::string("singleNonCircle"));
        if (hasCircle && loop.size() > 1) return e.with("detail", std::string("circleWithOthers"));
        for (const auto& s : loop)
            if (!seen.insert(s.src->id).second) return e.with("detail", std::string("duplicateId")).with("id", s.src->id);
    }
    // 3. Degenerate segments.
    for (const auto& loop : loops)
        for (const auto& s : loop) {
            Error e("KERNEL_PROFILE_DEGENERATE_SEGMENT");
            e.with("id", s.src->id);
            if (s.kind == Segment3::Kind::Line) {
                if ((s.b - s.a).norm() < KERNEL_MIN_SEGMENT) return e.with("detail", std::string("length"));
                continue;
            }
            if (s.kind == Segment3::Kind::Spline) {
                if (spline::length(*s.sp, s.sp->a, s.sp->b) < KERNEL_MIN_SEGMENT) return e.with("detail", std::string("length"));
                continue;
            }
            if (s.r < KERNEL_MIN_DIMENSION) return e.with("detail", std::string("radius"));
            if ((s.kind == Segment3::Kind::Ellipse || s.kind == Segment3::Kind::EllipseArc) && s.rb < KERNEL_MIN_DIMENSION)
                return e.with("detail", std::string("radius"));
            if (s.kind == Segment3::Kind::EllipseArc) {  // ends on the ellipse, length (S4-SKT-007)
                if (std::abs(ellipseSampson(s, s.a)) > GAP || std::abs(ellipseSampson(s, s.b)) > GAP)
                    return e.with("detail", std::string("arcRadiusMismatch"));
                if (s.alpha * std::min(s.r, s.rb) < KERNEL_MIN_SEGMENT) return e.with("detail", std::string("length"));
            }
            if (s.kind == Segment3::Kind::Arc) {
                if (std::abs((s.b - s.c).norm() - s.r) > GAP || std::abs((s.a - s.c).norm() - s.r) > GAP)
                    return e.with("detail", std::string("arcRadiusMismatch"));
                if (s.alpha * s.r < KERNEL_MIN_SEGMENT) return e.with("detail", std::string("length"));
            }
        }
    // 4. Closedness.
    for (const auto& loop : loops) {
        if (closedKind(loop[0].kind)) continue;
        for (size_t i = 0; i < loop.size(); ++i) {
            const Seg2& s = loop[i];
            const Seg2& n = loop[(i + 1) % loop.size()];
            const double gap = (s.to() - n.from()).norm();
            if (gap > GAP)
                return err("KERNEL_PROFILE_OPEN").with("ids", std::vector<std::string>{s.src->id, n.src->id}).with("actual", gap);
        }
    }
    // 5. Loop simplicity; a spline must not cross itself (its non-adjacent chords, S4A-SPL-003).
    for (const auto& loop : loops) {
        for (const auto& s : loop) {
            if (s.kind != Segment3::Kind::Spline) continue;
            const std::vector<Seg2> ch = splineChords(s);
            for (size_t i = 0; i < ch.size(); ++i)
                for (size_t j = i + 2; j < ch.size(); ++j)
                    if (intersects(ch[i], ch[j], {}))
                        return err("KERNEL_PROFILE_SELF_INTERSECTING").with("ids", std::vector<std::string>{s.src->id, s.src->id});
        }
        const size_t n = loop.size();
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j) {
                std::vector<Point2> allowed;
                if (j == i + 1) allowed.push_back(loop[i].to());
                if (i == 0 && j == n - 1) allowed.push_back(loop[j].to());
                if (n == 1) continue;
                if (intersects(loop[i], loop[j], allowed))
                    return err("KERNEL_PROFILE_SELF_INTERSECTING")
                        .with("ids", std::vector<std::string>{loop[i].src->id, loop[j].src->id});
            }
    }
    // 6. Loops pairwise.
    for (size_t a = 0; a < loops.size(); ++a)
        for (size_t b = a + 1; b < loops.size(); ++b)
            for (const auto& s : loops[a])
                for (const auto& t : loops[b])
                    if (intersects(s, t, {}))
                        return err("KERNEL_PROFILE_LOOPS_INTERSECT")
                            .with("ids", std::vector<std::string>{s.src->id, t.src->id});
    // 7. Nesting.
    for (size_t h = 1; h < loops.size(); ++h) {
        const Point2 probe = loops[h][0].kind == Segment3::Kind::Circle    ? loops[h][0].c + Point2(loops[h][0].r, 0)
                             : loops[h][0].kind == Segment3::Kind::Ellipse ? loops[h][0].c + loops[h][0].r * loops[h][0].u
                                                                           : loops[h][0].from();
        if (winding(loops[0], probe) == 0) return err("KERNEL_PROFILE_HOLE_OUTSIDE").with("ids", idsOf(loops[h]));
        for (size_t o = 1; o < loops.size(); ++o)
            if (o != h && winding(loops[o], probe) != 0)
                return err("KERNEL_PROFILE_NESTED_HOLE").with("ids", idsOf(loops[h])).with("containerIds", idsOf(loops[o]));
    }
    // 8. Orientation.
    ProfileInfo info;
    const double a0 = signedArea(loops[0]);
    info.outerCCW = a0 > 0;
    if (!info.outerCCW) {
        reverseLoop(loops[0]);
        rotateToMin(loops[0]);
    }
    info.area = std::abs(a0);
    for (size_t h = 1; h < loops.size(); ++h) {
        const double ah = signedArea(loops[h]);
        info.holesCW.push_back(ah < 0);
        if (ah > 0) {
            reverseLoop(loops[h]);
            rotateToMin(loops[h]);
        }
        info.area -= std::abs(ah);
    }
    if (normalized) {
        auto rebuild = [](const std::vector<Seg2>& loop) {
            Loop3 l;
            for (const Seg2& s : loop) {
                Segment3 x = *s.src;
                x.reversed = s.reversed;
                l.segments.push_back(x);
            }
            return l;
        };
        normalized->plane = pl;
        normalized->outer = rebuild(loops[0]);
        normalized->holes.clear();
        for (size_t h = 1; h < loops.size(); ++h) normalized->holes.push_back(rebuild(loops[h]));
    }
    return info;
}

}  // namespace construe::kernel
