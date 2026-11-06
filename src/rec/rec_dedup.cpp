// Identical bodies up to a proper rigid motion: fast signature, rotation candidates (principal axes, point pairs),
// point correspondence, Kabsch refinement and the §6 match check (spec/reconstruction.md §5.7; FX7-IO-001).
#include "rec/recognize.h"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>

namespace construe::rec {

namespace {

struct Signature {
    double volume = 0, area = 0;
    Point3 centroid = Point3::Zero();
    Vec3 moments = Vec3::Zero();               // λ₁ ≤ λ₂ ≤ λ₃
    Eigen::Matrix3d axes = Eigen::Matrix3d::Identity();  // e₁, e₂, e₃ as columns, right-handed
    std::array<int, 4> counts{};               // solids, faces, logical edges, principal vertices
    std::array<int, 6> kinds{};                // faces per surface kind
    std::vector<Point3> points;                // P(S): principal vertices, or face points if there are none
};

Signature signatureOf(const kernel::Kernel& k, const kernel::Shape& s) {
    Signature g;
    const auto mp = k.massProperties(s);
    g.volume = mp.volume;
    g.area = mp.area;
    g.centroid = mp.centroid;
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(mp.inertia);
    g.moments = es.eigenvalues();
    g.axes = es.eigenvectors();
    if (g.axes.determinant() < 0) g.axes.col(2) = -g.axes.col(2);
    const auto ct = kernel::canonicalTopology(k, s);
    const auto c = k.counts(s);
    g.counts = {c.solids, c.faces, int(ct.logicalEdges.size()), int(ct.essentialVertices.size())};
    for (int f = 0; f < c.faces; ++f) g.kinds[size_t(k.faceSurface(s, f).kind)]++;
    for (int v : ct.essentialVertices) g.points.push_back(k.vertexPoint(s, v));
    if (g.points.empty())
        for (int f = 0; f < c.faces; ++f) g.points.push_back(k.closestPointOnFace(s, f, k.faceCentroid(s, f)));
    return g;
}

// §5.7 item 1.
bool similar(const Signature& a, const Signature& b) {
    if (a.counts != b.counts || a.kinds != b.kinds || a.points.size() != b.points.size()) return false;
    if (std::abs(a.volume - b.volume) > REC_DUP_REL * std::abs(a.volume)) return false;
    if (std::abs(a.area - b.area) > REC_DUP_REL * a.area) return false;
    const double scale = std::abs(a.moments.z());
    for (int i = 0; i < 3; ++i)
        if (std::abs(a.moments[i] - b.moments[i]) > REC_DUP_REL * scale) return false;
    return true;
}

// F(u, v): right-handed orthonormal basis [û, ŵ, û×ŵ] (§5.7 item 2б).
Eigen::Matrix3d frame(const Vec3& u, const Vec3& v) {
    const Vec3 e1 = u.normalized();
    const Vec3 e2 = (v - v.dot(e1) * e1).normalized();
    Eigen::Matrix3d f;
    f.col(0) = e1;
    f.col(1) = e2;
    f.col(2) = e1.cross(e2);
    return f;
}

// §5.7 item 2: rotation candidates, at most REC_DUP_MAX_CAND.
std::vector<Eigen::Matrix3d> candidates(const Signature& A, const Signature& B) {
    std::vector<Eigen::Matrix3d> out;
    for (const Vec3 s : {Vec3(1, 1, 1), Vec3(1, -1, -1), Vec3(-1, 1, -1), Vec3(-1, -1, 1)})
        out.push_back(B.axes * s.asDiagonal() * A.axes.transpose());
    std::vector<Vec3> a, b;
    for (const auto& p : A.points) a.push_back(p - A.centroid);
    for (const auto& p : B.points) b.push_back(p - B.centroid);
    auto multiplicity = [&](double r) {
        int m = 0;
        for (const auto& p : a) m += std::abs(p.norm() - r) <= REC_DUP_DIST ? 1 : 0;
        return m;
    };
    int i1 = -1, best = 0;
    for (int i = 0; i < int(a.size()); ++i) {
        const double r = a[size_t(i)].norm();
        if (r <= REC_DUP_DIST) continue;
        const int m = multiplicity(r);
        if (i1 < 0 || m < best || (m == best && r > a[size_t(i1)].norm())) i1 = i, best = m;
    }
    if (i1 < 0) return out;
    const Vec3 p1 = a[size_t(i1)], axis = p1.normalized();
    int i2 = -1;
    double far = REC_DUP_DIST;
    for (int i = 0; i < int(a.size()); ++i) {
        const double d = (a[size_t(i)] - a[size_t(i)].dot(axis) * axis).norm();
        if (d > far) far = d, i2 = i;
    }
    if (i2 < 0) return out;
    const Vec3 p2 = a[size_t(i2)];
    const Eigen::Matrix3d fa = frame(p1, p2);
    const double r1 = p1.norm(), r2 = p2.norm(), d12 = (p1 - p2).norm();
    for (size_t j1 = 0; j1 < b.size() && int(out.size()) < REC_DUP_MAX_CAND; ++j1) {
        if (std::abs(b[j1].norm() - r1) > REC_DUP_DIST) continue;
        for (size_t j2 = 0; j2 < b.size() && int(out.size()) < REC_DUP_MAX_CAND; ++j2) {
            if (j2 == j1 || std::abs(b[j2].norm() - r2) > REC_DUP_DIST || std::abs((b[j1] - b[j2]).norm() - d12) > REC_DUP_DIST) continue;
            out.push_back(frame(b[j1], b[j2]) * fa.transpose());
        }
    }
    return out;
}

kernel::Motion toMotion(const Eigen::Matrix3d& R, const Vec3& t) {
    kernel::Motion m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m.r[size_t(3 * i + j)] = R(i, j);
    m.t = t;
    return m;
}

std::optional<kernel::Motion> match(const kernel::Kernel& k, const kernel::Shape& a, const Signature& A, const kernel::Shape& b,
                                    const Signature& B) {
    if (!similar(A, B)) return std::nullopt;
    for (const auto& R0 : candidates(A, B)) {
        Eigen::Matrix3d R = R0;
        Vec3 t = B.centroid - R * A.centroid;
        // §5.7 item 3: one-to-one point correspondence.
        std::vector<int> pair(A.points.size(), -1);
        std::vector<bool> used(B.points.size(), false);
        bool ok = true;
        for (size_t i = 0; i < A.points.size() && ok; ++i) {
            const Point3 q = R * A.points[i] + t;
            int best = -1;
            double bd = REC_DUP_DIST;
            for (size_t j = 0; j < B.points.size(); ++j) {
                const double d = (B.points[j] - q).norm();
                if (!used[j] && d <= bd) bd = d, best = int(j);
            }
            if (best < 0) ok = false;
            else pair[i] = best, used[size_t(best)] = true;
        }
        if (!ok) continue;
        // §5.7 item 4: Kabsch refinement over the correspondence (proper rotation only).
        if (A.points.size() >= 3) {
            Point3 ca = Point3::Zero(), cb = Point3::Zero();
            for (size_t i = 0; i < A.points.size(); ++i) ca += A.points[i], cb += B.points[size_t(pair[i])];
            ca /= double(A.points.size());
            cb /= double(A.points.size());
            Eigen::Matrix3d H = Eigen::Matrix3d::Zero();
            for (size_t i = 0; i < A.points.size(); ++i) H += (A.points[i] - ca) * (B.points[size_t(pair[i])] - cb).transpose();
            Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
            const Vec3 sv = svd.singularValues();
            if (sv(1) > REC_DUP_DIST * REC_DUP_DIST * sv(0)) {  // not collinear
                Eigen::Matrix3d V = svd.matrixV();
                if ((V * svd.matrixU().transpose()).determinant() < 0) V.col(2) = -V.col(2);
                R = V * svd.matrixU().transpose();
                t = cb - R * ca;
            }
        }
        // §5.7 item 5: the §6 match check of the moved copy.
        const kernel::Motion m = toMotion(R, t);
        auto moved = k.transform(a, m);
        if (moved && bodiesMatch(k, moved->shape, b)) return m;
    }
    return std::nullopt;
}

}  // namespace

std::optional<kernel::Motion> congruentMotion(const kernel::Kernel& k, const kernel::Shape& a, const kernel::Shape& b) {
    if (a.null() || b.null()) return std::nullopt;
    return match(k, a, signatureOf(k, a), b, signatureOf(k, b));
}

std::vector<CongruenceEntry> groupCongruent(const kernel::Kernel& k, const std::vector<kernel::Shape>& shapes) {
    std::vector<CongruenceEntry> out(shapes.size());
    std::vector<Signature> sig;
    sig.reserve(shapes.size());
    for (const auto& s : shapes) sig.push_back(s.null() ? Signature{} : signatureOf(k, s));
    std::vector<int> reps;
    for (size_t i = 0; i < shapes.size(); ++i) {
        out[i].group = int(i);
        if (!shapes[i].null())
            for (int r : reps)
                if (auto m = match(k, shapes[size_t(r)], sig[size_t(r)], shapes[i], sig[i])) {
                    out[i] = {r, *m};
                    break;
                }
        if (out[i].group == int(i)) reps.push_back(int(i));
    }
    return out;
}

}  // namespace construe::rec
