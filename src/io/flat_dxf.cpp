// Flat pattern export to DXF for laser cutting (io/flat_dxf.h, spec/io.md §2д; S5-DXF-001).
#include "io/flat_dxf.h"

#include "drawing/render.h"
#include "io/dxf_writer.h"
#include "io/stl.h"

#include "base/spline.h"
#include "base/geom2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <tuple>

namespace construe::io {

namespace {

constexpr int BULGE_DIGITS = 10;  // digits after the point of a bulge (§2д.3)
constexpr const char* CUT_LAYER = "CUT";
constexpr const char* BEND_UP_LAYER = "BEND-UP";
constexpr const char* BEND_DOWN_LAYER = "BEND-DOWN";
constexpr const char* BEND_LTYPE = "GOST_DASHDOTDOT";

Point2 rotateAbout(const Point2& p, const Point2& c, double a) {
    const Vec2 d = p - c;
    return c + Vec2(std::cos(a) * d.x() - std::sin(a) * d.y(), std::sin(a) * d.x() + std::cos(a) * d.y());
}

// Curve of a contour in traversal order: a line a → b, a polyline a → via… → b (a flattened spline, §2д.1 item 0) or an
// arc a → b about c with signed sweep (> 0 counterclockwise).
struct Seg {
    bool arc = false;
    Point2 a = Point2::Zero(), b = Point2::Zero(), c = Point2::Zero();
    double r = 0, sweep = 0;
    std::vector<Point2> via;
    Seg reversed() const {
        Seg s = *this;
        std::swap(s.a, s.b);
        s.sweep = -sweep;
        std::reverse(s.via.begin(), s.via.end());
        return s;
    }
    Point2 mid() const {
        if (!via.empty()) return via[via.size() / 2];
        return arc ? rotateAbout(a, c, sweep / 2) : Point2((a + b) / 2);
    }
    // Vertices a, via… (b is the start of the next curve).
    std::vector<Point2> chain() const {
        std::vector<Point2> v{a};
        v.insert(v.end(), via.begin(), via.end());
        return v;
    }
};

// Arc center from its chord and signed sweep φ (|φ| < 2π): m + perp(b − a)/2 · cot(φ/2).
Point2 arcCenter(const Point2& a, const Point2& b, double phi) {
    return Point2((a + b) / 2 + geom2::perp(b - a) / 2 * (std::cos(phi / 2) / std::sin(phi / 2)));
}

// Winding contribution, rad, of a curve seen from p (§2д.1 item 7).
double windingAngle(const Seg& s, const Point2& p) {
    if (!s.via.empty()) {
        auto pts = s.chain();
        pts.push_back(s.b);
        double w = 0;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const Vec2 u = pts[i] - p, v = pts[i + 1] - p;
            w += std::atan2(geom2::cross2(u, v), u.dot(v));
        }
        return w;
    }
    const Vec2 u = s.a - p, v = s.b - p;
    double w = std::atan2(geom2::cross2(u, v), u.dot(v));
    if (s.arc && (p - s.c).norm() < s.r) {
        const Vec2 chord = s.b - s.a;
        const double side = geom2::cross2(chord, s.mid() - s.a), ps = geom2::cross2(chord, p - s.a);
        if ((side > 0 && ps > 0) || (side < 0 && ps < 0)) w += s.sweep > 0 ? TWO_PI : -TWO_PI;
    }
    return w;
}

struct Loop {
    bool circle = false;
    Point2 c = Point2::Zero();
    double r = 0;
    std::vector<Seg> segs;
    int depth = 0;
    double area = 0;
    size_t order = 0;
    Point2 probe() const { return circle ? Point2(c + Vec2(r, 0)) : segs.front().mid(); }
    bool contains(const Point2& p) const {
        if (circle) return (p - c).norm() < r;
        double w = 0;
        for (const auto& s : segs) w += windingAngle(s, p);
        return std::lround(w / TWO_PI) != 0;
    }
};

double signedArea(const Loop& l) {
    if (l.circle) return PI * l.r * l.r;
    double a = 0;
    for (const auto& s : l.segs) {
        auto pts = s.chain();
        pts.push_back(s.b);
        for (size_t i = 0; i + 1 < pts.size(); ++i) a += geom2::cross2(pts[i], pts[i + 1]) / 2;
        if (s.arc) a += s.r * s.r / 2 * (s.sweep - std::sin(s.sweep));
    }
    return a;
}

std::string gapNumber(double x) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3g", x);
    return buf;
}

// Merges adjacent curves of a closed contour (§2д.1 item 5) until nothing merges.
void mergeAdjacent(std::vector<Seg>& segs) {
    bool merged = true;
    while (merged && segs.size() > 1) {
        merged = false;
        for (size_t i = 0; i < segs.size() && segs.size() > 1; ++i) {
            const size_t j = (i + 1) % segs.size();
            const Seg& s = segs[i];
            const Seg& t = segs[j];
            Seg m;
            if (!s.via.empty() || !t.via.empty()) continue;  // a flattened spline merges with nothing
            if (!s.arc && !t.arc) {
                const Vec2 ac = t.b - s.a;
                const double len = ac.norm();
                const double dist = len > 0 ? std::abs(geom2::cross2(ac, t.a - s.a)) / len : (t.a - s.a).norm();
                if (!(dist <= FLAT_MERGE_TOL && (t.a - s.a).dot(t.b - t.a) > 0)) continue;
                m.a = s.a, m.b = t.b;
            } else if (s.arc && t.arc && (s.sweep > 0) == (t.sweep > 0) && (s.c - t.c).norm() <= FLAT_MERGE_TOL &&
                       std::abs(s.r - t.r) <= FLAT_MERGE_TOL) {
                m = s;
                m.b = t.b;
                m.sweep = s.sweep + t.sweep;
            } else {
                continue;
            }
            segs[i] = m;
            segs.erase(segs.begin() + std::ptrdiff_t(j));
            merged = true;
            if (j < i) --i;  // the first curve was merged into the last one
        }
    }
}

}  // namespace

Result<std::vector<FlatLoop>> flatLoops(const std::vector<kernel::Curve2>& curves) {
    using K = kernel::Curve2::Kind;
    // 1. Cleaning and duplicates; circles are contours of their own.
    std::vector<Seg> segs;
    std::vector<Loop> loops;
    auto coincide = [](const Point2& p, const Point2& q) { return (p - q).norm() <= FLAT_JOIN_TOL; };
    auto addCircle = [&](const Point2& c, double r) {
        for (const auto& l : loops)
            if (coincide(l.c, c) && std::abs(l.r - r) <= FLAT_JOIN_TOL) return;
        Loop l;
        l.circle = true, l.c = c, l.r = r;
        loops.push_back(l);
    };
    for (const auto& cv : curves) {
        Seg s;
        if (cv.kind == K::Spline) {
            // Gear teeth on a sheet wall (FX7-SH-004): a polyline of chords with deviation ≤ FLAT_SPLINE_TOL; vertices
            // closer than FLAT_JOIN_TOL to the previous one are dropped, the end is kept exact.
            const spline::Flat f = spline::expand(cv.spline);
            std::vector<Point2> pts;
            for (double t : spline::flattenParams(f, FLAT_SPLINE_TOL)) {
                const Point2 q = spline::eval(f, t);
                if (pts.empty() || (q - pts.back()).norm() > FLAT_JOIN_TOL) pts.push_back(q);
            }
            if (pts.size() < 2) continue;
            pts.back() = spline::eval(f, f.b);
            s.a = pts.front(), s.b = pts.back();
            s.via.assign(pts.begin() + 1, pts.end() - 1);
            if (s.via.empty() && (s.b - s.a).norm() <= FLAT_JOIN_TOL) continue;
        } else if (cv.kind == K::Line) {
            s.a = cv.p1, s.b = cv.p2;
            if ((s.b - s.a).norm() <= FLAT_JOIN_TOL) continue;
        } else if (cv.kind == K::Circle) {
            addCircle(cv.center, cv.radius);
            continue;
        } else if (cv.kind == K::Arc) {
            s.arc = true, s.a = cv.start, s.b = cv.end, s.c = cv.center, s.r = cv.radius;
            s.sweep = dxf::sweep(std::atan2(cv.start.y() - cv.center.y(), cv.start.x() - cv.center.x()),
                                 std::atan2(cv.end.y() - cv.center.y(), cv.end.x() - cv.center.x()));
            if (s.sweep >= TWO_PI - FLAT_FULL_TURN_TOL) {
                addCircle(cv.center, cv.radius);
                continue;
            }
            if ((s.b - s.a).norm() <= FLAT_JOIN_TOL && s.sweep < PI) continue;
        } else {
            continue;  // Point
        }
        const bool dup = std::any_of(segs.begin(), segs.end(), [&](const Seg& q) {
            if (q.arc != s.arc) return false;
            // Arcs are counterclockwise: the same arc has the same start and end (b → a is the complementary arc).
            if (s.arc) return coincide(q.a, s.a) && coincide(q.b, s.b) && coincide(q.c, s.c) && std::abs(q.r - s.r) <= FLAT_JOIN_TOL;
            if (q.via.empty() != s.via.empty() || (!s.via.empty() && !coincide(q.mid(), s.mid()) && !coincide(q.reversed().mid(), s.mid())))
                return false;
            return (coincide(q.a, s.a) && coincide(q.b, s.b)) || (coincide(q.a, s.b) && coincide(q.b, s.a));
        });
        if (!dup) segs.push_back(s);
    }
    // 3. Chaining.
    std::vector<bool> used(segs.size(), false);
    for (size_t first = 0; first < segs.size(); ++first) {
        if (used[first]) continue;
        used[first] = true;
        std::vector<Seg> chain{segs[first]};
        const Point2 start = segs[first].a;
        for (;;) {
            const Point2 e = chain.back().b;
            if (coincide(e, start)) break;
            double best = std::numeric_limits<double>::infinity();
            size_t bi = 0;
            bool bEnd = false;
            for (size_t i = 0; i < segs.size(); ++i) {
                if (used[i]) continue;
                const double da = (segs[i].a - e).norm(), db = (segs[i].b - e).norm();
                if (da < best) best = da, bi = i, bEnd = false;
                if (db < best) best = db, bi = i, bEnd = true;
            }
            if (!(best <= FLAT_JOIN_TOL)) {
                const double gap = std::min(best, (e - start).norm());
                return Error("EXPORT_FLAT_OPEN")
                    .with("gap", std::stod(gapNumber(gap)))
                    .with("x", std::round(e.x() * 1e6) / 1e6)
                    .with("y", std::round(e.y() * 1e6) / 1e6);
            }
            used[bi] = true;
            chain.push_back(bEnd ? segs[bi].reversed() : segs[bi]);
        }
        // 4. Vertices: the shared point is the end of the previous curve; the contour closes on the first vertex.
        for (size_t i = 1; i < chain.size(); ++i) chain[i].a = chain[i - 1].b;
        chain.back().b = chain.front().a;
        // 5. Merging; a contour of a single arc is a circle.
        mergeAdjacent(chain);
        if (chain.size() == 1 && chain.front().arc) {
            addCircle(chain.front().c, chain.front().r);
            continue;
        }
        if (chain.size() < 2) continue;  // degenerate (cannot happen after cleaning)
        // Arc centers and radii from the final ends (the file holds ends and bulges, §2д.1 item 5).
        for (auto& s : chain)
            if (s.arc) {
                s.c = arcCenter(s.a, s.b, s.sweep);
                s.r = (s.a - s.c).norm();
            }
        Loop l;
        l.segs = std::move(chain);
        loops.push_back(std::move(l));
    }
    // 6–8. Area, nesting, orientation; 9. start vertex.
    for (size_t i = 0; i < loops.size(); ++i) {
        loops[i].order = i;
        const Point2 p = loops[i].probe();
        for (size_t j = 0; j < loops.size(); ++j)
            if (j != i && loops[j].contains(p)) ++loops[i].depth;
    }
    std::vector<FlatLoop> out;
    for (auto& l : loops) {
        l.area = signedArea(l);
        const bool ccw = l.depth % 2 == 0;
        if (!l.circle && (l.area > 0) != ccw) {
            std::vector<Seg> rev;
            for (auto it = l.segs.rbegin(); it != l.segs.rend(); ++it) rev.push_back(it->reversed());
            l.segs = std::move(rev);
            l.area = -l.area;
        }
        if (l.circle && !ccw) l.area = -l.area;
        if (!l.circle) {
            size_t s0 = 0;
            for (size_t i = 1; i < l.segs.size(); ++i)
                if (std::make_pair(l.segs[i].a.x(), l.segs[i].a.y()) < std::make_pair(l.segs[s0].a.x(), l.segs[s0].a.y())) s0 = i;
            std::rotate(l.segs.begin(), l.segs.begin() + std::ptrdiff_t(s0), l.segs.end());
        }
    }
    // 10. Order: deeper first, then by the smallest x, y.
    auto low = [](const Loop& l) {
        if (l.circle) return Point2(l.c - Vec2(l.r, l.r));
        Point2 m = l.segs.front().a;
        for (const auto& s : l.segs)
            for (const auto& q : s.chain()) m = m.cwiseMin(q);
        return m;
    };
    // Sort indices, not the loops: stable_sort of Eigen-holding structs needs an over-aligned buffer (MSVC C2338).
    std::vector<size_t> byOrder(loops.size());
    for (size_t i = 0; i < byOrder.size(); ++i) byOrder[i] = i;
    std::stable_sort(byOrder.begin(), byOrder.end(), [&](size_t i, size_t j) {
        const Loop& x = loops[i];
        const Loop& y = loops[j];
        const Point2 lx = low(x), ly = low(y);
        return std::make_tuple(-x.depth, lx.x(), lx.y(), x.order) < std::make_tuple(-y.depth, ly.x(), ly.y(), y.order);
    });
    {
        decltype(loops) sorted;
        sorted.reserve(loops.size());
        for (size_t i : byOrder) sorted.push_back(std::move(loops[i]));
        loops = std::move(sorted);
    }
    for (const auto& l : loops) {
        FlatLoop f;
        f.circle = l.circle, f.center = l.c, f.radius = l.r, f.depth = l.depth, f.area = l.area;
        for (const auto& s : l.segs) {
            f.vertices.push_back({s.a, s.arc ? std::tan(s.sweep / 4) : 0.0});
            for (const auto& q : s.via) f.vertices.push_back({q, 0.0});
        }
        out.push_back(std::move(f));
    }
    return out;
}

double flatArea(const std::vector<FlatLoop>& loops) {
    double a = 0;
    for (const auto& l : loops) a += l.depth % 2 == 0 ? std::abs(l.area) : -std::abs(l.area);
    return a;
}

namespace {

// Extents of a part: contour vertices, extreme points of arcs, circles, bend lines (§2д.2).
Eigen::AlignedBox2d partBox(const FlatDxfPart& p) {
    Eigen::AlignedBox2d b;
    for (const auto& l : p.loops) {
        if (l.circle) {
            b.extend(Point2(l.center - Vec2(l.radius, l.radius)));
            b.extend(Point2(l.center + Vec2(l.radius, l.radius)));
            continue;
        }
        for (size_t i = 0; i < l.vertices.size(); ++i) {
            const Point2 a = l.vertices[i].p, e = l.vertices[(i + 1) % l.vertices.size()].p;
            b.extend(a);
            const double bulge = l.vertices[i].bulge;
            if (bulge == 0) continue;
            const double phi = 4 * std::atan(bulge);
            const Point2 c = arcCenter(a, e, phi);
            const double r = (a - c).norm();
            const Point2 from = phi > 0 ? a : e;  // the arc counterclockwise from `from` by |φ|
            const double a0 = std::atan2(from.y() - c.y(), from.x() - c.x());
            for (int k = 0; k < 4; ++k) {
                if (geom2::ccwSweep(a0, k * PI / 2) < std::abs(phi)) b.extend(Point2(c + r * Vec2(std::cos(k * PI / 2), std::sin(k * PI / 2))));
            }
        }
    }
    if (!b.isEmpty())
        for (const auto& bl : p.bends) b.extend(bl.a), b.extend(bl.b);
    return b;
}

}  // namespace

FlatLayout flatLayout(const std::vector<FlatDxfPart>& parts) {
    FlatLayout lay;
    double x = 0;
    bool any = false;
    for (const auto& p : parts) {
        const auto b = partBox(p);
        if (b.isEmpty()) {
            lay.shifts.push_back(Point2::Zero());
            continue;
        }
        if (any) x += FLAT_LAYOUT_GAP;
        lay.shifts.push_back(Point2(x - b.min().x(), -b.min().y()));
        x += b.sizes().x();
        lay.height = std::max(lay.height, b.sizes().y());
        any = true;
    }
    lay.width = x;
    return lay;
}

std::string flatDxf(const std::vector<FlatDxfPart>& parts, bool bendLines) {
    const FlatLayout lay = flatLayout(parts);
    bool cut = false, up = false, down = false;
    for (const auto& p : parts) {
        if (p.loops.empty()) continue;
        cut = true;
        if (bendLines)
            for (const auto& b : p.bends) (b.up ? up : down) = true;
    }
    dxf::Tables tables;
    tables.linetypes.push_back({BEND_LTYPE, "GOST 2.303 dash-dot-dot", drawing::dashPattern(drawing::LineType::DashDotDot)});
    if (cut) tables.layers.push_back({CUT_LAYER, 7, "Continuous", false});
    if (up) tables.layers.push_back({BEND_UP_LAYER, 1, BEND_LTYPE, false});
    if (down) tables.layers.push_back({BEND_DOWN_LAYER, 5, BEND_LTYPE, false});
    dxf::File file(lay.width, lay.height, tables);
    dxf::Writer& w = file.w();
    for (size_t k = 0; k < parts.size(); ++k) {
        const FlatDxfPart& p = parts[k];
        if (p.loops.empty()) continue;
        const Vec2 d = lay.shifts[k];
        for (const auto& l : p.loops) {
            if (l.circle) {
                file.entity("CIRCLE", CUT_LAYER);
                w.pair(100, "AcDbCircle");
                w.point(10, Point2(l.center + d));
                w.number(40, l.radius);
                continue;
            }
            file.entity("LWPOLYLINE", CUT_LAYER);
            w.pair(100, "AcDbPolyline");
            w.integer(90, int(l.vertices.size()));
            w.integer(70, 1);
            for (const auto& v : l.vertices) {
                w.point(10, Point2(v.p + d), false);
                if (v.bulge != 0) w.pair(42, dxf::num(v.bulge, BULGE_DIGITS));
            }
        }
        if (!bendLines) continue;
        for (const auto& b : p.bends) {
            file.entity("LINE", b.up ? BEND_UP_LAYER : BEND_DOWN_LAYER);
            w.pair(100, "AcDbLine");
            w.point(10, Point2(b.a + d));
            w.point(11, Point2(b.b + d));
        }
    }
    return file.finish();
}

Status writeFlatDxf(const std::string& path, const std::vector<FlatDxfPart>& parts, bool bendLines) {
    return writeFileAtomic(path, flatDxf(parts, bendLines));
}

}  // namespace construe::io
