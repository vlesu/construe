// Surfaces: distance, normal, projection, fitting (spec/mesh-fit.md §4.1–4.3).
#include "meshfit/meshfit_internal.h"

#include "base/constants.h"

#include <Eigen/Eigenvalues>
#include <Eigen/SVD>

#include <algorithm>
#include <cmath>
#include <limits>

namespace construe::meshfit {

namespace {

// Unit vector perpendicular to d (deterministic).
Vec3 anyPerp(const Vec3& d) {
    const Vec3 a = std::abs(d.x()) < 0.9 ? Vec3::UnitX() : Vec3::UnitY();
    return d.cross(a).normalized();
}

// Radial direction from axis (o, a) to p; on the axis, an arbitrary perpendicular.
Vec3 radialDir(const Point3& o, const Vec3& a, const Point3& p, double* rho) {
    const Vec3 w = p - o;
    const Vec3 r = w - w.dot(a) * a;
    *rho = r.norm();
    return *rho > 1e-300 ? Vec3(r / *rho) : anyPerp(a);
}

}  // namespace

int parameterCount(Kind k) {
    switch (k) {
        case Kind::Plane: return 3;
        case Kind::Sphere: return 4;
        case Kind::Cylinder: return 5;
        case Kind::Cone: return 6;
        case Kind::Torus: return 7;
        default: return 0;
    }
}

double signedDistance(const SurfaceSpec& s, const Point3& p) {
    switch (s.kind) {
        case Kind::Plane: return (p - s.point).dot(s.dir);
        case Kind::Sphere: return (p - s.point).norm() - s.radius;
        case Kind::Cylinder: {
            const Vec3 w = p - s.point;
            return (w - w.dot(s.dir) * s.dir).norm() - s.radius;
        }
        case Kind::Cone: {
            const Vec3 w = p - s.point;
            const double t = w.dot(s.dir);
            const double rho = (w - t * s.dir).norm();
            return rho * std::cos(s.halfAngle) - t * std::sin(s.halfAngle);
        }
        case Kind::Torus: {
            const Vec3 w = p - s.point;
            const double h = w.dot(s.dir);
            const double rho = (w - h * s.dir).norm();
            return std::hypot(rho - s.radius, h) - s.minorRadius;
        }
        default: return std::numeric_limits<double>::infinity();
    }
}

Vec3 surfaceNormal(const SurfaceSpec& s, const Point3& p) {
    switch (s.kind) {
        case Kind::Plane: return s.dir;
        case Kind::Sphere: {
            const Vec3 w = p - s.point;
            return w.norm() > 1e-300 ? Vec3(w.normalized()) : s.dir;
        }
        case Kind::Cylinder: {
            double rho;
            return radialDir(s.point, s.dir, p, &rho);
        }
        case Kind::Cone: {
            double rho;
            const Vec3 e = radialDir(s.point, s.dir, p, &rho);
            return (std::cos(s.halfAngle) * e - std::sin(s.halfAngle) * s.dir).normalized();
        }
        case Kind::Torus: {
            double rho;
            const Vec3 e = radialDir(s.point, s.dir, p, &rho);
            const double h = (p - s.point).dot(s.dir);
            Vec3 q(rho - s.radius, h, 0);
            if (q.norm() < 1e-300) return e;
            q.normalize();
            return (q.x() * e + q.y() * s.dir).normalized();
        }
        default: return Vec3::UnitZ();
    }
}

Point3 projectToSurface(const SurfaceSpec& s, const Point3& p) {
    Point3 x = p;
    for (int i = 0; i < 4; ++i) x -= signedDistance(s, x) * surfaceNormal(s, x);
    return x;
}

namespace {

// ---------------------------------------------------------------- initial guesses (§4.2)

struct Pca {
    Point3 centroid = Point3::Zero();
    Vec3 eval = Vec3::Zero();  // ascending
    Mat3 evec = Mat3::Identity();
};

Pca pca(const std::vector<Point3>& pts) {
    Pca r;
    if (pts.empty()) return r;
    for (const auto& p : pts) r.centroid += p;
    r.centroid /= double(pts.size());
    Mat3 c = Mat3::Zero();
    for (const auto& p : pts) c += (p - r.centroid) * (p - r.centroid).transpose();
    Eigen::SelfAdjointEigenSolver<Mat3> es(c);
    r.eval = es.eigenvalues();
    r.evec = es.eigenvectors();
    return r;
}

// Circle through points in a plane (Kasa algebraic fit).
bool kasaCircle(const std::vector<Point2>& pts, Point2* c, double* r) {
    if (pts.size() < 3) return false;
    Point2 m = Point2::Zero();
    for (const auto& p : pts) m += p;
    m /= double(pts.size());
    Eigen::MatrixXd A(pts.size(), 3);
    Eigen::VectorXd b(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        const Point2 q = pts[i] - m;
        A(Eigen::Index(i), 0) = q.x();
        A(Eigen::Index(i), 1) = q.y();
        A(Eigen::Index(i), 2) = 1;
        b(Eigen::Index(i)) = -(q.squaredNorm());
    }
    const Eigen::Vector3d x = A.colPivHouseholderQr().solve(b);
    const Point2 cc(-x(0) / 2, -x(1) / 2);
    const double rr = cc.squaredNorm() - x(2);
    if (!(rr > 0) || !std::isfinite(rr)) return false;
    *c = cc + m;
    *r = std::sqrt(rr);
    return true;
}

bool kasaSphere(const std::vector<Point3>& pts, Point3* c, double* r) {
    if (pts.size() < 4) return false;
    Point3 m = Point3::Zero();
    for (const auto& p : pts) m += p;
    m /= double(pts.size());
    Eigen::MatrixXd A(pts.size(), 4);
    Eigen::VectorXd b(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        const Point3 q = pts[i] - m;
        A.row(Eigen::Index(i)) << q.x(), q.y(), q.z(), 1;
        b(Eigen::Index(i)) = -q.squaredNorm();
    }
    Eigen::JacobiSVD<Eigen::MatrixXd, Eigen::ComputeThinU | Eigen::ComputeThinV> svd(A);
    if (svd.singularValues()(3) < 1e-12 * std::max(1.0, svd.singularValues()(0))) return false;
    const Eigen::Vector4d x = svd.solve(b);
    const Point3 cc(-x(0) / 2, -x(1) / 2, -x(2) / 2);
    const double rr = cc.squaredNorm() - x(3);
    if (!(rr > 0) || !std::isfinite(rr)) return false;
    *c = cc + m;
    *r = std::sqrt(rr);
    return true;
}

std::pair<Vec3, Vec3> basis(const Vec3& d) {
    const Vec3 e1 = anyPerp(d);
    return {e1, d.cross(e1)};
}

// Circle in the plane ⊥ a through the point projections (for cylinder and torus).
bool circleAround(const std::vector<Point3>& pts, const Vec3& a, Point3* center, double* r, double* rms) {
    const auto [e1, e2] = basis(a);
    std::vector<Point2> q;
    for (const auto& p : pts) q.push_back({p.dot(e1), p.dot(e2)});
    Point2 c;
    if (!kasaCircle(q, &c, r)) return false;
    double h = 0;
    for (const auto& p : pts) h += p.dot(a);
    h /= double(pts.size());
    *center = c.x() * e1 + c.y() * e2 + h * a;
    if (rms) {
        double s = 0;
        for (const auto& p : q) s += std::pow((p - c).norm() - *r, 2);
        *rms = std::sqrt(s / double(q.size()));
    }
    return true;
}

std::optional<SurfaceSpec> initPlane(const std::vector<Point3>& pts, const std::vector<Vec3>& nrm) {
    if (pts.size() < 3) return std::nullopt;
    const Pca p = pca(pts);
    SurfaceSpec s;
    s.kind = Kind::Plane;
    s.point = p.centroid;
    s.dir = p.evec.col(0).normalized();
    Vec3 mean = Vec3::Zero();
    for (const auto& n : nrm) mean += n;
    if (mean.dot(s.dir) < 0) s.dir = -s.dir;
    return s;
}

std::optional<SurfaceSpec> initSphere(const std::vector<Point3>& pts) {
    SurfaceSpec s;
    s.kind = Kind::Sphere;
    if (!kasaSphere(pts, &s.point, &s.radius)) return std::nullopt;
    s.dir = Vec3::UnitZ();
    return s;
}

std::optional<SurfaceSpec> initCylinder(const std::vector<Point3>& pts, const std::vector<Vec3>& nrm) {
    if (nrm.size() < 2 || pts.size() < 3) return std::nullopt;
    Mat3 m = Mat3::Zero();
    for (const auto& n : nrm) m += n * n.transpose();
    Eigen::SelfAdjointEigenSolver<Mat3> es(m);
    if (es.eigenvalues()(1) < 1e-9 * std::max(1e-300, es.eigenvalues()(2))) return std::nullopt;  // normals in one direction
    SurfaceSpec s;
    s.kind = Kind::Cylinder;
    s.dir = es.eigenvectors().col(0).normalized();
    if (!circleAround(pts, s.dir, &s.point, &s.radius, nullptr)) return std::nullopt;
    return s;
}

std::optional<SurfaceSpec> initCone(const std::vector<Point3>& pts, const std::vector<Vec3>& nrm) {
    if (nrm.size() < 3 || pts.size() != nrm.size()) return std::nullopt;
    Vec3 nm = Vec3::Zero();
    for (const auto& n : nrm) nm += n;
    nm /= double(nrm.size());
    Mat3 c = Mat3::Zero();
    for (const auto& n : nrm) c += (n - nm) * (n - nm).transpose();
    Eigen::SelfAdjointEigenSolver<Mat3> es(c);
    Vec3 a = es.eigenvectors().col(0).normalized();
    // Apex: the tangent planes pass through it, so Σ n nᵀ A = Σ n nᵀ p.
    Mat3 M = Mat3::Zero();
    Vec3 rhs = Vec3::Zero();
    for (size_t i = 0; i < pts.size(); ++i) {
        M += nrm[i] * nrm[i].transpose();
        rhs += nrm[i] * nrm[i].dot(pts[i]);
    }
    Eigen::JacobiSVD<Mat3, Eigen::ComputeFullU | Eigen::ComputeFullV> svd(M);
    if (svd.singularValues()(2) < 1e-9 * std::max(1e-300, svd.singularValues()(0))) return std::nullopt;
    const Point3 apex = svd.solve(rhs);
    double t = 0;
    for (const auto& p : pts) t += (p - apex).dot(a);
    if (t < 0) a = -a;
    double sa = 0;
    for (const auto& n : nrm) sa += std::abs(n.dot(a));
    sa /= double(nrm.size());
    if (!(sa > 1e-4 && sa < 1 - 1e-4)) return std::nullopt;
    SurfaceSpec s;
    s.kind = Kind::Cone;
    s.point = apex;
    s.dir = a;
    s.halfAngle = std::asin(sa);
    return s;
}

// Torus: points q = p − s·n lie on the tube center circle for s = ±r (§4.2).
std::optional<SurfaceSpec> initTorus(const std::vector<Point3>& pts, const std::vector<Vec3>& nrm) {
    if (nrm.size() < 6 || pts.size() != nrm.size()) return std::nullopt;
    BBox3 bb;
    for (const auto& p : pts) bb.extend(p);
    const double D = std::max(bb.diagonal().norm(), 1e-9);
    auto score = [&](double sv, SurfaceSpec* out) {
        std::vector<Point3> q;
        for (size_t i = 0; i < pts.size(); ++i) q.push_back(pts[i] - sv * nrm[i]);
        const Pca p = pca(q);
        const Vec3 a = p.evec.col(0).normalized();
        Point3 c;
        double R, rms;
        if (!circleAround(q, a, &c, &R, &rms)) return std::numeric_limits<double>::infinity();
        double off = 0;
        for (const auto& x : q) off += std::pow((x - c).dot(a), 2);
        const double err = std::sqrt(rms * rms + off / double(q.size()));
        if (out) {
            out->kind = Kind::Torus;
            out->point = c;
            out->dir = a;
            out->radius = R;
            out->minorRadius = std::abs(sv);
        }
        return err;
    };
    double best = std::numeric_limits<double>::infinity(), bestS = 0;
    for (int sign : {1, -1})
        for (int i = 0; i <= 48; ++i) {
            const double sv = sign * D * std::pow(10.0, -3.0 + 3.0 * i / 48.0);
            const double e = score(sv, nullptr);
            if (e < best) best = e, bestS = sv;
        }
    if (!std::isfinite(best)) return std::nullopt;
    // Golden-section refinement at the adjacent nodes.
    double lo = bestS / std::pow(10.0, 3.0 / 48.0), hi = bestS * std::pow(10.0, 3.0 / 48.0);
    if (lo > hi) std::swap(lo, hi);
    const double g = (std::sqrt(5.0) - 1) / 2;
    double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo), f1 = score(x1, nullptr), f2 = score(x2, nullptr);
    for (int it = 0; it < 60; ++it) {
        if (f1 < f2) hi = x2, x2 = x1, f2 = f1, x1 = hi - g * (hi - lo), f1 = score(x1, nullptr);
        else lo = x1, x1 = x2, f1 = f2, x2 = lo + g * (hi - lo), f2 = score(x2, nullptr);
    }
    SurfaceSpec s;
    if (!std::isfinite(score((lo + hi) / 2, &s))) return std::nullopt;
    if (!(s.radius > 0 && s.minorRadius > 0)) return std::nullopt;
    return s;
}

// ---------------------------------------------------------------- Levenberg–Marquardt (§4.3)

// Parameterization of a kind's increments around the base surface: axis/normal rotation (u, v) in the basis e1, e2 ⊥ dir.
SurfaceSpec applyDelta(const SurfaceSpec& b, const Vec3& e1, const Vec3& e2, const Eigen::VectorXd& d) {
    SurfaceSpec s = b;
    auto rot = [&](double u, double v) { return (b.dir + u * e1 + v * e2).normalized(); };
    switch (b.kind) {
        case Kind::Plane:
            s.dir = rot(d(0), d(1));
            s.point = b.point + d(2) * s.dir;
            break;
        case Kind::Sphere:
            s.point = b.point + Vec3(d(0), d(1), d(2));
            s.radius = b.radius + d(3);
            break;
        case Kind::Cylinder:
            s.dir = rot(d(0), d(1));
            s.point = b.point + d(2) * e1 + d(3) * e2;
            s.radius = b.radius + d(4);
            break;
        case Kind::Cone:
            s.dir = rot(d(0), d(1));
            s.point = b.point + Vec3(d(2), d(3), d(4));
            s.halfAngle = b.halfAngle + d(5);
            break;
        case Kind::Torus:
            s.dir = rot(d(0), d(1));
            s.point = b.point + Vec3(d(2), d(3), d(4));
            s.radius = b.radius + d(5);
            s.minorRadius = b.minorRadius + d(6);
            break;
        default: break;
    }
    return s;
}

bool admissible(const SurfaceSpec& s) {
    if (!s.point.allFinite() || !s.dir.allFinite()) return false;
    switch (s.kind) {
        case Kind::Sphere:
        case Kind::Cylinder: return s.radius > 0 && std::isfinite(s.radius);
        case Kind::Cone: return s.halfAngle > 1e-6 && s.halfAngle < PI / 2 - 1e-6;
        case Kind::Torus: return s.radius > 0 && s.minorRadius > 0 && std::isfinite(s.radius) && std::isfinite(s.minorRadius);
        default: return true;
    }
}

double cost(const SurfaceSpec& s, const std::vector<Point3>& pts) {
    double c = 0;
    for (const auto& p : pts) c += std::pow(signedDistance(s, p), 2);
    return c;
}

}  // namespace

// Constrained refinement: the parameter increment is B·δ (columns of B are free directions; empty: all parameters).
FitResult refineConstrained(const SurfaceSpec& start, const std::vector<Point3>& ptsIn, const Eigen::MatrixXd& Bin) {
    FitResult r;
    r.surface = start;
    r.surface.dir.normalize();
    if (ptsIn.empty() || !admissible(r.surface)) return r;
    // At most MESHFIT_LM_MAX_POINTS points: uniform sampling in order.
    std::vector<Point3> pts;
    const size_t step = std::max<size_t>(1, (ptsIn.size() + MESHFIT_LM_MAX_POINTS - 1) / MESHFIT_LM_MAX_POINTS);
    for (size_t i = 0; i < ptsIn.size(); i += step) pts.push_back(ptsIn[i]);
    BBox3 bb;
    for (const auto& p : pts) bb.extend(p);
    const double scale = std::max(bb.diagonal().norm(), 1e-6);
    const int n = parameterCount(start.kind);
    const Eigen::MatrixXd B = Bin.size() ? Bin : Eigen::MatrixXd(Eigen::MatrixXd::Identity(n, n));
    const int k = int(B.cols());
    SurfaceSpec s = r.surface;
    const bool freeDir = Bin.size() == 0;
    double c = cost(s, pts);
    double lambda = 1e-3;
    for (int it = 0; it < MESHFIT_LM_MAX_ITER && k > 0; ++it) {
        if (freeDir && (s.kind == Kind::Plane || s.kind == Kind::Cylinder)) {  // pivot: projection of the points' center
            if (s.kind == Kind::Plane) s.point = s.point + (bb.center() - s.point) - (bb.center() - s.point).dot(s.dir) * s.dir;
            else s.point = s.point + (bb.center() - s.point).dot(s.dir) * s.dir;
        }
        const auto [e1, e2] = basis(s.dir);
        Eigen::VectorXd r0(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) r0(Eigen::Index(i)) = signedDistance(s, pts[i]);
        Eigen::MatrixXd J(pts.size(), k);
        for (int j = 0; j < k; ++j) {
            const bool angular = (B.col(j).head(std::min(2, n)).norm() > 0 && s.kind != Kind::Sphere) || (s.kind == Kind::Cone && B(5, j) != 0);
            const double h = angular ? 1e-7 : 1e-7 * scale;
            const SurfaceSpec sj = applyDelta(s, e1, e2, B.col(j) * h);
            for (size_t i = 0; i < pts.size(); ++i) J(Eigen::Index(i), j) = (signedDistance(sj, pts[i]) - r0(Eigen::Index(i))) / h;
        }
        const Eigen::MatrixXd A = J.transpose() * J;
        const Eigen::VectorXd g = J.transpose() * r0;
        bool improved = false;
        for (int q = 0; q < 12; ++q) {
            Eigen::MatrixXd M = A;
            for (int j = 0; j < k; ++j) M(j, j) += lambda * (A(j, j) + 1e-12);
            const Eigen::VectorXd d = M.ldlt().solve(-g);
            if (!d.allFinite()) break;
            const SurfaceSpec s2 = applyDelta(s, e1, e2, B * d);
            if (!admissible(s2)) {
                lambda *= 4;
                continue;
            }
            const double c2 = cost(s2, pts);
            if (c2 < c) {
                const double rel = (c - c2) / std::max(c, 1e-300);
                s = s2;
                c = c2;
                lambda = std::max(lambda / 3, 1e-12);
                improved = rel > 1e-14;
                break;
            }
            lambda *= 4;
        }
        if (!improved) break;
    }
    s.dir.normalize();
    r.surface = s;
    double mx = 0, sum = 0;
    for (const auto& p : ptsIn) {
        const double d = std::abs(signedDistance(s, p));
        mx = std::max(mx, d);
        sum += d * d;
    }
    r.maxDev = mx;
    r.rms = std::sqrt(sum / double(ptsIn.size()));
    r.ok = admissible(s);
    return r;
}

FitResult refineSurface(const SurfaceSpec& start, const std::vector<Point3>& ptsIn) {
    return refineConstrained(start, ptsIn, Eigen::MatrixXd());
}

FitResult refineLocked(const SurfaceSpec& start, const std::vector<Point3>& pts, Lock lock) {
    const int n = parameterCount(start.kind);
    const Vec3 d = start.dir.normalized();
    std::vector<Eigen::VectorXd> cols;
    auto unit = [&](int i) {
        Eigen::VectorXd v = Eigen::VectorXd::Zero(n);
        v(i) = 1;
        return v;
    };
    auto along = [&](int first) {  // point offset (parameters first..first+2) along the axis
        Eigen::VectorXd v = Eigen::VectorXd::Zero(n);
        v.segment(first, 3) = d;
        return v;
    };
    switch (start.kind) {
        case Kind::Plane: cols = {unit(2)}; break;
        case Kind::Sphere:
            if (lock == Lock::Dir) cols = {unit(0), unit(1), unit(2), unit(3)};
            else cols = {unit(3)};
            break;
        case Kind::Cylinder:
            if (lock == Lock::Dir) cols = {unit(2), unit(3), unit(4)};
            else cols = {unit(4)};
            break;
        case Kind::Cone:
            if (lock == Lock::Dir) cols = {unit(2), unit(3), unit(4), unit(5)};
            else if (lock == Lock::Axis) cols = {along(2), unit(5)};
            else cols = {along(2)};
            break;
        case Kind::Torus:
            if (lock == Lock::Dir) cols = {unit(2), unit(3), unit(4), unit(5), unit(6)};
            else cols = {along(2), unit(5), unit(6)};
            break;
        default: return {};
    }
    Eigen::MatrixXd B(n, Eigen::Index(cols.size()));
    for (size_t j = 0; j < cols.size(); ++j) B.col(Eigen::Index(j)) = cols[j];
    return refineConstrained(start, pts, B);
}

std::optional<SurfaceSpec> initialSurface(Kind kind, const std::vector<Point3>& pts, const std::vector<Vec3>& nrm) {
    std::optional<SurfaceSpec> init;
    switch (kind) {
        case Kind::Plane: init = initPlane(pts, nrm); break;
        case Kind::Sphere: init = initSphere(pts); break;
        case Kind::Cylinder: init = initCylinder(pts, nrm); break;
        case Kind::Cone: init = initCone(pts, nrm); break;
        case Kind::Torus: init = initTorus(pts, nrm); break;
        default: break;
    }
    if (init && !admissible(*init)) return std::nullopt;
    return init;
}

FitResult fitSurface(Kind kind, const std::vector<Point3>& pts, const std::vector<Vec3>& nrm) {
    auto init = initialSurface(kind, pts, nrm);
    if (!init) return {};
    return refineSurface(*init, pts);
}

// Canonical surface form (§4.4): axis direction by the a⁺ rule, except the cone (dir towards the opening) and the plane
// (dir is the outward normal); the cylinder axis point is the one closest to the coordinate origin.
SurfaceSpec canonical(const SurfaceSpec& in) {
    SurfaceSpec s = in;
    s.dir.normalize();
    auto plus = [](Vec3 a) {
        int k = 0;
        for (int i = 1; i < 3; ++i)
            if (std::abs(a[i]) > std::abs(a[k]) + 1e-12) k = i;
        return a[k] < 0 ? Vec3(-a) : a;
    };
    switch (s.kind) {
        case Kind::Cylinder:
            s.dir = plus(s.dir);
            s.point = s.point - s.point.dot(s.dir) * s.dir;
            break;
        case Kind::Torus:
        case Kind::Sphere: s.dir = plus(s.dir); break;
        default: break;
    }
    return s;
}

}  // namespace construe::meshfit
