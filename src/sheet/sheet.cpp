// Sheet model: formulas, folding, regions and feature builders (spec/sheet.md §2–§4.6).
#include "sheet/sheet.h"

#include "base/geom2.h"
#include "base/ids.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace construe::sheet {

namespace {

using geom2::angleOf;
using geom2::cross2;
using geom2::perp;
Point2 xy(const Vec3& p) { return {p.x(), p.y()}; }
Vec3 lift(const Point2& p, double z) { return {p.x(), p.y(), z}; }

std::string label(const std::string& feature, const std::string& tag) { return feature + ".sheet(" + tag + ")"; }
std::string labelK(const std::string& feature, const std::string& tag, int k) { return label(feature, tag + std::to_string(k)); }

// Counterclockwise sweep of an arc from a to b about c, in (0, 2π] (by the difference of direction angles).
double ccwSweep(const Point2& c, const Point2& a, const Point2& b) {
    return geom2::ccwSweepPositive(angleOf(a - c), angleOf(b - c));
}
// Point q (on the arc's circle) within the half-open angular range [start, end) of the counterclockwise arc.
bool onArc(const Seg& s, const Point2& q) {
    if (s.kind == Seg::Kind::Circle) return true;
    const double sweep = ccwSweep(s.c, s.a, s.b);
    const double d = geom2::ccwSweep(angleOf(s.a - s.c), angleOf(q - s.c));
    return d < sweep;
}

double distToLine(const Point2& a, const Point2& b, const Point2& p) { return geom2::distToSegment(p, a, b); }

// Polyline of a spline segment (§2.2): vertices at the flattening parameters, natural direction.
std::vector<Point2> splinePolyline(const Seg& s) {
    const spline::Flat f = spline::expand(s.curve);
    std::vector<Point2> out;
    for (double t : spline::flattenParams(f, SHEET_SPLINE_FLAT_TOL)) out.push_back(spline::eval(f, t));
    return out;
}

double distToSeg(const Seg& s, const Point2& p) {
    if (s.kind == Seg::Kind::Line) return distToLine(s.a, s.b, p);
    if (s.kind == Seg::Kind::Spline) {
        const auto pts = splinePolyline(s);
        double d = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i + 1 < pts.size(); ++i) d = std::min(d, distToLine(pts[i], pts[i + 1], p));
        return d;
    }
    const double r = (p - s.c).norm();
    if (s.kind == Seg::Kind::Circle) return std::abs(r - s.r);
    if (r > 0 && onArc(s, s.c + (p - s.c) * (s.r / r))) return std::abs(r - s.r);
    return std::min((p - s.a).norm(), (p - s.b).norm());
}

template <class Fn>
void forEachSeg(const Region& r, Fn fn) {
    for (const auto& s : r.outer) fn(s);
    for (const auto& h : r.holes)
        for (const auto& s : h) fn(s);
}

// Bounding box of a region (arcs — by their circles).
Eigen::AlignedBox2d regionBox(const Region& r) {
    Eigen::AlignedBox2d b;
    forEachSeg(r, [&](const Seg& s) {
        if (s.kind == Seg::Kind::Line) {
            b.extend(s.a), b.extend(s.b);
        } else if (s.kind == Seg::Kind::Spline) {
            for (const auto& q : s.curve.poles) b.extend(q);  // the curve lies in the convex hull of its poles
        } else {
            b.extend(Point2(s.c - Vec2(s.r, s.r))), b.extend(Point2(s.c + Vec2(s.r, s.r)));
        }
    });
    return b;
}

// Unit vector of the arc start ordering: the counterclockwise arc from a to b passing through m.
Seg arcThrough(const Point2& c, double r, const Point2& a, const Point2& b, const Point2& m, const std::string& lab) {
    Seg s;
    s.kind = Seg::Kind::Arc;
    s.c = c;
    s.r = r;
    if (cross2(m - a, b - a) > 0) s.a = a, s.b = b;
    else s.a = b, s.b = a, s.rev = true;
    s.label = lab;
    return s;
}

Seg line(const Point2& a, const Point2& b, const std::string& lab) {
    Seg s;
    s.kind = Seg::Kind::Line;
    s.a = a;
    s.b = b;
    s.label = lab;
    return s;
}

Error profileError(const char* detail) { return Error("SHEET_PROFILE_INVALID").with("detail", std::string(detail)); }

}  // namespace

// ---------------------------------------------------------------- formulas (§4.1, §4.2)

double bendAllowance(double angle, double radius, const Params& p) { return angle * (radius + p.kFactor * p.thickness); }

double setback(Position pos, double angle, double radius, double thickness) {
    switch (pos) {
        case Position::Inside: return (radius + thickness) * std::tan(angle / 2);
        case Position::Outside: return radius * std::tan(angle / 2);
        case Position::Edge: return 0;
    }
    return 0;
}

Transform3 bendMotion(const Piece& b, double T) {
    const Vec3 out3(b.out.x(), b.out.y(), 0);
    const Vec3 u = out3.cross(Vec3::UnitZ());
    const Vec3 q(b.origin.x(), b.origin.y(), b.up ? T + b.radius : -b.radius);
    Transform3 M = Transform3::Identity();
    M.translate(q);
    M.rotate(Eigen::AngleAxisd((b.up ? 1.0 : -1.0) * b.angle, u));
    M.translate(-q);
    M.translate(-b.ba * out3);
    return M;
}

Transform3 fold(const Model& m, int piece) {
    std::vector<int> chain;
    int p = m.pieces.at(size_t(piece)).kind == Piece::Kind::Bend ? m.pieces[size_t(piece)].parent : piece;
    while (p >= 0) {
        if (m.pieces[size_t(p)].kind == Piece::Kind::Bend) chain.push_back(p);
        p = m.pieces[size_t(p)].parent;
    }
    Transform3 F = m.root;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) F = F * bendMotion(m.pieces[size_t(*it)], m.params.thickness);
    return F;
}

Region bendRect(const Point2& origin, const Vec2& axis, const Vec2& out, double w0, double w1, double s0, double s1,
                const std::string& atS0, const std::string& atS1, const std::string& atW0, const std::string& atW1) {
    auto P = [&](double w, double s) { return Point2(origin + w * axis + s * out); };
    Region r;
    r.outer = {line(P(w0, s0), P(w1, s0), atS0), line(P(w1, s0), P(w1, s1), atW1), line(P(w1, s1), P(w0, s1), atS1),
               line(P(w0, s1), P(w0, s0), atW0)};
    return r;
}

// ---------------------------------------------------------------- regions

std::optional<Region> regionFromProfile(const kernel::Profile& p, const Transform3& toFlat,
                                        const std::function<std::string(const std::string&)>& lab) {
    const bool flip = (toFlat.linear() * p.plane.normal).z() < 0;
    auto loopOf = [&](const kernel::Loop3& l, Loop& out) {
        for (const auto& s : l.segments) {
            Seg g;
            g.label = lab(s.id);
            switch (s.kind) {
                case kernel::Segment3::Kind::Line:
                    g.kind = Seg::Kind::Line;
                    g.a = xy(toFlat * (s.reversed ? s.p2 : s.p1));
                    g.b = xy(toFlat * (s.reversed ? s.p1 : s.p2));
                    break;
                case kernel::Segment3::Kind::Arc:
                    g.kind = Seg::Kind::Arc;
                    g.c = xy(toFlat * s.center);
                    g.r = s.radius;
                    g.a = xy(toFlat * (flip ? s.end : s.start));
                    g.b = xy(toFlat * (flip ? s.start : s.end));
                    g.rev = s.reversed != flip;
                    break;
                case kernel::Segment3::Kind::Circle:
                    g.kind = Seg::Kind::Circle;
                    g.c = xy(toFlat * s.center);
                    g.r = s.radius;
                    break;
                case kernel::Segment3::Kind::Spline:
                    g.kind = Seg::Kind::Spline;
                    for (const auto& q : s.poles) g.curve.poles.push_back(xy(toFlat * q));
                    g.curve.weights = s.weights, g.curve.knots = s.knots, g.curve.mults = s.mults, g.curve.degree = s.degree;
                    g.a = g.curve.poles.front(), g.b = g.curve.poles.back();
                    g.rev = s.reversed;
                    break;
                default: return false;
            }
            out.push_back(g);
        }
        return true;
    };
    Region r;
    if (!loopOf(p.outer, r.outer)) return std::nullopt;
    for (const auto& h : p.holes) {
        r.holes.emplace_back();
        if (!loopOf(h, r.holes.back())) return std::nullopt;
    }
    return r;
}

std::vector<double> lineCrossings(const Region& r, const Point2& o, const Vec2& dir) {
    std::vector<double> out;
    const Vec2 n = perp(dir);
    auto chord = [&](const Point2& a, const Point2& b) {
        const double da = (a - o).dot(n), db = (b - o).dot(n);
        if ((da > 0) == (db > 0) || da == db) return;  // half-open by side: each vertex counted once
        const double t = da / (da - db);
        out.push_back((a + t * (b - a) - o).dot(dir));
    };
    forEachSeg(r, [&](const Seg& s) {
        if (s.kind == Seg::Kind::Line) return chord(s.a, s.b);
        if (s.kind == Seg::Kind::Spline) {
            const auto pts = splinePolyline(s);
            for (size_t i = 0; i + 1 < pts.size(); ++i) chord(pts[i], pts[i + 1]);
            return;
        }
        const double h = (s.c - o).dot(n);  // distance from the center to the line
        if (std::abs(h) >= s.r) return;
        const double w = (s.c - o).dot(dir), dw = std::sqrt(s.r * s.r - h * h);
        for (double x : {w - dw, w + dw}) {
            const Point2 q = o + x * dir;
            if (onArc(s, q)) out.push_back(x);
        }
    });
    std::sort(out.begin(), out.end());
    return out;
}

bool insideRegion(const Region& r, const Point2& p, double tol) {
    bool onBoundary = false;
    forEachSeg(r, [&](const Seg& s) { onBoundary = onBoundary || distToSeg(s, p) <= tol; });
    if (onBoundary) return true;
    int n = 0;
    for (double w : lineCrossings(r, p, Vec2::UnitX()))
        if (w > 0) ++n;
    return n % 2 == 1;
}

bool insidePiece(const Piece& piece, const Point2& p, double tol) {
    bool in = insideRegion(piece.region, p, tol);
    for (const auto& op : piece.ops) {
        switch (op.kind) {
            case PieceOp::Kind::Subtract: in = in && !insideRegion(op.region, p, -1); break;
            case PieceOp::Kind::Intersect: in = in && insideRegion(op.region, p, tol); break;
            case PieceOp::Kind::Add:
                if (!in && insideRegion(op.region, p, tol)) {
                    in = true;
                    for (const auto& m : op.minus) in = in && !insideRegion(m, p, -1);
                }
                break;
        }
    }
    return in;
}

// ---------------------------------------------------------------- base wall (§4.3)

Status addPlate(Model& m, const kernel::Profile& profile, const Plane& frame, bool reverse, const std::string& F) {
    Transform3 root = Transform3::Identity();
    root.linear().col(0) = frame.x;
    root.linear().col(1) = reverse ? Vec3(-frame.y) : frame.y;
    root.linear().col(2) = reverse ? Vec3(-frame.normal) : frame.normal;
    root.translation() = frame.origin;
    auto region = regionFromProfile(profile, root.inverse(), [&](const std::string& id) { return F + ".side(" + id + ")"; });
    if (!region) return profileError("ellipse");
    m.root = root;
    Piece w;
    w.kind = Piece::Kind::Wall;
    w.feature = F;
    w.region = std::move(*region);
    w.faceA = F + ".cap(start)";
    w.faceB = F + ".cap(end)";
    m.pieces.push_back(std::move(w));
    return okStatus();
}

Result<std::vector<ChainElem>> orderChain(const std::vector<ChainCurve>& curves) {
    if (curves.empty()) return profileError("empty");
    // Ends: 2·i — curve i at a, 2·i + 1 — at b; clusters of coincident ends.
    const size_t n = curves.size();
    auto endPt = [&](size_t e) { return e % 2 == 0 ? curves[e / 2].a : curves[e / 2].b; };
    std::vector<int> cluster(2 * n, -1);
    std::vector<std::vector<size_t>> members;
    for (size_t e = 0; e < 2 * n; ++e) {
        for (size_t c = 0; c < members.size() && cluster[e] < 0; ++c)
            if ((endPt(members[c][0]) - endPt(e)).norm() <= SHEET_JOIN_TOL) cluster[e] = int(c), members[c].push_back(e);
        if (cluster[e] < 0) cluster[e] = int(members.size()), members.push_back({e});
    }
    std::vector<size_t> free;
    for (const auto& c : members) {
        if (c.size() > 2) return profileError("branch");
        if (c.size() == 1) free.push_back(c[0]);
    }
    if (free.empty()) return profileError("closed");
    if (free.size() != 2) return profileError("branch");
    // Start: the free end of the curve with the smaller ID; one curve — its end a.
    size_t start = free[0];
    const size_t c0 = free[0] / 2, c1 = free[1] / 2;
    if (c0 == c1) start = std::min(free[0], free[1]);
    else if (compareIds(curves[c1].id, curves[c0].id) < 0) start = free[1];
    std::vector<ChainElem> out;
    std::vector<bool> used(n, false);
    size_t e = start;
    while (true) {
        const size_t ci = e / 2;
        used[ci] = true;
        const ChainCurve& cc = curves[ci];
        const bool fwd = e % 2 == 0;
        ChainElem el;
        el.id = cc.id;
        el.arc = cc.arc;
        el.a = fwd ? cc.a : cc.b;
        el.b = fwd ? cc.b : cc.a;
        el.c = cc.c;
        el.r = cc.r;
        if (cc.arc) el.sweep = (fwd ? 1.0 : -1.0) * ccwSweep(cc.c, cc.a, cc.b);
        out.push_back(el);
        const size_t other = fwd ? e + 1 : e - 1;
        const auto& cl = members[size_t(cluster[other])];
        if (cl.size() == 1) break;
        const size_t next = cl[0] == other ? cl[1] : cl[0];
        if (used[next / 2]) return profileError("branch");
        e = next;
    }
    if (out.size() != n) return profileError("branch");
    return out;
}

Status addOpenWall(Model& m, const std::vector<ChainElem>& chain, const Plane& frame, Side side, double width, bool reverse,
                   const std::string& F) {
    const Params& P = m.params;
    const double T = P.thickness;
    const size_t n = chain.size();
    if (n == 0) return profileError("empty");
    if (chain.front().arc || chain.back().arc) return profileError("arc");
    auto dirOf = [](const ChainElem& e) { return Vec2((e.b - e.a).normalized()); };
    auto tangent = [](const ChainElem& e, const Point2& p) {
        const Vec2 t = perp((p - e.c) / e.r);
        return e.sweep > 0 ? t : Vec2(-t);
    };
    auto angleBetween = [](const Vec2& a, const Vec2& b) { return std::atan2(cross2(a, b), a.dot(b)); };
    // Bends: after line i (corner) or at arc i; turn φ.
    struct Bend {
        size_t at;  // element index (line before the corner, or the arc)
        bool corner;
        double phi;
    };
    std::vector<Bend> bends;
    for (size_t i = 0; i + 1 < n; ++i) {
        const ChainElem& e = chain[i];
        const ChainElem& nx = chain[i + 1];
        if (!e.arc && !nx.arc) {
            const double phi = angleBetween(dirOf(e), dirOf(nx));
            if (std::abs(phi) <= SHEET_ANGLE_TOL || std::abs(phi) >= PI - SHEET_ANGLE_TOL) return profileError("angle");
            bends.push_back({i, true, phi});
        } else if (nx.arc) {
            if (e.arc || i + 2 >= n || chain[i + 2].arc) return profileError("arc");
            const ChainElem& after = chain[i + 2];
            if (std::abs(angleBetween(dirOf(e), tangent(nx, nx.a))) > SHEET_TANGENT_TOL ||
                std::abs(angleBetween(tangent(nx, nx.b), dirOf(after))) > SHEET_TANGENT_TOL)
                return profileError("arc");
            if (std::abs(nx.sweep) >= PI - SHEET_ANGLE_TOL) return profileError("angle");
            bends.push_back({i + 1, false, nx.sweep});
        }
    }
    double sum = 0;
    for (const auto& b : bends) sum += b.phi;
    const bool left = side == Side::Inside ? sum >= 0 : side == Side::Outside ? sum < 0 : true;
    const double zs = side == Side::Mid ? T / 2 : 0;
    // Bend radii and setbacks.
    struct BendGeom {
        bool up;
        double r, sb, theta;
    };
    std::vector<BendGeom> geom;
    for (const auto& b : bends) {
        BendGeom g;
        g.up = (b.phi > 0) == left;
        g.theta = std::abs(b.phi);
        if (b.corner) {
            g.r = P.radius;
            const double rs = g.up ? g.r + (T - zs) : g.r + zs;
            g.sb = rs * std::tan(g.theta / 2);
        } else {
            const double rho = chain[b.at].r;
            g.r = g.up ? rho - (T - zs) : rho - zs;
            if (g.r < -1e-12) return profileError("radius");
            g.r = std::max(g.r, 0.0);
            g.sb = 0;
        }
        geom.push_back(g);
    }
    // Wall lengths of lines.
    std::vector<double> length(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (chain[i].arc) continue;
        double L = (chain[i].b - chain[i].a).norm();
        for (size_t k = 0; k < bends.size(); ++k)
            if (bends[k].corner && (bends[k].at == i || bends[k].at + 1 == i)) L -= geom[k].sb;
        if (L < SHEET_MIN_LENGTH) return profileError("short");
        length[i] = L;
    }
    // Root fold (§4.3 item 6).
    const Vec2 d0 = dirOf(chain[0]);
    const Vec3 X = frame.x * d0.x() + frame.y * d0.y();
    const Vec2 l2 = perp(d0);
    const Vec3 Lw = frame.x * l2.x() + frame.y * l2.y();
    const Vec3 Z = left ? Lw : Vec3(-Lw);
    const Vec3 Y = Z.cross(X);
    const Vec3 Wd = reverse ? Vec3(-frame.normal) : frame.normal;
    const double y0 = Y.dot(Wd) > 0 ? 0 : -width;
    Transform3 root = Transform3::Identity();
    root.linear().col(0) = X;
    root.linear().col(1) = Y;
    root.linear().col(2) = Z;
    root.translation() = frame.toWorld(chain[0].a) - zs * Z;
    m.root = root;
    const std::string capLo = F + (y0 == 0 ? ".cap(start)" : ".cap(end)"), capHi = F + (y0 == 0 ? ".cap(end)" : ".cap(start)");
    double x = 0;
    int prev = -1;
    size_t bi = 0;
    for (size_t i = 0; i < n; ++i) {
        const ChainElem& e = chain[i];
        if (!e.arc) {
            Piece w;
            w.kind = Piece::Kind::Wall;
            w.feature = F;
            w.parent = prev;
            const std::string sLab = label(F, "s," + (i == 0 ? e.id : e.id)), tLab = label(F, "t," + e.id);
            w.region = bendRect(Point2(x, 0), Vec2::UnitY(), Vec2::UnitX(), y0, y0 + width, 0, length[i], sLab, tLab, capLo, capHi);
            w.faceA = label(F, "a," + e.id);
            w.faceB = label(F, "b," + e.id);
            x += length[i];
            m.pieces.push_back(std::move(w));
            prev = int(m.pieces.size()) - 1;
        }
        if (bi < bends.size() && bends[bi].at == i) {
            const BendGeom& g = geom[bi];
            Piece b;
            b.kind = Piece::Kind::Bend;
            b.feature = F;
            b.parent = prev;
            b.origin = Point2(x, 0);
            b.axis = Vec2::UnitY();
            b.out = Vec2::UnitX();
            b.w0 = y0;
            b.w1 = y0 + width;
            b.angle = g.theta;
            b.radius = g.r;
            b.up = g.up;
            b.ba = bendAllowance(g.theta, g.r, P);
            b.faceIn = label(F, "bi," + e.id);
            b.faceOut = label(F, "bo," + e.id);
            b.faceL = capLo;
            b.faceR = capHi;
            b.faceS = label(F, "bs," + e.id);
            b.faceE = label(F, "be," + e.id);
            b.region = bendRect(b.origin, b.axis, b.out, b.w0, b.w1, 0, b.ba, b.faceS, b.faceE, b.faceL, b.faceR);
            x += b.ba;
            m.pieces.push_back(std::move(b));
            prev = int(m.pieces.size()) - 1;
            ++bi;
        }
    }
    return okStatus();
}

// ---------------------------------------------------------------- flange (§4.4)

std::optional<EdgeOnWall> edgeOnWall(const Model& m, const std::string& baseA, const std::string& baseB, const Point3& p1,
                                     const Point3& p2, const Vec3& nA, const Vec3& nB) {
    const double T = m.params.thickness;
    for (int pass = 0; pass < 2; ++pass) {
        const std::string& big = pass == 0 ? baseA : baseB;
        const std::string& sideBase = pass == 0 ? baseB : baseA;
        const Vec3& sideN = pass == 0 ? nB : nA;
        for (size_t i = 0; i < m.pieces.size(); ++i) {
            const Piece& pc = m.pieces[i];
            if (pc.kind != Piece::Kind::Wall || (pc.faceA != big && pc.faceB != big)) continue;
            if (sideBase == pc.faceA || sideBase == pc.faceB) continue;
            const Transform3 inv = fold(m, int(i)).inverse();
            const Vec3 a = inv * p1, b = inv * p2, mid = (a + b) / 2;
            const bool onB = pc.faceB == big;
            const double z = onB ? T : 0;
            if (std::abs(mid.z() - z) > SHEET_MATCH_TOL || std::abs(a.z() - z) > SHEET_MATCH_TOL || std::abs(b.z() - z) > SHEET_MATCH_TOL)
                continue;
            if (!insidePiece(pc, xy(mid), SHEET_MATCH_TOL)) continue;
            const Vec3 v = inv.linear() * sideN;
            Vec2 out(v.x(), v.y());
            if (out.norm() < 1e-9) return std::nullopt;
            out.normalize();
            EdgeOnWall e;
            e.piece = int(i);
            e.onB = onB;
            e.out = out;
            e.q1 = xy(a);
            e.q2 = xy(b);
            if ((e.q2 - e.q1).dot(perp(out)) < 0) std::swap(e.q1, e.q2);
            const Vec2 d = e.q2 - e.q1;
            if (d.norm() < SHEET_MIN_LENGTH || std::abs(d.normalized().dot(out)) > 1e-6) return std::nullopt;
            return e;
        }
    }
    return std::nullopt;
}

Status addFlanges(Model& m, const std::vector<EdgeOnWall>& edges, const FlangeSpec& spec, const std::string& F) {
    const Params& P = m.params;
    const double T = P.thickness, theta = spec.angle, r = spec.radius;
    if (spec.position != Position::Edge && theta >= PI - SHEET_ANGLE_TOL)
        return Error("SHEET_POSITION_ANGLE").with("actual", theta);
    const double d = setback(spec.position, theta, r, T);
    const double ba = bendAllowance(theta, r, P);
    const size_t n = edges.size();
    std::vector<double> cs(n, 0), ce(n, 0);
    std::vector<bool> sharedS(n, false), sharedE(n, false);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            if (edges[i].piece != edges[j].piece || edges[i].onB != edges[j].onB) continue;
            for (int ei = 0; ei < 2; ++ei)
                for (int ej = 0; ej < 2; ++ej) {
                    const Point2& pi = ei == 0 ? edges[i].q1 : edges[i].q2;
                    const Point2& pj = ej == 0 ? edges[j].q1 : edges[j].q2;
                    if ((pi - pj).norm() > SHEET_MATCH_TOL) continue;
                    const double c = d + spec.gap / 2;
                    (ei == 0 ? cs[i] : ce[i]) += c;
                    (ej == 0 ? cs[j] : ce[j]) += c;
                    (ei == 0 ? sharedS : sharedE)[i] = true;
                    (ej == 0 ? sharedS : sharedE)[j] = true;
                }
        }
    const double wr = P.widthOfRelief(), dr = P.depthOfRelief(), M = SHEET_TOOL_MARGIN;
    for (size_t i = 0; i < n; ++i) {
        const EdgeOnWall& e = edges[i];
        const int k = int(i) + 1;
        const Vec2 axis = perp(e.out);
        const double len = (e.q2 - e.q1).norm();
        const double w0 = spec.offsetStart + cs[i], w1 = len - spec.offsetEnd - ce[i];
        if (w1 - w0 <= SHEET_MIN_LENGTH) return Error("SHEET_FLANGE_TOO_SHORT").with("name", e.name);
        auto Pt = [&](double w, double s) { return Point2(e.q1 + w * axis + s * e.out); };
        Piece& parent = m.pieces[size_t(e.piece)];
        if (d > 0)
            parent.ops.push_back({PieceOp::Kind::Subtract, bendRect(e.q1, axis, e.out, w0, w1, -d, M, labelK(F, "tc", k), labelK(F, "to", k),
                                                  labelK(F, "tl", k), labelK(F, "tr", k))});
        // Reliefs at ends inside the edge that are not shared corners.
        auto relief = [&](double wa, double wb, const std::string& tag) {
            auto lab = [&](int j) { return label(F, tag + std::to_string(k) + "_" + std::to_string(j)); };
            const double sBottom = -d - dr;
            if (spec.relief == Relief::Rect) {
                parent.ops.push_back({PieceOp::Kind::Subtract, bendRect(e.q1, axis, e.out, wa, wb, sBottom, M, lab(1), lab(3), lab(4), lab(2))});
            } else if (spec.relief == Relief::Round) {
                const double sc = sBottom + wr / 2;
                Region rg;
                rg.outer = {line(Pt(wb, M), Pt(wa, M), lab(1)), line(Pt(wa, M), Pt(wa, sc), lab(2)),
                            arcThrough(Pt((wa + wb) / 2, sc), wr / 2, Pt(wa, sc), Pt(wb, sc), Pt((wa + wb) / 2, sBottom), lab(3)),
                            line(Pt(wb, sc), Pt(wb, M), lab(4))};
                parent.ops.push_back({PieceOp::Kind::Subtract, rg});
            }
        };
        if (!sharedS[i] && w0 > SHEET_MATCH_TOL) relief(w0 - wr, w0, "rl");
        if (!sharedE[i] && w1 < len - SHEET_MATCH_TOL) relief(w1, w1 + wr, "rr");
        Piece b;
        b.kind = Piece::Kind::Bend;
        b.feature = F;
        b.parent = e.piece;
        b.origin = e.q1 - d * e.out;
        b.axis = axis;
        b.out = e.out;
        b.w0 = w0;
        b.w1 = w1;
        b.ba = ba;
        b.angle = theta;
        b.radius = r;
        b.up = e.onB != spec.reverse;
        b.faceIn = labelK(F, "bi", k);
        b.faceOut = labelK(F, "bo", k);
        b.faceL = labelK(F, "bl", k);
        b.faceR = labelK(F, "br", k);
        b.faceS = labelK(F, "bs", k);
        b.faceE = labelK(F, "be", k);
        b.region = bendRect(b.origin, axis, e.out, w0, w1, 0, ba, b.faceS, b.faceE, b.faceL, b.faceR);
        Piece w;
        w.kind = Piece::Kind::Wall;
        w.feature = F;
        w.parent = int(m.pieces.size());
        w.region = bendRect(b.origin, axis, e.out, w0, w1, ba, ba + spec.length, labelK(F, "n", k), labelK(F, "e", k),
                            labelK(F, "l", k), labelK(F, "r", k));
        w.faceA = labelK(F, "a", k);
        w.faceB = labelK(F, "b", k);
        m.pieces.push_back(std::move(b));
        m.pieces.push_back(std::move(w));
    }
    m.flanges.push_back({F, spec, edges});  // the source of pattern and mirror copies (§4.6д)
    return okStatus();
}

// ---------------------------------------------------------------- walls by plane (§4.5, §4.6)

std::optional<int> wallOnPlane(const Model& m, const Plane& plane, const Point3& probe) {
    const double T = m.params.thickness;
    for (size_t i = 0; i < m.pieces.size(); ++i) {
        const Piece& pc = m.pieces[i];
        if (pc.kind != Piece::Kind::Wall) continue;
        const Transform3 F = fold(m, int(i));
        if (1 - std::abs((F.linear() * Vec3::UnitZ()).dot(plane.normal)) > SHEET_PARALLEL_TOL) continue;
        const Transform3 inv = F.inverse();
        const double z = (inv * plane.origin).z();
        if (std::abs(z) > SHEET_MATCH_TOL && std::abs(z - T) > SHEET_MATCH_TOL) continue;
        if (insidePiece(pc, xy(inv * probe), SHEET_MATCH_TOL)) return int(i);
    }
    return std::nullopt;
}

std::optional<int> wallUnder(const Model& m, const Vec3& normal, const Point3& probe) {
    const double T = m.params.thickness;
    std::optional<int> best;
    double bestD = 0;
    for (size_t i = 0; i < m.pieces.size(); ++i) {
        const Piece& pc = m.pieces[i];
        if (pc.kind != Piece::Kind::Wall) continue;
        const Transform3 F = fold(m, int(i));
        if (1 - std::abs((F.linear() * Vec3::UnitZ()).dot(normal)) > SHEET_PARALLEL_TOL) continue;
        const Vec3 q = F.inverse() * probe;
        if (!insidePiece(pc, xy(q), SHEET_MATCH_TOL)) continue;
        const double dist = std::abs(q.z() - T / 2);
        if (!best || dist < bestD - SHEET_MATCH_TOL) best = int(i), bestD = dist;
    }
    return best;
}

std::vector<int> wallsAlong(const Model& m, const Vec3& normal, const Point3& probe, double lo, double hi) {
    const double T = m.params.thickness;
    std::vector<int> out;
    for (size_t i = 0; i < m.pieces.size(); ++i) {
        const Piece& pc = m.pieces[i];
        if (pc.kind != Piece::Kind::Wall) continue;
        const Transform3 F = fold(m, int(i));
        if (1 - std::abs((F.linear() * Vec3::UnitZ()).dot(normal)) > SHEET_PARALLEL_TOL) continue;
        const Vec3 q = F.inverse() * probe;
        if (!insidePiece(pc, xy(q), SHEET_MATCH_TOL)) continue;
        const double s = normal.dot(F * Vec3(q.x(), q.y(), T / 2) - probe);
        if (s >= lo - SHEET_MATCH_TOL && s <= hi + SHEET_MATCH_TOL) out.push_back(int(i));
    }
    return out;
}

std::optional<int> wallOfRim(const Model& m, const Vec3& axis, const Point3& facePoint) {
    const double T = m.params.thickness;
    for (size_t i = 0; i < m.pieces.size(); ++i) {
        const Piece& pc = m.pieces[i];
        if (pc.kind != Piece::Kind::Wall) continue;
        const Transform3 F = fold(m, int(i));
        if (1 - std::abs((F.linear() * Vec3::UnitZ()).dot(axis.normalized())) > SHEET_PARALLEL_TOL) continue;
        const Vec3 q = F.inverse() * facePoint;
        if (q.z() < -SHEET_MATCH_TOL || q.z() > T + SHEET_MATCH_TOL) continue;
        if (insidePiece(pc, xy(q), SHEET_MATCH_TOL)) return int(i);
    }
    return std::nullopt;
}

void addTab(Model& m, int wall, Region region, const std::string& feature) {
    Piece p;
    p.kind = Piece::Kind::Wall;
    p.feature = feature;
    p.parent = wall;
    p.region = std::move(region);
    p.faceA = m.pieces.at(size_t(wall)).faceA;
    p.faceB = m.pieces.at(size_t(wall)).faceB;
    m.pieces.push_back(std::move(p));
}

// ---------------------------------------------------------------- sketch bend (§4.5)

Status addLineBend(const kernel::Kernel& k, Model& m, int wall, const Point2& q, const Vec2& a, bool fixedLeft, bool up,
                   double angle, double radius, const std::string& F) {
    const Params& P = m.params;
    const double T = P.thickness, ba = bendAllowance(angle, radius, P);
    const Vec2 left = perp(a);
    const Vec2 out = fixedLeft ? Vec2(-left) : left;
    const Vec2 axis = perp(out);
    const Point2 origin = q - (ba / 2) * out;
    const Piece W = m.pieces.at(size_t(wall));
    double ext = 1;
    forEachSeg(W.region, [&](const Seg& s) {
        ext = std::max({ext, (s.a - origin).norm(), (s.b - origin).norm(), (s.c - origin).norm() + s.r});
    });
    const double BIG = 2 * ext + 1;
    // Rectangle check in strip coordinates (x — s along out, y — w along axis; §4.5 item 3).
    Transform3 G = Transform3::Identity();
    G.linear().row(0) = Vec3(out.x(), out.y(), 0);
    G.linear().row(1) = Vec3(axis.x(), axis.y(), 0);
    G.linear().row(2) = Vec3::UnitZ();
    G.translation() = -(G.linear() * lift(origin, 0));
    const Transform3 toFlat = G.inverse();  // strip coordinates → flat
    Piece probe = W;
    probe.parent = -1;
    Model mm;
    mm.params = P;
    mm.root = toFlat.inverse();  // flat → strip coordinates
    mm.pieces = {probe};
    const double bandW = std::max(ba, 1e-3);
    auto solid = buildPieces(k, mm, true, [](const kernel::OpResult& r, const std::vector<std::vector<std::string>>&) {
        return std::vector<std::string>(r.history.faceOrigins.size());
    });
    if (!solid || solid->empty()) return Error("SHEET_BEND_NO_WALL");
    kernel::Profile band;
    band.plane.origin = Point3(0, 0, -SHEET_TOOL_MARGIN);
    auto addL = [&](Point2 p1, Point2 p2, const char* id) {
        kernel::Segment3 s;
        s.id = id;
        s.p1 = lift(p1, -SHEET_TOOL_MARGIN);
        s.p2 = lift(p2, -SHEET_TOOL_MARGIN);
        band.outer.segments.push_back(s);
    };
    addL({0, -BIG}, {bandW, -BIG}, "s1"), addL({bandW, -BIG}, {bandW, BIG}, "s2"), addL({bandW, BIG}, {0, BIG}, "s3"),
        addL({0, BIG}, {0, -BIG}, "s4");
    auto bandBody = k.extrude(band, kernel::Extent{kernel::Extent::Kind::Blind, T + 2 * SHEET_TOOL_MARGIN, {}}, false);
    if (!bandBody) return bandBody.error();
    auto inter = k.boolean(kernel::BoolKind::Intersect, solid->front().shape, bandBody->shape);
    if (!inter || inter->shape.null() || k.counts(inter->shape).solids == 0) return Error("SHEET_BEND_NO_WALL");
    const double V = k.massProperties(inter->shape).volume;
    const BBox3 bb = k.bbox(inter->shape);
    const double w0 = bb.min().y(), w1 = bb.max().y();
    const double rect = (w1 - w0) * bandW * T;
    if (!(V > 0) || std::abs(V - rect) > SHEET_AREA_REL_TOL * rect + 1e-9) return Error("SHEET_BEND_NOT_RECT");
    // Children of the wall: on the moving side — re-parented; crossing the band — error.
    const int bendIdx = int(m.pieces.size()), movingIdx = bendIdx + 1;
    std::vector<int> moving;
    for (size_t i = 0; i < m.pieces.size(); ++i) {
        const Piece& c = m.pieces[i];
        if (c.parent != wall || c.kind != Piece::Kind::Bend) continue;
        const double s0 = (c.origin + c.w0 * c.axis - origin).dot(out), s1 = (c.origin + c.w1 * c.axis - origin).dot(out);
        if (s0 > ba + SHEET_MATCH_TOL && s1 > ba + SHEET_MATCH_TOL) moving.push_back(int(i));
        else if (!(s0 < -SHEET_MATCH_TOL && s1 < -SHEET_MATCH_TOL)) return Error("SHEET_BEND_CROSSES");
    }
    for (int i : moving) m.pieces[size_t(i)].parent = movingIdx;
    auto lab = [&](const char* tag, int j) { return label(F, std::string(tag) + "_" + std::to_string(j)); };
    Piece fixedPart = W;
    fixedPart.ops.push_back({PieceOp::Kind::Intersect, bendRect(origin, axis, out, -BIG, BIG, -BIG, 0, lab("c1", 1), lab("c1", 3), lab("c1", 4), lab("c1", 2))});
    Piece movingPart = W;
    movingPart.parent = bendIdx;
    movingPart.ops.push_back({PieceOp::Kind::Intersect, bendRect(origin, axis, out, -BIG, BIG, ba, BIG, lab("c2", 1), lab("c2", 3), lab("c2", 4), lab("c2", 2))});
    Piece b;
    b.kind = Piece::Kind::Bend;
    b.feature = F;
    b.parent = wall;
    b.origin = origin;
    b.axis = axis;
    b.out = out;
    b.w0 = w0;
    b.w1 = w1;
    b.ba = ba;
    b.angle = angle;
    b.radius = radius;
    b.up = up;
    b.faceIn = label(F, "bi1");
    b.faceOut = label(F, "bo1");
    b.faceL = label(F, "bl1");
    b.faceR = label(F, "br1");
    b.faceS = label(F, "bs1");
    b.faceE = label(F, "be1");
    b.region = bendRect(origin, axis, out, w0, w1, 0, ba, b.faceS, b.faceE, b.faceL, b.faceR);
    m.pieces[size_t(wall)] = std::move(fixedPart);
    m.pieces.push_back(std::move(b));
    m.pieces.push_back(std::move(movingPart));
    return okStatus();
}

void addCut(Model& m, Region region, const std::string& feature, int wall) { m.cuts.push_back({std::move(region), feature, wall}); }

// ---------------------------------------------------------------- corners through the thickness (§4.6г)

std::optional<CornerAt> cornerOfEdge(const Model& m, const Point3& p1, const Point3& p2) {
    const double T = m.params.thickness;
    for (size_t i = 0; i < m.pieces.size(); ++i) {
        const Piece& pc = m.pieces[i];
        if (pc.kind != Piece::Kind::Wall) continue;
        const Transform3 inv = fold(m, int(i)).inverse();
        const Vec3 a = inv * p1, b = inv * p2;
        const Vec3 d = b - a;
        if (d.norm() < SHEET_MIN_LENGTH || 1 - std::abs(d.normalized().z()) > SHEET_PARALLEL_TOL) continue;
        const double lo = std::min(a.z(), b.z()), hi = std::max(a.z(), b.z());
        if (std::abs(lo) > SHEET_MATCH_TOL || std::abs(hi - T) > SHEET_MATCH_TOL) continue;
        const Point2 at = xy((a + b) / 2);
        if (insidePiece(pc, at, SHEET_MATCH_TOL)) return CornerAt{int(i), at};
    }
    return std::nullopt;
}

// ---------------------------------------------------------------- pattern and mirror copies (§4.6д)

namespace {

// Flat map of H (§4.6д item 1): (x, y) of H·(p, 0).
Point2 mapXY(const Transform3& H, const Point2& p) { return xy(H * lift(p, 0)); }

Region mapRegion(const Region& r, const Transform3& H, const std::function<std::string(const std::string&)>& relabel) {
    const Eigen::Matrix2d L = H.linear().topLeftCorner<2, 2>();
    const bool reflect = L.determinant() < 0;
    auto loop = [&](const Loop& in) {
        Loop out;
        for (Seg g : in) {
            g.a = mapXY(H, g.a), g.b = mapXY(H, g.b), g.c = mapXY(H, g.c);
            for (auto& q : g.curve.poles) q = mapXY(H, q);
            if (reflect && g.kind == Seg::Kind::Arc) {
                std::swap(g.a, g.b);
                g.rev = !g.rev;
            }
            g.label = relabel(g.label);
            out.push_back(std::move(g));
        }
        return out;
    };
    Region o;
    o.outer = loop(r.outer);
    for (const auto& h : r.holes) o.holes.push_back(loop(h));
    return o;
}

// Reference point of a region on piece P (§4.6д item 1).
Point2 anchorOf(const Region& r, const Piece& P) {
    std::vector<Point2> pts;
    for (const auto& g : r.outer) {
        if (g.kind == Seg::Kind::Circle) {
            pts.push_back(g.c + Vec2(g.r, 0));
            continue;
        }
        pts.push_back(g.a);
        if (g.kind == Seg::Kind::Line) pts.push_back((g.a + g.b) / 2);
        else if (g.kind == Seg::Kind::Arc) {
            const double a0 = angleOf(g.a - g.c), sw = ccwSweep(g.c, g.a, g.b);
            pts.push_back(g.c + g.r * Vec2(std::cos(a0 + sw / 2), std::sin(a0 + sw / 2)));
        } else pts.push_back((g.a + g.b) / 2);
    }
    for (const auto& q : pts)
        if (insidePiece(P, q, SHEET_MATCH_TOL)) return q;
    return pts.empty() ? Point2::Zero() : pts.front();
}

// Target wall of a region moved from wall P by G = X·F_P (§4.6д item 1); the map H to its flat coordinates.
std::optional<std::pair<int, Transform3>> targetWall(const Model& m, const Transform3& G, const Point2& anchor) {
    const double T = m.params.thickness;
    for (size_t i = 0; i < m.pieces.size(); ++i) {
        const Piece& pc = m.pieces[i];
        if (pc.kind != Piece::Kind::Wall) continue;
        const Transform3 H = fold(m, int(i)).inverse() * G;
        if (1 - std::abs((H.linear() * Vec3::UnitZ()).z()) > SHEET_PARALLEL_TOL) continue;
        if (std::abs((H * Vec3(0, 0, T / 2)).z() - T / 2) > SHEET_MATCH_TOL) continue;
        if (insidePiece(pc, mapXY(H, anchor), SHEET_MATCH_TOL)) return std::make_pair(int(i), H);
    }
    return std::nullopt;
}

}  // namespace

Status copyContribution(Model& m, const Model& m0, const std::string& src, const Transform3& X,
                        const std::function<std::string(const std::string&)>& relabel, const std::string& F,
                        const std::string& instance) {
    const double T = m0.params.thickness;
    auto noWall = [&] { return Error("SHEET_PATTERN_NO_WALL").with("id", src).with("name", instance); };
    // Contributions that are not copied (§4.6д item 6).
    bool any = false;
    std::vector<const FlangeRec*> recs;
    for (const auto& r : m0.flanges)
        if (r.feature == src) recs.push_back(&r);
    for (size_t i = 0; i < m0.pieces.size(); ++i) {
        const Piece& p = m0.pieces[i];
        if (p.feature != src) continue;
        any = true;
        const bool tab = p.kind == Piece::Kind::Wall && p.parent >= 0 && m0.pieces[size_t(p.parent)].kind == Piece::Kind::Wall;
        if (!tab && recs.empty()) return Error("SHEET_PATTERN_SOURCE").with("id", src);
    }
    for (const auto& c : m0.corners)
        if (c.feature == src) return Error("SHEET_PATTERN_SOURCE").with("id", src);
    for (const auto& p : m0.pieces)
        for (const auto& op : p.ops)
            if (op.kind == PieceOp::Kind::Add && !op.region.outer.empty() && op.region.outer.front().label.rfind(src + ".", 0) == 0)
                return Error("SHEET_PATTERN_SOURCE").with("id", src);
    // Cuts (item 2).
    for (const auto& c : m0.cuts) {
        if (c.feature != src) continue;
        any = true;
        int P = c.wall;
        if (P < 0 || P >= int(m0.pieces.size())) {
            P = -1;
            for (size_t i = 0; i < m0.pieces.size() && P < 0; ++i)
                if (m0.pieces[i].kind == Piece::Kind::Wall && insidePiece(m0.pieces[i], anchorOf(c.region, m0.pieces[i]), SHEET_MATCH_TOL))
                    P = int(i);
            if (P < 0) return noWall();
        }
        const Point2 anchor = anchorOf(c.region, m0.pieces[size_t(P)]);
        const auto t = targetWall(m, X * fold(m0, P), anchor);
        if (!t) return noWall();
        addCut(m, mapRegion(c.region, t->second, relabel), F, t->first);
    }
    // Tabs (item 3).
    for (size_t i = 0; i < m0.pieces.size(); ++i) {
        const Piece& p = m0.pieces[i];
        if (p.feature != src || p.kind != Piece::Kind::Wall || p.parent < 0 || m0.pieces[size_t(p.parent)].kind != Piece::Kind::Wall) continue;
        if (p.region.outer.empty()) continue;
        const auto t = targetWall(m, X * fold(m0, p.parent), p.region.outer.front().a);
        if (!t) return noWall();
        addTab(m, t->first, mapRegion(p.region, t->second, relabel), F);
    }
    // Flanges (item 4).
    for (const FlangeRec* rec : recs) {
        any = true;
        std::vector<EdgeOnWall> edges;
        for (const auto& e : rec->edges) {
            const Transform3 G = X * fold(m0, e.piece);
            const double z = e.onB ? T : 0;
            const Point3 p1 = G * lift(e.q1, z), p2 = G * lift(e.q2, z);
            const Vec3 o = G.linear() * lift(e.out, 0);
            std::optional<EdgeOnWall> found;
            for (size_t i = 0; i < m.pieces.size() && !found; ++i) {
                const Piece& pc = m.pieces[i];
                if (pc.kind != Piece::Kind::Wall) continue;
                const Transform3 inv = fold(m, int(i)).inverse();
                const Vec3 a = inv * p1, b = inv * p2, ol = inv.linear() * o;
                const bool onB = std::abs(a.z() - T) <= SHEET_MATCH_TOL && std::abs(b.z() - T) <= SHEET_MATCH_TOL;
                const bool onA = std::abs(a.z()) <= SHEET_MATCH_TOL && std::abs(b.z()) <= SHEET_MATCH_TOL;
                if ((!onA && !onB) || std::abs(ol.z()) > 1e-6) continue;
                const Vec2 out = xy(ol).normalized();
                const Point2 mid = xy((a + b) / 2);
                const double eps = std::min(T, 0.5);
                if (!insidePiece(pc, mid - eps * out, SHEET_MATCH_TOL) || insidePiece(pc, mid + eps * out, SHEET_MATCH_TOL)) continue;
                // An edge that already carries a flange is not free (a mirror onto itself).
                bool taken = false;
                for (const auto& r : m.flanges)
                    for (const auto& oe : r.edges) {
                        const bool same = (oe.q1 - xy(a)).norm() <= SHEET_MATCH_TOL ? (oe.q2 - xy(b)).norm() <= SHEET_MATCH_TOL
                                                                                    : (oe.q1 - xy(b)).norm() <= SHEET_MATCH_TOL && (oe.q2 - xy(a)).norm() <= SHEET_MATCH_TOL;
                        taken = taken || (oe.piece == int(i) && oe.onB == onB && same);
                    }
                if (taken) continue;
                EdgeOnWall w;
                w.piece = int(i);
                w.onB = onB;
                w.out = out;
                w.q1 = xy(a);
                w.q2 = xy(b);
                if ((w.q2 - w.q1).dot(perp(out)) < 0) std::swap(w.q1, w.q2);
                w.name = e.name;
                found = w;
            }
            if (!found) return noWall();
            edges.push_back(*found);
        }
        FlangeSpec spec = rec->spec;
        if (X.linear().determinant() < 0) std::swap(spec.offsetStart, spec.offsetEnd);
        // Built with the source labels, then relabeled: the new pieces and the ops added to the parents.
        const size_t piecesBefore = m.pieces.size();
        std::vector<size_t> opsBefore;
        for (const auto& p : m.pieces) opsBefore.push_back(p.ops.size());
        if (auto st = addFlanges(m, edges, spec, src); !st) return st;
        m.flanges.back().feature = F;
        auto relabelRegion = [&](Region& r) {
            for (auto& g : r.outer) g.label = relabel(g.label);
            for (auto& h : r.holes)
                for (auto& g : h) g.label = relabel(g.label);
        };
        for (size_t i = 0; i < m.pieces.size(); ++i) {
            Piece& p = m.pieces[i];
            const size_t from = i < piecesBefore ? opsBefore[i] : 0;
            for (size_t k = from; k < p.ops.size(); ++k) relabelRegion(p.ops[k].region);
            if (i < piecesBefore) continue;
            p.feature = F;
            relabelRegion(p.region);
            for (auto* l : {&p.faceA, &p.faceB, &p.faceIn, &p.faceOut, &p.faceL, &p.faceR, &p.faceS, &p.faceE})
                if (!l->empty()) *l = relabel(*l);
        }
    }
    if (!any) return Error("REGEN_INPUT_ERROR").with("id", src);
    return okStatus();
}

std::vector<BendLine> bendLines(const Model& m) {
    std::vector<BendLine> out;
    for (const auto& p : m.pieces)
        if (p.kind == Piece::Kind::Bend) {
            const Point2 mid = p.origin + (p.ba / 2) * p.out;
            out.push_back({mid + p.w0 * p.axis, mid + p.w1 * p.axis, p.up, p.angle, p.radius, p.feature});
        }
    return out;
}

}  // namespace construe::sheet
