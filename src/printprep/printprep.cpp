// 3D-print preparation of a body (printprep/printprep.h, spec/printprep.md).
#include "printprep/printprep.h"

#include "base/constants.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace construe::printprep {

namespace {

// Closed polygon profile from (u, ρ) points: origin + u·a + ρ·uh, in the plane spanned by a and uh (contains the axis).
kernel::Profile polygonProfile(const Point3& origin, const Vec3& a, const Vec3& uh, const std::vector<ProfilePoint>& pts) {
    kernel::Profile p;
    p.plane.origin = origin;
    p.plane.x = a;
    p.plane.y = uh;
    p.plane.normal = a.cross(uh).normalized();
    for (size_t i = 0; i < pts.size(); ++i) {
        const ProfilePoint& q0 = pts[i];
        const ProfilePoint& q1 = pts[(i + 1) % pts.size()];
        kernel::Segment3 s;
        s.id = "s" + std::to_string(i + 1);
        s.kind = kernel::Segment3::Kind::Line;
        s.p1 = origin + q0.u * a + q0.rho * uh;
        s.p2 = origin + q1.u * a + q1.rho * uh;
        p.outer.segments.push_back(s);
    }
    return p;
}

// Ring u ∈ [t0, t1], ρ ∈ [ra, rb] about the axis (§4.3 item 4): full revolution of a rectangle.
Result<kernel::Shape> ring(const kernel::Kernel& k, const Point3& c, const Vec3& a, const Vec3& uh, double t0, double t1,
                           double ra, double rb) {
    const auto p = polygonProfile(c, a, uh, {{t0, ra}, {t1, ra}, {t1, rb}, {t0, rb}});
    auto r = k.revolve(p, kernel::Axis{c, a}, kernel::RevolveExtent{kernel::RevolveExtent::Kind::Full, 0});
    if (!r) return r.error();
    return r->shape;
}

// Radius change r → r2 of a cylindrical surface on [t0, t1] (§4.3 item 4); the error detail is the kernel code or
// "radius".
Result<kernel::Shape> changeRadius(const kernel::Kernel& k, const kernel::Shape& shape, const Point3& c, const Vec3& a, const Vec3& uh,
                                   double t0, double t1, double r, double r2, bool hole, const Cancel* cancel) {
    if (std::abs(r2 - r) < PRINT_MIN_CHANGE) return shape;
    const double e = PRINT_AIR_MARGIN;
    if (!(r2 > 2 * e) || !(r > 2 * e)) return Error("radius");
    bool cut = false;
    double ra = 0, rb = 0;
    if (!hole) {
        if (r2 < r) cut = true, ra = r2, rb = r + e;
        else ra = r - e, rb = r2;
    } else {
        if (r2 > r) cut = true, ra = r - e, rb = r2;
        else ra = r2, rb = r + e;
    }
    auto tool = ring(k, c, a, uh, t0, t1, ra, rb);
    if (!tool) return Error(tool.error().code);
    auto b = k.boolean(cut ? kernel::BoolKind::Subtract : kernel::BoolKind::Union, shape, *tool, cancel);
    if (!b) return Error(b.error().code);
    return b->shape;
}

// Fit target geometry computed on the input shape (§4.3 item 2).
struct FitGeom {
    std::string id;
    Point3 c = Point3::Zero();
    Vec3 a = Vec3::UnitZ(), uh = Vec3::UnitX();
    double t0 = 0, t1 = 0, r = 0, r2 = 0, nominal = 0;
    bool hole = false;
};

// Whether a cylinder (c, a, r) on [t0, t1] is the surface of thread t (§4.3 item 2).
bool sameAsThread(const FitGeom& g, const ThreadTarget& t) {
    if (g.a.cross(t.dir).norm() > PRINT_SAME_TOL) return false;
    const Vec3 off = t.origin - g.c;
    if ((off - off.dot(g.a) * g.a).norm() > PRINT_SAME_TOL) return false;
    if (std::abs(g.r - t.radius) > PRINT_SAME_TOL) return false;
    const double s0 = off.dot(g.a), s1 = s0 + t.length * t.dir.dot(g.a);
    return std::min(g.t1, std::max(s0, s1)) - std::max(g.t0, std::min(s0, s1)) > PRINT_SAME_TOL;
}

}  // namespace

double fitTargetDiameter(double nominal, double upper, double lower, bool hole, double clearance) {
    return nominal + (upper + lower) / 2 + (hole ? clearance : -clearance);
}

Result<double> threadFieldDeviation(const std::string& field, double pitch, bool internal) {
    auto bad = [&] { return Error("PRINT_THREAD_FIELD").with("name", field); };
    if (field.size() != 2 || field[0] < '3' || field[0] > '9') return bad();
    const char l = field[1];
    const double p11 = 11 * pitch;
    if (internal) {
        if (l == 'H') return 0.0;
        if (l == 'G') return (15 + p11) / 1000;
        return bad();
    }
    switch (l) {
    case 'h': return 0.0;
    case 'g': return -(15 + p11) / 1000;
    case 'f': return -(30 + p11) / 1000;
    case 'e': return -(50 + p11) / 1000;
    default: return bad();
    }
}

ThreadDiameters basicThreadDiameters(double d, double pitch) {
    const double s3 = std::sqrt(3.0);
    return {d, d - 3 * s3 / 8 * pitch, d - 5 * s3 / 8 * pitch};
}

Result<ThreadDiameters> printThreadDiameters(double d, double pitch, bool internal, const Options& options) {
    auto dev = threadFieldDeviation(internal ? options.internalThreadField : options.externalThreadField, pitch, internal);
    if (!dev) return dev.error();
    const double s = internal ? *dev + options.clearance : *dev - options.clearance;
    const auto b = basicThreadDiameters(d, pitch);
    return ThreadDiameters{b.major + s, b.pitch + s, b.minor + s};
}

double threadAirMargin(double pitch) { return std::min(PRINT_AIR_MARGIN, PRINT_THREAD_AIR_REL * pitch); }

std::vector<ProfilePoint> grooveProfile(double P, double minor, double major, bool internal) {
    // Flanks at 30° to the radius continue past the blank surface into the air by e: the groove crosses the blank
    // cylinder transversally (no profile vertex on it).
    const double r1 = minor / 2, rM = major / 2, e = threadAirMargin(P), t = 1 / std::sqrt(3.0);
    if (!internal) {
        const double w = P / 8 + (rM + e - r1) * t;
        return {{-P / 8, r1}, {P / 8, r1}, {w, rM + e}, {-w, rM + e}};
    }
    const double w = 3 * P / 8 + e * t;
    return {{-w, r1 - e}, {w, r1 - e}, {P / 16, rM}, {-P / 16, rM}};
}

bool threadOnBody(const kernel::Kernel& k, const kernel::Shape& body, const ThreadTarget& t) {
    const Vec3 u = t.dir.unitOrthogonal(), v = t.dir.cross(u);
    const Point3 mid = t.origin + (t.length / 2) * t.dir;
    for (int i = 0; i < 4; ++i) {
        const double phi = PI / 2 * i;
        if (k.distanceToShape(body, mid + t.radius * (std::cos(phi) * u + std::sin(phi) * v)) <= PRINT_SAME_TOL) return true;
    }
    return false;
}

Result<Prepared> prepareBody(const kernel::Kernel& k, const kernel::Shape& body, const std::vector<FitTarget>& fits,
                             const std::vector<ThreadTarget>& threads, const Options& options, const Cancel* cancel) {
    const double c = options.clearance;
    if (!(c >= 0 && c <= PRINT_MAX_CLEARANCE)) return Error("PRINT_INVALID_CLEARANCE").with("actual", c);
    Prepared out;
    // 2. Fit targets on the input shape (face indices change after the first boolean).
    std::vector<FitGeom> geoms;
    for (const auto& f : fits) {
        const auto si = k.faceSurface(body, f.face);
        if (si.kind != kernel::SurfaceInfo::Kind::Cylinder) {
            out.warnings.push_back(Error("PRINT_FIT_SKIPPED").with("id", f.id).with("detail", std::string("surface")));
            continue;
        }
        FitGeom g;
        g.id = f.id;
        g.c = si.point;
        g.a = si.dir.normalized();
        g.uh = g.a.unitOrthogonal();
        g.r = si.radius;
        g.hole = k.faceNormalAt(body, f.face, si.point + g.r * g.uh).dot(g.uh) < 0;
        const auto range = kernel::axialRange(k, body, f.face, g.c, g.a);
        if (!range) {
            out.warnings.push_back(Error("PRINT_FIT_SKIPPED").with("id", f.id).with("detail", std::string("extent")));
            continue;
        }
        g.t0 = range->first, g.t1 = range->second;
        if (std::any_of(threads.begin(), threads.end(), [&](const ThreadTarget& t) { return sameAsThread(g, t); })) {
            out.warnings.push_back(Error("PRINT_FIT_SKIPPED").with("id", f.id).with("detail", std::string("thread")));
            continue;
        }
        g.nominal = 2 * g.r;
        g.r2 = fitTargetDiameter(g.nominal, f.upper, f.lower, g.hole, c) / 2;
        geoms.push_back(g);
    }
    // 3. Thread diameters.
    std::vector<ThreadDiameters> dims;
    for (const auto& t : threads) {
        auto d = printThreadDiameters(t.d, t.pitch, t.internal, options);
        if (!d) return d.error();
        // Free thread with its own profile height (§4.2 item 3, FX7-PT-003): the depth of the turns, from the crests.
        if (t.height > 0) {
            if (t.internal) d->major = d->minor + 2 * t.height;
            else d->minor = d->major - 2 * t.height;
        }
        dims.push_back(*d);
    }
    // 5. Fits, then threads; each operation on the result of the previous one.
    kernel::Shape shape = body;
    auto failed = [](const char* code, const std::string& id, const Error& e) -> Error {
        if (e.code == "KERNEL_CANCELLED") return e;
        return Error(code).with("id", id).with("detail", e.code);
    };
    for (const auto& g : geoms) {
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        auto s = changeRadius(k, shape, g.c, g.a, g.uh, g.t0, g.t1, g.r, g.r2, g.hole, cancel);
        if (!s) return failed("PRINT_FIT_FAILED", g.id, s.error());
        shape = *s;
        out.changes.push_back({Change::Kind::Fit, g.id, g.hole, g.nominal, {2 * g.r2}});
    }
    for (size_t i = 0; i < threads.size(); ++i) {
        if (cancelled(cancel)) return Error("KERNEL_CANCELLED");
        const ThreadTarget& t = threads[i];
        const ThreadDiameters& dd = dims[i];
        const double P = t.pitch, L = t.length;
        const Vec3 a = t.dir.normalized(), uh = a.unitOrthogonal();
        const double r1 = dd.minor / 2, rM = dd.major / 2;
        // 5.2. Groove extent: past an end with air beyond it — fully out, otherwise the groove stops inside (§4.2 item 5).
        auto pts = grooveProfile(P, dd.minor, dd.major, t.internal);
        double w = 0;
        for (const auto& q : pts) w = std::max(w, std::abs(q.u));
        const double et = threadAirMargin(P);
        auto air = [&](double u, double side) {
            const Point3 q = t.origin + (u + side * P / 2) * a + (r1 + rM) / 2 * uh;
            const Vec3 v = a.cross(uh);
            for (const Vec3& d : {uh, v, Vec3(-uh), Vec3(-v)})
                if (!k.rayHitsShape(shape, q, d, 0)) return true;
            return false;
        };
        const double us = air(0, -1) ? -(w + et) : w, ue = air(L, 1) ? L + w + et : L - w;
        const double turns = (ue - us) / P;
        if (!(turns > 0)) return Error("PRINT_THREAD_FAILED").with("id", t.feature).with("detail", std::string("length"));
        if (!(turns <= PRINT_THREAD_MAX_TURNS)) return Error("PRINT_THREAD_FAILED").with("id", t.feature).with("detail", std::string("turns"));
        // 5.1. Blank: crests of the bolt turns or the bore under the nut turns.
        auto blank = changeRadius(k, shape, t.origin, a, uh, 0, L, t.radius, t.internal ? r1 : rM, t.internal, cancel);
        if (!blank) return failed("PRINT_THREAD_FAILED", t.feature, blank.error());
        // 5.3. Groove: helical sweep of the profile centred at u = us over (ue − us)/P turns.
        for (auto& q : pts) q.u += us;
        auto helix = k.helicalSweep(polygonProfile(t.origin, a, uh, pts), kernel::Axis{t.origin, a}, P, turns, cancel);
        if (!helix) return failed("PRINT_THREAD_FAILED", t.feature, helix.error());
        // 5.4. Blank minus the groove.
        auto cut = k.boolean(kernel::BoolKind::Subtract, *blank, helix->shape, cancel);
        if (!cut) return failed("PRINT_THREAD_FAILED", t.feature, cut.error());
        shape = cut->shape;
        out.changes.push_back({Change::Kind::Thread, t.feature, t.internal, t.d, {dd.major, dd.pitch, dd.minor}});
    }
    out.shape = shape;
    return out;
}

}  // namespace construe::printprep
