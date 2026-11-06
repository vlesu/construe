// Sketch offset as a linked group (spec/sketch.md §2.6.5, S7F-SK-005).
#include "sketch/offset.h"

#include "base/constants.h"
#include "base/geom2.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace construe::sketch {

namespace {

using geom2::cross2;
constexpr int ARC_SAMPLES = 16;  // chords per arc in the inside / area tests (§2.6.5)

// Offset source or contour curve: a line, an arc (counterclockwise a → b) or a circle.
struct Curve {
    enum Kind { Line, Arc, Circle };
    std::string id;
    Kind kind = Line;
    Point2 a = Point2::Zero(), b = Point2::Zero(), c = Point2::Zero();
    double r = 0;
    std::string pa, pb;  // end point IDs (none for a projection)
    bool construction = false;
};

double sweepOf(const Curve& k) {
    return geom2::ccwSweepPositive(geom2::angleOf(k.c, k.a), geom2::angleOf(k.c, k.b));
}

// Points along the curve in its own direction (a line — its ends, an arc — ARC_SAMPLES chords).
std::vector<Point2> samples(const Curve& k) {
    if (k.kind == Curve::Line) return {k.a, k.b};
    const double a0 = std::atan2((k.a - k.c).y(), (k.a - k.c).x());
    const double sw = k.kind == Curve::Circle ? TWO_PI : sweepOf(k);
    std::vector<Point2> v;
    for (int i = 0; i <= ARC_SAMPLES; ++i) v.push_back(k.c + k.r * Point2(std::cos(a0 + sw * i / ARC_SAMPLES), std::sin(a0 + sw * i / ARC_SAMPLES)));
    return v;
}

std::optional<Curve> curveOf(const SketchData& s, const std::string& id) {
    const Entity* e = s.findEntity(id);
    if (!e) return std::nullopt;
    auto pos = [&](const std::string& p) -> std::optional<Point2> {
        const Entity* q = s.findEntity(p);
        if (!q || q->type != EntityType::Point) return std::nullopt;
        return Point2(q->x, q->y);
    };
    Curve k;
    k.id = id;
    k.construction = e->construction;
    if (e->type == EntityType::Line) {
        const auto a = pos(e->p1), b = pos(e->p2);
        if (!a || !b) return std::nullopt;
        k.kind = Curve::Line, k.a = *a, k.b = *b, k.pa = e->p1, k.pb = e->p2;
    } else if (e->type == EntityType::Arc) {
        const auto c = pos(e->center), a = pos(e->start), b = pos(e->end);
        if (!c || !a || !b) return std::nullopt;
        k.kind = Curve::Arc, k.c = *c, k.a = *a, k.b = *b, k.r = (*a - *c).norm(), k.pa = e->start, k.pb = e->end;
    } else if (e->type == EntityType::Circle) {
        const auto c = pos(e->center);
        if (!c) return std::nullopt;
        k.kind = Curve::Circle, k.c = *c, k.r = e->r;
    } else if (e->type == EntityType::Projected) {
        const auto it = s.projections.find(id);
        if (it == s.projections.end()) return std::nullopt;
        const Projection& g = it->second;
        if (g.kind == Projection::Kind::Line) {
            k.kind = Curve::Line, k.a = Point2(g.x1, g.y1), k.b = Point2(g.x2, g.y2);
        } else if (g.kind == Projection::Kind::Arc) {
            k.kind = Curve::Arc, k.c = Point2(g.cx, g.cy), k.r = g.r, k.a = Point2(g.sx, g.sy), k.b = Point2(g.ex, g.ey);
        } else if (g.kind == Projection::Kind::Circle) {
            k.kind = Curve::Circle, k.c = Point2(g.cx, g.cy), k.r = g.r;
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (k.kind == Curve::Line && (k.b - k.a).norm() <= OFFSET_MIN) return std::nullopt;
    if (k.kind != Curve::Line && !(k.r > OFFSET_MIN)) return std::nullopt;
    return k;
}

// Joints of curve ends (§2.6.5): end 2k — the start of curve k, 2k+1 — its end; circles have no ends. node[end] —
// the joint representative, at[node] — the ends meeting there.
struct Joints {
    std::vector<int> node;
    std::map<int, std::vector<int>> at;
};

Joints joinEnds(const SketchData* s, const std::vector<Curve>& cs) {
    // Coincidence clusters of points (transitive).
    std::map<std::string, std::string> root;
    std::function<std::string(const std::string&)> find = [&](const std::string& x) -> std::string {
        auto it = root.find(x);
        if (it == root.end() || it->second == x) return x;
        return it->second = find(it->second);
    };
    if (s)
        for (const auto& k : s->constraints)
            if (k.type == "coincident" && k.refs.size() == 2) {
                const std::string a = find(k.refs[0]), b = find(k.refs[1]);
                root[a] = a, root[b] = b;
                if (a != b) root[a] = b;
            }
    const int n = int(cs.size()) * 2;
    std::vector<int> uf(n);
    for (int i = 0; i < n; ++i) uf[i] = i;
    std::function<int(int)> top = [&](int x) { return uf[x] == x ? x : uf[x] = top(uf[x]); };
    auto endPos = [&](int e) { return e % 2 ? cs[e / 2].b : cs[e / 2].a; };
    auto endId = [&](int e) { return e % 2 ? cs[e / 2].pb : cs[e / 2].pa; };
    for (int i = 0; i < n; ++i) {
        if (cs[i / 2].kind == Curve::Circle) continue;
        for (int j = i + 1; j < n; ++j) {
            if (cs[j / 2].kind == Curve::Circle) continue;
            const std::string pi = endId(i), pj = endId(j);
            const bool same = (!pi.empty() && !pj.empty() && find(pi) == find(pj)) || (endPos(i) - endPos(j)).norm() <= OFFSET_JOINT_TOL;
            if (same) uf[top(i)] = top(j);
        }
    }
    Joints out;
    out.node.resize(n);
    for (int i = 0; i < n; ++i) {
        out.node[i] = top(i);
        if (cs[i / 2].kind != Curve::Circle) out.at[out.node[i]].push_back(i);
    }
    return out;
}

// The other end at the joint of end e: -1 — none.
int otherEnd(const Joints& g, int e) {
    const auto it = g.at.find(g.node[e]);
    if (it == g.at.end()) return -1;
    for (int x : it->second)
        if (x != e) return x;
    return -1;
}

bool insidePolygon(const std::vector<Point2>& poly, const Point2& q) {
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y() > q.y()) != (poly[j].y() > q.y()) &&
            q.x() < (poly[j].x() - poly[i].x()) * (q.y() - poly[i].y()) / (poly[j].y() - poly[i].y()) + poly[i].x())
            inside = !inside;
    return inside;
}

// Polygon of a chain: items (curve, forward) in traversal order.
std::vector<Point2> chainPolygon(const std::vector<Curve>& cs, const std::vector<std::pair<size_t, bool>>& items) {
    std::vector<Point2> poly;
    for (const auto& [k, fwd] : items) {
        auto v = samples(cs[k]);
        if (!fwd) std::reverse(v.begin(), v.end());
        poly.insert(poly.end(), v.begin(), v.end());
    }
    return poly;
}

// Intersections of two offset curves (line — through a and b; arc/circle — its circle).
std::vector<Point2> intersections(const Curve& p, const Curve& q) {
    std::vector<Point2> out;
    if (p.kind == Curve::Line && q.kind == Curve::Line) {
        const Point2 u = p.b - p.a, v = q.b - q.a;
        const double den = cross2(u, v);
        if (std::abs(den) > 1e-12 * u.norm() * v.norm()) out.push_back(p.a + u * (cross2(q.a - p.a, v) / den));
        return out;
    }
    if (p.kind == Curve::Line || q.kind == Curve::Line) {
        const Curve& l = p.kind == Curve::Line ? p : q;
        const Curve& c = p.kind == Curve::Line ? q : p;
        const Point2 u = (l.b - l.a).normalized(), w = l.a - c.c;
        const double bq = w.dot(u), cq = w.squaredNorm() - c.r * c.r, disc = bq * bq - cq;
        if (disc < 0) return out;
        const double sq = std::sqrt(disc);
        out.push_back(l.a + u * (-bq - sq));
        out.push_back(l.a + u * (-bq + sq));
        return out;
    }
    const Point2 dc = q.c - p.c;
    const double dd = dc.norm();
    if (dd <= 1e-12 || dd > p.r + q.r || dd < std::abs(p.r - q.r)) return out;
    const double x = (dd * dd + p.r * p.r - q.r * q.r) / (2 * dd), h = std::sqrt(std::max(0.0, p.r * p.r - x * x));
    const Point2 e = dc / dd, m = p.c + e * x;
    out.push_back(m + geom2::perp(e) * h);
    out.push_back(m - geom2::perp(e) * h);
    return out;
}

struct Analysis {
    std::string error, errorId;
    std::vector<Curve> cs;
    OffsetChains chains;
};

Analysis analyze(const SketchData& s, const std::vector<std::string>& sources, const std::string& selfGroup) {
    Analysis an;
    auto fail = [&](const std::string& id) {
        an.error = "sources", an.errorId = id;
        an.chains.error = an.error, an.chains.errorId = id;
        return an;
    };
    if (sources.empty()) return fail("");
    const Group* self = selfGroup.empty() ? nullptr : s.findGroup(selfGroup);
    std::set<std::string> seen;
    for (const auto& id : sources) {
        const auto k = curveOf(s, id);
        if (!k || !seen.insert(id).second || (self && self->hasMember(id))) return fail(id);
        an.cs.push_back(*k);
    }
    const Joints g = joinEnds(&s, an.cs);
    for (const auto& [node, ends] : g.at)
        if (ends.size() > 2) return fail(an.cs[ends[2] / 2].id);
    const size_t n = an.cs.size();
    auto& out = an.chains;
    out.sigma.assign(n, 0);
    std::vector<bool> visited(n, false);
    for (size_t i = 0; i < n; ++i) {
        if (visited[i]) continue;
        visited[i] = true;
        if (an.cs[i].kind == Curve::Circle) {  // a closed chain by itself; "left" of a circle is inward
            out.sigma[i] = -1;
            continue;
        }
        std::vector<std::pair<size_t, bool>> items{{i, true}};
        bool closed = false;
        // Forward from the end of source i.
        size_t cur = i;
        bool fwd = true;
        for (size_t guard = 0; guard <= n; ++guard) {
            const int exitEnd = int(2 * cur + (fwd ? 1 : 0));
            const int other = otherEnd(g, exitEnd);
            if (other < 0) break;
            const size_t j = size_t(other / 2);
            const bool jEnd = other % 2 == 1;
            if (j != i && visited[j]) break;
            out.joints.push_back({cur, j, fwd, jEnd});
            if (j == i) {
                closed = true;
                break;
            }
            visited[j] = true;
            items.push_back({j, !jEnd});
            cur = j, fwd = !jEnd;
        }
        if (!closed) {  // backward from the start of source i
            cur = i, fwd = true;
            for (size_t guard = 0; guard <= n; ++guard) {
                const int exitEnd = int(2 * cur + (fwd ? 0 : 1));
                const int other = otherEnd(g, exitEnd);
                if (other < 0) break;
                const size_t j = size_t(other / 2);
                const bool jEnd = other % 2 == 1;
                if (visited[j]) break;
                out.joints.push_back({cur, j, !fwd, jEnd});
                visited[j] = true;
                items.insert(items.begin(), {j, jEnd});
                cur = j, fwd = jEnd;
            }
        }
        int sChain = 1;  // left of the traversal
        if (closed) {
            const Curve& k = an.cs[i];
            Point2 mid, left;
            double scale = 1;
            if (k.kind == Curve::Line) {
                mid = (k.a + k.b) / 2, left = geom2::perp(k.b - k.a).normalized(), scale = (k.b - k.a).norm();
            } else {
                const double a0 = std::atan2((k.a - k.c).y(), (k.a - k.c).x()) + sweepOf(k) / 2;
                mid = k.c + k.r * Point2(std::cos(a0), std::sin(a0)), left = (k.c - mid).normalized(), scale = k.r;
            }
            // Outward: the left of the traversal is inside — offset to the right.
            if (insidePolygon(chainPolygon(an.cs, items), mid + left * (1e-4 * std::max(1.0, scale)))) sChain = -1;
        }
        for (const auto& [k, f] : items) out.sigma[k] = sChain * (f ? 1 : -1);
    }
    return an;
}

}  // namespace

OffsetChains offsetChains(const SketchData& s, const std::vector<std::string>& sources, const std::string& selfGroup) {
    return analyze(s, sources, selfGroup).chains;
}

int offsetSign(const SketchData& s, const std::vector<std::string>& sources, const std::string& curve, int side) {
    const Analysis an = analyze(s, sources, {});
    if (!an.error.empty()) return 0;
    for (size_t k = 0; k < an.cs.size(); ++k)
        if (an.cs[k].id == curve) return side * an.chains.sigma[k] >= 0 ? 1 : -1;
    return 0;
}

OffsetItems offsetItems(const SketchData& s, const std::vector<std::string>& sources, double distance, const std::string& valueText,
                        const std::string& selfGroup) {
    OffsetItems out;
    const Analysis an = analyze(s, sources, selfGroup);
    if (!an.error.empty()) {
        out.error = an.error, out.errorId = an.errorId;
        return out;
    }
    if (!(std::abs(distance) > OFFSET_MIN)) {
        out.error = "distance";
        return out;
    }
    const double d = std::abs(distance);
    const int sg = distance > 0 ? 1 : -1;
    const size_t n = an.cs.size();
    std::vector<Curve> off = an.cs;
    std::vector<int> side(n);
    for (size_t k = 0; k < n; ++k) {
        side[k] = an.chains.sigma[k] * sg;
        Curve& o = off[k];
        if (o.kind == Curve::Line) {
            const Point2 nl = geom2::perp(o.b - o.a).normalized() * (side[k] * d);
            o.a += nl, o.b += nl;
        } else {
            o.r = an.cs[k].r - side[k] * d;  // left of an arc / a circle is towards the center
            if (!(o.r > OFFSET_MIN)) {
                out.error = "distance";
                return out;
            }
            if (o.kind == Curve::Arc) {
                o.a = o.c + (an.cs[k].a - o.c).normalized() * o.r;
                o.b = o.c + (an.cs[k].b - o.c).normalized() * o.r;
            }
        }
    }
    // Tangent joints (FX12-SK-001): the offset ends meet already; a joint of a curve with an arc carries the offset
    // across it, so of a run of curves joined by such joints only the first one gets the distance dimension.
    std::vector<bool> tangentJoint(an.chains.joints.size(), false);
    for (size_t k = 0; k < an.chains.joints.size(); ++k) {
        const auto& jt = an.chains.joints[k];
        Point2& q1 = jt.iEnd ? off[jt.i].b : off[jt.i].a;
        Point2& q2 = jt.jEnd ? off[jt.j].b : off[jt.j].a;
        const Point2 m = (q1 + q2) / 2;
        if ((q1 - q2).norm() <= OFFSET_JOINT_TOL) {  // tangent joint — the offset ends meet already
            q1 = q2 = m;
            tangentJoint[k] = off[jt.i].kind == Curve::Arc || off[jt.j].kind == Curve::Arc;
            continue;
        }
        Point2 best = m;
        double bd = 1e300;
        for (const Point2& x : intersections(off[jt.i], off[jt.j]))
            if ((x - m).norm() < bd) bd = (x - m).norm(), best = x;
        q1 = q2 = best;
    }
    auto& items = out.items;
    auto point = [&](const std::string& role, const Point2& p) {
        Entity e;
        e.id = "@" + role, e.type = EntityType::Point, e.x = p.x(), e.y = p.y();
        items.push_back({role, e, std::nullopt});
    };
    auto con = [&](const std::string& role, const std::string& type, std::vector<std::string> refs) -> Constraint& {
        Constraint c;
        c.id = "@" + role, c.type = type, c.refs = std::move(refs);
        items.push_back({role, std::nullopt, c});
        return *items.back().constraint;
    };
    auto endRole = [&](size_t k, bool end) {
        const std::string seg = "seg" + std::to_string(k);
        return an.cs[k].kind == Curve::Line ? seg + (end ? "/p2" : "/p1") : seg + (end ? "/end" : "/start");
    };
    // Runs of curves joined by tangent joints (a joint closing a run into a loop carries nothing new and gets no
    // tangency); each run's dimension sits on its first curve in the order of the sources.
    std::vector<size_t> run(n);
    for (size_t k = 0; k < n; ++k) run[k] = k;
    std::function<size_t(size_t)> root = [&](size_t k) { return run[k] == k ? k : run[k] = root(run[k]); };
    std::vector<bool> tangency(an.chains.joints.size(), false);
    for (size_t k = 0; k < an.chains.joints.size(); ++k) {
        if (!tangentJoint[k]) continue;
        const size_t a = root(an.chains.joints[k].i), b = root(an.chains.joints[k].j);
        if (a == b) continue;
        run[std::max(a, b)] = std::min(a, b);
        tangency[k] = true;
    }
    for (size_t k = 0; k < n; ++k) {
        const std::string seg = "seg" + std::to_string(k);
        const Curve& o = off[k];
        Entity e;
        e.id = "@" + seg;
        std::string dimPoint;
        if (o.kind == Curve::Line) {
            point(seg + "/p1", o.a), point(seg + "/p2", o.b);
            e.type = EntityType::Line, e.p1 = "@" + seg + "/p1", e.p2 = "@" + seg + "/p2";
            dimPoint = seg + "/p1";
        } else if (o.kind == Curve::Arc) {
            point(seg + "/center", o.c), point(seg + "/start", o.a), point(seg + "/end", o.b);
            e.type = EntityType::Arc, e.center = "@" + seg + "/center", e.start = "@" + seg + "/start", e.end = "@" + seg + "/end";
            dimPoint = seg + "/start";
        } else {
            point(seg + "/center", o.c);
            e.type = EntityType::Circle, e.center = "@" + seg + "/center", e.r = o.r;
        }
        items.push_back({seg, e, std::nullopt});
        if (o.kind == Curve::Circle) {
            point(seg + "/on", o.c + Point2(o.r, 0));
            dimPoint = seg + "/on";
        }
        con("rel" + std::to_string(k), o.kind == Curve::Line ? "parallel" : "concentric", {an.cs[k].id, "@" + seg});
        if (o.kind == Curve::Circle) con("on" + std::to_string(k), "pointOnObject", {"@" + dimPoint, "@" + seg});
        if (root(k) != k) continue;  // the offset comes through a tangent joint of its run (FX12-SK-001)
        Constraint& dim = con("dim" + std::to_string(k), "distance", {"@" + dimPoint, an.cs[k].id});
        dim.value = valueText;
        dim.driving = true;
        Branch b;
        b.kind = Branch::Kind::Side;
        if (o.kind == Curve::Line) {
            b.side = side[k];
        } else {
            b.side = o.r > an.cs[k].r ? 1 : -1;  // outside the source
            dim.arc = "near";
        }
        dim.branch = b;
    }
    for (size_t k = 0; k < an.chains.joints.size(); ++k) {
        const auto& jt = an.chains.joints[k];
        con("join" + std::to_string(k), "coincident", {"@" + endRole(jt.i, jt.iEnd), "@" + endRole(jt.j, jt.jEnd)});
        if (!tangency[k]) continue;
        // Tangency at the shared end (sketch.md §2.6.5): pins the joint to the touch point of the two offset curves; a
        // segment goes first.
        const bool lineFirst = off[jt.i].kind == Curve::Line || off[jt.j].kind != Curve::Line;
        const size_t f = lineFirst ? jt.i : jt.j, g = lineFirst ? jt.j : jt.i;
        Constraint& tc = con("tan" + std::to_string(k), "tangent", {"@seg" + std::to_string(f), "@seg" + std::to_string(g)});
        // The branch the schema asks for (sketch.md §2.2) by the generated geometry: a segment — the side of the arc
        // centre (left of p1 → p2 — +1); two arcs — outer touch or which one is inside.
        Branch tb;
        const Curve &cf = off[f], &cg = off[g];
        if (cf.kind == Curve::Line) {
            tb.kind = Branch::Kind::Side;
            tb.side = cross2(cf.b - cf.a, cg.c - cf.a) >= 0 ? 1 : -1;
        } else {
            tb.kind = Branch::Kind::Contact;
            const double dc = (cf.c - cg.c).norm();
            tb.contact = std::abs(dc - (cf.r + cg.r)) <= std::abs(dc - std::abs(cf.r - cg.r)) ? "External" : cf.r >= cg.r ? "Internal1" : "Internal2";
        }
        tc.branch = tb;
    }
    return out;
}

bool derivedOffsetDimension(const SketchData& s, const std::string& constraintId) {
    const Group* g = s.groupOfMember(constraintId);
    if (!g || g->type != "offset") return false;
    for (const auto& m : g->members)
        if (m.id == constraintId) return m.role.rfind("dim", 0) == 0 && m.role != "dim0";
    return false;
}

std::vector<std::string> contourOf(const SketchData& s, const std::string& curve) {
    const auto c0 = curveOf(s, curve);
    if (!c0) return {};
    if (c0->kind == Curve::Circle) return {curve};
    std::vector<Curve> cs;
    size_t start = 0;
    for (const auto& e : s.entities) {
        if (e.type != EntityType::Line && e.type != EntityType::Arc && e.type != EntityType::Projected) continue;
        const auto k = curveOf(s, e.id);
        if (!k || k->kind == Curve::Circle || k->construction != c0->construction) continue;
        if (e.id == curve) start = cs.size();
        cs.push_back(*k);
    }
    const Joints g = joinEnds(&s, cs);
    std::vector<std::string> out{curve};
    size_t cur = start;
    bool fwd = true;
    for (size_t guard = 0; guard <= cs.size(); ++guard) {
        const int exitEnd = int(2 * cur + (fwd ? 1 : 0));
        if (g.at.at(g.node[exitEnd]).size() != 2) return {};
        const int other = otherEnd(g, exitEnd);
        const size_t j = size_t(other / 2);
        if (j == start) return other % 2 == 0 ? out : std::vector<std::string>{};
        if (std::find(out.begin(), out.end(), cs[j].id) != out.end()) return {};
        out.push_back(cs[j].id);
        cur = j, fwd = other % 2 == 0;
    }
    return {};
}

std::vector<std::string> edgeContour(const std::vector<std::pair<std::string, Projection>>& curves, const std::string& start) {
    // 1. Curves (lines, arcs, circles); coinciding ones are one curve (the start one wins, otherwise the first).
    auto near = [](const Point2& a, const Point2& b) { return (a - b).norm() <= OFFSET_JOINT_TOL; };
    auto same = [&](const Curve& p, const Curve& q) {
        if (p.kind != q.kind) return false;
        if (p.kind == Curve::Line) return (near(p.a, q.a) && near(p.b, q.b)) || (near(p.a, q.b) && near(p.b, q.a));
        if (!near(p.c, q.c) || std::abs(p.r - q.r) > OFFSET_JOINT_TOL) return false;
        return p.kind == Curve::Circle || (near(p.a, q.a) && near(p.b, q.b));
    };
    std::vector<Curve> cs;
    for (const auto& [name, g] : curves) {
        SketchData one;
        Entity e;
        e.id = name, e.type = EntityType::Projected;
        one.entities.push_back(e);
        one.projections[name] = g;
        const auto k = curveOf(one, name);
        if (!k) continue;
        auto dup = std::find_if(cs.begin(), cs.end(), [&](const Curve& x) { return same(x, *k); });
        if (dup == cs.end()) cs.push_back(*k);
        else if (name == start) *dup = *k;
    }
    // 2. The start curve.
    size_t s0 = cs.size();
    for (size_t k = 0; k < cs.size(); ++k)
        if (cs[k].id == start) s0 = k;
    if (s0 == cs.size()) return {};
    if (cs[s0].kind == Curve::Circle) return {start};
    // 3. Face tracing of the planar graph, to the left and to the right.
    const Joints g = joinEnds(nullptr, cs);
    // Tangent of curve k in the traversal direction (fwd — its own) at its traversal start (atEnd = false) or end.
    auto tangent = [&](size_t k, bool fwd, bool atEnd) -> Point2 {
        const Curve& c = cs[k];
        Point2 t;
        if (c.kind == Curve::Line) {
            t = (c.b - c.a).normalized();
        } else {
            const bool atB = fwd == atEnd;  // the point: b — the traversal end going forward, the start going backward
            t = geom2::perp(((atB ? c.b : c.a) - c.c).normalized());
        }
        return fwd ? t : Point2(-t);
    };
    struct Trace {
        bool ok = false;
        std::vector<std::pair<size_t, bool>> items;
    };
    auto trace = [&](bool left) {
        Trace tr;
        tr.items = {{s0, true}};
        std::set<size_t> used{s0};
        for (size_t guard = 0; guard <= cs.size(); ++guard) {
            const auto [k, fwd] = tr.items.back();
            const int exitEnd = int(2 * k + (fwd ? 1 : 0));
            const Point2 t = tangent(k, fwd, true);
            int best = -1;
            double ba = 0;
            for (int e : g.at.at(g.node[exitEnd])) {
                if (e / 2 == int(k)) continue;
                const bool f = e % 2 == 0;
                const Point2 t2 = tangent(size_t(e / 2), f, false);
                const double ang = std::atan2(cross2(t, t2), t.dot(t2));
                if (best < 0 || (left ? ang > ba : ang < ba)) best = e, ba = ang;
            }
            if (best < 0) return tr;
            const size_t j = size_t(best / 2);
            const bool f = best % 2 == 0;
            if (j == s0) {
                tr.ok = f;
                return tr;
            }
            if (!used.insert(j).second) return tr;
            tr.items.push_back({j, f});
        }
        return tr;
    };
    auto area = [&](const Trace& tr) {
        const auto poly = chainPolygon(cs, tr.items);
        double a = 0;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) a += cross2(poly[j], poly[i]);
        return std::abs(a) / 2;
    };
    const Trace l = trace(true), r = trace(false);
    const Trace* pick = nullptr;
    if (l.ok && r.ok) pick = area(r) > area(l) * (1 + 1e-9) ? &r : &l;
    else if (l.ok) pick = &l;
    else if (r.ok) pick = &r;
    if (!pick) return {};
    std::vector<std::string> out;
    for (const auto& [k, f] : pick->items) out.push_back(cs[k].id);
    return out;
}

}  // namespace construe::sketch
