// Patterns of identical features (cuts, bosses, ribs, slots, not only holes): identical closed loops of one
// extrusion operation, arranged along a line, on a grid or on a circle, → feature and pattern
// (spec/reconstruction.md §4.8; S4-REC-015).
#include "rec/rec_internal.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace construe::rec::detail {

namespace {

using C2 = kernel::Curve2::Kind;

bool lexLess3(const Point3& a, const Point3& b) {
    for (int i = 0; i < 3; ++i)
        if (std::abs(a[i] - b[i]) > REC_LIN_TOL * 10) return a[i] < b[i];
    return false;
}

// Closed loop of an operation (§4.8 item 1).
struct Loop {
    std::vector<kernel::Curve2> curves;
    Point2 centroid = Point2::Zero();  // length-weighted center of mass of the curves
    double length = 0;
    int lines = 0, arcs = 0, circles = 0;
    Eigen::AlignedBox2d box;
};

double arcSweep(const kernel::Curve2& c) {
    const double a0 = std::atan2(c.start.y() - c.center.y(), c.start.x() - c.center.x());
    double a1 = std::atan2(c.end.y() - c.center.y(), c.end.x() - c.center.x());
    while (a1 <= a0 + 1e-12) a1 += 2 * PI;
    return a1 - a0;
}

// Curves → closed loops (connectivity by common ends); nullopt means there is an open one.
std::optional<std::vector<Loop>> loopsOf(const std::vector<kernel::Curve2>& cs) {
    const size_t n = cs.size();
    std::vector<size_t> parent(n);
    std::iota(parent.begin(), parent.end(), size_t(0));
    std::function<size_t(size_t)> find = [&](size_t x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    auto ends = [&](const kernel::Curve2& c) -> std::vector<Point2> {
        if (c.kind == C2::Line) return {c.p1, c.p2};
        if (c.kind == C2::Arc) return {c.start, c.end};
        return {};
    };
    for (size_t i = 0; i < n; ++i) {
        // Patterns of spline outlines are deferred (S4A-SPL-004): the operation stays as it is.
        if (cs[i].kind == C2::Point || cs[i].kind == C2::Spline) return std::nullopt;
        for (const auto& p : ends(cs[i])) {
            bool matched = false;
            for (size_t j = 0; j < n; ++j) {
                if (j == i) continue;
                for (const auto& q : ends(cs[j]))
                    if ((p - q).norm() <= REC_LIN_TOL * 10) {
                        matched = true;
                        parent[find(i)] = find(j);
                    }
            }
            if (!matched) return std::nullopt;
        }
    }
    std::map<size_t, Loop> by;
    std::vector<size_t> order;
    for (size_t i = 0; i < n; ++i) {
        const size_t r = find(i);
        if (!by.count(r)) order.push_back(r);
        Loop& L = by[r];
        const auto& c = cs[i];
        L.curves.push_back(c);
        double len = 0;
        Point2 m;
        if (c.kind == C2::Line) {
            len = (c.p2 - c.p1).norm();
            m = (c.p1 + c.p2) / 2;
            L.box.extend(c.p1), L.box.extend(c.p2);
            ++L.lines;
        } else if (c.kind == C2::Arc) {
            const double sw = arcSweep(c);
            len = c.radius * sw;
            const double a0 = std::atan2(c.start.y() - c.center.y(), c.start.x() - c.center.x()), am = a0 + sw / 2;
            m = c.center + c.radius * (std::sin(sw / 2) / (sw / 2)) * Point2(std::cos(am), std::sin(am));
            L.box.extend(c.start), L.box.extend(c.end);
            for (int q = 0; q < 4; ++q) {  // extreme points of the arc along the axes
                double a = q * PI / 2;
                while (a < a0) a += 2 * PI;
                if (a <= a0 + sw) L.box.extend(c.center + c.radius * Point2(std::cos(a), std::sin(a)));
            }
            ++L.arcs;
        } else {
            len = 2 * PI * c.radius;
            m = c.center;
            L.box.extend(c.center - Point2(c.radius, c.radius)), L.box.extend(c.center + Point2(c.radius, c.radius));
            ++L.circles;
        }
        L.centroid += m * len;
        L.length += len;
    }
    std::vector<Loop> out;
    for (size_t r : order) {
        Loop L = by[r];
        if (L.length > 0) L.centroid /= L.length;
        out.push_back(L);
    }
    return out;
}

bool sameKindCounts(const Loop& a, const Loop& b) {
    return a.lines == b.lines && a.arcs == b.arcs && a.circles == b.circles && a.curves.size() == b.curves.size() &&
           std::abs(a.length - b.length) <= REC_LIN_TOL * 100;
}

// A curve in world coordinates (3 points and a radius), for transfer between planes.
struct Curve3 {
    C2 kind;
    Point3 a, b, c;  // Line: a, b; Arc: c is the center, a the start, b the end; Circle: c
    double r = 0;
};
Curve3 toWorld(const kernel::Curve2& k, const Plane& P) {
    Curve3 w{k.kind, Point3::Zero(), Point3::Zero(), Point3::Zero(), k.radius};
    if (k.kind == C2::Line) w.a = P.toWorld(k.p1), w.b = P.toWorld(k.p2);
    else if (k.kind == C2::Arc) w.c = P.toWorld(k.center), w.a = P.toWorld(k.start), w.b = P.toWorld(k.end);
    else w.c = P.toWorld(k.center);
    return w;
}
kernel::Curve2 toLocal(const Curve3& w, const Plane& P, bool swapArc) {
    kernel::Curve2 k;
    k.kind = w.kind;
    k.radius = w.r;
    if (w.kind == C2::Line) k.p1 = P.toLocal(w.a), k.p2 = P.toLocal(w.b);
    else if (w.kind == C2::Arc) {
        k.center = P.toLocal(w.c);
        k.start = P.toLocal(swapArc ? w.b : w.a);
        k.end = P.toLocal(swapArc ? w.a : w.b);
    } else {
        k.center = P.toLocal(w.c);
    }
    return k;
}

// All curves of a (after the transform) are among b and vice versa (same count).
bool sameCurveSet(const std::vector<kernel::Curve2>& a, const std::vector<kernel::Curve2>& b) {
    if (a.size() != b.size()) return false;
    std::vector<bool> used(b.size(), false);
    for (const auto& x : a) {
        bool f = false;
        for (size_t j = 0; j < b.size() && !f; ++j)
            if (!used[j] && sameCurve(x, b[j], REC_LIN_TOL * 100)) used[j] = true, f = true;
        if (!f) return false;
    }
    return true;
}

// Mirror plane of two loops (§4.8 item 2, S4-REC-020): centers symmetric about a built-in plane and loop a,
// reflected, coincides with loop b (reflection reverses the traversal: arcs swap start and end).
std::optional<std::string> mirrorOf(const Loop& a, const Loop& b, const Plane& P) {
    const Point3 ca = P.toWorld(a.centroid), cb = P.toWorld(b.centroid);
    for (const auto& [name, n] : sketch::datumPlaneNormals()) {
        if (std::abs(((ca + cb) / 2).dot(n)) > REC_LIN_TOL * 100) continue;
        const Vec3 v = cb - ca;
        if (v.norm() <= REC_LIN_TOL * 100 || !parallel(v, n, REC_ANGLE_TOL * 100)) continue;
        std::vector<kernel::Curve2> moved;
        for (const auto& c : a.curves) {
            Curve3 w = toWorld(c, P);
            w.a = reflect(w.a, n), w.b = reflect(w.b, n), w.c = reflect(w.c, n);
            moved.push_back(toLocal(w, P, true));
        }
        if (sameCurveSet(moved, b.curves)) return name;
    }
    return std::nullopt;
}

}  // namespace

std::optional<PatternFit> fitPattern(const std::vector<Point3>& ptsIn, const Vec3& nIn) {
    const size_t N = ptsIn.size();
    if (N < 2) return std::nullopt;
    const Vec3 n = nIn.normalized();
    std::vector<size_t> idx(N);
    std::iota(idx.begin(), idx.end(), size_t(0));
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return lexLess3(ptsIn[a], ptsIn[b]); });
    for (size_t i = 1; i < N; ++i)
        if ((ptsIn[idx[i]] - ptsIn[idx[i - 1]]).norm() <= REC_LIN_TOL * 100) return std::nullopt;  // coincident points
    PatternFit fit;
    fit.seed = idx[0];
    const Point3 p0 = ptsIn[idx[0]];
    fit.motion.assign(N, Motion{});
    // Circular (N ≥ 3): equal distances to the mean in the plane ⊥ n, equal angular step (as in §4.4.4 item 1).
    if (N >= 3) {
        Point3 o = Point3::Zero();
        for (const auto& p : ptsIn) o += p;
        o /= double(N);
        bool ok = true;
        for (const auto& p : ptsIn) ok = ok && std::abs((p - o).dot(n)) <= REC_LIN_TOL * 100;
        const double R = (p0 - o).norm();
        ok = ok && R > REC_LIN_TOL * 100;
        for (const auto& p : ptsIn) ok = ok && std::abs((p - o).norm() - R) <= REC_LIN_TOL * 100;
        if (ok) {
            Vec3 ax = -n;
            const Vec3 u = (p0 - o).normalized();
            std::vector<double> ang(N);
            for (size_t i = 0; i < N; ++i) {
                const Vec3 r = ptsIn[i] - o;
                double t = std::atan2(ax.dot(u.cross(r)), u.dot(r));
                if (t < -1e-9) t += 2 * PI;
                ang[i] = t;
            }
            std::vector<double> sorted = ang;
            std::sort(sorted.begin(), sorted.end());
            for (size_t i = 0; i < N; ++i) ok = ok && std::abs(sorted[i] - 2 * PI * double(i) / double(N)) <= 1e-6;
            if (ok) {
                fit.circular = true;
                fit.origin = o;
                fit.axis = ax;
                fit.count = int(N);
                for (size_t i = 0; i < N; ++i) {
                    fit.motion[i].rotate = true;
                    fit.motion[i].angle = ang[i];
                }
                return fit;
            }
        }
    }
    // Linear: nodes of an n1 × n2 grid along the coordinate axes ⊥ n, with equal steps (as in §4.4.4 item 2).
    std::vector<Op::Direction> dirs;
    size_t total = 1;
    for (int axi = 0; axi < 3; ++axi) {
        Vec3 e = Vec3::Zero();
        e[axi] = 1;
        std::vector<double> vals;
        for (const auto& p : ptsIn) {
            bool seen = false;
            for (double v : vals) seen = seen || std::abs(v - p[axi]) <= REC_LIN_TOL * 10;
            if (!seen) vals.push_back(p[axi]);
        }
        if (vals.size() < 2) continue;
        if (!perpendicular(e, n, 1e-9)) return std::nullopt;
        std::sort(vals.begin(), vals.end());
        const double step = vals[1] - vals[0];
        for (size_t i = 1; i < vals.size(); ++i)
            if (std::abs(vals[i] - vals[0] - step * double(i)) > REC_LIN_TOL * 10) return std::nullopt;
        dirs.push_back({e, int(vals.size()), step});
        total *= vals.size();
    }
    if (dirs.empty() || dirs.size() > 2 || total != N) return std::nullopt;
    fit.linear = dirs;
    for (size_t i = 0; i < N; ++i) fit.motion[i].shift = ptsIn[i] - p0;
    return fit;
}

std::vector<Op> splitPatterns(const std::vector<Op>& block) {
    std::vector<Op> out;
    for (const Op& op : block) {
        const int shift = int(out.size()) - int(&op - block.data());  // shift of target indices inside the block
        auto keep = [&] {
            Op o = op;
            if (o.target >= 0) o.target += shift;
            out.push_back(o);
        };
        if (op.kind != Op::Kind::Extrude || op.curves.size() < 2) {
            keep();
            continue;
        }
        auto loops = loopsOf(op.curves);
        if (!loops || loops->size() < 2) {
            keep();
            continue;
        }
        const auto& L = *loops;
        // Free loops: the bounding box intersects no other loop's box (neither nested nor containing others).
        std::vector<bool> isFree(L.size(), true);
        for (size_t i = 0; i < L.size(); ++i)
            for (size_t j = 0; j < L.size(); ++j) {
                if (i == j) continue;
                Eigen::AlignedBox2d a = L[i].box, b = L[j].box;
                a.min().array() -= REC_LIN_TOL * 10, a.max().array() += REC_LIN_TOL * 10;
                if (a.intersects(b)) isFree[i] = false;
            }
        std::vector<bool> taken(L.size(), false);
        struct Group {
            size_t seed;
            PatternFit fit;
            std::vector<size_t> members;
            std::string mirror;  // mirror plane (§4.8 item 2); empty — pattern
        };
        std::vector<Group> groups;
        for (size_t i = 0; i < L.size(); ++i) {
            if (!isFree[i] || taken[i]) continue;
            std::vector<size_t> m{i};
            for (size_t j = i + 1; j < L.size(); ++j)
                if (isFree[j] && !taken[j] && sameKindCounts(L[i], L[j])) m.push_back(j);
            if (m.size() < 2) continue;
            // Two loops: a mirror about a built-in plane first (as for holes, §4.4.4 item 3); the seed is the
            // lexicographically first center.
            if (m.size() == 2) {
                const bool firstSeed = !lexLess3(op.plane.toWorld(L[m[1]].centroid), op.plane.toWorld(L[m[0]].centroid));
                const size_t sd = firstSeed ? m[0] : m[1], other = firstSeed ? m[1] : m[0];
                if (auto plane = mirrorOf(L[sd], L[other], op.plane)) {
                    for (size_t x : m) taken[x] = true;
                    groups.push_back({sd, PatternFit{}, m, *plane});
                    continue;
                }
            }
            std::vector<Point3> pts;
            for (size_t x : m) pts.push_back(op.plane.toWorld(L[x].centroid));
            auto fit = fitPattern(pts, op.plane.normal);
            if (!fit) continue;
            // Loop match: the seed loop, moved by the instance motion, coincides with the instance loop.
            const Loop& seed = L[m[fit->seed]];
            bool ok = true;
            for (size_t q = 0; q < m.size() && ok; ++q) {
                if (q == fit->seed) continue;
                const Motion& mv = fit->motion[q];
                const Motion& m0 = fit->motion[fit->seed];
                std::vector<kernel::Curve2> moved;
                for (const auto& c : seed.curves) {
                    Curve3 w = toWorld(c, op.plane);
                    auto apply = [&](const Point3& p) -> Point3 {
                        if (mv.rotate) return fit->origin + rotateAbout(p - fit->origin, fit->axis.normalized(), mv.angle - m0.angle);
                        return p + mv.shift - m0.shift;
                    };
                    w.a = apply(w.a), w.b = apply(w.b), w.c = apply(w.c);
                    moved.push_back(toLocal(w, op.plane, false));
                }
                ok = sameCurveSet(moved, L[m[q]].curves);
            }
            if (!ok) continue;
            for (size_t x : m) taken[x] = true;
            groups.push_back({m[fit->seed], *fit, m, {}});
        }
        if (groups.empty()) {
            keep();
            continue;
        }
        // The remaining loops go to their own operation (first), then per group: feature and pattern.
        Op rest = op;
        rest.curves.clear();
        for (size_t i = 0; i < L.size(); ++i)
            if (!taken[i]) rest.curves.insert(rest.curves.end(), L[i].curves.begin(), L[i].curves.end());
        if (!rest.curves.empty()) {
            if (rest.target >= 0) rest.target += shift;
            out.push_back(rest);
        }
        for (const auto& g : groups) {
            Op seed = op;
            seed.target = -1;
            seed.curves = L[g.seed].curves;
            out.push_back(seed);
            if (!g.mirror.empty()) {
                Op mi;
                mi.kind = Op::Kind::Mirror;
                mi.target = int(out.size()) - 1;
                mi.mirrorPlane = g.mirror;
                out.push_back(mi);
                continue;
            }
            Op p;
            p.kind = Op::Kind::Pattern;
            p.target = int(out.size()) - 1;
            p.plane = op.plane;
            for (size_t x : g.members) p.curves.insert(p.curves.end(), L[x].curves.begin(), L[x].curves.end());  // all instances (§5.3)
            if (g.fit.circular) {
                p.circular = true;
                p.axisOrigin = g.fit.origin;
                p.axisDir = g.fit.axis;
                p.count = g.fit.count;
            } else {
                p.linear = g.fit.linear;
            }
            out.push_back(p);
        }
    }
    return out;
}

}  // namespace construe::rec::detail

namespace construe::rec {

bool insideCurves(const std::vector<kernel::Curve2>& curves, const Point2& q) {
    using C2 = kernel::Curve2::Kind;
    const double chord = REC_LIN_TOL * 100;
    bool in = false;
    auto edge = [&](const Point2& a, const Point2& b) {
        if ((a.y() > q.y()) != (b.y() > q.y()) && q.x() < a.x() + (q.y() - a.y()) * (b.x() - a.x()) / (b.y() - a.y())) in = !in;
    };
    // Arc of radius r from angle a0 by sweep th (counterclockwise) as chords.
    auto arc = [&](const Point2& c, double r, double a0, double th) {
        const double step = chord < r ? 2 * std::acos(1 - chord / r) : th;
        const int k = std::clamp(int(std::ceil(th / std::max(step, 1e-3))), 8, 4096);
        Point2 prev = c + r * Point2(std::cos(a0), std::sin(a0));
        for (int i = 1; i <= k; ++i) {
            const double t = a0 + th * i / k;
            const Point2 p = c + r * Point2(std::cos(t), std::sin(t));
            edge(prev, p);
            prev = p;
        }
    };
    for (const auto& c : curves) {
        if (c.kind == C2::Line) {
            edge(c.p1, c.p2);
        } else if (c.kind == C2::Circle) {
            arc(c.center, c.radius, 0, 2 * M_PI);
        } else if (c.kind == C2::Arc) {
            const Point2 s = c.start - c.center, e = c.end - c.center;
            const double a0 = std::atan2(s.y(), s.x());
            double th = std::atan2(e.y(), e.x()) - a0;
            while (th <= 0) th += 2 * M_PI;
            arc(c.center, s.norm(), a0, th);
        } else if (c.kind == C2::Spline) {
            const auto f = spline::expand(c.spline);
            const auto ts = spline::flattenParams(f, chord);
            for (size_t i = 1; i < ts.size(); ++i) edge(spline::eval(f, ts[i - 1]), spline::eval(f, ts[i]));
        }
    }
    return in;
}

}  // namespace construe::rec

namespace construe::rec::detail {

// One sketch for operations of one level (§4.11, FX10-REC-004).
void mergeLevelOps(Plan& plan) {
    using K = Op::Kind;
    const size_t n = plan.ops.size();
    std::vector<char> targeted(n, 0);
    // A round cut with one pattern or mirror of known points can spread its points over the sketch if it joins another cut.
    std::vector<int> spreadBy(n, -1);
    for (size_t j = 0; j < n; ++j) {
        const Op& o = plan.ops[j];
        if (o.target < 0) continue;
        const size_t t = size_t(o.target);
        const bool spreads = (o.kind == K::Pattern || o.kind == K::Mirror) && !o.points.empty() && plan.ops[t].kind == K::CircleCut && !targeted[t] && spreadBy[t] < 0;
        if (spreads) spreadBy[t] = int(j);
        else targeted[t] = 1, spreadBy[t] = -1;
    }
    // A simple round cut without a pattern is an extrusion of circles; it stays as it was if nothing joins it.
    const std::vector<Op> original = plan.ops;
    std::vector<char> merged(n, 0);
    for (size_t i = 0; i < n; ++i) {
        Op& o = plan.ops[i];
        if (o.kind != K::CircleCut || targeted[i]) continue;
        Op e;
        e.kind = K::Extrude;
        e.plane = o.plane;
        e.cut = true;
        e.reverse = true;
        e.through = o.through;
        e.both = o.both;
        e.depth = o.depth;
        for (const auto& p : spreadBy[i] >= 0 ? plan.ops[size_t(spreadBy[i])].points : o.points) {
            kernel::Curve2 c;
            c.kind = kernel::Curve2::Kind::Circle;
            c.center = o.plane.toLocal(p);
            c.radius = o.radius;
            e.curves.push_back(c);
        }
        o = e;
    }
    // Both operations act the same way on one sketch: the same plane and direction, operation, end.
    auto sameDepth = [](const Op& a, const Op& b) { return a.through || std::abs(a.depth - b.depth) <= REC_LIN_TOL * 100; };
    // Of one level (item 2) apart from the depth; a different depth is shared on one sketch (item 5).
    auto sameLevel = [](const Op& a, const Op& b) {
        if (a.kind != K::Extrude || b.kind != K::Extrude || a.text || b.text || a.cut != b.cut || a.through != b.through || a.both != b.both) return false;
        if (a.plane.normal.normalized().dot(b.plane.normal.normalized()) < 1 - REC_ANGLE_TOL && !(a.both && std::abs(a.plane.normal.normalized().dot(b.plane.normal.normalized())) > 1 - REC_ANGLE_TOL)) return false;
        if (a.reverse != b.reverse && !a.both) return false;
        return std::abs((b.plane.origin - a.plane.origin).dot(a.plane.normal.normalized())) <= REC_LIN_TOL * 100;
    };
    // An operation between them that the later one can be moved over: of the same sign, not depending on the shape.
    auto commutes = [&](const Op& a, bool cut) {
        if ((a.kind == K::Pattern || a.kind == K::Mirror) && a.target >= 0) return cut && (original[size_t(a.target)].kind == K::CircleCut || original[size_t(a.target)].kind == K::Hole);
        return (a.kind == K::Extrude && a.cut == cut && !a.text) || (cut && (a.kind == K::CircleCut || a.kind == K::Hole));
    };
    std::vector<char> gone(n, 0);
    std::vector<size_t> block(n);
    {
        size_t b = 0;
        for (size_t i = 0; i < n; ++i) {
            block[i] = b;
            if (plan.resume[i] == i + 1) ++b;
        }
    }
    // Curves of b into the local coordinates of a's plane (its normal may be opposite for a cut through both ways).
    auto curvesIn = [](const Op& a, const Op& b) {
        std::vector<kernel::Curve2> out;
        const bool flip = a.plane.normal.dot(b.plane.normal) < 0;
        auto m = [&](const Point2& q) { return a.plane.toLocal(b.plane.toWorld(q)); };
        for (auto c : b.curves) {
            c.p = m(c.p), c.p1 = m(c.p1), c.p2 = m(c.p2), c.center = m(c.center);
            const Point2 st = m(c.start), en = m(c.end);
            c.start = flip ? en : st, c.end = flip ? st : en;
            if (c.kind == kernel::Curve2::Kind::Spline)
                for (auto& q : c.spline.poles) q = m(q);
            out.push_back(c);
        }
        return out;
    };
    // j joins i (items 3-4): its curves in i's sketch, the blocks from i's to j's become one.
    auto join = [&](size_t i, size_t j) {
        const auto cs = curvesIn(plan.ops[i], plan.ops[j]);
        plan.ops[i].curves.insert(plan.ops[i].curves.end(), cs.begin(), cs.end());
        gone[j] = 1;
        merged[i] = 1;
        for (size_t q = block[i] + 1; q <= block[j]; ++q)
            for (size_t x = 0; x < n; ++x)
                if (block[x] == q) block[x] = block[i];
    };
    std::vector<std::vector<size_t>> sharers(n);  // item 5: extrusions of another depth built on the sketch of i
    for (size_t i = 0; i < n; ++i) {
        if (gone[i] || plan.ops[i].kind != K::Extrude || targeted[i] || plan.ops[i].sketchOf >= 0) continue;
        for (size_t j = i + 1; j < n; ++j) {
            if (gone[j]) continue;
            if (!targeted[j] && plan.ops[j].sketchOf < 0 && sameLevel(plan.ops[i], plan.ops[j])) {
                if (sameDepth(plan.ops[i], plan.ops[j])) {
                    join(i, j);
                    continue;
                }
                auto s = std::find_if(sharers[i].begin(), sharers[i].end(), [&](size_t x) { return sameDepth(plan.ops[x], plan.ops[j]); });
                if (s != sharers[i].end()) {
                    join(*s, j);
                    continue;
                }
                Op& b = plan.ops[j];
                b.curves = curvesIn(plan.ops[i], b);
                b.plane = plan.ops[i].plane;
                b.sketchOf = int(i);
                sharers[i].push_back(j);
                merged[i] = merged[j] = 1;
                continue;
            }
            if (!commutes(plan.ops[j], plan.ops[i].cut)) break;
        }
    }
    for (size_t i = 0; i < n; ++i)
        for (size_t j : sharers[i]) plan.ops[i].shared.insert(plan.ops[i].shared.end(), plan.ops[j].curves.begin(), plan.ops[j].curves.end());
    for (size_t i = 0; i < n; ++i)
        if (!merged[i] && !gone[i]) plan.ops[i] = original[i];
        else if (spreadBy[i] >= 0) gone[size_t(spreadBy[i])] = 1;  // its points are in the sketch now
    if (std::none_of(gone.begin(), gone.end(), [](char c) { return c != 0; })) return;
    // Rebuild the lists: a block is the merged range of operations, its body is the last one's.
    std::vector<int> index(n, -1);
    std::vector<Op> ops;
    std::vector<kernel::Shape> after;
    std::vector<size_t> blockOf;
    size_t baseOps = 0;
    std::map<size_t, kernel::Shape> blockAfter;
    for (size_t i = 0; i < n; ++i) blockAfter[block[i]] = plan.after[i];  // the last operation of a block wins
    for (size_t i = 0; i < n; ++i) {
        if (gone[i]) continue;
        index[i] = int(ops.size());
        if (i < plan.baseOps) ++baseOps;
        ops.push_back(plan.ops[i]);
        after.push_back(blockAfter[block[i]]);
        blockOf.push_back(block[i]);
    }
    std::vector<size_t> resume(ops.size(), ops.size());
    for (size_t i = ops.size(); i-- > 0;) resume[i] = i + 1 < ops.size() && blockOf[i + 1] == blockOf[i] ? resume[i + 1] : i + 1;
    for (auto& o : ops) {
        if (o.target >= 0) o.target = index[size_t(o.target)];
        if (o.sketchOf >= 0) o.sketchOf = index[size_t(o.sketchOf)];
    }
    plan.ops = std::move(ops);
    plan.after = std::move(after);
    plan.resume = std::move(resume);
    plan.baseOps = baseOps;
    if (plan.baseOps > 0) plan.core = plan.after[0];  // a later cut may have joined the base
}

}  // namespace construe::rec::detail
