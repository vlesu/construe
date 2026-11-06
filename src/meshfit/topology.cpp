// Solid topology from segmentation, and edge and vertex geometry (spec/mesh-fit.md §4.6–4.8).
#include "meshfit/meshfit_internal.h"

#include "base/constants.h"

#include <Eigen/SVD>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace construe::meshfit {

namespace {

Error topoError(const std::string& detail) { return Error("MESHFIT_TOPOLOGY").with("detail", detail); }

// ---------------------------------------------------------------- intersection curve of two surfaces (§4.7)

struct Curve {
    enum class Kind { Line, Circle, General } kind = Kind::General;
    Point3 p = Point3::Zero();  // Line: point; Circle: center
    Vec3 d = Vec3::UnitZ();     // Line: direction; Circle: axis
    double r = 0;
};

double curveDistance(const Curve& c, const Point3& x) {
    if (c.kind == Curve::Kind::Line) {
        const Vec3 w = x - c.p;
        return (w - w.dot(c.d) * c.d).norm();
    }
    const Vec3 w = x - c.p;
    const double h = w.dot(c.d);
    return std::hypot((w - h * c.d).norm() - c.r, h);
}

Point3 projectToCurve(const Curve& c, const Point3& x) {
    const Vec3 w = x - c.p;
    if (c.kind == Curve::Kind::Line) return c.p + w.dot(c.d) * c.d;
    Vec3 rr = w - w.dot(c.d) * c.d;
    if (rr.norm() < 1e-300) {
        rr = c.d.cross(std::abs(c.d.x()) < 0.9 ? Vec3::UnitX() : Vec3::UnitY());
    }
    return c.p + c.r * rr.normalized();
}

// Rotation axis shared by two surfaces (§4.7 item 2).
struct AxisOpt {
    Point3 o;
    Vec3 a;
};
std::optional<AxisOpt> commonAxis(const SurfaceSpec& A, const SurfaceSpec& B, double tol) {
    auto fixed = [](const SurfaceSpec& s) { return s.kind == Kind::Cylinder || s.kind == Kind::Cone || s.kind == Kind::Torus; };
    auto lineDist = [](const Point3& o, const Vec3& a, const Point3& p) {
        const Vec3 w = p - o;
        return (w - w.dot(a) * a).norm();
    };
    const double angTol = 1e-9;
    if (fixed(A) && fixed(B)) {
        if (A.dir.cross(B.dir).norm() <= angTol && lineDist(A.point, A.dir, B.point) <= tol) return AxisOpt{A.point, A.dir};
        return std::nullopt;
    }
    if (fixed(B) && !fixed(A)) return commonAxis(B, A, tol);
    if (fixed(A)) {
        if (B.kind == Kind::Plane && A.dir.cross(B.dir).norm() <= angTol) return AxisOpt{A.point, A.dir};
        if (B.kind == Kind::Sphere && lineDist(A.point, A.dir, B.point) <= tol) return AxisOpt{A.point, A.dir};
        return std::nullopt;
    }
    if (A.kind == Kind::Plane && B.kind == Kind::Sphere) return AxisOpt{B.point, A.dir};
    if (B.kind == Kind::Plane && A.kind == Kind::Sphere) return AxisOpt{A.point, B.dir};
    if (A.kind == Kind::Sphere && B.kind == Kind::Sphere && (B.point - A.point).norm() > tol)
        return AxisOpt{A.point, (B.point - A.point).normalized()};
    return std::nullopt;
}

// Meridian in the half-plane (ρ, h): a line (point, direction; for a cone, the ray λ ≥ 0) or a circle.
struct Meridian {
    bool circle = false;
    Point2 p = Point2::Zero();
    Vec2 d = Vec2::UnitX();
    bool ray = false;
    double r = 0;
};
Meridian meridianOf(const SurfaceSpec& s, const AxisOpt& ax) {
    Meridian m;
    const double h = (s.point - ax.o).dot(ax.a);
    switch (s.kind) {
        case Kind::Plane: m.p = {0, h}; m.d = {1, 0}; break;
        case Kind::Cylinder: m.p = {s.radius, 0}; m.d = {0, 1}; break;
        case Kind::Cone: {
            const double sg = s.dir.dot(ax.a) >= 0 ? 1 : -1;
            m.p = {0, h};
            m.d = {std::sin(s.halfAngle), sg * std::cos(s.halfAngle)};
            m.ray = true;
            break;
        }
        case Kind::Sphere: m.circle = true; m.p = {0, h}; m.r = s.radius; break;
        case Kind::Torus: m.circle = true; m.p = {s.radius, h}; m.r = s.minorRadius; break;
        default: break;
    }
    return m;
}

std::vector<Point2> meridianIntersections(const Meridian& A, const Meridian& B, double tol) {
    std::vector<Point2> out;
    auto lineCircle = [&](const Meridian& L, const Meridian& C) {
        const Vec2 d = L.d.normalized();
        const double t0 = (C.p - L.p).dot(d);
        const Point2 foot = L.p + t0 * d;
        const double dist = (C.p - foot).norm();
        if (dist > C.r + tol) return;
        if (dist >= C.r - tol) {
            out.push_back(foot);
            return;
        }
        const double w = std::sqrt(C.r * C.r - dist * dist);
        out.push_back(foot - w * d);
        out.push_back(foot + w * d);
    };
    if (!A.circle && !B.circle) {
        const double det = A.d.x() * (-B.d.y()) - A.d.y() * (-B.d.x());
        if (std::abs(det) < 1e-12) return out;
        const Vec2 rhs = B.p - A.p;
        const double t = (rhs.x() * (-B.d.y()) - rhs.y() * (-B.d.x())) / det;
        out.push_back(A.p + t * A.d);
    } else if (A.circle && B.circle) {
        const Vec2 dc = B.p - A.p;
        const double d = dc.norm();
        if (d < 1e-12) return out;
        if (d > A.r + B.r + tol || d < std::abs(A.r - B.r) - tol) return out;
        const double a = (A.r * A.r - B.r * B.r + d * d) / (2 * d);
        const double hh = A.r * A.r - a * a;
        const Point2 m = A.p + a * dc / d;
        if (hh <= (tol * (A.r + B.r))) {
            out.push_back(m);
            return out;
        }
        const double h = std::sqrt(hh);
        const Vec2 perp(-dc.y() / d, dc.x() / d);
        out.push_back(m + h * perp);
        out.push_back(m - h * perp);
    } else if (A.circle) {
        lineCircle(B, A);
    } else {
        lineCircle(A, B);
    }
    // Cone rays: only λ ≥ 0.
    std::vector<Point2> f;
    for (const auto& q : out) {
        bool ok = q.x() > tol;
        for (const Meridian* m : {&A, &B})
            if (!m->circle && m->ray && (q - m->p).dot(m->d) < -tol) ok = false;
        if (ok) f.push_back(q);
    }
    return f;
}

Curve analyticCurve(const SurfaceSpec& A, const SurfaceSpec& B, const std::vector<Point3>& hint, double tol) {
    Curve best;
    double bestErr = std::numeric_limits<double>::infinity();
    auto consider = [&](const Curve& c) {
        double e = 0;
        for (const auto& p : hint) e = std::max(e, curveDistance(c, p));
        if (e < bestErr) bestErr = e, best = c;
    };
    // 1. Plane and plane.
    if (A.kind == Kind::Plane && B.kind == Kind::Plane) {
        const Vec3 d = A.dir.cross(B.dir);
        if (d.norm() > 1e-9) {
            Point3 m = Point3::Zero();
            for (const auto& p : hint) m += p;
            m /= double(hint.size());
            Mat3 M;
            M.row(0) = A.dir;
            M.row(1) = B.dir;
            M.row(2) = d.normalized();
            const Vec3 rhs(A.dir.dot(A.point), B.dir.dot(B.point), d.normalized().dot(m));
            consider({Curve::Kind::Line, M.colPivHouseholderQr().solve(rhs), d.normalized(), 0});
        }
    }
    // 2. Shared rotation axis: circles.
    if (auto ax = commonAxis(A, B, tol)) {
        for (const auto& q : meridianIntersections(meridianOf(A, *ax), meridianOf(B, *ax), tol))
            consider({Curve::Kind::Circle, ax->o + q.y() * ax->a, ax->a, q.x()});
    }
    // 3. A plane parallel to the cylinder axis, and cylinders with parallel axes: lines.
    auto planeCyl = [&](const SurfaceSpec& P, const SurfaceSpec& C) {
        if (std::abs(P.dir.dot(C.dir)) > 1e-9) return;
        const double delta = P.dir.dot(C.point - P.point);
        if (std::abs(delta) > C.radius + tol) return;
        const Vec3 m = C.dir.cross(P.dir).normalized();
        const double w = std::abs(delta) >= C.radius - tol ? 0 : std::sqrt(C.radius * C.radius - delta * delta);
        for (double sgn : {1.0, -1.0}) {
            consider({Curve::Kind::Line, C.point - delta * P.dir + sgn * w * m, C.dir, 0});
            if (w == 0) break;
        }
    };
    if (A.kind == Kind::Plane && B.kind == Kind::Cylinder) planeCyl(A, B);
    if (B.kind == Kind::Plane && A.kind == Kind::Cylinder) planeCyl(B, A);
    if (A.kind == Kind::Cylinder && B.kind == Kind::Cylinder && A.dir.cross(B.dir).norm() <= 1e-9) {
        const Vec3 a = A.dir;
        const Vec3 e1 = a.cross(std::abs(a.x()) < 0.9 ? Vec3::UnitX() : Vec3::UnitY()).normalized(), e2 = a.cross(e1);
        Meridian ca, cb;
        ca.circle = cb.circle = true;
        ca.p = {A.point.dot(e1), A.point.dot(e2)};
        cb.p = {B.point.dot(e1), B.point.dot(e2)};
        ca.r = A.radius;
        cb.r = B.radius;
        std::vector<Point2> pts;
        const Vec2 dc = cb.p - ca.p;
        const double d = dc.norm();
        if (d > 1e-12 && d <= ca.r + cb.r + tol && d >= std::abs(ca.r - cb.r) - tol) {
            const double aa = (ca.r * ca.r - cb.r * cb.r + d * d) / (2 * d);
            const double hh = std::max(0.0, ca.r * ca.r - aa * aa);
            const Point2 m = ca.p + aa * dc / d;
            const Vec2 perp(-dc.y() / d, dc.x() / d);
            pts.push_back(m + std::sqrt(hh) * perp);
            if (hh > 0) pts.push_back(m - std::sqrt(hh) * perp);
        }
        for (const auto& q : pts) consider({Curve::Kind::Line, q.x() * e1 + q.y() * e2 + A.point.dot(a) * a, a, 0});
    }
    // Valid if it passes through the chain points (mesh vertices on the edge) within tolerance.
    if (best.kind != Curve::Kind::General && bestErr <= MESHFIT_CHECK_DIST * tol) return best;
    return {};
}

// Projection of a point onto the intersection of two surfaces (Newton with pseudo-inverse, §4.7 item 4).
Point3 projectToIntersection(const SurfaceSpec& A, const SurfaceSpec& B, const Point3& x0) {
    Point3 x = x0;
    for (int it = 0; it < 60; ++it) {
        const Eigen::Vector2d f(signedDistance(A, x), signedDistance(B, x));
        if (f.cwiseAbs().maxCoeff() < 1e-13 * std::max(1.0, x.norm())) break;
        Eigen::Matrix<double, 2, 3> J;
        J.row(0) = surfaceNormal(A, x);
        J.row(1) = surfaceNormal(B, x);
        Eigen::JacobiSVD<Eigen::Matrix<double, 2, 3>, Eigen::ComputeFullU | Eigen::ComputeFullV> svd(J);
        svd.setThreshold(1e-8);
        const Vec3 dx = svd.solve(f);
        x -= dx;
        if (dx.norm() < 1e-15 * std::max(1.0, x.norm())) break;
    }
    return x;
}

// ---------------------------------------------------------------- vertices (§4.8)

struct Constraint {
    const SurfaceSpec* surface = nullptr;
    const Curve* curve = nullptr;
};

Point3 solveVertex(const std::vector<Constraint>& cs, const Point3& x0, double* residual) {
    Point3 x = x0;
    auto eval = [&](const Point3& p, Eigen::VectorXd& f, Eigen::MatrixXd& J) {
        std::vector<double> fv;
        std::vector<Vec3> jv;
        for (const auto& c : cs) {
            if (c.surface) {
                fv.push_back(signedDistance(*c.surface, p));
                jv.push_back(surfaceNormal(*c.surface, p));
                continue;
            }
            const Curve& k = *c.curve;
            const Vec3 w = p - k.p;
            if (k.kind == Curve::Kind::Line) {
                const Vec3 e1 = k.d.cross(std::abs(k.d.x()) < 0.9 ? Vec3::UnitX() : Vec3::UnitY()).normalized(), e2 = k.d.cross(e1);
                fv.push_back(w.dot(e1)), jv.push_back(e1);
                fv.push_back(w.dot(e2)), jv.push_back(e2);
            } else {
                const double h = w.dot(k.d);
                Vec3 rr = w - h * k.d;
                const double rho = rr.norm();
                const Vec3 er = rho > 1e-300 ? Vec3(rr / rho) : k.d.cross(Vec3::UnitX()).normalized();
                fv.push_back(h), jv.push_back(k.d);
                fv.push_back(rho - k.r), jv.push_back(er);
            }
        }
        f.resize(Eigen::Index(fv.size()));
        J.resize(Eigen::Index(fv.size()), 3);
        for (size_t i = 0; i < fv.size(); ++i) {
            f(Eigen::Index(i)) = fv[i];
            J.row(Eigen::Index(i)) = jv[i];
        }
    };
    Eigen::VectorXd f;
    Eigen::MatrixXd J;
    for (int it = 0; it < 60; ++it) {
        eval(x, f, J);
        Eigen::JacobiSVD<Eigen::MatrixXd, Eigen::ComputeThinU | Eigen::ComputeThinV> svd(J);
        svd.setThreshold(1e-7);
        Vec3 dx = svd.solve(f);
        // Backtracking step (§4.8): for tangent surfaces and curves (adjacent loop arcs meeting tangentially)
        // the system is nearly singular and a full step leads away from the solution; the step is halved until the sum of
        // squares decreases; if it has not decreased after 30 halvings, stop.
        const double f0 = f.squaredNorm();
        Eigen::VectorXd f1;
        Eigen::MatrixXd J1;
        bool improved = false;
        for (int h = 0; h < 30 && !improved; ++h) {
            eval(x - dx, f1, J1);
            if (f1.squaredNorm() < f0 || f0 == 0) improved = true;
            else dx /= 2;
        }
        if (!improved) break;
        x -= dx;
        if (dx.norm() < 1e-15 * std::max(1.0, x.norm())) break;
    }
    eval(x, f, J);
    *residual = f.size() ? f.cwiseAbs().maxCoeff() : 0;
    return x;
}

struct Chain {
    std::vector<int> verts;  // mesh vertices in chain direction (for a closed chain, without repeating the first)
    int left = -1, right = -1;
    bool closed = false;
};

}  // namespace

Result<kernel::SolidSpec> buildSolidSpec(const FitContext& c, const std::vector<Seg>& regions) {
    const Topo& t = *c.topo;
    const Mesh& m = *t.mesh;
    const int nt = int(m.triangles.size());
    std::vector<int> regionOf(size_t(nt), -1);
    for (size_t r = 0; r < regions.size(); ++r)
        for (int i : regions[r].tris) regionOf[size_t(i)] = int(r);
    // 1. Boundary edges, vertex degrees, corners (§4.6 item 1).
    std::map<std::pair<int, int>, std::pair<int, int>> half;  // (a, b) → (triangle, k): a → b is edge k of the triangle
    std::vector<std::vector<int>> bnb(m.vertices.size());     // vertex neighbors along boundary edges
    for (int i = 0; i < nt; ++i)
        for (int k = 0; k < 3; ++k) {
            const int nb = t.nbr[size_t(i)][size_t(k)];
            if (nb < 0) return topoError("open");
            if (regionOf[size_t(nb)] == regionOf[size_t(i)]) continue;
            const int a = int(m.triangles[size_t(i)][size_t(k)]), b = int(m.triangles[size_t(i)][size_t((k + 1) % 3)]);
            half[{a, b}] = {i, k};
            if (a < b) {
                bnb[size_t(a)].push_back(b);
                bnb[size_t(b)].push_back(a);
            }
        }
    for (auto& l : bnb) std::sort(l.begin(), l.end());
    std::vector<std::vector<int>> vertTris(m.vertices.size());
    for (int i = 0; i < nt; ++i)
        for (uint32_t q : m.triangles[size_t(i)]) vertTris[q].push_back(i);
    auto isCorner = [&](int v) { return bnb[size_t(v)].size() >= 3; };
    // 2. Chains (§4.6 item 2): from corners along boundary edges to a corner; the rest are closed.
    std::vector<Chain> chains;
    std::set<std::pair<int, int>> used;
    std::map<std::pair<int, int>, std::pair<int, bool>> chainOfHalf;  // half-edge → (chain, along chain direction)
    auto walk = [&](int start, int next) {
        Chain ch;
        ch.verts = {start};
        int prev = start, cur = next;
        while (true) {
            used.insert({std::min(prev, cur), std::max(prev, cur)});
            if (cur == start) {
                ch.closed = true;
                break;
            }
            ch.verts.push_back(cur);
            if (isCorner(cur)) break;
            int nx = -1;
            for (int w : bnb[size_t(cur)])
                if (w != prev && !used.count({std::min(cur, w), std::max(cur, w)})) nx = w;
            if (nx < 0) {
                for (int w : bnb[size_t(cur)])
                    if (w == start && w != prev) nx = w;
                if (nx < 0) break;
            }
            prev = cur;
            cur = nx;
        }
        const auto h = half.find({ch.verts[0], ch.verts[1 % ch.verts.size()]});
        const auto& tr = h->second;
        ch.left = regionOf[size_t(tr.first)];
        ch.right = regionOf[size_t(t.nbr[size_t(tr.first)][size_t(tr.second)])];
        const int id = int(chains.size());
        const size_t nv = ch.verts.size();
        for (size_t i = 0; i + 1 < nv + (ch.closed ? 1 : 0); ++i) {
            const int a = ch.verts[i], b = ch.verts[(i + 1) % nv];
            chainOfHalf[{a, b}] = {id, true};
            chainOfHalf[{b, a}] = {id, false};
        }
        chains.push_back(std::move(ch));
    };
    for (int v = 0; v < int(m.vertices.size()); ++v)
        if (isCorner(v))
            for (int w : bnb[size_t(v)])
                if (!used.count({std::min(v, w), std::max(v, w)})) walk(v, w);
    for (int v = 0; v < int(m.vertices.size()); ++v)
        for (int w : bnb[size_t(v)])
            if (!used.count({std::min(v, w), std::max(v, w)})) {
                // Closed chain without corners: start at the smallest vertex, direction such that the region of the
                // triangle of half-edge v → w is on the left.
                walk(v, w);
            }
    for (const auto& ch : chains)
        if (ch.left < 0 || ch.right < 0 || ch.left == ch.right || ch.verts.size() < 2) return topoError("chain");

    // 3. Chain curves (§4.7).
    std::vector<Curve> curves(chains.size());
    for (size_t i = 0; i < chains.size(); ++i) {
        std::vector<Point3> hint;
        for (int v : chains[i].verts) hint.push_back(m.vertices[size_t(v)]);
        // Between two mesh faces — their common mesh edge (S4-MESH-002): adjacent triangles of a free-form part may be
        // nearly coplanar, and the intersection of their planes is ill-conditioned.
        if (regions[size_t(chains[i].left)].mesh && regions[size_t(chains[i].right)].mesh && hint.size() == 2 && (hint[1] - hint[0]).norm() > 0) {
            curves[i] = Curve{Curve::Kind::Line, hint[0], (hint[1] - hint[0]).normalized(), 0};
            continue;
        }
        curves[i] = analyticCurve(regions[size_t(chains[i].left)].surface, regions[size_t(chains[i].right)].surface, hint, c.eps);
    }

    // 4. Vertices (§4.8): corners and starts of closed chains.
    kernel::SolidSpec spec;
    spec.tolerance = MESHFIT_SEW_TOL;
    std::map<int, int> vertexOf;  // mesh vertex → solid vertex
    for (size_t ci = 0; ci < chains.size(); ++ci) {
        const auto& ch = chains[ci];
        for (int v : {ch.verts.front(), ch.verts.back()}) {
            if (vertexOf.count(v)) continue;
            Point3 x;
            double res = 0;
            if (ch.closed) {
                const Curve& k = curves[ci];
                // Circle: vertex at the parameter origin of surfaces of revolution (kernel-interface §3.22: the X direction
                // of the axis), so that seams of coaxial faces pass through the vertices of their circles.
                if (k.kind == Curve::Kind::Circle) x = k.p + k.r * kernel::seamDirection(k.d);
                else if (k.kind == Curve::Kind::General)
                    x = projectToIntersection(regions[size_t(ch.left)].surface, regions[size_t(ch.right)].surface, m.vertices[size_t(v)]);
                else x = projectToCurve(k, m.vertices[size_t(v)]);
            } else {
                // Region surfaces around the vertex and analytic curves of its chains.
                std::set<int> regs;
                for (int i : vertTris[size_t(v)]) regs.insert(regionOf[size_t(i)]);
                std::vector<Constraint> cs;
                for (int r : regs) cs.push_back({&regions[size_t(r)].surface, nullptr});
                for (size_t cj = 0; cj < chains.size(); ++cj)
                    if (!chains[cj].closed && (chains[cj].verts.front() == v || chains[cj].verts.back() == v) &&
                        curves[cj].kind != Curve::Kind::General)
                        cs.push_back({nullptr, &curves[cj]});
                x = solveVertex(cs, m.vertices[size_t(v)], &res);
                if (!(res <= MESHFIT_VERTEX_TOL)) return topoError("vertex");
            }
            vertexOf[v] = int(spec.vertices.size());
            spec.vertices.push_back(x);
        }
    }

    // 5. Edges (§4.7 items 3–4).
    for (size_t ci = 0; ci < chains.size(); ++ci) {
        const auto& ch = chains[ci];
        kernel::EdgeSpec e;
        e.v1 = vertexOf.at(ch.verts.front());
        e.v2 = ch.closed ? e.v1 : vertexOf.at(ch.verts.back());
        const Point3 p1 = spec.vertices[size_t(e.v1)], p2 = spec.vertices[size_t(e.v2)];
        const Curve& k = curves[ci];
        const SurfaceSpec& A = regions[size_t(ch.left)].surface;
        const SurfaceSpec& B = regions[size_t(ch.right)].surface;
        if (k.kind == Curve::Kind::Line && !ch.closed && (p2 - p1).norm() > 1e-9) {
            e.curve.kind = kernel::CurveSpec::Kind::Line;
        } else if (k.kind == Curve::Kind::Circle) {
            e.curve.kind = kernel::CurveSpec::Kind::Circle;
            e.curve.center = k.p;
            e.curve.radius = k.r;
            Vec3 a = k.d;
            // Traversal direction from a chain point: the midpoint (second point if closed) is on the counterclockwise arc.
            const Vec3 x = (p1 - k.p).normalized();
            auto angle = [&](const Point3& q, const Vec3& ax) {
                const Vec3 w = q - k.p;
                double th = std::atan2(w.dot(ax.cross(x)), w.dot(x));
                return th < 0 ? th + 2 * PI : th;
            };
            if (ch.closed) {
                // Closed: the chain's traversal direction from its first two mesh vertices.
                const Vec3 w0 = m.vertices[size_t(ch.verts[0])] - k.p, w1 = m.vertices[size_t(ch.verts[1])] - k.p;
                if (w0.cross(w1).dot(a) < 0) a = -a;
            } else {
                const Point3 mid = m.vertices[size_t(ch.verts[ch.verts.size() / 2])];
                if (angle(mid, a) > angle(p2, a)) a = -a;
            }
            e.curve.axis = a;
        } else {
            e.curve.kind = kernel::CurveSpec::Kind::Interp;
            std::vector<Point3> pts{p1};
            for (size_t i = 1; i < ch.verts.size(); ++i) pts.push_back(projectToIntersection(A, B, m.vertices[size_t(ch.verts[i])]));
            if (!ch.closed) pts.back() = p2;
            // Densification: chord midpoint projected onto the intersection, if it is off the chord by more than tolerance.
            for (int pass = 0; pass < 3; ++pass) {
                std::vector<Point3> dense{pts.front()};
                const size_t segs = pts.size() - (ch.closed ? 0 : 1);
                for (size_t i = 0; i < segs; ++i) {
                    const Point3& a0 = pts[i];
                    const Point3& b0 = pts[(i + 1) % pts.size()];
                    const Point3 mid = projectToIntersection(A, B, (a0 + b0) / 2);
                    if ((mid - (a0 + b0) / 2).norm() > c.eps) dense.push_back(mid);
                    if (i + 1 < pts.size()) dense.push_back(b0);
                }
                if (dense.size() == pts.size()) break;
                pts = std::move(dense);
            }
            e.curve.points = std::move(pts);
            e.curve.surfaceA = ch.left;
            e.curve.surfaceB = ch.right;
        }
        spec.edges.push_back(std::move(e));
    }

    // 6. Face loops (§4.6 item 3): region boundary half-edges; the next one by turning around the end inside the region.
    for (size_t r = 0; r < regions.size(); ++r) {
        kernel::FaceSpec f;
        f.surface = int(r);
        spec.surfaces.push_back(regions[r].surface);
        std::vector<std::pair<int, int>> halves;  // (triangle, k) of region boundary half-edges
        for (int i : regions[r].tris)
            for (int k = 0; k < 3; ++k)
                if (regionOf[size_t(t.nbr[size_t(i)][size_t(k)])] != int(r)) halves.push_back({i, k});
        std::set<std::pair<int, int>> visited;
        auto vtx = [&](int tri, int k) { return int(m.triangles[size_t(tri)][size_t(k)]); };
        auto nextHalf = [&](std::pair<int, int> h) {
            int ct = h.first, ck = (h.second + 1) % 3;  // edge b → c of the same triangle
            for (int guard = 0; guard < 4096; ++guard) {
                const int nb = t.nbr[size_t(ct)][size_t(ck)];
                if (regionOf[size_t(nb)] != int(r)) return std::make_pair(ct, ck);
                const int b = vtx(ct, ck);
                int j = 0;
                for (; j < 3; ++j)
                    if (vtx(nb, (j + 1) % 3) == b && vtx(nb, j) == vtx(ct, (ck + 1) % 3)) break;
                ct = nb;
                ck = (j + 1) % 3;
            }
            return std::make_pair(-1, -1);
        };
        for (const auto& h0 : halves) {
            if (visited.count(h0)) continue;
            std::vector<std::pair<int, int>> cyc;
            auto h = h0;
            while (h.first >= 0 && !visited.count(h)) {
                visited.insert(h);
                cyc.push_back(h);
                h = nextHalf(h);
            }
            if (h != h0) return topoError("loop");
            // Splitting a cycle into chains: start at a half-edge leaving a corner (a cycle without corners is one chain).
            size_t s0 = 0;
            bool any = false;
            for (size_t i = 0; i < cyc.size(); ++i)
                if (isCorner(vtx(cyc[i].first, cyc[i].second))) {
                    s0 = i;
                    any = true;
                    break;
                }
            std::vector<kernel::LoopEdge> loop;
            size_t i = 0;
            while (i < cyc.size()) {
                const auto& hh = cyc[(s0 + i) % cyc.size()];
                const int a = vtx(hh.first, hh.second), b = vtx(hh.first, (hh.second + 1) % 3);
                auto it = chainOfHalf.find({a, b});
                if (it == chainOfHalf.end()) return topoError("loop chain");
                const auto [cid, fwd] = it->second;
                loop.push_back({cid, !fwd});
                const size_t len = chains[size_t(cid)].verts.size() - (chains[size_t(cid)].closed ? 0 : 1);
                i += len;
                if (!any) break;
            }
            if (i != cyc.size() && any) return topoError("loop length");
            f.loops.push_back(std::move(loop));
        }
        if (f.loops.empty()) return topoError("no loops");
        // Inside point: the triangle center farthest from the region boundary (among MESHFIT_INSIDE_CANDIDATES
        // largest triangles; with mesh chords the face edge runs differently than the region edge), projected onto the
        // surface; the outward normal is that triangle's normal. On a surface of revolution the seam half-plane (the
        // parameter origin, kernel-interface §3.22) counts as boundary too: a point on the seam is "on" the face boundary
        // for the kernel's classifier (FX10-STL-001).
        const SurfaceSpec& rs = regions[r].surface;
        const bool revolved = !regions[r].mesh && rs.kind != Kind::Plane && rs.dir.squaredNorm() > 0;
        const Vec3 ax = revolved ? Vec3(rs.dir.normalized()) : Vec3::UnitZ();
        const Vec3 sx = revolved ? kernel::seamDirection(ax) : Vec3::UnitX(), sy = ax.cross(sx);
        std::vector<int> byArea = regions[r].tris;
        std::stable_sort(byArea.begin(), byArea.end(), [&](int a, int b) { return t.area[size_t(a)] > t.area[size_t(b)]; });
        if (byArea.size() > MESHFIT_INSIDE_CANDIDATES) byArea.resize(MESHFIT_INSIDE_CANDIDATES);
        int big = byArea.front();
        double bestClear = -1;
        for (int i : byArea) {
            const Point3& q = t.centroid[size_t(i)];
            double clear = std::numeric_limits<double>::infinity();
            for (const auto& h : halves) {
                const Point3& a = m.vertices[m.triangles[size_t(h.first)][size_t(h.second)]];
                const Point3& b = m.vertices[m.triangles[size_t(h.first)][size_t((h.second + 1) % 3)]];
                const Vec3 ab = b - a;
                const double u = std::clamp((q - a).dot(ab) / std::max(ab.squaredNorm(), 1e-300), 0.0, 1.0);
                clear = std::min(clear, (q - (a + u * ab)).norm());
            }
            if (revolved) {
                // Distance to the seam half-plane {axis + s·X, s ≥ 0}.
                const Vec3 w = q - rs.point;
                const Vec3 rp = w - w.dot(ax) * ax;
                clear = std::min(clear, rp.dot(sx) >= 0 ? std::abs(rp.dot(sy)) : rp.norm());
            }
            if (clear > bestClear + 1e-12) bestClear = clear, big = i;
        }
        f.inside = projectToSurface(regions[r].surface, t.centroid[size_t(big)]);
        f.outward = t.normal[size_t(big)];
        spec.faces.push_back(std::move(f));
    }
    return spec;
}

}  // namespace construe::meshfit
