// Assembly mates (regen/mates.h, spec/assembly.md §7).
#include "regen/mates.h"

#include "regen/naming.h"
#include "regen/regen.h"
#include "sketch/sketch.h"
#include "solver/system.h"
#include "base/constants.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <cctype>
#include <array>
#include <cstdio>
#include <deque>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace construe::regen {

namespace {

// assembly.md §8.
constexpr double ANGLE_PARALLEL_EPS = 1e-9;   // rad: angle θ' closer to 0 or π is solved as parallel
constexpr double MATE_ANGLE_START_SIN = 1e-3;  // |w_A × w_B| below — angular mate starts from parallel directions
constexpr double MATE_PARALLEL_START_COS = 1e-3;  // |w_A·w_B| below — a parallel-type mate starts from perpendicular directions
constexpr int MATE_SHAKE_TRIES = 6;               // deterministic perturbations after a failed solve (§7.5, FX10-ASM-002)
constexpr double MATE_SHAKE_ANGLE = 0.05;         // rad: rotation of each perturbation
constexpr double MATE_SIDE_EPS = 1e-6;  // |w_A·w_B| below — the side is undefined by the placement: the default one
constexpr int PULL_ITER_MAX = 30;
constexpr double PULL_NULL_TOL = 1e-8;
constexpr double PULL_STEP_MAX = 0.25;  // × L
constexpr double PULL_STOP = 1e-9;      // × L
constexpr int PULL_HALVINGS = 6;

// ---------------------------------------------------------------- dual numbers (≤ 12 equation parameters)

constexpr int N = 12;
struct D {
    double v = 0;
    std::array<double, N> d{};
    D() = default;
    D(double x) : v(x) {}  // NOLINT: constant
};
D operator+(const D& a, const D& b) {
    D r(a.v + b.v);
    for (int i = 0; i < N; ++i) r.d[i] = a.d[i] + b.d[i];
    return r;
}
D operator-(const D& a, const D& b) {
    D r(a.v - b.v);
    for (int i = 0; i < N; ++i) r.d[i] = a.d[i] - b.d[i];
    return r;
}
D operator-(const D& a) { return D(0.0) - a; }
D operator*(const D& a, const D& b) {
    D r(a.v * b.v);
    for (int i = 0; i < N; ++i) r.d[i] = a.d[i] * b.v + a.v * b.d[i];
    return r;
}
D operator/(const D& a, const D& b) {
    D r(a.v / b.v);
    for (int i = 0; i < N; ++i) r.d[i] = (a.d[i] * b.v - a.v * b.d[i]) / (b.v * b.v);
    return r;
}
D dsqrt(const D& a) {
    D r(std::sqrt(a.v));
    const double k = r.v > 0 ? 0.5 / r.v : 0.0;
    for (int i = 0; i < N; ++i) r.d[i] = a.d[i] * k;
    return r;
}
D dsin(const D& a) {
    D r(std::sin(a.v));
    const double c = std::cos(a.v);
    for (int i = 0; i < N; ++i) r.d[i] = a.d[i] * c;
    return r;
}
D dcos(const D& a) {
    D r(std::cos(a.v));
    const double s = -std::sin(a.v);
    for (int i = 0; i < N; ++i) r.d[i] = a.d[i] * s;
    return r;
}

D datan2(const D& y, const D& x) {
    D r(std::atan2(y.v, x.v));
    const double q = x.v * x.v + y.v * y.v;
    if (q > 0)
        for (int i = 0; i < N; ++i) r.d[i] = (x.v * y.d[i] - y.v * x.d[i]) / q;
    return r;
}

using V = std::array<D, 3>;
V vec(const Vec3& x) { return {D(x.x()), D(x.y()), D(x.z())}; }
V add(const V& a, const V& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V sub(const V& a, const V& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V mul(const V& a, const D& s) { return {a[0] * s, a[1] * s, a[2] * s}; }
D dot(const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V& a, const V& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
D norm(const V& a) { return dsqrt(dot(a, a)); }

// Rotating vector x by rotation vector w (Rodrigues' formula; a series near zero).
V rotate(const V& w, const V& x) {
    const D th2 = dot(w, w);
    if (th2.v < 1e-16) return add(add(x, cross(w, x)), mul(cross(w, cross(w, x)), D(0.5)));
    const D th = dsqrt(th2);
    const V k = mul(w, D(1.0) / th);
    const D c = dcos(th), s = dsin(th);
    return add(add(mul(x, c), mul(cross(k, x), s)), mul(k, dot(k, x) * (D(1.0) - c)));
}

// Rim angles of a gear pair (§7.4, FX10-GEAR-004): α — the angle about n = W_A from the line of centres c (P_B − P_A
// without its n component) to the rim's reference direction U.
std::array<D, 2> gearAnglesD(const V& PA, const V& WA, const V& UA, const V& PB, const V& UB) {
    const V d = sub(PB, PA);
    const V c = sub(d, mul(WA, dot(d, WA)));
    return {datan2(dot(cross(c, UA), WA), dot(c, UA)), datan2(dot(cross(c, UB), WA), dot(c, UB))};
}

// Value wrapped to (−π, π]; the derivative is unchanged.
D wrapPi(const D& x) {
    const double k = std::ceil((x.v - PI) / (2 * PI));
    return x - D(2 * PI * k);
}

// ---------------------------------------------------------------- primitives and references

// Point in the frame of the top-level instance, or world for source geometry.
using Prim = MatePrimitive;

struct Ref {
    int inst = -1;   // index of the top-level instance in the list of unknowns/constants; −1 — assembly source geometry
    std::string top; // path of the body of the system (top-level instance or free unit of a nested assembly, §2б)
    Prim prim;       // in the frame of that body or world
    std::string leaf;  // entry carrying the geometry (empty — the assembly frame) and the primitive in its frame (§2б)
    Prim local;
};

// Orthogonal pair (u, v) to unit n (§7.4).
std::pair<Vec3, Vec3> orthoPair(const Vec3& n) {
    int i = 0;  // basis vector least parallel to n (on a tie — the lower index)
    if (std::abs(n.y()) < std::abs(n[i])) i = 1;
    if (std::abs(n.z()) < std::abs(n[i])) i = 2;
    const Vec3 e = Vec3::Unit(i);
    const Vec3 u = n.cross(e).normalized();
    return {u, n.cross(u)};
}

// Reference direction of a gear rim (§7.4, FX10-GEAR-004): orthoPair of its axis direction with rounding noise removed
// (components below 1e-9 set to zero) — the direction in the instance frame passes through placements and carries noise
// that would otherwise switch the basis vector chosen by orthoPair from one solve to the next.
Vec3 rimReference(const Vec3& w) {
    Vec3 c = w.normalized();
    for (int i = 0; i < 3; ++i)
        if (std::abs(c[i]) < 1e-9) c[i] = 0;
    return orthoPair(c.normalized()).first;
}

Motion3 inverse(const Motion3& m) { return {m.R.transpose(), -(m.R.transpose() * m.t)}; }

}  // namespace

std::optional<int> mateFace(const kernel::Kernel& k, const PartState& state, const std::string& name) {
    const auto rr = naming::resolve(name, naming::EntityKind::Face, state.names);
    if (naming::refFate(rr, false) == naming::RefFate::Resolved) return rr.entities.front().index;
    if (rr.error || rr.status != naming::ResolveStatus::Split || rr.entities.empty()) return std::nullopt;
    // A split face (§7.2, FX10-LIB-009): every part is the same cylinder, cone or plane as the first.
    using Kind = kernel::SurfaceInfo::Kind;
    auto surf = [&](int i) {
        const auto& fe = state.names.faces.at(size_t(i));
        const kernel::Shape& body = state.bodies.at(size_t(fe.body)).shape;
        kernel::SurfaceInfo si = k.faceSurface(body, fe.face);
        if (si.kind == Kind::Plane) {
            si.point = k.faceCentroid(body, fe.face);
            si.dir = k.faceNormalAt(body, fe.face, si.point);
        }
        si.dir = si.dir.normalized();
        return si;
    };
    const kernel::SurfaceInfo first = surf(rr.entities.front().index);
    if (first.kind != Kind::Plane && first.kind != Kind::Cylinder && first.kind != Kind::Cone) return std::nullopt;
    for (const auto& e : rr.entities) {
        const kernel::SurfaceInfo si = surf(e.index);
        if (si.kind != first.kind) return std::nullopt;
        const Vec3 d = si.point - first.point;
        if (first.kind == Kind::Plane) {
            if (si.dir.dot(first.dir) < 1 - ASM_SPLIT_TOL || std::abs(d.dot(first.dir)) > ASM_SPLIT_TOL) return std::nullopt;
        } else {
            if (std::abs(si.dir.dot(first.dir)) < 1 - ASM_SPLIT_TOL || (d - first.dir * d.dot(first.dir)).norm() > ASM_SPLIT_TOL) return std::nullopt;
            if (first.kind == Kind::Cylinder && std::abs(si.radius - first.radius) > ASM_SPLIT_TOL) return std::nullopt;
        }
    }
    return rr.entities.front().index;
}

namespace {

// Primitive of a stable name in part coordinates (§7.2); err — ASM_MATE_BAD_REF for unsupported geometry (with `ref`).
std::optional<Prim> primitiveOf(const kernel::Kernel& k, const PartState& state, const std::string& name, std::optional<Error>& err,
                                const std::string& ref) {
    auto pn = naming::parse(name);
    if (!pn) return std::nullopt;
    int idx = -1;
    if (pn->kind == naming::EntityKind::Face) {
        const auto f = mateFace(k, state, name);
        if (!f) return std::nullopt;
        idx = *f;
    } else {
        const auto rr = naming::resolve(name, pn->kind, state.names);
        if (naming::refFate(rr, false) != naming::RefFate::Resolved) return std::nullopt;
        idx = rr.entities.front().index;
    }
    Prim local;  // in part coordinates
    if (pn->kind == naming::EntityKind::Face) {
        const auto& fe = state.names.faces.at(size_t(idx));
        const kernel::Shape& body = state.bodies.at(size_t(fe.body)).shape;
        const auto si = k.faceSurface(body, fe.face);
        if (si.kind == kernel::SurfaceInfo::Kind::Plane) {
            const Point3 c = k.faceCentroid(body, fe.face);
            local = {Prim::Kind::Plane, c, k.faceNormalAt(body, fe.face, c).normalized(), 0};
        } else if (si.kind == kernel::SurfaceInfo::Kind::Cylinder) {
            local = {Prim::Kind::Axis, si.point, si.dir.normalized(), si.radius};
        } else if (si.kind == kernel::SurfaceInfo::Kind::Cone) {
            local = {Prim::Kind::Axis, si.point, si.dir.normalized(), 0};
        } else {
            err = Error("ASM_MATE_BAD_REF").with("ref", ref);
            return std::nullopt;
        }
    } else if (pn->kind == naming::EntityKind::Edge) {
        const auto& ee = state.names.edges.at(size_t(idx));
        const auto ci = k.edgeCurve(state.bodies.at(size_t(ee.body)).shape, ee.logical);
        if (ci.kind == kernel::CurveInfo::Kind::Line) local = {Prim::Kind::Line, ci.p1, (ci.p2 - ci.p1).normalized(), 0};
        else if (ci.kind == kernel::CurveInfo::Kind::Circle) local = {Prim::Kind::Axis, ci.center, ci.axis.normalized(), 0};
        else {
            err = Error("ASM_MATE_BAD_REF").with("ref", ref);
            return std::nullopt;
        }
    } else {
        const auto& ve = state.names.vertices.at(size_t(idx));
        local = {Prim::Kind::Point, k.vertexPoint(state.bodies.at(size_t(ve.body)).shape, ve.vertex), Vec3::UnitZ(), 0};
    }
    return local;
}

// Primitive of a construction element of a part (§7.2, FX9-ASM-001) in part coordinates: source geometry, reference
// planes and axes, sketches and their points, lines, circles and arcs; nullopt — not found.
std::optional<Prim> constructionPrimitive(const InstanceEntry& leaf, const std::string& name, std::optional<Error>& err, const std::string& ref) {
    const PartState& state = *leaf.state;
    if (name == "origin") return Prim{Prim::Kind::Point, Vec3::Zero(), Vec3::UnitZ(), 0};
    if (name.rfind("plane:", 0) == 0) {
        auto pl = sketch::datumPlane(name);
        if (!pl) return std::nullopt;
        return Prim{Prim::Kind::Plane, pl->origin, pl->normal, 0};
    }
    if (name.rfind("axis:", 0) == 0) {
        if (name != "axis:x" && name != "axis:y" && name != "axis:z") return std::nullopt;
        return Prim{Prim::Kind::Axis, Vec3::Zero(), sketch::datumAxis(name).value_or(Vec3::UnitZ()), 0};
    }
    if (auto it = state.planes.find(name); it != state.planes.end()) return Prim{Prim::Kind::Plane, it->second.origin, it->second.normal.normalized(), 0};
    if (auto it = state.axes.find(name); it != state.axes.end()) return Prim{Prim::Kind::Axis, it->second.point, it->second.dir.normalized(), 0};
    // Pitch cylinder of a gear rim (FX10-GEAR-003): signed pitch radius, negative for an internal rim (concave).
    for (const auto& g : state.gears)
        if (g.feature == name) {
            Prim p{Prim::Kind::Axis, g.origin, g.dir.normalized(), g.internal ? -g.d / 2 : g.d / 2};
            p.teeth = g.teeth;  // the gear mate couples rotations by it (FX10-GEAR-004)
            return p;
        }
    const size_t colon = name.find(':');
    const auto sk = state.sketches.find(name.substr(0, colon));
    if (sk == state.sketches.end()) return std::nullopt;
    const Plane& fr = sk->second.frame;
    if (colon == std::string::npos) return Prim{Prim::Kind::Plane, fr.origin, fr.normal.normalized(), 0};
    const std::string ent = name.substr(colon + 1);
    const auto& P = sk->second.result.state.points;
    if (auto it = P.find(ent); it != P.end()) return Prim{Prim::Kind::Point, fr.toWorld(it->second), Vec3::UnitZ(), 0};
    if (!leaf.sketchEntities) return std::nullopt;
    const auto es = leaf.sketchEntities->find(sk->first);
    if (es == leaf.sketchEntities->end()) return std::nullopt;
    const auto en = es->second.find(ent);
    if (en == es->second.end()) return std::nullopt;
    const auto& v = en->second;  // kind and point IDs
    for (size_t q = 1; q < v.size(); ++q)
        if (!P.count(v[q])) return std::nullopt;
    if (v[0] == "line") {
        const Point3 a = fr.toWorld(P.at(v[1])), b = fr.toWorld(P.at(v[2]));
        if ((b - a).norm() < kernel::KERNEL_MIN_DIMENSION) {
            err = Error("ASM_MATE_BAD_REF").with("ref", ref);
            return std::nullopt;
        }
        return Prim{Prim::Kind::Line, a, (b - a).normalized(), 0};
    }
    const auto& radii = sk->second.result.state.radii;
    const auto rit = radii.find(ent);
    const double r = rit != radii.end() ? rit->second : v[0] == "arc" ? (P.at(v[2]) - P.at(v[1])).norm() : 0;
    return Prim{Prim::Kind::Axis, fr.toWorld(P.at(v[1])), fr.normal.normalized(), r};
}

// Primitive of a reference plane, axis or visible sketch (its plane, point, line, circle or arc) of the nested assembly
// carried by entry `node` (§7.2, FX9-ASM-014), in the frame of that entry; nullopt — not found.
std::optional<Prim> nestedDatumPrimitive(const AssemblyState& st, const std::string& node, const std::string& name,
                                         std::optional<Error>& err, const std::string& ref) {
    const size_t colon = name.find(':');
    const std::string fid = name.substr(0, colon);
    for (const auto& d : st.datums) {
        if (d.node != node || d.id != fid) continue;
        if (d.kind == AssemblyDatum::Kind::Plane && colon == std::string::npos)
            return Prim{Prim::Kind::Plane, d.plane.origin, d.plane.normal.normalized(), 0};
        if (d.kind == AssemblyDatum::Kind::Axis && colon == std::string::npos) return Prim{Prim::Kind::Axis, d.point, d.dir.normalized(), 0};
        if (d.kind != AssemblyDatum::Kind::Sketch) return std::nullopt;
        const DatumSketch& ds = d.entities;
        if (colon == std::string::npos) return Prim{Prim::Kind::Plane, ds.frame.origin, ds.frame.normal.normalized(), 0};
        const std::string ent = name.substr(colon + 1);
        if (auto it = ds.points.find(ent); it != ds.points.end()) return Prim{Prim::Kind::Point, it->second, Vec3::UnitZ(), 0};
        if (auto it = ds.lines.find(ent); it != ds.lines.end()) {
            const Vec3 dl = it->second.second - it->second.first;
            if (dl.norm() < kernel::KERNEL_MIN_DIMENSION) {
                err = Error("ASM_MATE_BAD_REF").with("ref", ref);
                return std::nullopt;
            }
            return Prim{Prim::Kind::Line, it->second.first, dl.normalized(), 0};
        }
        if (auto it = ds.circles.find(ent); it != ds.circles.end())
            return Prim{Prim::Kind::Axis, it->second.first, ds.frame.normal.normalized(), it->second.second};
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

std::string mateUnitOf(const AssemblyState& st, const std::string& path, const std::set<std::string>* rigid) {
    const std::string top = path.substr(0, path.find('/'));
    const auto* e = st.find(top);
    if (path == top || !e || !e->flex || (rigid && rigid->count(top))) return top;
    const std::string rel = path.substr(top.size() + 1);
    std::string best;
    for (const auto& u : e->flex->flexUnits)
        if ((rel == u || rel.rfind(u + "/", 0) == 0) && u.size() > best.size()) best = u;
    return best.empty() ? top : top + "/" + best;
}

namespace {

std::optional<Ref> resolveRefIn(const kernel::Kernel& k, const AssemblyState& st, const std::string& ref, std::optional<Error>& err,
                                const std::set<std::string>* rigid) {
    Ref out;
    if (ref == "origin") {
        out.prim.kind = Prim::Kind::Point;
        return out;
    }
    if (ref.rfind("plane:", 0) == 0) {
        auto pl = sketch::datumPlane(ref);
        if (!pl) return std::nullopt;
        out.prim = {Prim::Kind::Plane, pl->origin, pl->normal, 0};
        return out;
    }
    if (ref.rfind("axis:", 0) == 0) {
        const Vec3 d = sketch::datumAxis(ref).value_or(Vec3::UnitZ());
        out.prim = {Prim::Kind::Axis, Vec3::Zero(), d, 0};
        return out;
    }
    // Assembly reference plane / axis not depending on instances (§7б); parsing — assembly.md §3.1.
    const AssemblyRef ar = parseAssemblyRef(st, ref);
    if (ar.kind == AssemblyRef::Kind::Own) {
        if (auto it = st.datumPlanes.find(ref); it != st.datumPlanes.end()) {
            out.prim = {Prim::Kind::Plane, it->second.origin, it->second.normal, 0};
            return out;
        }
        if (auto it = st.datumAxes.find(ref); it != st.datumAxes.end()) {
            out.prim = {Prim::Kind::Axis, it->second.first, it->second.second.normalized(), 0};
            return out;
        }
        // Assembly sketch (FX7-ASM-006): "<sketch>" — its plane, "<sketch>:<entity>" — point, line, circle/arc (§7.2).
        const size_t colon = ref.find(':');
        auto sk = st.datumSketches.find(ref.substr(0, colon));
        if (sk == st.datumSketches.end()) return std::nullopt;
        const DatumSketch& ds = sk->second;
        if (colon == std::string::npos) {
            out.prim = {Prim::Kind::Plane, ds.frame.origin, ds.frame.normal.normalized(), 0};
            return out;
        }
        const std::string ent = ref.substr(colon + 1);
        if (auto it = ds.points.find(ent); it != ds.points.end()) {
            out.prim = {Prim::Kind::Point, it->second, Vec3::UnitZ(), 0};
            return out;
        }
        if (auto it = ds.lines.find(ent); it != ds.lines.end()) {
            const Vec3 d = it->second.second - it->second.first;
            if (d.norm() < kernel::KERNEL_MIN_DIMENSION) {
                err = Error("ASM_MATE_BAD_REF").with("ref", ref);
                return std::nullopt;
            }
            out.prim = {Prim::Kind::Line, it->second.first, d.normalized(), 0};
            return out;
        }
        if (auto it = ds.circles.find(ent); it != ds.circles.end()) {
            out.prim = {Prim::Kind::Axis, it->second.first, ds.frame.normal.normalized(), it->second.second};
            return out;
        }
        return std::nullopt;
    }
    // <instance path>/<name> of a part entry, or a reference plane, axis or visible sketch of a nested assembly (§7.2,
    // FX9-ASM-014): "<node>/<feature ID>" or "<node>/<sketch ID>:<entity ID>" (AssemblyState.datums).
    if (ar.kind == AssemblyRef::Kind::Invalid) return std::nullopt;
    const InstanceEntry* leaf = ar.entry;
    std::optional<Prim> local;
    if (ar.kind == AssemblyRef::Kind::Nested) {
        local = nestedDatumPrimitive(st, ar.path, ar.leaf, err, ref);
        if (!local) return std::nullopt;
    }
    const std::string top = mateUnitOf(st, leaf->path, rigid);
    const auto* topEntry = st.find(top);
    if (!topEntry) return std::nullopt;
    if (!local)
        local = ar.construction ? constructionPrimitive(*leaf, ar.leaf, err, ref) : primitiveOf(k, *leaf->state, ar.leaf, err, ref);
    if (!local) return std::nullopt;
    // Into the frame of the body: M_top⁻¹ · M_leaf.
    const Motion3 rel = inverse(topEntry->world) * leaf->world;
    out.top = top;
    out.prim = {local->kind, rel.apply(local->p), rel.applyDir(local->w).normalized(), local->r, local->teeth};
    out.leaf = leaf->path;
    out.local = *local;
    return out;
}

std::optional<Ref> resolveRef(const kernel::Kernel& k, const AssemblyState& st, const std::string& ref, std::optional<Error>& err,
                              const std::set<std::string>* rigid = nullptr) {
    auto r = resolveRefIn(k, st, ref, err, rigid);
    if (r && r->top.empty()) r->local = r->prim;  // the assembly's own geometry: its frame
    return r;
}

// Mate resolved for the system: equation functions are built from the references.
struct Unknown {
    std::string path;
    Motion3 base;  // placement at the start of the solve (after branch selection)
    bool free = false;
    int first = -1;  // index of the first of 6 parameters
};

// Residual function: equation parameters — 6 (one side free) or 12 (both).
struct MateFn final : solver::SystemFn {
    std::function<D(const D*)> f;
    int n = 0;
    double value(const double* x) const override {
        std::array<D, N> a;
        for (int i = 0; i < n; ++i) a[size_t(i)] = D(x[i]);
        return f(a.data()).v;
    }
    void gradient(const double* x, double* g) const override {
        std::array<D, N> a;
        for (int i = 0; i < n; ++i) {
            a[size_t(i)] = D(x[i]);
            a[size_t(i)].d[size_t(i)] = 1;
        }
        const D r = f(a.data());
        for (int i = 0; i < n; ++i) g[i] = r.d[size_t(i)];
    }
};

}  // namespace

bool mateHasSide(const kernel::Kernel& k, const AssemblyState& st, const std::string& type, const std::array<std::string, 2>& refs) {
    std::optional<Error> err;
    const auto a = resolveRef(k, st, refs[0], err);
    const auto b = a ? resolveRef(k, st, refs[1], err) : std::nullopt;
    if (!a || !b || a->prim.kind == Prim::Kind::Point || b->prim.kind == Prim::Kind::Point) return false;
    if (type == "angle" || type == "parallel") return true;
    const auto axial = [](Prim::Kind kk) { return kk == Prim::Kind::Axis || kk == Prim::Kind::Line; };
    const bool planes = a->prim.kind == Prim::Kind::Plane && b->prim.kind == Prim::Kind::Plane;
    return (type == "coincident" || type == "distance" || type == "concentric") && (planes || (axial(a->prim.kind) && axial(b->prim.kind)));
}

std::optional<double> mateCurrentValue(const kernel::Kernel& k, const AssemblyState& st, const std::string& type,
                                       const std::array<std::string, 2>& refs) {
    auto a = worldMatePrimitive(k, st, refs[0]);
    auto b = a ? worldMatePrimitive(k, st, refs[1]) : std::nullopt;
    if (!a || !b) return std::nullopt;
    const auto axial = [](Prim::Kind kk) { return kk == Prim::Kind::Axis || kk == Prim::Kind::Line; };
    if (type == "angle") {
        if (a->kind == Prim::Kind::Point || b->kind == Prim::Kind::Point) return std::nullopt;
        return std::acos(std::clamp(a->w.normalized().dot(b->w.normalized()), -1.0, 1.0));
    }
    if (type != "distance") return std::nullopt;
    if (b->kind == Prim::Kind::Plane && a->kind == Prim::Kind::Point) std::swap(a, b);  // the plane is A in the equation
    if (a->kind == Prim::Kind::Plane && b->kind == Prim::Kind::Plane) {
        // (P_B − P_A)·w_A = −s·D with the side s by the current placements (initSide).
        const double c = a->w.normalized().dot(b->w.normalized());
        const double s = std::abs(c) < MATE_SIDE_EPS ? -1.0 : (c > 0 ? 1.0 : -1.0);
        return -s * (b->p - a->p).dot(a->w.normalized());
    }
    if (a->kind == Prim::Kind::Plane && b->kind == Prim::Kind::Point) return (b->p - a->p).dot(a->w.normalized());
    if (a->kind == Prim::Kind::Point && b->kind == Prim::Kind::Point) return (b->p - a->p).norm();
    if (axial(a->kind) && axial(b->kind)) return (b->p - a->p).cross(a->w.normalized()).norm();
    return std::nullopt;
}

std::optional<std::pair<std::string, Motion3>> mateTurn(const kernel::Kernel& k, const AssemblyState& st, const std::string& type,
                                                        const std::array<std::string, 2>& refs) {
    if (!mateHasSide(k, st, type, refs)) return std::nullopt;
    std::optional<Error> err;
    for (const int side : {1, 0}) {
        const auto r = resolveRef(k, st, refs[size_t(side)], err);
        const InstanceEntry* e = r && !r->top.empty() ? st.find(r->top) : nullptr;
        if (!e || e->fixed) continue;
        const Vec3 p = e->world.apply(r->prim.p), w = e->world.applyDir(r->prim.w).normalized();
        const Eigen::Matrix3d R = Eigen::AngleAxisd(PI, orthoPair(w).first).toRotationMatrix();
        return std::make_pair(e->path, Motion3{R, p - R * p} * e->world);
    }
    return std::nullopt;
}

std::optional<MatePrimitive> partPrimitive(const kernel::Kernel& k, const PartState& st, const std::string& name) {
    std::optional<Error> e;
    return primitiveOf(k, st, name, e, name);
}

std::optional<std::pair<std::string, MatePrimitive>> matePrimitive(const kernel::Kernel& k, const AssemblyState& st, const std::string& ref) {
    std::optional<Error> e;
    auto r = resolveRef(k, st, ref, e);
    if (!r) return std::nullopt;
    return std::make_pair(r->top, r->prim);
}

std::optional<MatePrimitive> worldMatePrimitive(const kernel::Kernel& k, const AssemblyState& st, const std::string& ref) {
    std::optional<Error> e;
    auto r = resolveRef(k, st, ref, e);
    if (!r) return std::nullopt;
    if (r->top.empty()) return r->prim;
    const Motion3& M = st.find(r->top)->world;
    MatePrimitive p = r->prim;
    p.p = M.apply(p.p);
    p.w = M.applyDir(p.w).normalized();
    return p;
}

doc::Element explodedMateElement(const doc::Element& asmEl) {
    doc::Element el = asmEl;
    if (el.exploded)
        for (auto& m : el.mates)
            if (std::find(el.exploded->suppressedMates.begin(), el.exploded->suppressedMates.end(), m.id) != el.exploded->suppressedMates.end())
                m.suppressed = true;
    return el;
}

std::optional<std::array<double, 7>> mateRelative(const kernel::Kernel& k, const AssemblyState& st, const std::string& refA, const std::string& refB) {
    std::optional<Error> e;
    auto a = resolveRef(k, st, refA, e), b = resolveRef(k, st, refB, e);
    if (!a || !b) return std::nullopt;
    const Motion3 Ma = a->top.empty() ? Motion3{} : st.find(a->top)->world;
    const Motion3 Mb = b->top.empty() ? Motion3{} : st.find(b->top)->world;
    const Motion3 rel = inverse(Ma) * Mb;
    const Eigen::Quaterniond q(rel.R);
    return std::array<double, 7>{q.w(), q.x(), q.y(), q.z(), rel.t.x(), rel.t.y(), rel.t.z()};
}

std::optional<std::array<double, 2>> mateGearAngles(const kernel::Kernel& k, const AssemblyState& st, const std::string& refA, const std::string& refB) {
    std::optional<Error> e;
    auto a = resolveRef(k, st, refA, e), b = a ? resolveRef(k, st, refB, e) : std::nullopt;
    if (!a || !b || a->prim.kind != Prim::Kind::Axis || b->prim.kind != Prim::Kind::Axis || a->prim.teeth <= 0 || b->prim.teeth <= 0 ||
        (a->prim.r < 0 && b->prim.r < 0))
        return std::nullopt;
    auto world = [&](const Ref& r, const Vec3& v, bool dir) {
        const Motion3 M = r.top.empty() ? Motion3{} : st.find(r.top)->world;
        return vec(dir ? Vec3(M.applyDir(v)) : Vec3(M.apply(v)));
    };
    const V PA = world(*a, a->prim.p, false), WA = world(*a, a->prim.w.normalized(), true), UA = world(*a, rimReference(a->prim.w), true);
    const V PB = world(*b, b->prim.p, false), UB = world(*b, rimReference(b->prim.w), true);
    const auto g = gearAnglesD(PA, WA, UA, PB, UB);
    return std::array<double, 2>{g[0].v, g[1].v};
}

void solveMates(const kernel::Kernel& k, const doc::Element& asmEl, const expr::VariableTable& vars, const expr::DocumentUnits& units,
                AssemblyState& st, const std::optional<std::pair<std::string, Motion3>>& drag, std::map<std::string, double>* residuals,
                const MateSolveOptions& opts) {
    // residuals — only evaluate the equations at the current placements (assembly.md §7д.3 item 5): no branch selection,
    // no initial shift, no solve.
    const bool check = residuals != nullptr;
    st.mates.clear();
    st.dof = 0;
    st.flexUnits.clear();
    st.systemMates.clear();
    // Underdetermined nested assemblies (§2б): their free units and mates join this system.
    auto flexNode = [&](const InstanceEntry& e) {
        return e.depth == 0 && e.flex && !e.suppressed && !e.error && !opts.rigid.count(e.path);
    };
    bool hasFlex = false;
    for (const auto& e : st.instances) hasFlex = hasFlex || flexNode(e);
    if (asmEl.mates.empty() && !hasFlex) return;
    // Characteristic length: bounding box diagonal of the instances' bodies (§7.3).
    BBox3 all;
    all.setEmpty();
    for (const auto& e : st.instances)
        if (e.state)
            for (const auto& b : e.state->bodies) {
                const BBox3 bb = k.bbox(b.shape);
                if (bb.isEmpty()) continue;
                for (int c = 0; c < 8; ++c) all.extend(Point3(e.world.apply(bb.corner(BBox3::CornerType(c)))));
            }
    const double L = all.isEmpty() ? 1.0 : std::max(1.0, all.diagonal().norm());
    // Unknowns — top-level instances.
    std::vector<Unknown> unk;
    std::map<std::string, int> unkOf;
    auto unknownFor = [&](const std::string& top) {
        auto it = unkOf.find(top);
        if (it != unkOf.end()) return it->second;
        const auto* e = st.find(top);
        Unknown u;
        u.path = top;
        u.base = e->world;
        if (drag && drag->first == top) u.base = drag->second;
        u.free = !e->fixed;
        unk.push_back(u);
        return unkOf[top] = int(unk.size() - 1);
    };
    struct RM {
        const doc::Mate* m;
        Ref a, b;
        int ia = -1, ib = -1;
        double value = 0;
        double theta = 0;            // angle: θ' — θ or π − θ, the one nearer the current angle (§7.4)
        double side = 1;             // orientation sign s by the current placements (§7.4, FX9-ASM-005)
        bool angleParallel = false;  // angle with θ' ≈ 0 or π — solved as parallel (§7.4)
        bool sub = false;            // a mate of a nested underdetermined assembly (§2б): no status here
    };
    std::vector<RM> rms;
    // Side by the current placements (§7.4, FX9-ASM-005): the sign of w_A·w_B at the start of the solve; the
    // document field flip is ignored (old files: the placement already reflects it).
    auto initSide = [&](RM& r) {
        auto wOf = [&](const Ref& rf, int ui) { return ui < 0 ? rf.prim.w : Vec3(unk[size_t(ui)].base.R * rf.prim.w); };
        const double c = std::clamp(wOf(r.a, r.ia).normalized().dot(wOf(r.b, r.ib).normalized()), -1.0, 1.0);
        const bool planes = r.a.prim.kind == Prim::Kind::Plane && r.b.prim.kind == Prim::Kind::Plane;
        r.side = std::abs(c) < MATE_SIDE_EPS ? (planes ? -1.0 : 1.0) : (c > 0 ? 1.0 : -1.0);
        if (r.m->type == "angle") {
            const double phi = std::acos(c);
            r.theta = std::abs(phi - r.value) <= std::abs(phi - (PI - r.value)) ? r.value : PI - r.value;
            r.angleParallel = r.theta <= ANGLE_PARALLEL_EPS || r.theta >= PI - ANGLE_PARALLEL_EPS;
        }
    };
    for (const auto& m : asmEl.mates) {
        MateStatus ms{m.id, {}, {}};
        if (m.suppressed) {
            st.mates.push_back(ms);
            continue;
        }
        std::optional<Error> err;
        auto a = resolveRef(k, st, m.refs[0], err, &opts.rigid);
        auto b = a ? resolveRef(k, st, m.refs[1], err, &opts.rigid) : std::nullopt;
        if (!a || !b) {
            ms.error = err ? *err : Error("ASM_MATE_REF_LOST").with("ref", !a ? m.refs[0] : m.refs[1]);
            ms.error->with("id", m.id);
            st.mates.push_back(ms);
            continue;
        }
        RM r{&m, *a, *b};
        if (!a->top.empty()) r.ia = unknownFor(a->top);
        if (!b->top.empty()) r.ib = unknownFor(b->top);
        if (m.value) {
            auto q = expr::evaluateField(*m.value, m.type == "angle" ? expr::Expected(expr::ANGLE) : expr::Expected(expr::LENGTH), vars.env(), units);
            if (!q) {
                ms.error = q.error();
                st.mates.push_back(ms);
                continue;
            }
            r.value = q->value;
        }
        initSide(r);
        rms.push_back(r);
        st.mates.push_back(ms);
    }
    // Mates of the nested underdetermined assemblies (§2б): always satisfied; their primitives — through the entries
    // carrying them into the frames of this system's bodies.
    std::deque<doc::Mate> subMates;
    for (const auto& node : st.instances) {
        if (!flexNode(node)) continue;
        for (const auto& sm : node.flex->systemMates) {
            std::array<Ref, 2> rf;
            bool ok = true;
            for (int side = 0; side < 2 && ok; ++side) {
                const std::string lp = sm.leaf[size_t(side)].empty() ? node.path : node.path + "/" + sm.leaf[size_t(side)];
                const auto* le = st.find(lp);
                const std::string top = mateUnitOf(st, lp, &opts.rigid);
                const auto* ue = st.find(top);
                if (!le || !ue) {
                    ok = false;
                    break;
                }
                const Prim& p = sm.prim[size_t(side)];
                const Motion3 rel = inverse(ue->world) * le->world;
                Ref& x = rf[size_t(side)];
                x.top = top;
                x.prim = {p.kind, rel.apply(p.p), rel.applyDir(p.w).normalized(), p.r, p.teeth};
                x.leaf = lp;
                x.local = p;
            }
            if (!ok) continue;
            doc::Mate dm;
            dm.id = node.path + "/" + sm.id;
            dm.type = sm.type;
            if (sm.type == "lock") {  // the bodies' relative placement at the start: the mate is satisfied there
                const Motion3 rel = inverse(st.find(rf[0].top)->world) * st.find(rf[1].top)->world;
                const Eigen::Quaterniond q(rel.R);
                dm.relative = std::array<double, 7>{q.w(), q.x(), q.y(), q.z(), rel.t.x(), rel.t.y(), rel.t.z()};
            }
            subMates.push_back(dm);
            RM r{&subMates.back(), rf[0], rf[1]};
            r.ia = unknownFor(rf[0].top);
            r.ib = unknownFor(rf[1].top);
            r.value = sm.value;
            r.sub = true;
            initSide(r);
            rms.push_back(r);
        }
    }
    // Parameters: 6 per free instance.
    solver::System sys;
    sys.L = L;
    for (auto& u : unk)
        if (u.free) {
            u.first = int(sys.x0.size());
            for (int i = 0; i < 3; ++i) sys.x0.push_back(u.base.t[i]);
            for (int i = 0; i < 3; ++i) sys.x0.push_back(0.0);
        }
    sys.fixed.assign(sys.x0.size(), false);
    // Exploded view: translations only, the orientation of every instance stays as it starts (§7.4, FX8-ASM-006).
    if (opts.translationOnly)
        for (const auto& u : unk)
            if (u.free)
                for (int i = 3; i < 6; ++i) sys.fixed[size_t(u.first + i)] = true;
    // World primitives from parameters (dual): side — equation slots (0 — first 6, 6 — second).
    auto worldPrim = [&](const Ref& r, int ui, const D* x, int off, V& p, V& w) {
        if (ui < 0) {
            p = vec(r.prim.p), w = vec(r.prim.w);
            return;
        }
        const Unknown& u = unk[size_t(ui)];
        const Vec3 p0 = u.base.R * r.prim.p, w0 = u.base.R * r.prim.w;
        if (!u.free) {
            p = vec(p0 + u.base.t), w = vec(w0);
            return;
        }
        const V wv{x[off + 3] / D(L), x[off + 4] / D(L), x[off + 5] / D(L)};
        p = add(rotate(wv, vec(p0)), V{x[off], x[off + 1], x[off + 2]});
        w = rotate(wv, vec(w0));
    };
    // Orientation branch selection (§7.4): 180° rotation of free instance B (or A).
    auto bboxCenter = [&](const std::string& top) {
        BBox3 bb;
        bb.setEmpty();
        for (const auto& e : st.instances)
            if (e.state && (e.path == top || e.path.rfind(top + "/", 0) == 0))
                for (const auto& b : e.state->bodies) {
                    const BBox3 x = k.bbox(b.shape);
                    if (x.isEmpty()) continue;
                    for (int c = 0; c < 8; ++c) bb.extend(Point3(e.world.apply(x.corner(BBox3::CornerType(c)))));
                }
        return bb.isEmpty() ? Vec3::Zero() : Vec3(bb.center());
    };
    // First plane pairs, then axis pairs; directions already chosen for an instance ("locked") are not changed by the
    // rotation (§7.4, FX1-ASM-005: otherwise concentricity would undo the plane coincidence by flipping it).
    std::map<int, std::vector<Vec3>> claims;  // unknown index → locked world directions
    const bool noBranches = check || opts.translationOnly;
    for (int pass = 0; pass < (noBranches ? 0 : 2); ++pass)
        for (auto& r : rms) {
            const auto& m = *r.m;
            const bool planes = r.a.prim.kind == Prim::Kind::Plane && r.b.prim.kind == Prim::Kind::Plane;
            const bool axes = (r.a.prim.kind == Prim::Kind::Axis || r.a.prim.kind == Prim::Kind::Line) &&
                              (r.b.prim.kind == Prim::Kind::Axis || r.b.prim.kind == Prim::Kind::Line);
            const bool directed = r.a.prim.kind != Prim::Kind::Point && r.b.prim.kind != Prim::Kind::Point;
            const bool oriented = ((m.type == "coincident" || m.type == "distance" || m.type == "concentric") && (planes || axes)) ||
                                  (m.type == "angle" && r.angleParallel && directed);
            const bool parallelType = oriented || (m.type == "parallel" && directed);
            if (!parallelType || planes != (pass == 0)) continue;
            const double s = m.type == "angle" ? (r.theta < PI / 2 ? 1.0 : -1.0) : r.side;
            auto wOf = [&](const Ref& rf, int ui) { return ui < 0 ? rf.prim.w : Vec3(unk[size_t(ui)].base.R * rf.prim.w); };
            auto freeUnk = [&](int ui) { return ui >= 0 && unk[size_t(ui)].free; };
            // Start for parallel directions (§7.4, FX10-ASM-002): perpendicular at the start, the orientation equations
            // have a zero gradient over rotations (an extremum of w_B·u_A) — a free instance is turned by π/2 about
            // k = w × (s·w_other), which keeps its locked directions only when they are all parallel to k.
            if (std::abs(wOf(r.a, r.ia).normalized().dot(wOf(r.b, r.ib).normalized())) < MATE_PARALLEL_START_COS) {
                auto quarterAxis = [&](int ui, const Vec3& w, const Vec3& other) -> std::optional<Vec3> {
                    const Vec3 kk = w.normalized().cross(s * other.normalized());
                    if (kk.norm() < 0.5) return std::nullopt;
                    const auto it = claims.find(ui);
                    if (it != claims.end())
                        for (const Vec3& c : it->second)
                            if (c.normalized().cross(kk.normalized()).norm() > 1e-9) return std::nullopt;
                    return kk.normalized();
                };
                const Vec3 wa0 = wOf(r.a, r.ia), wb0 = wOf(r.b, r.ib);
                int target = -1;
                std::optional<Vec3> axis;
                if (freeUnk(r.ib) && (axis = quarterAxis(r.ib, wb0, wa0))) target = r.ib;
                else if (freeUnk(r.ia) && r.ia != r.ib && (axis = quarterAxis(r.ia, wa0, wb0))) target = r.ia;
                if (target >= 0) {
                    Unknown& u = unk[size_t(target)];
                    const Vec3 c = bboxCenter(u.path);
                    const Eigen::Matrix3d Rq = Eigen::AngleAxisd(PI / 2, *axis).toRotationMatrix();
                    u.base = Motion3{Rq, c - Rq * c} * u.base;
                    if (u.first >= 0)
                        for (int i = 0; i < 3; ++i) sys.x0[size_t(u.first + i)] = u.base.t[i];
                    for (auto& cl : claims[target]) cl = Rq * cl;
                }
            }
            const Vec3 wa = wOf(r.a, r.ia), wb = wOf(r.b, r.ib);
            if (oriented && s * wa.dot(wb) < 0) {
                // Axis of the 180° rotation: with no locked directions — u_A; with one — that direction itself, if ⊥ to the
                // instance's reference direction (it is kept, and the reference direction flips); otherwise the instance is not rotated.
                auto axisFor = [&](int ui, const Vec3& w) -> std::optional<Vec3> {
                    const auto it = claims.find(ui);
                    if (it == claims.end() || it->second.empty()) return orthoPair(wa).first;
                    const Vec3 c0 = it->second.front();
                    for (const Vec3& c : it->second)
                        if (c.cross(c0).norm() > 1e-9) return std::nullopt;  // two independent directions — orientation is fixed
                    if (std::abs(c0.dot(w)) > 1e-9) return std::nullopt;
                    return c0.normalized();
                };
                int target = -1;
                std::optional<Vec3> axis;
                if (freeUnk(r.ib) && (axis = axisFor(r.ib, wb))) target = r.ib;
                else if (freeUnk(r.ia) && r.ia != r.ib && (axis = axisFor(r.ia, wa))) target = r.ia;
                if (target >= 0) {
                    Unknown& u = unk[size_t(target)];
                    const Vec3 c = bboxCenter(u.path);
                    const Eigen::Matrix3d Rf = Eigen::AngleAxisd(PI, *axis).toRotationMatrix();
                    u.base = Motion3{Rf, c - Rf * c} * u.base;
                    if (u.first >= 0)
                        for (int i = 0; i < 3; ++i) sys.x0[size_t(u.first + i)] = u.base.t[i];
                    for (auto& cl : claims[target]) cl = Rf * cl;
                }
            }
            if (!oriented) continue;  // parallel claims nothing for the branch selection (as before; §7.4)
            if (freeUnk(r.ia)) claims[r.ia].push_back(wOf(r.a, r.ia));
            if (freeUnk(r.ib)) claims[r.ib].push_back(wOf(r.b, r.ib));
        }
    // Axis–axis distance with axes coincident at the start (§7.4): direction is undefined — the free instance
    // is shifted by D along u_A.
    for (auto& r : rms) {
        if (check) break;
        const auto& m = *r.m;
        const bool axA = r.a.prim.kind == Prim::Kind::Axis || r.a.prim.kind == Prim::Kind::Line;
        const bool axB = r.b.prim.kind == Prim::Kind::Axis || r.b.prim.kind == Prim::Kind::Line;
        // Tangent and gear of two cylinders (FX10-GEAR-003/004): the centre distance |r_A + r_B| plays the role of D.
        const bool cyl = (m.type == "tangent" || m.type == "gear") && axA && axB && r.a.prim.r != 0 && r.b.prim.r != 0;
        const double dist = cyl ? std::abs(r.a.prim.r + r.b.prim.r) : r.value;
        if ((m.type != "distance" && !cyl) || !axA || !axB || std::abs(dist) < 1e-12) continue;
        auto wp = [&](const Ref& rf, int ui) { return ui < 0 ? rf.prim.p : Vec3(unk[size_t(ui)].base.apply(rf.prim.p)); };
        auto ww = [&](const Ref& rf, int ui) { return ui < 0 ? rf.prim.w : Vec3(unk[size_t(ui)].base.R * rf.prim.w); };
        const Vec3 wa = ww(r.a, r.ia).normalized();
        if ((wp(r.b, r.ib) - wp(r.a, r.ia)).cross(wa).norm() > 1e-9 * L) continue;
        const bool bFree = r.ib >= 0 && unk[size_t(r.ib)].free;
        const int target = bFree ? r.ib : (r.ia >= 0 && unk[size_t(r.ia)].free ? r.ia : -1);
        if (target < 0) continue;
        Unknown& u = unk[size_t(target)];
        const Vec3 shift = orthoPair(wa).first * (bFree ? dist : -dist);
        u.base = Motion3{Eigen::Matrix3d::Identity(), shift} * u.base;
        if (u.first >= 0)
            for (int i = 0; i < 3; ++i) sys.x0[size_t(u.first + i)] = u.base.t[i];
    }
    // Start for angular mates (§7.4, FX8-ASM-004): perpendicular / angle whose directions are parallel at the start —
    // the gradient over rotations is zero there, so a free instance is turned by the target angle about a free axis.
    {
        auto wOf = [&](const Ref& rf, int ui) { return ui < 0 ? rf.prim.w : Vec3(unk[size_t(ui)].base.R * rf.prim.w); };
        std::map<int, std::vector<Vec3>> locked = claims;  // branch-selection claims plus parallel references
        for (const auto& r : rms) {
            if (noBranches || r.m->type != "parallel") continue;
            if (r.ia >= 0 && unk[size_t(r.ia)].free) locked[r.ia].push_back(wOf(r.a, r.ia));
            if (r.ib >= 0 && unk[size_t(r.ib)].free) locked[r.ib].push_back(wOf(r.b, r.ib));
        }
        for (const auto& r : rms) {
            if (noBranches) break;
            const auto& m = *r.m;
            const bool angular = m.type == "perpendicular" || (m.type == "angle" && !r.angleParallel);
            if (!angular || r.a.prim.kind == Prim::Kind::Point || r.b.prim.kind == Prim::Kind::Point) continue;
            const Vec3 wa = wOf(r.a, r.ia).normalized(), wb = wOf(r.b, r.ib).normalized();
            if (wa.cross(wb).norm() >= MATE_ANGLE_START_SIN) continue;
            const double target = m.type == "perpendicular" ? PI / 2 : r.theta;
            const double alpha = wa.dot(wb) > 0 ? target : PI - target;
            auto axisFor = [&](int ui, const Vec3& w) -> std::optional<Vec3> {
                const auto it = locked.find(ui);
                if (it == locked.end() || it->second.empty()) return orthoPair(wa).first;
                for (const Vec3& c : it->second) {
                    if (std::abs(c.normalized().dot(w)) > 1e-9) continue;
                    bool allParallel = true;
                    for (const Vec3& c2 : it->second) allParallel = allParallel && c2.cross(c).norm() <= 1e-9 * c.norm() * c2.norm();
                    if (allParallel) return c.normalized();
                    return std::nullopt;
                }
                return std::nullopt;
            };
            int target_ = -1;
            std::optional<Vec3> axis;
            if (r.ib >= 0 && unk[size_t(r.ib)].free && (axis = axisFor(r.ib, wb))) target_ = r.ib;
            else if (r.ia >= 0 && unk[size_t(r.ia)].free && r.ia != r.ib && (axis = axisFor(r.ia, wa))) target_ = r.ia;
            if (target_ < 0) continue;
            Unknown& u = unk[size_t(target_)];
            const Vec3 c = bboxCenter(u.path);
            const Eigen::Matrix3d Rf = Eigen::AngleAxisd(alpha, *axis).toRotationMatrix();
            u.base = Motion3{Rf, c - Rf * c} * u.base;
            if (u.first >= 0)
                for (int i = 0; i < 3; ++i) sys.x0[size_t(u.first + i)] = u.base.t[i];
            for (auto& cl : locked[target_]) cl = Rf * cl;
        }
    }
    // Equations (§7.4).
    for (size_t ri = 0; ri < rms.size(); ++ri) {
        const RM& r = rms[ri];
        const auto& m = *r.m;
        std::vector<int> params;
        int offA = -1, offB = -1;
        if (r.ia >= 0 && unk[size_t(r.ia)].free) {
            offA = int(params.size());
            for (int i = 0; i < 6; ++i) params.push_back(unk[size_t(r.ia)].first + i);
        }
        if (r.ib >= 0 && unk[size_t(r.ib)].free && r.ib != r.ia) {
            offB = int(params.size());
            for (int i = 0; i < 6; ++i) params.push_back(unk[size_t(r.ib)].first + i);
        } else if (r.ib >= 0 && r.ib == r.ia) {
            offB = offA;
        }
        if (params.empty()) continue;  // both sides fixed — equation without unknowns
        Ref A = r.a, B = r.b;
        int ia = r.ia, ib = r.ib, oa = offA, ob = offB;
        auto kindRank = [](Prim::Kind kk) { return kk == Prim::Kind::Plane ? 0 : (kk == Prim::Kind::Axis || kk == Prim::Kind::Line) ? 1 : 2; };
        // Pair order: plane/axis first (point–plane → plane, point); not for lock — its relative placement is of the refs'
        // instances in the order of the mate (§7.4).
        if (m.type != "lock" && kindRank(A.prim.kind) > kindRank(B.prim.kind)) std::swap(A, B), std::swap(ia, ib), std::swap(oa, ob);
        const Prim pa = A.prim, pb = B.prim;
        const std::string& t0 = m.type;
        const bool planes = pa.kind == Prim::Kind::Plane && pb.kind == Prim::Kind::Plane;
        const bool axisA = pa.kind == Prim::Kind::Axis || pa.kind == Prim::Kind::Line;
        const bool axisB = pb.kind == Prim::Kind::Axis || pb.kind == Prim::Kind::Line;
        // Two cylinders for tangent / gear: radii given, not two concave (internal) ones, a nonzero centre distance;
        // a gear also needs both to be pitch cylinders of rims (teeth) — §7.4.
        const bool cylinders = axisA && axisB && pa.r != 0 && pb.r != 0 && !(pa.r < 0 && pb.r < 0) &&
                               std::abs(pa.r + pb.r) > kernel::KERNEL_MIN_DIMENSION && (t0 != "gear" || (pa.teeth > 0 && pb.teeth > 0));
        const double s = r.side;
        const double val = r.value;
        const auto [uA, vA] = orthoPair(pa.w);  // rigidly attached to instance A: rotate with it
        auto add = [&](std::function<D(const V& PA, const V& WA, const V& UA, const V& VA, const V& PB, const V& WB)> g, bool aux = false) {
            auto fn = std::make_shared<MateFn>();
            fn->n = int(params.size());
            const Ref Ac = A, Bc = B;
            const int iac = ia, ibc = ib, oac = oa, obc = ob;
            const Vec3 uAc = uA, vAc = vA;
            fn->f = [=, &unk](const D* x) {
                V PA, WA, PB, WB, UA, VA, dummy;
                worldPrim(Ac, iac, x, std::max(oac, 0), PA, WA);
                worldPrim(Bc, ibc, x, std::max(obc, 0), PB, WB);
                Ref ru = Ac;
                ru.prim.w = uAc;
                worldPrim(ru, iac, x, std::max(oac, 0), dummy, UA);
                ru.prim.w = vAc;
                worldPrim(ru, iac, x, std::max(oac, 0), dummy, VA);
                return g(PA, WA, UA, VA, PB, WB);
            };
            sys.equations.push_back({uint64_t(ri + 1), params, fn, aux});
        };
        const D Ld(L);
        // W_B ∥ W_A: two projections onto (u, v) of instance A; within coincidence/distance/concentricity — auxiliary
        // (their mutual dependency is not redundancy, solver.md §3.8).
        auto orient2 = [&](bool aux) {
            add([](const V&, const V&, const V& UA, const V&, const V&, const V& WB) { return dot(WB, UA); }, aux);
            add([](const V&, const V&, const V&, const V& VA, const V&, const V& WB) { return dot(WB, VA); }, aux);
        };
        bool ok = true;
        const std::string& t = m.type;
        if ((t == "coincident" || t == "distance") && planes) {
            orient2(true);
            const double dv = t == "distance" ? val : 0.0;
            add([Ld, dv, s](const V& PA, const V& WA, const V&, const V&, const V& PB, const V&) {
                return (dot(sub(PB, PA), WA) - D(dv) * D(-s)) / Ld;
            });
        } else if ((t == "coincident" || t == "distance") && pa.kind == Prim::Kind::Plane && pb.kind == Prim::Kind::Point) {
            const double dv = t == "distance" ? val : 0.0;
            add([Ld, dv](const V& PA, const V& WA, const V&, const V&, const V& PB, const V&) { return (dot(sub(PB, PA), WA) - D(dv)) / Ld; });
        } else if (t == "coincident" && pa.kind == Prim::Kind::Point && pb.kind == Prim::Kind::Point) {
            for (int c = 0; c < 3; ++c)
                add([Ld, c](const V& PA, const V&, const V&, const V&, const V& PB, const V&) { return (PB[size_t(c)] - PA[size_t(c)]) / Ld; });
        } else if (t == "distance" && pa.kind == Prim::Kind::Point && pb.kind == Prim::Kind::Point) {
            add([Ld, val](const V& PA, const V&, const V&, const V&, const V& PB, const V&) { return (norm(sub(PB, PA)) - D(val)) / Ld; });
        } else if ((t == "coincident" || t == "concentric") && axisA && axisB) {
            orient2(true);
            add([Ld](const V& PA, const V&, const V& UA, const V&, const V& PB, const V&) { return dot(sub(PB, PA), UA) / Ld; });
            add([Ld](const V& PA, const V&, const V&, const V& VA, const V& PB, const V&) { return dot(sub(PB, PA), VA) / Ld; });
        } else if (t == "coincident" && axisA && pb.kind == Prim::Kind::Point) {
            add([Ld](const V& PA, const V&, const V& UA, const V&, const V& PB, const V&) { return dot(sub(PB, PA), UA) / Ld; });
            add([Ld](const V& PA, const V&, const V&, const V& VA, const V& PB, const V&) { return dot(sub(PB, PA), VA) / Ld; });
        } else if (t == "coincident" && pa.kind == Prim::Kind::Plane && axisB) {
            add([](const V&, const V& WA, const V&, const V&, const V&, const V& WB) { return dot(WB, WA); });
            add([Ld](const V& PA, const V& WA, const V&, const V&, const V& PB, const V&) { return dot(sub(PB, PA), WA) / Ld; });
        } else if (t == "angle" && pa.kind != Prim::Kind::Point && pb.kind != Prim::Kind::Point && r.angleParallel) {
            orient2(false);  // θ' ≈ 0 or π: parallel directions — two rotational equations (§7.4)
        } else if (t == "angle" && pa.kind != Prim::Kind::Point && pb.kind != Prim::Kind::Point) {
            // Angle between the directions: unit derivative for 0 < θ' < π (no cos θ degeneracy, §7.4).
            const double th = r.theta;
            add([th](const V&, const V& WA, const V&, const V&, const V&, const V& WB) { return datan2(norm(cross(WA, WB)), dot(WA, WB)) - D(th); });
        } else if (t == "parallel" && pa.kind != Prim::Kind::Point && pb.kind != Prim::Kind::Point) {
            orient2(false);
        } else if (t == "perpendicular" && pa.kind != Prim::Kind::Point && pb.kind != Prim::Kind::Point) {
            add([](const V&, const V& WA, const V&, const V&, const V&, const V& WB) { return dot(WA, WB); });
        } else if (t == "tangent" && pa.kind == Prim::Kind::Plane && pb.kind == Prim::Kind::Axis && pb.r > 0) {
            // Cylinder side — by the initial placement.
            auto wp = [&](const Ref& rf, int ui) { return ui < 0 ? rf.prim.p : Vec3(unk[size_t(ui)].base.apply(rf.prim.p)); };
            auto ww = [&](const Ref& rf, int ui) { return ui < 0 ? rf.prim.w : Vec3(unk[size_t(ui)].base.R * rf.prim.w); };
            const double sigma = (wp(B, ib) - wp(A, ia)).dot(ww(A, ia)) >= 0 ? 1.0 : -1.0;
            const double rr = pb.r;
            add([](const V&, const V& WA, const V&, const V&, const V&, const V& WB) { return dot(WB, WA); });
            add([Ld, sigma, rr](const V& PA, const V& WA, const V&, const V&, const V& PB, const V&) {
                return (dot(sub(PB, PA), WA) - D(sigma * rr)) / Ld;
            });
        } else if (((t == "tangent" || t == "gear") && cylinders) || (t == "distance" && axisA && axisB)) {
            // Cylinders: external tangency |r_A + r_B|; a pitch cylinder of an internal rim has a negative radius —
            // internal tangency |r_B| − |r_A| (FX10-GEAR-003).
            const double rs = t == "distance" ? val : std::abs(pa.r + pb.r);
            orient2(true);
            add([Ld, rs](const V& PA, const V& WA, const V&, const V&, const V& PB, const V&) {
                return (norm(cross(sub(PB, PA), WA)) - D(rs)) / Ld;
            });
            if (t == "gear" && !opts.translationOnly) {
                // Rotations coupled by the teeth (FX10-GEAR-004): z_A·Δα_A + κ·z_B·Δα_B = 0 from the stored angles (none —
                // those at the start of the solve); κ = +1 — two external rims turn opposite ways, −1 — with an internal
                // one the same way. Not in the exploded view: rotations are fixed there.
                const double za = pa.teeth, zb = pb.teeth, kappa = pa.r * pb.r > 0 ? 1.0 : -1.0;
                const Vec3 uAl = rimReference(pa.w), uBl = rimReference(pb.w);
                const Ref Ac = A, Bc = B;
                const int iac = ia, ibc = ib, oac = std::max(oa, 0), obc = std::max(ob, 0);
                auto anglesAt = [=, &unk](const D* x) {
                    V PA, WA, PB, WB, UA, UB, dummy;
                    worldPrim(Ac, iac, x, oac, PA, WA);
                    worldPrim(Bc, ibc, x, obc, PB, WB);
                    Ref ru = Ac;
                    ru.prim.w = uAl;
                    worldPrim(ru, iac, x, oac, dummy, UA);
                    ru = Bc;
                    ru.prim.w = uBl;
                    worldPrim(ru, ibc, x, obc, dummy, UB);
                    return gearAnglesD(PA, WA, UA, PB, UB);
                };
                std::array<double, 2> ref{};
                if (m.angles) {
                    ref = *m.angles;
                } else {
                    std::vector<D> x0;
                    for (int pi : params) x0.push_back(D(sys.x0[size_t(pi)]));
                    const auto a0 = anglesAt(x0.data());
                    ref = {a0[0].v, a0[1].v};
                }
                auto fn = std::make_shared<MateFn>();
                fn->n = int(params.size());
                const double zs = za + zb;
                // Each turn is taken within (−π, π] of its reference: a turn by more than a half turn between two solves
                // is taken the shorter way (the other rim then differs by a whole number of its tooth pitches).
                fn->f = [anglesAt, ref, za, zb, kappa, zs](const D* x) {
                    const auto a = anglesAt(x);
                    return (D(za) * wrapPi(a[0] - D(ref[0])) + D(kappa * zb) * wrapPi(a[1] - D(ref[1]))) / D(zs);
                };
                sys.equations.push_back({uint64_t(ri + 1), params, fn, false});
            }
        } else if (t == "lock" && m.relative) {
            const auto& q = *m.relative;
            const Eigen::Quaterniond rq(q[0], q[1], q[2], q[3]);
            const Eigen::Matrix3d Rrel = rq.normalized().toRotationMatrix();
            const Vec3 trel(q[4], q[5], q[6]);
            // M_B = M_A · rel: points and axes of B through A.
            auto lockEq = [&](std::function<D(const V&, const V&)> g, const Vec3& qa, const Vec3& qb, bool dir) {
                Ref ra = A, rb = B;
                ra.prim = {Prim::Kind::Point, dir ? Vec3::Zero() : qa, dir ? qa : Vec3::UnitZ(), 0};
                rb.prim = {Prim::Kind::Point, dir ? Vec3::Zero() : qb, dir ? qb : Vec3::UnitZ(), 0};
                auto fn = std::make_shared<MateFn>();
                fn->n = int(params.size());
                const int iac = ia, ibc = ib, oac = oa, obc = ob;
                fn->f = [=, &unk](const D* x) {
                    V PA, WA, PB, WB;
                    worldPrim(ra, iac, x, std::max(oac, 0), PA, WA);
                    worldPrim(rb, ibc, x, std::max(obc, 0), PB, WB);
                    return dir ? g(WA, WB) : g(PA, PB);
                };
                sys.equations.push_back({uint64_t(ri + 1), params, fn, false});
            };
            // Translation: the origin of B (0 in its own frame) coincides with rel.t in the frame of A.
            for (int c = 0; c < 3; ++c)
                lockEq([Ld, c](const V& PA, const V& PB) { return (PB[size_t(c)] - PA[size_t(c)]) / Ld; }, trel, Vec3::Zero(), false);
            const Vec3 xA = Rrel * Vec3::UnitX(), yA = Rrel * Vec3::UnitY(), zA = Rrel * Vec3::UnitZ();
            lockEq([](const V& WA, const V& WB) { return dot(WA, WB); }, yA, Vec3::UnitX(), true);
            lockEq([](const V& WA, const V& WB) { return dot(WA, WB); }, zA, Vec3::UnitX(), true);
            lockEq([](const V& WA, const V& WB) { return dot(WA, WB); }, zA, Vec3::UnitY(), true);
            (void)xA;
        } else {
            ok = false;
        }
        if (!ok) {
            for (auto& ms : st.mates)
                if (ms.id == m.id) ms.error = Error("ASM_MATE_BAD_REF").with("id", m.id).with("ref", m.refs[1]);
        }
    }
    if (check) {
        for (const auto& eq : sys.equations) {
            std::vector<double> x;
            for (int pi : eq.params) x.push_back(sys.x0[size_t(pi)]);
            if (rms[size_t(eq.owner - 1)].sub) continue;
            double& r = (*residuals)[rms[size_t(eq.owner - 1)].m->id];
            r = std::max(r, std::abs(eq.fn->value(x.data())));
        }
        return;
    }
    // Pull (§7.6б): projected steps of the soft goal "grab point → target" onto the tangent space of the mates.
    auto pullSolve = [&](const MatePull& pull) -> solver::SystemResult {
        const auto pit = unkOf.find(pull.instance);
        if (pit == unkOf.end() || !unk[size_t(pit->second)].free) return solver::solveSystem(sys);
        const int pu = pit->second;
        // Instances not linked to the pulled one through free instances stay in place.
        std::vector<int> parent(unk.size());
        for (size_t i = 0; i < unk.size(); ++i) parent[i] = int(i);
        std::function<int(int)> root = [&](int i) { return parent[size_t(i)] == i ? i : parent[size_t(i)] = root(parent[size_t(i)]); };
        std::vector<int> ownerOf(sys.x0.size(), -1);
        for (size_t i = 0; i < unk.size(); ++i)
            if (unk[i].free)
                for (int j = 0; j < 6; ++j) ownerOf[size_t(unk[i].first + j)] = int(i);
        for (const auto& eq : sys.equations) {
            int first = -1;
            for (int p : eq.params) {
                const int o = ownerOf[size_t(p)];
                if (o < 0) continue;
                if (first < 0) first = o;
                else parent[size_t(root(o))] = root(first);
            }
        }
        solver::System sc = sys;
        for (size_t p = 0; p < sc.x0.size(); ++p)
            if (ownerOf[p] >= 0 && root(ownerOf[p]) != root(pu)) sc.fixed[p] = true;
        sc.skipDiagnosis = true;
        std::vector<int> fp;  // free parameters (columns)
        for (size_t p = 0; p < sc.x0.size(); ++p)
            if (!sc.fixed[p]) fp.push_back(int(p));
        const int n = int(fp.size());
        std::vector<int> col(sc.x0.size(), -1);
        for (int c = 0; c < n; ++c) col[size_t(fp[size_t(c)])] = c;
        std::vector<bool> isRot = std::vector<bool>(size_t(n), false);
        for (int c = 0; c < n; ++c) isRot[size_t(c)] = fp[size_t(c)] - unk[size_t(ownerOf[size_t(fp[size_t(c)])])].first >= 3;
        const Unknown& U = unk[size_t(pu)];
        Ref gref;
        gref.inst = pu;
        gref.top = U.path;
        gref.prim = {Prim::Kind::Point, pull.grab, Vec3::UnitZ(), 0};  // in the instance frame, like a reference
        auto soft = [&](const std::vector<double>& x, Eigen::Vector3d& r, Eigen::MatrixXd* J) {
            std::array<D, N> a;
            for (int i = 0; i < 6; ++i) {
                a[size_t(i)] = D(x[size_t(U.first + i)]);
                a[size_t(i)].d[size_t(i)] = 1;
            }
            V P, W;
            worldPrim(gref, pu, a.data(), 0, P, W);
            for (int c = 0; c < 3; ++c) r[c] = (P[size_t(c)].v - pull.target[c]) / L;
            if (!J) return;
            J->setZero(3, n);
            for (int i = 0; i < 6; ++i)
                if (const int cc = col[size_t(U.first + i)]; cc >= 0)
                    for (int c = 0; c < 3; ++c) (*J)(c, cc) = P[size_t(c)].d[size_t(i)] / L;
        };
        solver::SystemResult cur = solver::solveSystem(sc);
        std::vector<double> x = cur.x;
        for (int it = 0; it < PULL_ITER_MAX; ++it) {
            Eigen::Vector3d r;
            Eigen::MatrixXd Js;
            soft(x, r, &Js);
            Eigen::MatrixXd Jh = Eigen::MatrixXd::Zero(Eigen::Index(sc.equations.size()), n);
            for (size_t e = 0; e < sc.equations.size(); ++e) {
                const auto& eq = sc.equations[e];
                std::vector<double> xe, g(eq.params.size());
                for (int p : eq.params) xe.push_back(x[size_t(p)]);
                eq.fn->gradient(xe.data(), g.data());
                for (size_t j = 0; j < eq.params.size(); ++j)
                    if (const int cc = col[size_t(eq.params[j])]; cc >= 0) Jh(Eigen::Index(e), cc) += g[j];
            }
            // Null space of the mate Jacobian over the columns `use` (embedded into all n columns).
            auto nullSpace = [&](const std::vector<bool>& use) {
                std::vector<int> cols;
                for (int c = 0; c < n; ++c)
                    if (use[size_t(c)]) cols.push_back(c);
                const Eigen::Index nc = Eigen::Index(cols.size());
                Eigen::MatrixXd Zc;
                if (nc == 0) return Eigen::MatrixXd(n, 0);
                Eigen::MatrixXd Jc(Jh.rows(), nc);
                for (Eigen::Index c = 0; c < nc; ++c) Jc.col(c) = Jh.col(cols[size_t(c)]);
                if (Jc.rows() == 0) {
                    Zc = Eigen::MatrixXd::Identity(nc, nc);
                } else {
                    Eigen::JacobiSVD<Eigen::MatrixXd> svd(Jc, Eigen::ComputeFullV);
                    const auto& sv = svd.singularValues();
                    const double thr = PULL_NULL_TOL * std::max(1.0, sv.size() ? sv[0] : 0.0);
                    Eigen::Index rank = 0;
                    for (Eigen::Index i = 0; i < sv.size(); ++i) rank += sv[i] > thr;
                    Zc = svd.matrixV().rightCols(nc - rank);
                }
                Eigen::MatrixXd Z = Eigen::MatrixXd::Zero(n, Zc.cols());
                for (Eigen::Index c = 0; c < nc; ++c) Z.row(cols[size_t(c)]) = Zc.row(c);
                return Z;
            };
            std::vector<bool> trans = std::vector<bool>(size_t(n), false), all = std::vector<bool>(size_t(n), true);
            for (int c = 0; c < n; ++c) trans[size_t(c)] = !isRot[size_t(c)];
            // Translation first: motions without rotations, then the remainder over all free motions (§7.6б).
            const Eigen::MatrixXd Zt = nullSpace(trans), Z = nullSpace(all);
            if (Z.cols() == 0) break;
            Eigen::VectorXd delta = Eigen::VectorXd::Zero(n);
            Eigen::Vector3d rem = r;
            // Least squares with the least norm; directions of the target the free motions reach only numerically
            // (singular values below PULL_NULL_TOL of the largest) are left out — otherwise an unreachable remainder
            // (the target off the sliding plane) gave a step of 1e14 and the pull stood still (S7F-ASM-001).
            auto lsq = [](const Eigen::MatrixXd& M, const Eigen::Vector3d& b) {
                Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> cod(M.rows(), M.cols());
                cod.setThreshold(PULL_NULL_TOL);
                cod.compute(M);
                return Eigen::VectorXd(cod.solve(b));
            };
            if (Zt.cols() > 0) {
                const Eigen::VectorXd yt = lsq(Js * Zt, -rem);
                delta += Zt * yt;
                rem += Js * Zt * yt;
            }
            delta += Z * lsq(Js * Z, -rem);
            const double dn = delta.norm();
            if (dn < PULL_STOP * L) break;
            if (dn > PULL_STEP_MAX * L) delta *= PULL_STEP_MAX * L / dn;
            bool accepted = false;
            for (int h = 0; h <= PULL_HALVINGS && !accepted; ++h, delta *= 0.5) {
                solver::System t = sc;
                t.x0 = x;
                for (int c = 0; c < n; ++c) t.x0[size_t(fp[size_t(c)])] += delta[c];
                const solver::SystemResult tr = solver::solveSystem(t);
                if (tr.status != solver::SystemStatus::Converged && tr.status != solver::SystemStatus::Redundant) continue;
                Eigen::Vector3d r2;
                soft(tr.x, r2, nullptr);
                if (r2.norm() < r.norm() - 1e-15) {
                    x = tr.x;
                    accepted = true;
                }
            }
            if (!accepted) break;
        }
        solver::System fin = sys;  // statuses and DOF over the whole system, from the pulled placements
        fin.x0 = x;
        return solver::solveSystem(fin);
    };
    if (sys.equations.empty()) return;
    // Mates unsatisfied at the start — the place of a non-convergence (§7.5).
    std::set<std::string> startBad;
    for (const auto& eq : sys.equations) {
        std::vector<double> x;
        for (int pi : eq.params) x.push_back(sys.x0[size_t(pi)]);
        if (std::abs(eq.fn->value(x.data())) > MATE_TOL / L) startBad.insert(rms[size_t(eq.owner - 1)].m->id);
    }
    solver::SystemResult res;
    if (opts.pull) res = pullSolve(*opts.pull);
    else res = solver::solveSystem(sys);
    // "Shake" (§7.5, FX10-ASM-002): a solve that failed from a special position (a stationary point of the orientation
    // residuals) is repeated from deterministic small rotations of the free instances; the first converged one is taken.
    if (!opts.pull && !check && (res.status == solver::SystemStatus::NotConverged || res.status == solver::SystemStatus::Conflict)) {
        for (int t = 0; t < MATE_SHAKE_TRIES; ++t) {
            solver::System sh = sys;
            sh.skipDiagnosis = true;
            bool moved = false;
            for (size_t i = 0; i < unk.size(); ++i) {
                const Unknown& u = unk[i];
                if (!u.free) continue;
                const int ax = int((size_t(t) + i) % 3);
                const double sign = t < 3 ? 1.0 : -1.0;
                if (sh.fixed[size_t(u.first + 3 + ax)]) continue;
                sh.x0[size_t(u.first + 3 + ax)] += sign * MATE_SHAKE_ANGLE * L;
                moved = true;
            }
            if (!moved) break;
            const solver::SystemResult tr = solver::solveSystem(sh);
            if (tr.status != solver::SystemStatus::Converged && tr.status != solver::SystemStatus::Redundant) continue;
            solver::System fin = sys;  // statuses, DOF and diagnosis over the whole system from the shaken solution
            fin.x0 = tr.x;
            const solver::SystemResult fr = solver::solveSystem(fin);
            if (fr.status == solver::SystemStatus::Converged || fr.status == solver::SystemStatus::Redundant) {
                res = fr;
                break;
            }
        }
    }
    st.dof = res.dof;
    // Degrees of freedom of each instance (§7.5): rank of the rows of its 6 parameters in the null-space basis of the
    // Jacobian at the solution.
    std::vector<int> fpar;
    for (size_t p = 0; p < sys.x0.size(); ++p)
        if (!sys.fixed[p]) fpar.push_back(int(p));
    std::vector<int> colOf(sys.x0.size(), -1);
    for (size_t c = 0; c < fpar.size(); ++c) colOf[size_t(fpar[c])] = int(c);
    Eigen::MatrixXd nullZ;
    {
        const Eigen::Index n = Eigen::Index(fpar.size());
        Eigen::MatrixXd J = Eigen::MatrixXd::Zero(Eigen::Index(sys.equations.size()), n);
        for (size_t e = 0; e < sys.equations.size(); ++e) {
            const auto& eq = sys.equations[e];
            std::vector<double> xe, g(eq.params.size());
            for (int p : eq.params) xe.push_back(res.x[size_t(p)]);
            eq.fn->gradient(xe.data(), g.data());
            for (size_t j = 0; j < eq.params.size(); ++j)
                if (const int c = colOf[size_t(eq.params[j])]; c >= 0) J(Eigen::Index(e), c) += g[j];
        }
        if (n > 0) {
            Eigen::JacobiSVD<Eigen::MatrixXd> svd(J, Eigen::ComputeFullV);
            const auto& sv = svd.singularValues();
            const double thr = PULL_NULL_TOL * std::max(1.0, sv.size() ? sv[0] : 0.0);
            Eigen::Index rank = 0;
            for (Eigen::Index i = 0; i < sv.size(); ++i) rank += sv[i] > thr;
            nullZ = svd.matrixV().rightCols(n - rank);
        }
    }
    auto instanceDof = [&](const Unknown& u) {
        std::vector<int> rows;
        for (int i = 0; i < 6; ++i)
            if (const int c = colOf[size_t(u.first + i)]; c >= 0) rows.push_back(c);
        if (rows.empty() || nullZ.cols() == 0) return 0;
        Eigen::MatrixXd B(Eigen::Index(rows.size()), nullZ.cols());
        for (size_t r = 0; r < rows.size(); ++r) B.row(Eigen::Index(r)) = nullZ.row(rows[r]);
        Eigen::JacobiSVD<Eigen::MatrixXd> svd(B);
        int k = 0;
        for (Eigen::Index i = 0; i < svd.singularValues().size(); ++i) k += svd.singularValues()[i] > 1e-6;
        return k;
    };
    // Placements of the bodies; every entry moves with its deepest body (§2б: free units inside nested assemblies).
    std::map<std::string, Motion3> moved;  // body path → nm · M_old⁻¹
    for (auto& u : unk) {
        Motion3 nm = u.base;
        if (u.free) {
            const Vec3 w(res.x[size_t(u.first + 3)] / L, res.x[size_t(u.first + 4)] / L, res.x[size_t(u.first + 5)] / L);
            const double th = w.norm();
            const Eigen::Matrix3d Rw = th > 0 ? Eigen::AngleAxisd(th, w / th).toRotationMatrix() : Eigen::Matrix3d::Identity();
            nm.R = Rw * u.base.R;
            nm.t = Vec3(res.x[size_t(u.first)], res.x[size_t(u.first + 1)], res.x[size_t(u.first + 2)]);
            if (auto* e = const_cast<InstanceEntry*>(st.find(u.path))) e->freeDof = instanceDof(u);
        } else if (auto* e = const_cast<InstanceEntry*>(st.find(u.path))) {
            e->freeDof = 0;
        }
        moved[u.path] = nm * inverse(st.find(u.path)->world);
    }
    // Relative placements of nested free units before the move (§2б: formChanged).
    std::map<std::string, Motion3> relBefore;
    for (const auto& u : unk)
        if (const size_t sl = u.path.rfind('/'); sl != std::string::npos)
            if (const auto* pe = st.find(u.path.substr(0, sl))) relBefore[u.path] = inverse(pe->world) * st.find(u.path)->world;
    for (auto& e : st.instances)
        for (std::string p = e.path;;) {
            if (auto it = moved.find(p); it != moved.end()) {
                e.world = it->second * e.world;
                break;
            }
            const size_t sl = p.rfind('/');
            if (sl == std::string::npos) break;
            p.resize(sl);
        }
    for (const auto& [path, r0] : relBefore) {
        auto* e = const_cast<InstanceEntry*>(st.find(path));
        const Motion3 r1 = inverse(st.find(path.substr(0, path.rfind('/')))->world) * e->world;
        if ((r1.t - r0.t).norm() > 1e-9 || Eigen::AngleAxisd(r0.R.transpose() * r1.R).angle() > 1e-9) e->formChanged = true;
    }
    // Statuses (§7.5).
    auto idOf = [&](uint64_t owner) { return rms[size_t(owner - 1)].m->id; };
    if (res.diagnosis) {
        for (const auto& g0 : res.diagnosis->groups) {
            // Orientation conflict (FX9-ASM-013): mates whose auxiliary (orientation) equations take part in a conflicting
            // dependency — the solver reports their owners in `arcs` (solver.md §3.8) — are in the group too when they come
            // after all its mates with main equations in document order (the later mate brought the conflict; an earlier
            // one only gives the orientation the others rely on).
            solver::Group g = g0;
            if ((g.kind == solver::GroupKind::Conflicting || res.status == solver::SystemStatus::Conflict) && !g.constraints.empty()) {
                const uint64_t last = *std::max_element(g0.constraints.begin(), g0.constraints.end());
                for (auto a : g0.arcs)
                    if (uint64_t(a) > last && std::find(g.constraints.begin(), g.constraints.end(), uint64_t(a)) == g.constraints.end())
                        g.constraints.push_back(uint64_t(a));
                std::sort(g.constraints.begin(), g.constraints.end());
            }
            std::vector<std::string> ids;
            for (auto c : g.constraints)
                if (!rms[size_t(c - 1)].sub) ids.push_back(idOf(c));
            std::string suggested;
            if (!res.diagnosis->suggestedRemoval.empty() && !rms[size_t(res.diagnosis->suggestedRemoval.front() - 1)].sub)
                suggested = idOf(res.diagnosis->suggestedRemoval.front());
            for (auto c : g.constraints)
                for (auto& ms : st.mates)
                    if (ms.id == idOf(c)) {
                        if (g.kind == solver::GroupKind::Conflicting || res.status == solver::SystemStatus::Conflict)
                            ms.error = Error("ASM_MATE_CONFLICT").with("ids", ids).with("suggested", suggested);
                        else if (!ms.error) ms.warning = Error("ASM_MATE_REDUNDANT").with("ids", ids);
                    }
        }
    }
    if (res.status == solver::SystemStatus::NotConverged) {
        bool placed = false;
        for (auto& ms : st.mates) placed = placed || (!ms.error && startBad.count(ms.id));
        for (auto& ms : st.mates)
            if (!ms.error && !ms.warning && (!placed || startBad.count(ms.id)))
                ms.error = Error("ASM_MATE_CONFLICT").with("ids", std::vector<std::string>{ms.id}).with("suggested", ms.id);
    }
    // Underdetermined (§2б): free bodies with free degrees — and then the whole system for the enclosing assembly.
    for (const auto& u : unk)
        if (const auto* e = st.find(u.path); u.free && e && e->freeDof > 0) st.flexUnits.push_back(u.path);
    if (st.flexUnits.empty()) return;
    for (const auto& r : rms) {
        if (!r.sub)
            if (const auto* ms = st.mate(r.m->id); !ms || ms->error) continue;
        st.systemMates.push_back({r.m->id, r.m->type, {r.a.leaf, r.b.leaf}, {r.a.local, r.b.local}, r.value});
    }
}

}  // namespace construe::regen
