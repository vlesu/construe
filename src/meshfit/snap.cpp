// Snapping surface parameters to axes, shared axes and "round" values (spec/mesh-fit.md §4.5).
#include "meshfit/meshfit_internal.h"

#include "base/constants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace construe::meshfit {

namespace {

double gridRound(double v) { return std::round(v / MESHFIT_SNAP_GRID) * MESHFIT_SNAP_GRID; }

bool hasAxis(Kind k) { return k == Kind::Cylinder || k == Kind::Cone || k == Kind::Torus; }
bool hasDir(Kind k) { return k != Kind::Sphere; }

// Direction with small components zeroed (|c| ≤ MESHFIT_SNAP_ANGLE).
Vec3 zeroSmall(const Vec3& d) {
    Vec3 r = d;
    for (int i = 0; i < 3; ++i)
        if (std::abs(r[i]) <= MESHFIT_SNAP_ANGLE) r[i] = 0;
    return r.norm() > 0 ? Vec3(r.normalized()) : d;
}

// Direction snapping (§4.5 A): small components are zeroed; for a direction in a coordinate plane (one zero
// component) the angle in that plane snaps to a multiple of MESHFIT_ANGLE_GRID if off by ≤ MESHFIT_SNAP_ANGLE.
Vec3 snapDir(const Vec3& d) {
    Vec3 r = zeroSmall(d);
    int zero = -1, zeros = 0;
    for (int i = 0; i < 3; ++i)
        if (r[i] == 0) zero = i, ++zeros;
    if (zeros != 1) return r;
    const int i1 = (zero + 1) % 3, i2 = (zero + 2) % 3;
    const double th = std::atan2(r[i2], r[i1]);
    const double step = MESHFIT_ANGLE_GRID * PI / 180;
    const double ts = std::round(th / step) * step;
    if (std::abs(ts - th) > MESHFIT_SNAP_ANGLE) return r;
    Vec3 q = Vec3::Zero();
    q[i1] = std::cos(ts);
    q[i2] = std::sin(ts);
    for (int i = 0; i < 3; ++i)
        if (std::abs(q[i]) < 1e-15) q[i] = 0;
    return q.normalized();
}

// Index of the world axis parallel to d (−1: parallel to none).
int worldAxis(const Vec3& d) {
    for (int i = 0; i < 3; ++i)
        if (std::abs(std::abs(d[i]) - 1) < 1e-15) return i;
    return -1;
}

struct Snapper {
    const FitContext& c;
    std::vector<Seg>& regs;
    std::vector<std::vector<Point3>> pts;
    double limit;                // axis clustering distance and the upper bound of a region's snapping deviation
    std::vector<double> allowed;  // per region: largest vertex deviation after snapping (§4.5)

    Snapper(const FitContext& ctx, std::vector<Seg>& r) : c(ctx), regs(r), limit(MESHFIT_SNAP_CHECK * ctx.eps) {
        for (const auto& g : regs) {
            std::vector<Point3> p;
            for (int v : regionVertices(*c.topo, g.tris)) p.push_back(c.topo->mesh->vertices[size_t(v)]);
            pts.push_back(std::move(p));
        }
        // Mesh noise bound (FX10-STL-001): a snap may make a region fit its vertices at most MESHFIT_SNAP_NOISE times
        // worse than its own fit (not below the float32 quantization of the coordinates): on an exact mesh a
        // non-round dimension (3.571875 → 3.572) is rejected although it is within 3ε; on a noisy mesh the bound
        // stays 3ε.
        const double quant = MESHFIT_SNAP_QUANT_REL * ctx.topo->maxCoord;
        for (size_t i = 0; i < regs.size(); ++i)
            allowed.push_back(std::min(limit, std::max(MESHFIT_SNAP_NOISE * dev(i, regs[i].surface), quant)));
    }
    double dev(size_t i, const SurfaceSpec& s) const {
        double m = 0;
        for (const auto& p : pts[i]) m = std::max(m, std::abs(signedDistance(s, p)));
        return m;
    }
    // Apply a set of new surfaces if all are within tolerance; otherwise change nothing.
    bool tryApply(const std::map<size_t, SurfaceSpec>& next) {
        for (const auto& [i, s] : next)
            if (!(dev(i, s) <= allowed[i])) return false;
        for (const auto& [i, s] : next) regs[i].surface = s;
        return true;
    }
    double area(size_t i) const {
        double a = 0;
        for (int t : regs[i].tris) a += c.topo->area[size_t(t)];
        return a;
    }
};

// Surface with a new direction (same sign) and the other parameters refined.
SurfaceSpec withDir(const SurfaceSpec& s, const Vec3& d, const std::vector<Point3>& pts) {
    SurfaceSpec t = s;
    // Pivot: projection of the points' center onto the axis or plane: rotating about it moves the surface less.
    Point3 m = Point3::Zero();
    for (const auto& p : pts) m += p;
    if (!pts.empty()) m /= double(pts.size());
    if (s.kind == Kind::Plane) t.point = m - signedDistance(s, m) * s.dir;
    else if (s.kind == Kind::Cylinder) t.point = s.point + (m - s.point).dot(s.dir) * s.dir;
    t.dir = s.dir.dot(d) >= 0 ? d : Vec3(-d);
    auto f = refineLocked(t, pts, Lock::Dir);
    return f.ok ? f.surface : t;
}

}  // namespace

void snapRegions(const FitContext& c, std::vector<Seg>& regs, bool grid) {
    Snapper sn(c, regs);
    const size_t n = regs.size();
    std::vector<size_t> byArea(n);
    for (size_t i = 0; i < n; ++i) byArea[i] = i;
    std::stable_sort(byArea.begin(), byArea.end(), [&](size_t a, size_t b) { return sn.area(a) > sn.area(b); });

    // A. Directions: zeroing small components and shared directions (clusters of parallel ones).
    std::vector<char> done(n, 0);
    const double cosTol = std::cos(MESHFIT_SNAP_ANGLE);
    for (size_t ii : byArea) {
        if (done[ii] || !hasDir(regs[ii].surface.kind)) continue;
        std::vector<size_t> cl;
        const Vec3 d0 = regs[ii].surface.dir;
        for (size_t j : byArea)
            if (!done[j] && hasDir(regs[j].surface.kind) && std::abs(regs[j].surface.dir.dot(d0)) >= cosTol) cl.push_back(j);
        Vec3 sum = Vec3::Zero();
        for (size_t j : cl) sum += sn.area(j) * (regs[j].surface.dir.dot(d0) >= 0 ? regs[j].surface.dir : Vec3(-regs[j].surface.dir));
        const Vec3 rep = snapDir(sum.normalized());
        std::map<size_t, SurfaceSpec> next;
        for (size_t j : cl) next[j] = withDir(regs[j].surface, rep, sn.pts[j]);
        if (!sn.tryApply(next))
            for (size_t j : cl) {  // the cluster does not fit: one by one, zeroing components only
                std::map<size_t, SurfaceSpec> one{{j, withDir(regs[j].surface, snapDir(regs[j].surface.dir), sn.pts[j])}};
                sn.tryApply(one);
            }
        for (size_t j : cl) done[j] = 1;
    }

    // B. Shared axes: coaxial cylinders, cones, tori (same direction, axes closer than tolerance); axis point on the grid.
    std::fill(done.begin(), done.end(), 0);
    for (size_t ii : byArea) {
        if (done[ii] || !hasAxis(regs[ii].surface.kind)) continue;
        const Vec3 d = regs[ii].surface.dir;
        auto perp = [&](const Point3& p) { return Point3(p - p.dot(d) * d); };
        const Point3 o0 = perp(regs[ii].surface.point);
        std::vector<size_t> cl;
        for (size_t j : byArea)
            if (!done[j] && hasAxis(regs[j].surface.kind) && std::abs(std::abs(regs[j].surface.dir.dot(d)) - 1) < 1e-12 &&
                (perp(regs[j].surface.point) - o0).norm() <= sn.limit)
                cl.push_back(j);
        Point3 o = Point3::Zero();
        double w = 0;
        for (size_t j : cl) o += sn.area(j) * perp(regs[j].surface.point), w += sn.area(j);
        o /= w;
        const int ax = worldAxis(d);
        Point3 og = o;
        if (ax >= 0 && grid)
            for (int k = 0; k < 3; ++k)
                if (k != ax) og[k] = gridRound(o[k]);
        for (const Point3& target : {og, o}) {
            std::map<size_t, SurfaceSpec> next;
            for (size_t j : cl) {
                SurfaceSpec s = regs[j].surface;
                s.point = target + s.point.dot(d) * d;
                auto f = refineLocked(s, sn.pts[j], Lock::Axis);
                next[j] = f.ok ? f.surface : s;
            }
            if (sn.tryApply(next)) break;
        }
        for (size_t j : cl) done[j] = 1;
    }

    // C. Dimensions: plane offsets, radii, center positions along the axis (only with the grid, §4.5).
    for (size_t i = 0; grid && i < n; ++i) {
        SurfaceSpec s = regs[i].surface;
        switch (s.kind) {
            case Kind::Plane: {
                // Offset along a world axis for a plane ⊥ axis; inclined: intersection with a world axis (by decreasing |n_k|).
                const double o = s.point.dot(s.dir);
                std::array<int, 3> ks{0, 1, 2};
                std::stable_sort(ks.begin(), ks.end(), [&](int a, int b) { return std::abs(s.dir[a]) > std::abs(s.dir[b]); });
                for (int k : ks) {
                    if (std::abs(s.dir[k]) < 1e-6) break;
                    const double x = o / s.dir[k];
                    SurfaceSpec t = s;
                    t.point += (gridRound(x) * s.dir[k] - o) * s.dir;
                    if (sn.tryApply({{i, t}})) break;
                }
                break;
            }
            case Kind::Cylinder: {
                s.radius = gridRound(s.radius);
                sn.tryApply({{i, s}});
                break;
            }
            case Kind::Sphere: {
                // Center on the shared axis if it passes nearby; center coordinates and radius on the grid.
                for (size_t j = 0; j < n; ++j) {
                    if (!hasAxis(regs[j].surface.kind)) continue;
                    const SurfaceSpec& a = regs[j].surface;
                    const Vec3 w = s.point - a.point;
                    const Vec3 off = w - w.dot(a.dir) * a.dir;
                    if (off.norm() <= sn.limit) {
                        SurfaceSpec t = s;
                        t.point = s.point - off;
                        if (sn.tryApply({{i, t}})) s = t;
                        break;
                    }
                }
                SurfaceSpec t = s;
                for (int k = 0; k < 3; ++k) t.point[k] = gridRound(t.point[k]);
                auto f = refineLocked(t, sn.pts[i], Lock::Axis);  // center fixed, radius refined
                if (f.ok) t = f.surface;
                if (sn.tryApply({{i, t}})) s = t;
                t = s;
                t.radius = gridRound(t.radius);
                sn.tryApply({{i, t}});
                break;
            }
            case Kind::Torus: {
                const double h = s.point.dot(s.dir);
                SurfaceSpec t = s;
                t.point += (gridRound(h) - h) * s.dir;
                auto f = refineLocked(t, sn.pts[i], Lock::Axis);
                if (f.ok) {
                    // refinement moves the center along the axis: put it back on the grid
                    t = f.surface;
                    const double h2 = t.point.dot(t.dir);
                    t.point += (gridRound(h2) - h2) * t.dir;
                }
                if (sn.tryApply({{i, t}})) s = t;
                t = s;
                t.radius = gridRound(t.radius);
                t.minorRadius = gridRound(t.minorRadius);
                sn.tryApply({{i, t}});
                break;
            }
            case Kind::Cone: {
                // Angle on the MESHFIT_ANGLE_GRID grid; then the apex position: radius at the level of an adjacent plane ⊥ axis
                // or the level at the radius of an adjacent coaxial cylinder on the grid; otherwise the apex level.
                const double step = MESHFIT_ANGLE_GRID * PI / 180;
                SurfaceSpec t = s;
                t.halfAngle = std::round(s.halfAngle / step) * step;
                auto f = refineLocked(t, sn.pts[i], Lock::AxisAngle);
                if (f.ok) t = f.surface;
                if (sn.tryApply({{i, t}})) s = t;
                std::set<size_t> nbs;
                for (int tri : regs[i].tris)
                    for (int nb : c.topo->nbr[size_t(tri)])
                        for (size_t j = 0; j < n; ++j)
                            if (j != i && std::binary_search(regs[j].tris.begin(), regs[j].tris.end(), nb)) nbs.insert(j);
                const Vec3 d = s.dir;
                const double ta = std::tan(s.halfAngle);
                const double ha = s.point.dot(d);
                std::vector<double> apexes;  // apex level candidates (along d)
                for (size_t j : nbs) {
                    const SurfaceSpec& q = regs[j].surface;
                    if (q.kind == Kind::Plane && std::abs(std::abs(q.dir.dot(d)) - 1) < 1e-12) {
                        const double hp = q.point.dot(d);
                        const double r = gridRound(std::abs(hp - ha) * ta);
                        apexes.push_back(hp - (hp >= ha ? 1 : -1) * r / ta);
                    }
                    if (q.kind == Kind::Cylinder && std::abs(std::abs(q.dir.dot(d)) - 1) < 1e-12) {
                        const double hc = gridRound(ha + q.radius / ta);
                        apexes.push_back(hc - q.radius / ta);
                    }
                }
                apexes.push_back(gridRound(ha));
                for (double a : apexes) {
                    SurfaceSpec u = s;
                    u.point += (a - ha) * d;
                    if (sn.tryApply({{i, u}})) break;
                }
                break;
            }
            default: break;
        }
    }
    for (auto& g : regs) g.surface = canonical(g.surface);
}

}  // namespace construe::meshfit
