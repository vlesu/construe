// Recognition of a sheet model from a finished body — «Лист из тела» ("Sheet from body") (spec/sheet.md §4.9, S5-SHEET-006).
#include "sheet/sheet.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <numbers>
#include <set>

namespace construe::sheet {

namespace {

using SK = kernel::SurfaceInfo::Kind;

Error recError(const std::string& detail, int face = -1) {
    Error e("SHEET_RECOGNIZE");
    e.with("detail", detail);
    if (face >= 0) e.with("face", double(face));
    return e;
}

bool parallelDirs(const Vec3& a, const Vec3& b) { return 1 - std::abs(a.normalized().dot(b.normalized())) <= SHEET_REC_ANGLE; }
bool perpendicularDirs(const Vec3& a, const Vec3& b) { return std::abs(a.normalized().dot(b.normalized())) <= SHEET_REC_ANGLE; }

// Closed loops from unordered segments by matching ends (§4.9 item 10); nullopt — a loop does not close.
std::optional<std::vector<Loop>> chainLoops(std::vector<Seg> segs) {
    std::vector<Loop> loops;
    std::vector<bool> used(segs.size(), false);
    auto start = [](const Seg& s) { return s.rev ? s.b : s.a; };
    auto end = [](const Seg& s) { return s.rev ? s.a : s.b; };
    for (size_t i = 0; i < segs.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;
        Loop l = {segs[i]};
        if (segs[i].kind == Seg::Kind::Circle) {
            loops.push_back(std::move(l));
            continue;
        }
        const Point2 first = start(segs[i]);
        Point2 cur = end(segs[i]);
        while ((cur - first).norm() > SHEET_REC_TOL) {
            bool found = false;
            for (size_t j = 0; j < segs.size() && !found; ++j) {
                if (used[j] || segs[j].kind == Seg::Kind::Circle) continue;
                Seg s = segs[j];
                if ((s.a - cur).norm() <= SHEET_REC_TOL) {
                    s.rev = false;
                } else if ((s.b - cur).norm() <= SHEET_REC_TOL) {
                    if (s.kind == Seg::Kind::Line) std::swap(s.a, s.b);
                    else s.rev = true;
                } else {
                    continue;
                }
                used[j] = true;
                found = true;
                cur = end(s);
                l.push_back(s);
            }
            if (!found) return std::nullopt;
        }
        loops.push_back(std::move(l));
    }
    return loops;
}

// Area of a loop (polygon through segment ends and arc midpoints; a circle — exactly): only for choosing the outer loop.
double loopArea(const Loop& l) {
    if (l.size() == 1 && l[0].kind == Seg::Kind::Circle) return std::numbers::pi * l[0].r * l[0].r;
    std::vector<Point2> pts;
    for (const auto& s : l) {
        const Point2 a = s.rev ? s.b : s.a;
        pts.push_back(a);
        if (s.kind == Seg::Kind::Arc) {
            const double a0 = std::atan2(s.a.y() - s.c.y(), s.a.x() - s.c.x());
            double a1 = std::atan2(s.b.y() - s.c.y(), s.b.x() - s.c.x());
            if (a1 <= a0) a1 += 2 * std::numbers::pi;
            const double am = (a0 + a1) / 2;
            pts.push_back(s.c + s.r * Vec2(std::cos(am), std::sin(am)));
        }
    }
    double A = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Point2& p = pts[i];
        const Point2& q = pts[(i + 1) % pts.size()];
        A += p.x() * q.y() - q.x() * p.y();
    }
    return std::abs(A) / 2;
}

struct Topo {
    const kernel::Kernel& k;
    const kernel::Shape& s;
    kernel::CanonicalTopology ct;
    std::vector<kernel::SurfaceInfo> surf;
    std::vector<std::vector<int>> faceLE;      // logical edges of each face
    std::vector<std::set<int>> neighbours;     // faces sharing a logical edge

    Topo(const kernel::Kernel& kk, const kernel::Shape& ss) : k(kk), s(ss), ct(kernel::canonicalTopology(kk, ss)) {
        const int n = ct.counts.faces > 0 ? ct.counts.faces : k.counts(s).faces;
        surf.resize(size_t(n));
        faceLE.resize(size_t(n));
        neighbours.resize(size_t(n));
        for (int f = 0; f < n; ++f) surf[size_t(f)] = k.faceSurface(s, f);
        for (size_t le = 0; le < ct.logicalEdges.size(); ++le) {
            const auto [a, b] = ct.logicalEdges[le].faces;
            faceLE[size_t(a)].push_back(int(le));
            if (b != a) faceLE[size_t(b)].push_back(int(le));
            if (a != b) neighbours[size_t(a)].insert(b), neighbours[size_t(b)].insert(a);
        }
    }
    int faces() const { return int(surf.size()); }
    int other(int le, int f) const {
        const auto [a, b] = ct.logicalEdges[size_t(le)].faces;
        return a == f ? b : a;
    }
    std::vector<Point3> vertices(int f) const {
        std::set<int> vs;
        for (int e : k.faceEdges(s, f)) {
            const auto [v1, v2] = k.edgeVertices(s, e);
            vs.insert(v1), vs.insert(v2);
        }
        std::vector<Point3> out;
        for (int v : vs) out.push_back(k.vertexPoint(s, v));
        return out;
    }
};

Plane planeOf(const Transform3& F) {
    Plane p;
    p.origin = F * Vec3(0, 0, 0);
    p.x = F.linear() * Vec3::UnitX();
    p.y = F.linear() * Vec3::UnitY();
    p.normal = F.linear() * Vec3::UnitZ();
    return p;
}

}  // namespace

Result<Model> recognize(const kernel::Kernel& k, const kernel::Shape& S, const std::vector<std::string>& bases, const Params& base,
                        std::optional<double> thicknessField, std::optional<double> radiusField, const std::string& feature) {
    Topo t(k, S);
    const int nf = t.faces();
    auto baseOf = [&](int f) { return size_t(f) < bases.size() ? bases[size_t(f)] : feature + ".face(" + std::to_string(f + 1) + ")"; };
    // 1. Faces: planes and cylinders only.
    for (int f = 0; f < nf; ++f)
        if (t.surf[size_t(f)].kind != SK::Plane && t.surf[size_t(f)].kind != SK::Cylinder) return recError("surface", f);
    std::vector<double> area(size_t(nf), 0);
    auto point = std::vector<Point3>(size_t(nf));
    for (int f = 0; f < nf; ++f) {
        area[size_t(f)] = k.faceArea(S, f);
        point[size_t(f)] = k.closestPointOnFace(S, f, k.faceCentroid(S, f));
    }
    auto isPlane = [&](int f) { return t.surf[size_t(f)].kind == SK::Plane; };
    auto normal = [&](int f) { return Vec3(t.surf[size_t(f)].dir.normalized()); };
    // 3. Partners of planar faces.
    std::vector<int> partner(size_t(nf), -1);
    std::vector<double> thick(size_t(nf), 0);
    for (int P = 0; P < nf; ++P) {
        if (!isPlane(P)) continue;
        const Vec3 n = normal(P);
        double best = std::numeric_limits<double>::infinity();
        for (int Q = 0; Q < nf; ++Q) {
            if (Q == P || !isPlane(Q) || normal(Q).dot(n) > -1 + SHEET_REC_ANGLE) continue;
            const double d = n.dot(point[size_t(P)] - t.surf[size_t(Q)].point);
            if (d <= SHEET_REC_TOL || d >= best) continue;
            if (k.distanceToFace(S, Q, point[size_t(P)] - d * n) <= SHEET_REC_TOL) best = d, partner[size_t(P)] = Q;
        }
        if (partner[size_t(P)] >= 0) thick[size_t(P)] = best;
    }
    // 4. Thickness: at the largest planar face with a partner.
    int big = -1, bigAny = -1;
    for (int f = 0; f < nf; ++f) {
        if (!isPlane(f)) continue;
        if (bigAny < 0 || area[size_t(f)] > area[size_t(bigAny)]) bigAny = f;
        if (partner[size_t(f)] >= 0 && (big < 0 || area[size_t(f)] > area[size_t(big)])) big = f;
    }
    if (big < 0) return recError("thickness", bigAny);
    const double T = thick[size_t(big)];
    if (thicknessField && std::abs(*thicknessField - T) > SHEET_REC_TOL) {
        Error e = recError("thickness", big);
        e.with("actual", T);
        return e;
    }
    // 5. Wall sides: mutual partners at distance T.
    std::vector<int> wallOf(size_t(nf), -1);
    std::vector<std::pair<int, int>> walls;  // (lower FaceIdx, higher)
    for (int f = 0; f < nf; ++f) {
        const int q = partner[size_t(f)];
        if (q < 0 || q < f || partner[size_t(q)] != f || std::abs(thick[size_t(f)] - T) > SHEET_REC_TOL ||
            std::abs(thick[size_t(q)] - T) > SHEET_REC_TOL)
            continue;
        wallOf[size_t(f)] = wallOf[size_t(q)] = int(walls.size());
        walls.push_back({f, q});
    }
    auto isSide = [&](int f) { return wallOf[size_t(f)] >= 0; };
    // 6. Bends: coaxial cylinder pairs r, r + T whose axis is ⊥ the adjacent sides; end cylinders have the axis ∥ a side normal.
    struct Bend {
        int in = -1, out = -1, w1 = -1, w2 = -1;  // inner / outer cylinder, the two walls
    };
    std::vector<Bend> bends;
    std::vector<int> bendOf(size_t(nf), -1);
    auto endCylinder = [&](int f) {
        for (int g : t.neighbours[size_t(f)])
            if (isSide(g) && parallelDirs(t.surf[size_t(f)].dir, normal(g))) return true;
        return false;
    };
    for (int f = 0; f < nf; ++f) {
        if (isPlane(f) || bendOf[size_t(f)] >= 0 || endCylinder(f)) continue;
        const auto& a = t.surf[size_t(f)];
        int mate = -1;
        for (int g = 0; g < nf && mate < 0; ++g) {
            if (g == f || isPlane(g) || bendOf[size_t(g)] >= 0) continue;
            const auto& b = t.surf[size_t(g)];
            if (!parallelDirs(a.dir, b.dir)) continue;
            const Vec3 d = b.point - a.point;
            if ((d - d.dot(a.dir.normalized()) * a.dir.normalized()).norm() > SHEET_REC_TOL) continue;
            if (std::abs(std::abs(a.radius - b.radius) - T) <= SHEET_REC_TOL) mate = g;
        }
        if (mate < 0) return recError("bend", f);
        Bend b;
        b.in = t.surf[size_t(f)].radius < t.surf[size_t(mate)].radius ? f : mate;
        b.out = b.in == f ? mate : f;
        std::set<int> ws;
        for (int g : t.neighbours[size_t(b.in)])
            if (isSide(g)) {
                if (!perpendicularDirs(normal(g), t.surf[size_t(f)].dir)) return recError("bend", b.in);
                ws.insert(wallOf[size_t(g)]);
            }
        if (ws.size() != 2) return recError("bend", b.in);
        b.w1 = *ws.begin(), b.w2 = *ws.rbegin();
        bendOf[size_t(f)] = bendOf[size_t(mate)] = int(bends.size());
        bends.push_back(b);
    }
    // 5 (cont.). Planar faces that are neither sides nor ends.
    for (int f = 0; f < nf; ++f) {
        if (!isPlane(f) || isSide(f)) continue;
        bool end = true, step = false;  // step: parallel to an adjacent side and has a partner — a thickness step
        for (int g : t.neighbours[size_t(f)]) {
            if (isSide(g) && !perpendicularDirs(normal(f), normal(g))) end = false;
            if (isSide(g) && parallelDirs(normal(f), normal(g)) && partner[size_t(f)] >= 0) step = true;
            if (bendOf[size_t(g)] >= 0 && !parallelDirs(normal(f), t.surf[size_t(g)].dir)) end = false;
        }
        if (!end) return recError(step ? "thickness" : "face", f);
    }
    // 7. Corners without a bend.
    for (const auto& le : t.ct.logicalEdges) {
        const auto [a, b] = le.faces;
        if (a != b && isSide(a) && isSide(b) && wallOf[size_t(a)] != wallOf[size_t(b)]) return recError("corner", std::min(a, b));
    }
    if (walls.empty()) return recError("thickness", big);
    // 8. Root and F₀.
    int root = 0;
    auto wallArea = [&](int w) { return std::max(area[size_t(walls[size_t(w)].first)], area[size_t(walls[size_t(w)].second)]); };
    for (int w = 1; w < int(walls.size()); ++w)
        if (wallArea(w) > wallArea(root)) root = w;
    const int rootA = walls[size_t(root)].first;
    const Vec3 z = -normal(rootA);
    Point3 org = Point3::Zero();
    {
        auto vs = t.vertices(rootA);
        std::sort(vs.begin(), vs.end(), [](const Point3& a, const Point3& b) {
            if (a.x() != b.x()) return a.x() < b.x();
            if (a.y() != b.y()) return a.y() < b.y();
            return a.z() < b.z();
        });
        if (!vs.empty()) org = vs.front();
    }
    Vec3 x = Vec3::Zero();
    double longest = 0;
    for (int le : t.faceLE[size_t(rootA)]) {
        const auto c = k.edgeCurve(S, le);
        if (c.kind != kernel::CurveInfo::Kind::Line) continue;
        const double len = (c.p2 - c.p1).norm();
        if (len > longest) longest = len, x = (c.p2 - c.p1) / len;
    }
    if (longest == 0) {
        double least = 2;
        for (const Vec3& a : {Vec3::UnitX(), Vec3::UnitY(), Vec3::UnitZ()})
            if (std::abs(a.dot(z)) < least) least = std::abs(a.dot(z)), x = a;
    }
    x = (x - x.dot(z) * z).normalized();
    Model m;
    m.params = base;
    m.params.thickness = T;
    m.root = Transform3::Identity();
    m.root.linear().col(0) = x;
    m.root.linear().col(1) = z.cross(x);
    m.root.linear().col(2) = z;
    m.root.translation() = org;
    // 10. Contour of a wall: edges of side A projected onto the plane z = 0 of the wall's fold.
    auto contour = [&](int A, const Transform3& F) -> Result<Region> {
        const Plane P = planeOf(F);
        std::vector<Seg> segs;
        for (int le : t.faceLE[size_t(A)]) {
            auto c = k.projectEdge(S, le, P);
            if (!c) return recError("contour", A);
            const int o = t.other(le, A);
            Seg s;
            s.label = baseOf(o);  // the end face (or the bend cylinder at the bend line, internal)
            switch (c->kind) {
                case kernel::Curve2::Kind::Line:
                    s.kind = Seg::Kind::Line, s.a = c->p1, s.b = c->p2;
                    break;
                case kernel::Curve2::Kind::Arc:
                    s.kind = Seg::Kind::Arc, s.a = c->start, s.b = c->end, s.c = c->center, s.r = c->radius;
                    break;
                case kernel::Curve2::Kind::Circle:
                    s.kind = Seg::Kind::Circle, s.c = c->center, s.r = c->radius;
                    break;
                default:
                    return recError("contour", A);
            }
            segs.push_back(s);
        }
        auto loops = chainLoops(segs);
        if (!loops || loops->empty()) return recError("contour", A);
        size_t outer = 0;
        for (size_t i = 1; i < loops->size(); ++i)
            if (loopArea((*loops)[i]) > loopArea((*loops)[outer])) outer = i;
        Region r;
        r.outer = (*loops)[outer];
        for (size_t i = 0; i < loops->size(); ++i)
            if (i != outer) r.holes.push_back((*loops)[i]);
        return r;
    };
    auto wallPiece = [&](int A, int B, int parent) {
        Piece p;
        p.kind = Piece::Kind::Wall;
        p.feature = feature;
        p.parent = parent;
        p.faceA = baseOf(A);
        p.faceB = baseOf(B);
        return p;
    };
    // 9. Breadth-first unfolding from the root.
    std::vector<int> wallPieceOf(walls.size(), -1), sideA(walls.size(), -1);
    m.pieces.push_back(wallPiece(rootA, walls[size_t(root)].second, -1));
    {
        auto r = contour(rootA, m.root);
        if (!r) return r.error();
        m.pieces[0].region = std::move(*r);
    }
    wallPieceOf[size_t(root)] = 0, sideA[size_t(root)] = rootA;
    std::vector<bool> bendUsed(bends.size(), false);
    std::optional<double> firstRadius;
    std::deque<int> queue = {root};
    while (!queue.empty()) {
        const int W = queue.front();
        queue.pop_front();
        const int wp = wallPieceOf[size_t(W)];
        const Transform3 FW = fold(m, wp), Fi = FW.inverse();
        // Bends of the wall in the order of their inner faces.
        std::vector<int> order;
        for (size_t b = 0; b < bends.size(); ++b)
            if (!bendUsed[b] && (bends[b].w1 == W || bends[b].w2 == W)) order.push_back(int(b));
        std::sort(order.begin(), order.end(), [&](int a, int b) { return bends[size_t(a)].in < bends[size_t(b)].in; });
        for (int bi : order) {
            const Bend& b = bends[size_t(bi)];
            bendUsed[size_t(bi)] = true;
            const int C = b.w1 == W ? b.w2 : b.w1;
            if (wallPieceOf[size_t(C)] >= 0) return recError("closed", b.in);
            const auto& cyl = t.surf[size_t(b.in)];
            const double r = cyl.radius;
            const Vec3 c = Fi * cyl.point, u = (Fi.linear() * cyl.dir).normalized();
            if (std::abs(u.z()) > SHEET_REC_ANGLE) return recError("bend", b.in);
            bool up;
            if (std::abs(c.z() - (T + r)) <= SHEET_REC_TOL) up = true;
            else if (std::abs(c.z() + r) <= SHEET_REC_TOL) up = false;
            else return recError("bend", b.in);
            const Point2 origin(c.x(), c.y());
            const Vec2 dir = Vec2(u.x(), u.y()).normalized();
            Vec2 out(dir.y(), -dir.x());
            const Vec3 g = Fi * k.faceCentroid(S, sideA[size_t(W)]);
            if ((Vec2(g.x(), g.y()) - origin).dot(out) > 0) out = -out;
            const Vec2 axis(-out.y(), out.x());
            double w0 = std::numeric_limits<double>::infinity(), w1 = -w0;
            for (const Point3& v : t.vertices(b.in)) {
                const Vec3 q = Fi * v;
                const double w = (Vec2(q.x(), q.y()) - origin).dot(axis);
                w0 = std::min(w0, w), w1 = std::max(w1, w);
            }
            // The bend angle by the normals of the sides adjacent to the inner face (the same side of the sheet).
            int sW = -1, sC = -1;
            for (int g2 : t.neighbours[size_t(b.in)]) {
                if (wallOf[size_t(g2)] == W) sW = g2;
                if (wallOf[size_t(g2)] == C) sC = g2;
            }
            if (sW < 0 || sC < 0) return recError("bend", b.in);
            const double theta = std::acos(std::clamp(normal(sW).dot(normal(sC)), -1.0, 1.0));
            if (theta <= SHEET_ANGLE_TOL) return recError("bend", b.in);
            // The inner face is a rectangle: two lines and two arcs.
            {
                int lines = 0, arcs = 0;
                for (int le : t.faceLE[size_t(b.in)]) {
                    const auto ci = k.edgeCurve(S, le);
                    if (ci.kind == kernel::CurveInfo::Kind::Line) ++lines;
                    else if (ci.kind == kernel::CurveInfo::Kind::Circle) ++arcs;
                }
                if (lines != 2 || arcs != 2) return recError("bend", b.in);
            }
            if (!firstRadius) firstRadius = r;
            Piece bp;
            bp.kind = Piece::Kind::Bend;
            bp.feature = feature;
            bp.parent = wp;
            bp.origin = origin, bp.axis = axis, bp.out = out;
            bp.w0 = w0, bp.w1 = w1;
            bp.angle = theta, bp.radius = r, bp.up = up;
            bp.ba = bendAllowance(theta, r, m.params);
            bp.faceIn = baseOf(b.in), bp.faceOut = baseOf(b.out);
            bp.faceL = bp.faceR = bp.faceS = bp.faceE = baseOf(b.in);
            for (int g2 : t.neighbours[size_t(b.in)]) {
                if (!isPlane(g2) || isSide(g2)) continue;
                const Vec3 q = Fi * k.faceCentroid(S, g2);
                const double w = (Vec2(q.x(), q.y()) - origin).dot(axis);
                (std::abs(w - w0) <= std::abs(w - w1) ? bp.faceL : bp.faceR) = baseOf(g2);
            }
            bp.region = bendRect(origin, axis, out, w0, w1, 0, bp.ba, bp.faceS, bp.faceE, bp.faceL, bp.faceR);
            m.pieces.push_back(bp);
            const int bpi = int(m.pieces.size()) - 1;
            // The child wall: side A lies on z = 0 of its fold, side B on z = T.
            m.pieces.push_back(wallPiece(walls[size_t(C)].first, walls[size_t(C)].second, bpi));
            const int cp = int(m.pieces.size()) - 1;
            const Transform3 FC = fold(m, cp), FCi = FC.inverse();
            auto onLevel = [&](int f, double level) {
                for (const Point3& v : t.vertices(f))
                    if (std::abs((FCi * v).z() - level) > SHEET_REC_TOL) return false;
                return true;
            };
            int A = walls[size_t(C)].first, B = walls[size_t(C)].second;
            if (!(onLevel(A, 0) && onLevel(B, T))) std::swap(A, B);
            if (!(onLevel(A, 0) && onLevel(B, T))) return recError("bend", b.in);
            m.pieces[size_t(cp)].faceA = baseOf(A), m.pieces[size_t(cp)].faceB = baseOf(B);
            auto rg = contour(A, FC);
            if (!rg) return rg.error();
            m.pieces[size_t(cp)].region = std::move(*rg);
            wallPieceOf[size_t(C)] = cp, sideA[size_t(C)] = A;
            queue.push_back(C);
        }
    }
    for (int pi : wallPieceOf)
        if (pi < 0) return recError("shape");  // walls not connected through bends
    // 11. Parameters.
    m.params.radius = radiusField ? *radiusField : firstRadius ? *firstRadius : T;
    return m;
}

}  // namespace construe::sheet
