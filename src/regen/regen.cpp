// Regeneration: feature evaluators, cache, rollback, body IDs (spec/regen.md).
#include "regen/regen.h"
#include "regen/eval_common.h"
#include "regen/hole_tables.h"
#include "regen/instance_patterns.h"
#include "regen/mates.h"

#include "base/tolerance.h"

#include "base/files.h"
#include "base/hash.h"
#include "base/ids.h"
#include "base/number_format.h"
#include "base/spline.h"
#include "base/geom2.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <numbers>
#include <set>

namespace construe::regen {

const char* featureStatusName(FeatureStatus s) {
    switch (s) {
        case FeatureStatus::Ok: return "ok";
        case FeatureStatus::Warning: return "warning";
        case FeatureStatus::Error: return "error";
        case FeatureStatus::Suppressed: return "suppressed";
        case FeatureStatus::RolledBack: return "rolledBack";
    }
    return "?";
}

// ---------------------------------------------------------------- body order (§2.4)

namespace {

std::vector<long long> creationKey(const std::string& id) {
    // body:f<N>[.<k>]{.s<k>}
    std::vector<long long> key;
    size_t i = 6;  // after "body:f"
    size_t j = i;
    while (j < id.size() && std::isdigit(static_cast<unsigned char>(id[j]))) ++j;
    key.push_back(std::stoll(id.substr(i, j - i)));
    i = j;
    if (i < id.size() && id[i] == '.' && i + 1 < id.size() && id[i + 1] != 's') {
        j = i + 1;
        while (j < id.size() && std::isdigit(static_cast<unsigned char>(id[j]))) ++j;
        key.push_back(std::stoll(id.substr(i + 1, j - i - 1)));
        i = j;
    } else {
        key.push_back(0);
    }
    while (i + 2 < id.size() && id[i] == '.' && id[i + 1] == 's') {
        j = i + 2;
        while (j < id.size() && std::isdigit(static_cast<unsigned char>(id[j]))) ++j;
        key.push_back(std::stoll(id.substr(i + 2, j - i - 2)));
        i = j;
    }
    return key;
}

}  // namespace

bool bodyIdLess(const std::string& a, const std::string& b) { return creationKey(a) < creationKey(b); }

std::optional<Plane> planeFromFrame(const std::array<double, 9>& f) {
    Plane p;
    p.origin = Point3(f[0], f[1], f[2]);
    Vec3 x(f[3], f[4], f[5]), y(f[6], f[7], f[8]);
    if (x.norm() < LINEAR_TOL) return std::nullopt;
    x.normalize();
    y -= y.dot(x) * x;  // Gram–Schmidt (document.md §2.7)
    if (y.norm() < LINEAR_TOL) return std::nullopt;
    y.normalize();
    p.x = x, p.y = y, p.normal = x.cross(y);
    return p;
}

kernel::Profile toKernelProfile(const sketch::Region& r, const Plane& frame) {
    kernel::Profile p;
    p.plane = frame;
    auto W = [&](const Point2& q) { return frame.toWorld(q); };
    auto loop = [&](const sketch::Loop& l) {
        kernel::Loop3 out;
        for (const auto& s : l.segments) {
            kernel::Segment3 g;
            g.id = s.id;
            g.reversed = s.reversed;
            switch (s.kind) {
                case sketch::Segment::Kind::Line:
                    g.kind = kernel::Segment3::Kind::Line;
                    g.p1 = W(s.p1);
                    g.p2 = W(s.p2);
                    break;
                case sketch::Segment::Kind::Arc:
                    g.kind = kernel::Segment3::Kind::Arc;
                    g.center = W(s.center);
                    g.start = W(s.start);
                    g.end = W(s.end);
                    g.radius = (s.start - s.center).norm();
                    break;
                case sketch::Segment::Kind::Circle:
                    g.kind = kernel::Segment3::Kind::Circle;
                    g.center = W(s.center);
                    g.radius = s.radius;
                    break;
                case sketch::Segment::Kind::Ellipse:  // S4-SKT-004
                    g.kind = kernel::Segment3::Kind::Ellipse;
                    g.center = W(s.center);
                    g.radius = s.radius;
                    g.minorRadius = s.minorRadius;
                    g.xDir = (std::cos(s.angle) * frame.x + std::sin(s.angle) * frame.y).normalized();
                    break;
                case sketch::Segment::Kind::Spline:  // S4A-SPL-003: poles to world coordinates, the rest as is
                    g.kind = kernel::Segment3::Kind::Spline;
                    for (const auto& q : s.curve.poles) g.poles.push_back(W(q));
                    g.weights = s.curve.weights;
                    g.knots = s.curve.knots;
                    g.mults = s.curve.mults;
                    g.degree = s.curve.degree;
                    break;
                case sketch::Segment::Kind::EllipseArc:  // S4-SKT-007
                    g.kind = kernel::Segment3::Kind::EllipseArc;
                    g.center = W(s.center);
                    g.radius = s.radius;
                    g.minorRadius = s.minorRadius;
                    g.xDir = (std::cos(s.angle) * frame.x + std::sin(s.angle) * frame.y).normalized();
                    g.start = W(s.start);
                    g.end = W(s.end);
                    break;
            }
            out.segments.push_back(g);
        }
        return out;
    };
    p.outer = loop(r.outer);
    for (const auto& h : r.holes) p.holes.push_back(loop(h));
    return p;
}

expr::DocumentUnits documentUnits(const doc::Document& d) {
    return {expr::parseLengthUnit(d.lengthUnit).value_or(expr::LengthUnit::mm),
            expr::parseAngleUnit(d.angleUnit).value_or(expr::AngleUnit::deg)};
}

sketch::Values dimensionValues(const sketch::SketchData& s, const expr::VariableTable& vars,
                               const expr::DocumentUnits& units, const std::string& featurePath) {
    sketch::Values values;
    const expr::Env env = vars.env();
    for (size_t ci = 0; ci < s.constraints.size(); ++ci) {
        const auto& con = s.constraints[ci];
        if (!con.value || !con.driving.value_or(true) || !sketch::isDimensionType(con.type)) continue;
        const expr::Dim dim = sketch::isAngleDimension(con.type) ? expr::ANGLE : sketch::isNumberDimension(con.type) ? expr::DIMLESS : expr::LENGTH;
        auto q = expr::evaluateField(*con.value, dim, env, units);
        if (q) {
            values.emplace(con.id, Result<double>(q->value));
        } else {
            Error e = q.error();
            e.at(featurePath + "/constraints/" + std::to_string(ci) + "/value");
            values.emplace(con.id, Result<double>(e));
        }
    }
    return values;
}

Regenerator::Regenerator(const kernel::Kernel& k) : k_(k) {}

uint64_t Regenerator::initialKey() { return hash64("construe-initial"); }

// ---------------------------------------------------------------- key (§5.1)

namespace {

// Feature expressions (for references to variables).
std::vector<std::string> featureExpressions(const doc::Feature& f) {
    std::vector<std::string> out;
    for (const auto& e : doc::expressionFields(f))  // document.md §2.11
        if (e.driving) out.push_back(e.text);
    return out;
}

std::string hex16(uint64_t v) {
    static const char* d = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[i] = d[v & 0xF];
        v >>= 4;
    }
    return s;
}

}  // namespace

uint64_t Regenerator::evaluatorKey(const doc::Document& d, const doc::Feature& f, uint64_t inputKey,
                                   const expr::VariableTable& vars) {
    std::string bytes = std::to_string(REGEN_ALGO_VERSION);
    bytes += '\x1F';
    if (f.tolerances) {  // dimension tolerances do not change the geometry (§5.1, FX12-PMI-007)
        doc::Feature plain = f;
        plain.tolerances.reset();
        bytes += doc::serializeFeature(plain);
    } else {
        bytes += doc::serializeFeature(f);
    }
    bytes += '\x1F';
    bytes += hex16(inputKey);
    bytes += '\x1F';
    bytes += d.lengthUnit + "/" + d.angleUnit;
    std::set<std::string> names;
    for (const auto& text : featureExpressions(f)) {
        auto ast = expr::parse(text);
        if (!ast) continue;
        for (const auto& n : expr::references(*ast)) names.insert(n);
    }
    for (const auto& n : names) {
        bytes += '\x1F';
        bytes += n + "=";
        const expr::VariableEntry* e = vars.find(n);
        if (!e) bytes += "U";
        else if (!e->value.ok()) bytes += "E:" + e->value.error().code;
        else
            bytes += canonicalNumber(e->value->value) + "L" + std::to_string(e->value->dim.L) + "A" +
                     std::to_string(e->value->dim.A);
    }
    return hash64(bytes);
}

// ---------------------------------------------------------------- cache

EvalPtr Regenerator::cacheGet(uint64_t key) {
    auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    if (recording_) record(key, it->second->second);
    return it->second->second;
}

void Regenerator::record(uint64_t key, const EvalPtr& r) {
    if (auto it = recordedIndex_.find(key); it != recordedIndex_.end()) {
        recorded_[it->second].second = r;
        return;
    }
    recordedIndex_[key] = recorded_.size();
    recorded_.emplace_back(key, r);
}

void Regenerator::startRecording() {
    recording_ = true;
    recorded_.clear();
    recordedIndex_.clear();
}

std::vector<std::pair<uint64_t, EvalPtr>> Regenerator::stopRecording() {
    recording_ = false;
    recordedIndex_.clear();
    return std::exchange(recorded_, {});
}

void Regenerator::preload(const std::vector<std::pair<uint64_t, EvalPtr>>& entries) {
    const bool was = recording_;
    recording_ = false;  // preloading is not usage (§5.2)
    for (const auto& [k, r] : entries) cachePut(k, r);
    recording_ = was;
}

void Regenerator::cachePut(uint64_t key, EvalPtr r) {
    if (recording_) record(key, r);
    if (auto it = index_.find(key); it != index_.end()) {
        it->second->second = std::move(r);
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    lru_.emplace_front(key, std::move(r));
    index_[key] = lru_.begin();
    while (lru_.size() > CACHE_MAX_ENTRIES) {
        index_.erase(lru_.back().first);
        lru_.pop_back();
    }
}

// ---------------------------------------------------------------- regeneration (§3)


std::string parentDir(const std::string& path) {
    if (path.empty()) return {};
    return u8str(u8path(path).parent_path());
}

std::string linkPath(const std::string& dir, const std::string& ref) {
    if (doc::isLibraryRef(ref)) return ref;  // a library part copy of the document — the key as is (library.md §6.3)
    std::filesystem::path p = u8path(ref);
    if (p.is_relative() && !dir.empty()) p = u8path(dir) / p;
    return u8str(p.lexically_normal());
}

Motion3 motionOf(const std::array<double, 3>& p, const std::array<double, 4>& q0) {
    const double n = std::sqrt(q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2] + q0[3] * q0[3]);
    const Eigen::Quaterniond q = n > 0 ? Eigen::Quaterniond(q0[0] / n, q0[1] / n, q0[2] / n, q0[3] / n) : Eigen::Quaterniond::Identity();
    Motion3 m;
    m.R = q.toRotationMatrix();
    m.t = Vec3(p[0], p[1], p[2]);
    return m;
}

void appendInstanceState(const kernel::Kernel& k, PartState& v, const std::string& path, const PartState& part, const Motion3& M,
                         const std::map<std::string, kernel::Shape>* reuse) {
    kernel::Motion km;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) km.r[size_t(3 * i + j)] = M.R(i, j);
    km.t = M.t;
    const int body0 = int(v.bodies.size());
    const int face0 = int(v.names.faces.size()), edge0 = int(v.names.edges.size()), vertex0 = int(v.names.vertices.size());
    for (const auto& b : part.bodies) {
        Body nb = b;
        nb.id = path + "/" + b.id;
        const kernel::Shape* pre = nullptr;
        if (reuse)
            if (auto it = reuse->find(nb.id); it != reuse->end()) pre = &it->second;
        if (pre) nb.shape = *pre;
        else if (auto t = k.transform(b.shape, km)) nb.shape = t->shape;
        v.bodies.push_back(nb);
    }
    for (auto f : part.names.faces) {
        f.body += body0, f.full = path + "/" + f.full, f.key = M.apply(f.key);
        v.names.byName[f.full] = {naming::EntityKind::Face, int(v.names.faces.size())};
        v.names.faces.push_back(std::move(f));
    }
    for (auto ed : part.names.edges) {
        ed.body += body0, ed.f1 += face0, ed.f2 += face0, ed.full = path + "/" + ed.full, ed.key = M.apply(ed.key);
        v.names.byName[ed.full] = {naming::EntityKind::Edge, int(v.names.edges.size())};
        v.names.edges.push_back(std::move(ed));
    }
    for (auto vx : part.names.vertices) {
        vx.body += body0, vx.full = path + "/" + vx.full, vx.key = M.apply(vx.key);
        for (auto& fi : vx.faces) fi += face0;
        v.names.byName[vx.full] = {naming::EntityKind::Vertex, int(v.names.vertices.size())};
        v.names.vertices.push_back(std::move(vx));
    }
    v.names.subs[path] = {std::make_shared<const naming::NameTable>(part.names), face0, edge0, vertex0};
}

std::vector<std::string> contextRefs(const doc::Element& el) {
    std::vector<std::string> out;
    for (const auto& f : el.features)
        if (const auto* s = f->sketchData()) {
            if (naming::hasPath(s->plane.face)) out.push_back(s->plane.face);
            for (const auto& e : s->entities) {
                if (e.type != sketch::EntityType::Projected) continue;
                const std::string src = sketch::projectionSourceRef(e.source);
                if (naming::hasPath(src)) out.push_back(src);
            }
        }
    return out;
}

std::vector<std::string> usedContextPaths(const doc::Element& el) {
    std::set<std::string, IdLess> paths;
    for (const auto& r : contextRefs(el)) {
        const std::string p = naming::splitPath(r).path;
        paths.insert(p.substr(0, p.find('/')));  // top-level instance
    }
    return {paths.begin(), paths.end()};
}

namespace {
// Key link for assembly input and an in-context part (§5.1): path, part state key, placement — by content.
uint64_t instanceKey(uint64_t h, const std::string& path, uint64_t stateKey, const Motion3& M) {
    h = construe::hashCombine(h, std::string_view(path));
    h = construe::hashCombine(h, stateKey);
    for (int i = 0; i < 9; ++i) h = construe::hashCombine(h, double(M.R(i / 3, i % 3)));
    for (int i = 0; i < 3; ++i) h = construe::hashCombine(h, double(M.t[i]));
    return h;
}

const doc::Instance* contextInstance(const doc::Document& d, const doc::Element& el, const std::string& path) {
    if (!el.context) return nullptr;
    const int ai = d.elementIndex(el.context->assembly);
    if (ai < 0 || !d.elements[size_t(ai)].isAssembly()) return nullptr;
    for (const auto& in : d.elements[size_t(ai)].instances)
        if (in.id == path) return &in;
    return nullptr;
}

bool dependsRec(const doc::Document& d, const std::string& from, const std::string& target, std::set<std::string>& seen) {
    if (!seen.insert(from).second) return false;
    const int fi = d.elementIndex(from);
    if (fi < 0) return false;
    const doc::Element& el = d.elements[size_t(fi)];
    if (!el.context || el.context->frozen) return false;  // a frozen part depends on nobody (§7в.4)
    for (const auto& p : usedContextPaths(el)) {
        const doc::Instance* in = contextInstance(d, el, p);
        if (!in || !in->refFile.empty()) continue;
        if (in->refElement == target) return true;
        if (dependsRec(d, in->refElement, target, seen)) return true;
    }
    return false;
}
}  // namespace

std::vector<std::string> visibleSketchIds(const doc::Element& el) {
    std::set<std::string> used;
    for (const auto& f : el.features) {
        if (const auto* x = f->extrude()) used.insert(x->sketch);
        if (const auto* r = f->revolve()) used.insert(r->sketch);
        if (f->type == doc::FeatureType::Coil) used.insert(f->pattern()->def.value("sketch", std::string()));
        // The base wall of a sheet metal part hides its sketch like the other features (FX10-SH-001).
        if (f->type == doc::FeatureType::SheetWall) used.insert(f->pattern()->def.value("sketch", std::string()));
        if (f->type == doc::FeatureType::Sweep) {  // sweep profile and path sketch
            const auto& d = f->pattern()->def;
            used.insert(d.value("sketch", std::string()));
            if (d["path"].contains("sketch")) used.insert(d["path"]["sketch"].get<std::string>());
        }
        if (f->type == doc::FeatureType::Loft)  // loft sections
            for (const auto& s : f->pattern()->def["sections"]) used.insert(s["sketch"].get<std::string>());
    }
    std::vector<std::string> out;
    for (const auto& f : el.features)
        if (f->sketchData() && !f->suppressed && !used.count(f->id)) out.push_back(f->id);
    return out;
}

namespace {

// Points, lines, circles and arcs of sketch feature f with entry se, in the frame of se's state (assembly.md §7.2:
// FX7-ASM-006, FX9-ASM-014).
DatumSketch datumSketchOf(const doc::Feature& f, const SketchEntry& se) {
    DatumSketch ds;
    ds.frame = se.frame;
    const auto& P = se.result.state.points;
    for (const auto& [pid, q] : P) ds.points[pid] = se.frame.toWorld(q);
    for (const auto& e : f.sketchData()->entities) {
        if (e.type == sketch::EntityType::Line && P.count(e.p1) && P.count(e.p2))
            ds.lines[e.id] = {se.frame.toWorld(P.at(e.p1)), se.frame.toWorld(P.at(e.p2))};
        else if (e.type == sketch::EntityType::Circle && P.count(e.center)) {
            const auto rit = se.result.state.radii.find(e.id);
            ds.circles[e.id] = {se.frame.toWorld(P.at(e.center)), rit != se.result.state.radii.end() ? rit->second : e.r};
        } else if (e.type == sketch::EntityType::Arc && P.count(e.center) && P.count(e.start))
            ds.circles[e.id] = {se.frame.toWorld(P.at(e.center)), (P.at(e.start) - P.at(e.center)).norm()};
    }
    return ds;
}

// Ellipse point at parameter t (sketch coordinates) and the parameter of a point (sketch.md §2.1, S4-SKT-004, -007).
Point2 ellipseAt(const Point2& c, const sketch::SolvedState::EllipseShape& sh, double t) {
    const double x = sh.rx * std::cos(t), y = sh.ry * std::sin(t), cs = std::cos(sh.angle), sn = std::sin(sh.angle);
    return c + Point2(x * cs - y * sn, x * sn + y * cs);
}
double ellipseParamOf(const Point2& c, const sketch::SolvedState::EllipseShape& sh, const Point2& p) {
    const double cs = std::cos(sh.angle), sn = std::sin(sh.angle);
    const Point2 w = p - c;
    return std::atan2((-w.x() * sn + w.y() * cs) / sh.ry, (w.x() * cs + w.y() * sn) / sh.rx);
}

// Polyline of a non-construction ellipse, ellipse arc or spline of a sketch (assembly.md §2 «List of reference
// objects», FX9-ASM-014), world = the sketch frame; empty — not such a curve or not solved.
std::vector<Point3> curvePolyline(const sketch::Entity& e, const SketchEntry& se) {
    const auto& st = se.result.state;
    const auto& P = st.points;
    std::vector<Point2> pl;
    if (sketch::isEllipseType(e.type) && P.count(e.center)) {
        const auto it = st.ellipses.find(e.id);
        const auto sh = it != st.ellipses.end() ? it->second : sketch::SolvedState::EllipseShape{e.rx, e.ry, e.angle};
        if (sh.rx <= 0 || sh.ry <= 0) return {};
        const Point2 c = P.at(e.center);
        double t0 = 0, sweep = 2 * std::numbers::pi;
        if (e.type == sketch::EntityType::EllipseArc) {  // counterclockwise by the parameter from start to end
            if (!P.count(e.start) || !P.count(e.end)) return {};
            t0 = ellipseParamOf(c, sh, P.at(e.start));
            sweep = geom2::ccwSweepPositive(t0, ellipseParamOf(c, sh, P.at(e.end)));
        }
        const int steps = std::max(8, int(std::ceil(48 * sweep / (2 * std::numbers::pi))));
        for (int i = 0; i <= steps; ++i) pl.push_back(ellipseAt(c, sh, t0 + sweep * i / steps));
    } else if (sketch::isSplineType(e.type)) {
        for (const auto& q : e.poles)
            if (!P.count(q)) return {};
        const spline::Curve c = sketch::splineCurve(e, st);
        if (!spline::validate(c).empty() || c.poles.empty()) return {};
        Point2 lo = c.poles[0], hi = c.poles[0];
        for (const auto& q : c.poles) lo = lo.cwiseMin(q), hi = hi.cwiseMax(q);
        const spline::Flat f = spline::expand(c);
        for (double t : spline::flattenParams(f, std::max(1e-6, 1e-3 * (hi - lo).norm()))) pl.push_back(spline::eval(f, t));
    }
    std::vector<Point3> out;
    for (const auto& q : pl) out.push_back(se.frame.toWorld(q));
    return out;
}

}  // namespace

std::vector<AssemblyDatum> ownAssemblyDatums(const doc::Element& el, const PartState& final) {
    std::vector<AssemblyDatum> out;
    for (const auto& f : el.features) {  // history order
        if (f->suppressed) continue;
        AssemblyDatum a;
        a.id = f->id;
        a.name = f->name;
        if (auto it = final.planes.find(f->id); it != final.planes.end()) {
            a.kind = AssemblyDatum::Kind::Plane;
            a.plane = it->second;
            out.push_back(std::move(a));
        } else if (auto ax = final.axes.find(f->id); ax != final.axes.end()) {
            a.kind = AssemblyDatum::Kind::Axis;
            a.point = ax->second.point;
            a.dir = ax->second.dir;
            out.push_back(std::move(a));
        }
    }
    for (const auto& id : visibleSketchIds(el)) {
        const auto it = final.sketches.find(id);
        const auto f = el.find(id);
        if (it == final.sketches.end() || !f) continue;
        AssemblyDatum a;
        a.kind = AssemblyDatum::Kind::Sketch;
        a.id = id;
        a.name = f->name;
        for (const auto& c : it->second.curves) {
            std::vector<Point3> pl;
            if (c.kind == WorldCurve::Kind::Line) {
                pl = {c.a, c.b};
            } else {
                // Arc — counterclockwise about the axis from a to b; circle — the full turn from a.
                const Vec3 n = c.axis.normalized();
                Vec3 u0 = c.a - c.center;
                if (c.kind == WorldCurve::Kind::Circle) u0 = n.unitOrthogonal() * c.radius;  // a circle has no start point
                double sweep = 2 * std::numbers::pi;
                if (c.kind == WorldCurve::Kind::Arc) {
                    const Vec3 u1 = c.b - c.center;
                    sweep = std::atan2(n.dot(u0.cross(u1)), u0.dot(u1));
                    if (sweep <= 0) sweep += 2 * std::numbers::pi;
                }
                const int steps = std::max(8, int(std::ceil(48 * sweep / (2 * std::numbers::pi))));
                for (int i = 0; i <= steps; ++i)
                    pl.push_back(c.center + Eigen::AngleAxisd(sweep * i / steps, n).toRotationMatrix() * u0);
            }
            a.polylines.push_back(std::move(pl));
        }
        for (const auto& e : f->sketchData()->entities)  // ellipses and splines (FX9-ASM-014)
            if (!e.construction && (sketch::isEllipseType(e.type) || sketch::isSplineType(e.type)))
                if (auto pl = curvePolyline(e, it->second); pl.size() >= 2) a.polylines.push_back(std::move(pl));
        a.entities = datumSketchOf(*f, it->second);
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<AssemblyDatum> nestedDatumsWorld(const AssemblyState& st) {
    std::vector<AssemblyDatum> out;
    for (const auto& a : st.datums) {
        if (a.node.empty()) continue;
        const InstanceEntry* e = st.find(a.node);
        if (!e || e->hidden || e->suppressed || e->error) continue;
        const Motion3& M = e->world;
        AssemblyDatum w = a;
        w.plane.origin = M.apply(a.plane.origin);
        w.plane.x = M.applyDir(a.plane.x);
        w.plane.y = M.applyDir(a.plane.y);
        w.plane.normal = M.applyDir(a.plane.normal);
        w.point = M.apply(a.point);
        w.dir = M.applyDir(a.dir);
        for (auto& pl : w.polylines)
            for (auto& q : pl) q = M.apply(q);
        DatumSketch& ds = w.entities;  // FX9-ASM-014
        ds.frame.origin = M.apply(ds.frame.origin);
        ds.frame.x = M.applyDir(ds.frame.x);
        ds.frame.y = M.applyDir(ds.frame.y);
        ds.frame.normal = M.applyDir(ds.frame.normal);
        for (auto& [id, q] : ds.points) q = M.apply(q);
        for (auto& [id, l] : ds.lines) l = {M.apply(l.first), M.apply(l.second)};
        for (auto& [id, c] : ds.circles) c.first = M.apply(c.first);
        out.push_back(std::move(w));
    }
    return out;
}

std::optional<AssemblyPtr> Regenerator::nestedAssembly(const doc::Document& D, const doc::Element& asmEl, const std::string& file,
                                                       uint64_t revision, const Cancel* cancel, const LinkMap* links,
                                                       const expr::VariableTable& vars, const std::vector<std::string>& chain,
                                                       const std::vector<doc::Instance::FormPlacement>& form) {
    std::string key = file + "#" + asmEl.id + "#" + std::to_string(variableTableDigest(vars));
    for (const auto& c : chain) key += "|" + c;  // owners of connection cuts differ by the enclosing chain
    for (const auto& f : form) {  // instances of different forms are different evaluations (§2б)
        char buf[200];
        std::snprintf(buf, sizeof buf, "%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g", f.position[0], f.position[1], f.position[2],
                      f.rotation[0], f.rotation[1], f.rotation[2], f.rotation[3]);
        key += "@" + f.path + "=" + buf;
    }
    if (nestedMemo_)
        if (auto it = nestedMemo_->find(key); it != nestedMemo_->end()) return it->second;
    doc::Document pd = D;
    pd.activeElement = asmEl.id;
    // The instance's form (assembly.md §2б): a direct instance takes its placement, a deeper path goes into the form of
    // the nested instance on the way (and replaces its entry there).
    for (const auto& f : form) {
        const size_t sl = f.path.find('/');
        doc::Instance* in = nullptr;
        for (auto& x : pd.active().instances)
            if (x.id == f.path.substr(0, sl)) in = &x;
        if (!in) continue;  // the instance is gone from the subassembly: the entry has no effect
        if (sl == std::string::npos) {
            in->position = f.position;
            in->rotation = f.rotation;
            continue;
        }
        doc::Instance::FormPlacement g = f;
        g.path = f.path.substr(sl + 1);
        auto it = std::find_if(in->form.begin(), in->form.end(), [&](const auto& x) { return x.path == g.path; });
        if (it != in->form.end()) *it = g;
        else in->form.push_back(g);
    }
    const std::vector<std::string> saved = asmChain_;
    asmChain_ = chain;
    nestedCall_ = true;
    auto pr = regenerate(pd, revision, cancel, false, links, file, &vars);
    nestedCall_ = false;
    asmChain_ = saved;
    if (!pr) return std::nullopt;
    AssemblyPtr out = pr->assembly ? pr->assembly : std::make_shared<AssemblyState>();
    if (nestedMemo_) (*nestedMemo_)[key] = out;
    return out;
}

bool contextDependsOn(const doc::Document& d, const std::string& from, const std::string& target) {
    std::set<std::string> seen;
    return dependsRec(d, from, target, seen);
}

bool ownedByAssembly(const doc::Feature& f, const std::string& assemblyId, bool sameDocument, const std::string& partFile,
                     const std::string& assemblyFile) {
    if (f.type != doc::FeatureType::ConnectionCut || !f.pattern()) return false;
    const doc::Json& o = f.pattern()->def["owner"];
    // A cut of a library element (library.md §6.6 item 8) is an ordinary part feature: not excluded in its assembly.
    if (o.contains("library")) return false;
    if (o["assembly"].get<std::string>() != assemblyId) return false;
    if (!o.contains("file")) return sameDocument;
    return !assemblyFile.empty() && linkPath(parentDir(partFile), o["file"].get<std::string>()) == assemblyFile;
}

void withoutOwnedCuts(doc::Element& el, const std::string& assemblyId, bool sameDocument, const std::string& partFile,
                      const std::string& assemblyFile) {
    std::vector<doc::FeaturePtr> kept;
    for (const auto& f : el.features) {
        if (!ownedByAssembly(*f, assemblyId, sameDocument, partFile, assemblyFile)) {
            kept.push_back(f);
            continue;
        }
        // The rollback marker on a removed feature moves to the previous remaining one (assembly.md §2).
        if (el.rollback.kind == doc::Rollback::Kind::After && el.rollback.feature == f->id)
            el.rollback = kept.empty() ? doc::Rollback{doc::Rollback::Kind::Start, {}} : doc::Rollback{doc::Rollback::Kind::After, kept.back()->id};
    }
    el.features = std::move(kept);
}

std::optional<PartResult> Regenerator::regenerate(const doc::Document& d, uint64_t revision, const Cancel* cancel,
                                                  bool countStats, const LinkMap* links, const std::string& selfPath,
                                                  const expr::VariableTable* vars) {
    const bool nested = nestedCall_;
    nestedCall_ = false;
    const std::vector<std::string> outer = nested ? asmChain_ : std::vector<std::string>{};  // enclosing assemblies
    // Nested assemblies are memoized for the whole top-level regeneration (FX9-ASM-012).
    std::map<std::string, AssemblyPtr> memo;
    const bool memoOwner = nestedMemo_ == nullptr;
    if (memoOwner) nestedMemo_ = &memo;
    struct MemoReset {
        Regenerator* r;
        bool owner;
        ~MemoReset() {
            if (owner) r->nestedMemo_ = nullptr;
        }
    } memoReset{this, memoOwner};
    PartResult res;
    res.revision = revision;
    res.variables = vars ? *vars : documentVariables(d, links, selfPath);
    auto init = std::make_shared<PartState>();
    res.initial = init;
    // Exploded view (assembly.md §7д.2): the solve of the mode, applied after the assembly features.
    std::optional<AssemblyState> explodedSolved;
    bool explodedAuto = false;
    std::vector<std::string> explodedSuppressed;
    if (d.active().isAssembly()) {  // assembly (assembly.md §2, §5а)
        res.final = init;
        auto st = std::make_shared<AssemblyState>();
        std::map<std::string, std::pair<StatePtr, uint64_t>> parts;  // (file#part) → final of this revision and its key (§5.1)
        bool cancelled = false;
        struct Ctx {
            const doc::Document* D;
            std::string file;  // absolute path of document D (empty — unsaved root)
        };
        // Variables of the documents on the walk (assembly.md §2а): the root — res.variables, linked files — once.
        std::map<std::string, expr::VariableTable> fileVars;
        std::map<std::string, std::map<std::string, std::string>> fileQuals;
        auto varsOf = [&](const Ctx& cx) -> const expr::VariableTable& {
            if (cx.D == &d) return res.variables;
            auto it = fileVars.find(cx.file);
            if (it == fileVars.end()) it = fileVars.emplace(cx.file, documentVariables(*cx.D, links, cx.file)).first;
            return it->second;
        };
        auto qualsOf = [&](const Ctx& cx) -> const std::map<std::string, std::string>& {
            auto it = fileQuals.find(cx.file);
            if (it == fileQuals.end()) it = fileQuals.emplace(cx.file, instanceQualifiers(*cx.D)).first;
            return it->second;
        };
        std::function<void(const Ctx&, const doc::Element&, const Motion3&, const std::string&, std::vector<std::string>, int, bool, bool)> walk =
            [&](const Ctx& cx, const doc::Element& asmEl, const Motion3& M, const std::string& prefix, std::vector<std::string> chain, int depth,
                bool hiddenUp, bool transparentUp) {
                chain.push_back(cx.file + "#" + asmEl.id);
                for (const auto& in : asmEl.instances) {
                    if (cancelled) return;
                    InstanceEntry e;
                    e.path = prefix + in.id;
                    e.instance = in.id;
                    e.name = in.name;
                    e.element = in.refElement;
                    e.depth = depth;
                    e.fixed = in.fixed;
                    e.hidden = in.hidden || hiddenUp;
                    e.transparent = in.transparent || transparentUp;
                    e.suppressed = in.suppressed;
                    e.world = M * motionOf(in.position, in.rotation);
                    if (in.suppressed) {
                        st->instances.push_back(std::move(e));
                        continue;
                    }
                    // Document of the inserted object: this one or a linked file (§5а).
                    Ctx rc = cx;
                    if (!in.refFile.empty()) {
                        const std::string abs = linkPath(parentDir(cx.file), in.refFile);
                        e.file = abs;
                        const LinkedDoc* ld = nullptr;
                        if (links)
                            if (auto it = links->find(abs); it != links->end()) ld = &it->second;
                        if (!ld || (!ld->doc && !ld->error)) {
                            e.error = Error("ASM_FILE_NOT_FOUND").with("path", in.refFile);
                            st->instances.push_back(std::move(e));
                            continue;
                        }
                        if (ld->error) {
                            Error er = *ld->error;
                            er.with("path", in.refFile);
                            e.error = er;
                            st->instances.push_back(std::move(e));
                            continue;
                        }
                        rc = Ctx{static_cast<const doc::Document*>(ld->doc), abs};
                    } else {
                        e.file = cx.file == selfPath ? std::string() : cx.file;
                    }
                    const int ei = rc.D->elementIndex(in.refElement);
                    if (ei < 0) {
                        e.error = Error("ASM_REF_NOT_FOUND").with("ref", in.refElement);
                        st->instances.push_back(std::move(e));
                        continue;
                    }
                    const doc::Element& ref = rc.D->elements[size_t(ei)];
                    e.kind = ref.kind;
                    if (auto ve = instanceVariablesError(in, *rc.D)) {  // assembly.md §2а
                        e.error = *ve;
                        st->instances.push_back(std::move(e));
                        continue;
                    }
                    const std::string key = rc.file + "#" + ref.id;
                    if (std::find(chain.begin(), chain.end(), key) != chain.end()) {
                        std::vector<std::string> ids = chain;
                        ids.push_back(key);
                        e.error = Error("ASM_CYCLE").with("ids", ids);
                        st->instances.push_back(std::move(e));
                        continue;
                    }
                    if (ref.isAssembly()) {
                        // A nested assembly — as when opened by itself (assembly.md §2 «Nested assembly»): its entries
                        // with solved mates, assembly features, connections and patterns, moved into this frame.
                        const Motion3 W = e.world;
                        const bool h = e.hidden, t = e.transparent;
                        const std::string path = e.path;
                        st->instances.push_back(std::move(e));
                        // An assembly element of the library (§2, library.md §2.6): the copy with the instance's size row
                        // and formulas, so that the row acts on every part of the element.
                        std::optional<AssemblyPtr> sub;
                        if (doc::isLibraryRef(in.refFile) && (!in.variantRows.empty() || !in.overrides.empty())) {
                            doc::Document cd = *rc.D;
                            for (const auto& vr : in.variantRows)
                                for (auto& vt : cd.variantTables)
                                    if (vt.id == vr.table) vt.active = vr.row;
                            for (const auto& o : in.overrides)
                                for (auto& v : cd.variables)
                                    if (v.id == o.variable) v.expr = o.expr;
                            const expr::VariableTable cv = documentVariables(cd, links, rc.file);
                            sub = nestedAssembly(cd, cd.elements[size_t(ei)], rc.file, revision, cancel, links, cv, chain, in.form);
                        } else {
                            sub = nestedAssembly(*rc.D, ref, rc.file, revision, cancel, links, varsOf(rc), chain, in.form);
                        }
                        if (!sub) {
                            cancelled = true;
                            return;
                        }
                        // Underdetermined (§2б, FX9-ASM-010): its free units and mates join the system of this assembly.
                        if (!(*sub)->flexUnits.empty()) st->instances.back().flex = *sub;
                        // Its reference geometry and visible sketches (FX9-ASM-008), carried by the node or a nested entry.
                        for (const auto& x : (*sub)->datums) {
                            AssemblyDatum y = x;
                            y.node = x.node.empty() ? path : path + "/" + x.node;
                            st->datums.push_back(std::move(y));
                        }
                        for (const auto& x : (*sub)->instances) {
                            InstanceEntry y = x;
                            y.path = path + "/" + x.path;
                            y.depth = depth + 1 + x.depth;
                            y.world = W * x.world;
                            y.hidden = x.hidden || h;
                            y.transparent = x.transparent || t;
                            if (x.file.empty()) y.file = rc.file == selfPath ? std::string() : rc.file;
                            if (!x.connection.empty()) y.connection = path + "/" + x.connection;
                            if (!x.pattern.empty()) y.pattern = path + "/" + x.pattern;
                            if (!x.copyOf.empty()) y.copyOf = path + "/" + x.copyOf;
                            y.freeDof = -1;
                            st->instances.push_back(std::move(y));
                        }
                        continue;
                    }
                    e.transparent = e.transparent || ref.transparent;
                    // Instance variables (assembly.md §2а): the part is evaluated with its instance's values.
                    const expr::VariableTable instVars =
                        instanceVariableTable(varsOf(cx), qualsOf(cx).at(in.id), *rc.D, rc.D == cx.D);
                    const std::string partKey = key + "#" + std::to_string(variableTableDigest(instVars));
                    auto it = parts.find(partKey);
                    if (it == parts.end()) {
                        doc::Document pd = *rc.D;
                        pd.activeElement = ref.id;
                        withoutOwnedCuts(pd.active(), d.active().id, rc.D == &d, rc.file, selfPath);  // assembly.md §2
                        for (const auto& o : outer) {  // and of the enclosing assemblies (nested evaluation)
                            const size_t h = o.rfind('#');
                            const std::string of = o.substr(0, h);
                            withoutOwnedCuts(pd.active(), o.substr(h + 1), rc.file == of, rc.file, of);
                        }
                        auto pr = regenerate(pd, revision, cancel, false, links, rc.file, &instVars);
                        if (!pr) {
                            cancelled = true;
                            return;
                        }
                        it = parts.emplace(partKey, std::make_pair(pr->final, pr->finalKey)).first;
                    }
                    e.state = it->second.first;
                    e.stateKey = it->second.second;
                    {  // lines, circles and arcs of the part's sketches — mate references to them (assembly.md §7.2, FX9-ASM-001)
                        auto se = std::make_shared<std::map<std::string, std::map<std::string, std::vector<std::string>>>>();
                        for (const auto& f : ref.features)
                            if (const auto* sd = f->sketchData())
                                for (const auto& en : sd->entities) {
                                    if (en.type == sketch::EntityType::Line) (*se)[f->id][en.id] = {"line", en.p1, en.p2};
                                    else if (en.type == sketch::EntityType::Circle) (*se)[f->id][en.id] = {"circle", en.center};
                                    else if (en.type == sketch::EntityType::Arc) (*se)[f->id][en.id] = {"arc", en.center, en.start};
                                }
                        e.sketchEntities = std::move(se);
                    }
                    st->instances.push_back(std::move(e));
                }
            };
        walk(Ctx{&d, selfPath}, d.active(), Motion3{}, "", outer, 0, false, false);
        if (cancelled) return std::nullopt;
        // Assembly reference geometry not depending on instances — before mates (assembly.md §7б).
        {
            auto hasSlash = [](auto&& self, const doc::Json& j) -> bool {
                if (j.is_string()) return naming::hasPath(j.get<std::string>());
                if (j.is_object() || j.is_array())
                    for (const auto& x : j)
                        if (self(self, x)) return true;
                return false;
            };
            auto refsFeature = [](auto&& self, const doc::Json& j, const std::set<std::string>& ids) -> bool {
                if (j.is_object() && j.contains("feature") && j["feature"].is_string() && ids.count(j["feature"].get<std::string>())) return true;
                if (j.is_object() || j.is_array())
                    for (const auto& x : j)
                        if (self(self, x, ids)) return true;
                return false;
            };
            const doc::Element& ae = d.active();
            std::set<std::string> dependent;  // dependent on instances (and other features — sketches)
            StatePtr s0 = init;
            uint64_t k0 = initialKey();
            for (size_t i = 0; i < ae.features.size(); ++i) {
                const doc::Feature& f = *ae.features[i];
                const bool sketch = f.type == doc::FeatureType::Sketch;  // sketches as mate supports (FX7-ASM-006)
                const bool datum = f.type == doc::FeatureType::Plane || f.type == doc::FeatureType::Axis || sketch;
                doc::Json def = doc::featureToJsonValue(f);
                if (sketch) {  // instance names may be only in the plane and in projection sources (texts are not names)
                    doc::Json refs = doc::Json::array({def.value("plane", doc::Json::object())});
                    for (const auto& e : f.sketchData()->entities)
                        if (!e.source.empty()) refs.push_back(e.source);
                    def = doc::Json{{"refs", refs}};
                }
                if (!f.suppressed) {
                    if (!datum || hasSlash(hasSlash, def) || refsFeature(refsFeature, def, dependent)) {
                        dependent.insert(f.id);
                    } else {
                        if (construe::cancelled(cancel)) return std::nullopt;
                        const uint64_t fk = evaluatorKey(d, f, k0, res.variables);
                        EvalPtr r = cacheGet(fk);
                        if (!r) {
                            bool wasCancelled = false;
                            r = evaluate(d, f, static_cast<int>(i), s0, res.variables, cancel, wasCancelled);
                            if (wasCancelled) return std::nullopt;
                            ++evaluations_;
                            cachePut(fk, r);
                        }
                        if (r->status == FeatureStatus::Error) {
                            dependent.insert(f.id);
                        } else {
                            s0 = r->stateOut;
                            k0 = fk;
                        }
                    }
                }
                // The rollback does not act here (assembly.md §7б «Assembly feature rollback», FX9-TREE-002): mates keep their
                // supports when the marker moves.
            }
            for (const auto& [id, pl] : s0->planes)
                if (!dependent.count(id) && ae.find(id)) st->datumPlanes[id] = pl;
            for (const auto& [id, ax] : s0->axes)
                if (!dependent.count(id) && ae.find(id)) st->datumAxes[id] = {ax.point, ax.dir};
            for (const auto& [id, se] : s0->sketches) {  // FX7-ASM-006: world points, lines, circles of the sketch
                const doc::FeaturePtr f = ae.find(id);
                if (dependent.count(id) || !f || !f->sketchData()) continue;
                st->datumSketches[id] = datumSketchOf(*f, se);
            }
        }
        solveMates(k_, d.active(), res.variables, documentUnits(d), *st);  // mates (assembly.md §7)
        evaluateInstancePatterns(k_, d.active(), res.variables, documentUnits(d), *st);  // instance patterns (§7е)
        if (d.active().explodedActive() && !nested)  // the exploded view of a nested assembly is a property of its window
            explodedSolved = explodedSolve(k_, d.active(), res.variables, documentUnits(d), *st, explodedAuto, explodedSuppressed);
        res.assembly = st;
    }
    StatePtr state = init;
    uint64_t key = initialKey();
    if (res.assembly) {  // virtual assembly state (assembly.md §7б): input of assembly features
        auto v = std::make_shared<PartState>(*init);
        uint64_t h = initialKey();
        for (const auto& e : res.assembly->instances) {
            if (!e.state || e.suppressed || e.error) continue;
            appendInstanceState(k_, *v, e.path, *e.state, e.world);
            v->assemblyParts.push_back({e.path, e.world, e.state, e.element, e.file});  // parts (assembly.md §7г.2)
            // Input key of assembly features (§5.1): paths, part state keys and instance placements — by content.
            h = instanceKey(h, e.path, e.stateKey, e.world);
        }
        state = v;
        res.initial = v;
        res.final = v;
        key = h;
    }
    // Part in assembly context (assembly.md §7в.2): the in-context state goes into the initial state.
    if (!d.active().isAssembly() && d.active().context) {
        const doc::Element& P = d.active();
        const doc::PartContext& pc = *P.context;
        const int ai = d.elementIndex(pc.assembly);
        const doc::Element* A = ai >= 0 && d.elements[size_t(ai)].isAssembly() ? &d.elements[size_t(ai)] : nullptr;
        const doc::Instance* ctxIn = A ? contextInstance(d, P, pc.instance) : nullptr;
        if (ctxIn && ctxIn->refElement == P.id && ctxIn->refFile.empty()) {
            auto ci = std::make_shared<PartState>(*init);
            uint64_t h = initialKey();
            const std::vector<std::string> used = usedContextPaths(P);
            const auto isUsed = [&](const std::string& p) { return std::find(used.begin(), used.end(), p) != used.end(); };
            if (!pc.frozen && contextDependsOn(d, P.id, P.id)) {
                ci->contextError = Error("REGEN_CONTEXT_CYCLE");
                h = construe::hashCombine(h, uint64_t(0xC1C1Eull));
            } else {
                const Motion3 mc = motionOf(ctxIn->position, ctxIn->rotation);
                const Motion3 inv{mc.R.transpose(), -(mc.R.transpose() * mc.t)};
                auto ctx = std::make_shared<PartState>();
                auto add = [&](const std::string& path, const doc::Document& pd, const Motion3& M) -> bool {
                    auto pr = regenerate(pd, revision, cancel, false, links, selfPath);
                    if (!pr) return false;
                    appendInstanceState(k_, *ctx, path, *pr->final, M);
                    if (isUsed(path)) {  // input key — only used instances (§7в.2 item 6)
                        h = instanceKey(h, path, pr->finalKey, M);
                    }
                    return true;
                };
                if (pc.frozen) {  // snapshot (§7в.5)
                    int n = 0;
                    for (const auto& fi : *pc.frozen) {
                        doc::Document pd = d;
                        doc::Element E = doc::elementFromJsonValue(fi.element);
                        E.id = "~frozen" + std::to_string(n++);
                        E.context.reset();
                        pd.elements.push_back(std::move(E));
                        pd.activeElement = pd.elements.back().id;
                        if (!add(fi.instance, pd, motionOf(fi.position, fi.rotation))) return std::nullopt;
                    }
                } else {
                    contextBuilding_.push_back(P.id);
                    struct Pop {
                        std::vector<std::string>& v;
                        ~Pop() { v.pop_back(); }
                    } pop{contextBuilding_};
                    for (const auto& in : A->instances) {
                        if (in.id == pc.instance || in.refElement == P.id || !in.refFile.empty() || in.suppressed) continue;
                        // A part whose context is being built further up (its state needs this one) — not shown: this part
                        // does not use it (otherwise it depends on it and is skipped below), so its features are the same.
                        if (std::find(contextBuilding_.begin(), contextBuilding_.end(), in.refElement) != contextBuilding_.end()) continue;
                        const int ei = d.elementIndex(in.refElement);
                        if (ei < 0 || d.elements[size_t(ei)].isAssembly()) continue;
                        if (contextDependsOn(d, in.refElement, P.id)) continue;  // depends on this part — not shown
                        doc::Document pd = d;
                        pd.activeElement = in.refElement;
                        if (!add(in.id, pd, inv * motionOf(in.position, in.rotation))) return std::nullopt;
                    }
                }
                ci->context = ctx;
            }
            init = ci;
            res.initial = ci;
            state = ci;
            key = construe::hashCombine(key, h);
        }
    }
    const doc::Element& el = d.active();
    bool passed = el.rollback.kind == doc::Rollback::Kind::Start;
    for (size_t i = 0; i < el.features.size(); ++i) {
        const doc::Feature& f = *el.features[i];
        FeatureResult fr;
        if (passed) {
            fr.status = FeatureStatus::RolledBack;
            res.featureStatus[f.id] = fr;
            continue;
        }
        if (f.suppressed) {
            fr.status = FeatureStatus::Suppressed;
        } else {
            if (cancelled(cancel)) return std::nullopt;
            const uint64_t k = evaluatorKey(d, f, key, res.variables);
            EvalPtr r = cacheGet(k);
            if (!r) {
                bool wasCancelled = false;
                const auto t0 = std::chrono::steady_clock::now();
                r = evaluate(d, f, static_cast<int>(i), state, res.variables, cancel, wasCancelled);
                if (wasCancelled) return std::nullopt;
                ++evaluations_;
                // A completed evaluation is counted immediately: it is already in the cache even if the regeneration is then cancelled (§5.3).
                if (countStats) {
                    ++stats_[f.id];
                    timings_[f.id] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                }
                cachePut(k, r);
            }
            fr.status = r->status;
            fr.messages = r->messages;
            state = r->stateOut;
            key = k;
        }
        res.featureStatus[f.id] = fr;
        res.after[f.id] = state;
        if (el.rollback.kind == doc::Rollback::Kind::After && el.rollback.feature == f.id) passed = true;
    }
    res.final = state;
    res.finalKey = key;
    if (res.assembly) res.assembly = assemblyWithFeatures(res.assembly, *res.final, res.finalKey);  // assembly.md §7г.4
    if (res.assembly)  // own reference geometry and visible sketches first (FX9-ASM-008)
        if (auto own = ownAssemblyDatums(el, *res.final); !own.empty()) {
            auto a = std::make_shared<AssemblyState>(*res.assembly);
            a->datums.insert(a->datums.begin(), std::make_move_iterator(own.begin()), std::make_move_iterator(own.end()));
            res.assembly = a;
        }
    // Derived instances of connections (solutions.md §7.6): placed library parts after the assembly features.
    if (res.assembly && !res.final->connections.empty()) {
        auto a = std::make_shared<AssemblyState>(*res.assembly);
        std::map<std::string, std::pair<StatePtr, uint64_t>> placed;  // (copy, row) → final state and key
        for (const auto& f : el.features) {
            auto it = res.final->connections.find(f->id);
            if (it == res.final->connections.end()) continue;
            for (const auto& p : it->second["parts"]) {
                const doc::LibraryPart* lp = d.libraryPart(p["library"].get<std::string>());
                if (!lp) continue;
                InstanceEntry e;
                e.instance = p["output"].get<std::string>();
                e.path = f->id + "/" + e.instance;
                e.name = p["title"].get<std::string>() + " " + p["row"].get<std::string>();
                e.element = libraryCopyElement(*lp);
                e.file = doc::libraryRef(lp->id);
                e.kind = "part";
                e.depth = 1;
                e.fixed = true;
                e.connection = f->id;
                const auto& w = p["world"];
                for (int i = 0; i < 9; ++i) e.world.R(i / 3, i % 3) = w[size_t(i)].get<double>();
                for (int i = 0; i < 3; ++i) e.world.t[i] = w[size_t(9 + i)].get<double>();
                const std::string row = p["row"].get<std::string>();
                const std::string key = lp->id + "#" + row;
                auto pit = placed.find(key);
                if (pit == placed.end()) {
                    doc::Document pd = *lp->document;
                    for (auto& vt : pd.variantTables)
                        if (vt.id == lp->sizesTable) vt.active = row;
                    pd.activeElement = e.element;
                    auto pr = regenerate(pd, revision, cancel, false);
                    if (!pr) return std::nullopt;
                    pit = placed.emplace(key, std::make_pair(pr->final, pr->finalKey)).first;
                }
                e.state = pit->second.first;
                e.stateKey = pit->second.second;
                a->instances.push_back(std::move(e));
            }
        }
        res.assembly = a;
    }
    if (res.assembly && explodedSolved) res.assembly = applyExploded(k_, d.active(), *res.assembly, *explodedSolved, explodedAuto, explodedSuppressed);
    if (el.isDrawing() && !drawingModels(d, res, revision, cancel, links, selfPath)) return std::nullopt;
    // A specification: its assembly's final state — the connection reports for the fastener rows (specification.md
    // §3.1 item 2а, FX14-SPEC-001).
    if (el.isSpecification())
        if (const int ai = d.elementIndex(el.specAssembly); ai >= 0 && d.elements[size_t(ai)].isAssembly()) {
            doc::Document pd = d;
            pd.activeElement = el.specAssembly;
            auto pr = regenerate(pd, revision, cancel, false, links, selfPath);
            if (!pr) return std::nullopt;
            res.specAssembly = pr->final;
        }
    return res;
}

namespace {
// Part color for the render view of a drawing (§3а): own color, else the material color, else "".
std::string partColor(const doc::Element& el) {
    if (el.color) return *el.color;
    if (el.material && el.material->contains("color") && (*el.material)["color"].is_string()) return (*el.material)["color"].get<std::string>();
    return {};
}
}  // namespace

namespace {
DrawingModelExtras& modelExtras() {
    static DrawingModelExtras f;
    return f;
}
}  // namespace

void setDrawingModelExtras(DrawingModelExtras f) { modelExtras() = std::move(f); }

namespace {
DrawingModelSections& modelSections() {
    static DrawingModelSections f;
    return f;
}
}  // namespace

void setDrawingModelSections(DrawingModelSections f) { modelSections() = std::move(f); }

std::string drawingSectionsElement(const std::string& modelElement, const std::optional<std::vector<std::string>>& sections) {
    std::string out = modelElement + "#sections";
    if (!sections) return out;
    std::vector<std::string> ids = *sections;
    std::sort(ids.begin(), ids.end());
    out += ':';
    for (size_t i = 0; i < ids.size(); ++i) out += (i ? "," : "") + ids[i];
    return out;
}


// Drawing view models (§3а): regenerating each model of the main views — like a part of an assembly instance.
bool Regenerator::drawingModels(const doc::Document& d, PartResult& res, uint64_t revision, const Cancel* cancel, const LinkMap* links,
                                const std::string& selfPath) {
    for (const auto& sh : d.active().sheets)
        for (const auto& v : sh.views) {
            if (v.kind != "base") continue;
            const std::string abs = v.refFile.empty() ? std::string() : linkPath(parentDir(selfPath), v.refFile);
            // An exploded view of an assembly — its own model (drawing.md §6.24).
            const std::string key = drawingModelKey(abs, drawingModelElement(v.refElement, v.exploded));
            if (res.models.count(key)) continue;
            DrawingModel& m = res.models[key];
            const doc::Document* D = &d;
            std::string file = selfPath;
            if (!v.refFile.empty()) {
                const LinkedDoc* ld = nullptr;
                if (links)
                    if (auto it = links->find(abs); it != links->end()) ld = &it->second;
                if (!ld || (!ld->doc && !ld->error)) {
                    m.error = Error("ASM_FILE_NOT_FOUND").with("path", v.refFile);
                    continue;
                }
                if (ld->error) {
                    Error er = *ld->error;
                    er.with("path", v.refFile);
                    m.error = er;
                    continue;
                }
                D = static_cast<const doc::Document*>(ld->doc);
                file = abs;
            }
            const int ei = D->elementIndex(v.refElement);
            if (ei < 0) {
                Error er("DRAWING_VIEW_MODEL_NOT_FOUND");
                er.with("element", v.refElement);
                if (!v.refFile.empty()) er.with("file", v.refFile);
                m.error = er;
                continue;
            }
            const doc::Element& ref = D->elements[size_t(ei)];
            if (ref.isDrawing() || ref.isSpecification()) {
                m.error = Error("DRAWING_VIEW_MODEL_INVALID").with("element", v.refElement);
                continue;
            }
            doc::Document pd = *D;
            pd.activeElement = ref.id;
            if (ref.isAssembly()) {  // the view is exploded or assembled regardless of the assembly's mode (§6.24)
                auto& x = pd.active().exploded;
                if (v.exploded && !x) x.emplace();
                if (x) x->active = v.exploded;
            }
            auto pr = regenerate(pd, revision, cancel, false, links, file);
            if (!pr) return false;
            m.state = pr->final;
            // Derived connection fasteners (solutions.md §7.6) are drawn on the view but are not in the final state's
            // name table (they come after the assembly features): their faces are added to the view model's state — the
            // view entities and the item numbers of the fasteners resolve by them (drawing.md §3а, FX14-DRW-004).
            if (pr->assembly && pr->final) {
                std::shared_ptr<PartState> st;
                for (const auto& e : pr->assembly->instances) {
                    if (e.connection.empty() || !e.state || e.suppressed || e.hidden || e.error) continue;
                    if (!st) st = std::make_shared<PartState>(*pr->final);
                    appendInstanceState(k_, *st, e.path, *e.state, e.world);
                }
                if (st) m.state = st;
            }
            if (pr->assembly) {
                for (const auto& e : pr->assembly->instances) {
                    if (!e.state || e.suppressed || e.hidden || e.error) continue;
                    kernel::Motion mo;
                    for (int i = 0; i < 9; ++i) mo.r[size_t(i)] = e.world.R(i / 3, i % 3);
                    mo.t = e.world.t;
                    // Part color for the render view (§3а): the instance element of the model document or a linked one.
                    const doc::Document* ed = &pd;
                    if (!e.file.empty()) {
                        ed = nullptr;
                        if (links)
                            if (auto it = links->find(e.file); it != links->end() && it->second.doc) ed = static_cast<const doc::Document*>(it->second.doc);
                        if (!ed && doc::isLibraryRef(e.file))
                            if (const doc::LibraryPart* lp = pd.libraryPart(e.file.substr(std::string(doc::LIBRARY_REF_PREFIX).size())))
                                ed = lp->document.get();
                    }
                    const int pi = ed ? ed->elementIndex(e.element) : -1;
                    std::string color = pi >= 0 ? partColor(ed->elements[size_t(pi)]) : std::string();
                    // The instance color of the assembly over the part color (FX12-ASM-003): the top-level instance on the path.
                    const std::string top = e.path.substr(0, e.path.find('/'));
                    for (const auto& in : pd.active().instances)
                        if (in.id == top && in.color) color = *in.color;
                    for (const auto& b : e.state->bodies) {
                        m.bodies.push_back({b.shape, mo});
                        m.colors.push_back(color);
                    }
                }
                // Weld bodies (FX9-WLD-002) — the assembled view only: the exploded one has no weld placement yet.
                if (!v.exploded && modelExtras()) modelExtras()(k_, pd, *pr, links, m);
            } else {
                for (const auto& b : pr->final->bodies) {
                    m.bodies.push_back({b.shape, {}});
                    m.colors.push_back(partColor(ref));
                }
            }
            m.modelResult = std::make_shared<const PartResult>(std::move(*pr));
            m.modelDoc = std::make_shared<const doc::Document>(std::move(pd));
        }
    // The bodies of each view with its model sections (drawing.md §6.26, S7F-DRW-005, FX10-DRW-003): a variant of the
    // model of the main view of its chain — the same state, the bodies cut (no sections — the same bodies); the list —
    // the view's own or inherited from its parent.
    for (const auto& sh : d.active().sheets)
        for (const auto& view : sh.views) {
            const doc::DrawingView* bv = doc::chainBaseView(sh, view.id);
            if (!bv) continue;
            const doc::DrawingView& v = *bv;
            const auto list = doc::viewSectionsList(sh, view.id);
            const std::string abs = v.refFile.empty() ? std::string() : linkPath(parentDir(selfPath), v.refFile);
            const std::string element = drawingModelElement(v.refElement, v.exploded);
            const std::string vkey = drawingModelKey(abs, drawingSectionsElement(element, list));
            if (res.models.count(vkey)) continue;
            const auto it = res.models.find(drawingModelKey(abs, element));
            if (it == res.models.end()) continue;
            DrawingModel cm = it->second;
            if (!cm.error && cm.modelDoc && cm.modelResult && modelSections()) {
                const auto ids = doc::effectiveViewSections(cm.modelDoc->active(), list);
                if (!ids.empty()) modelSections()(k_, *cm.modelDoc, *cm.modelResult, ids, cm);
            }
            cm.modelDoc.reset();
            cm.modelResult.reset();
            res.models[vkey] = std::move(cm);
        }
    for (auto& [key, m] : res.models) m.modelDoc.reset(), m.modelResult.reset();  // only the variants needed them
    return true;
}

std::optional<EvalPtr> Regenerator::evaluateFeature(const doc::Document& d, const doc::Feature& f, int featureIndex,
                                                    const StatePtr& input, uint64_t inputKey,
                                                    const expr::VariableTable& vars, const Cancel* cancel) {
    const uint64_t k = evaluatorKey(d, f, inputKey, vars);
    if (EvalPtr r = cacheGet(k)) return r;
    bool wasCancelled = false;
    EvalPtr r = evaluate(d, f, featureIndex, input, vars, cancel, wasCancelled);
    if (wasCancelled) return std::nullopt;
    cachePut(k, r);
    return r;
}

// ---------------------------------------------------------------- evaluators (§4)

namespace detail {



// Name table warnings of state out that were not present in the input (naming.md §7):
// NAMING_GEOMETRIC_TIE / NAMING_ORPHAN_FACE — feature warnings, NAMING_INTERNAL — REGEN_KERNEL_FAILURE.
void nameTableMessages(Ctx& c, const PartState& in, const PartState& out) {
    auto key = [](const Error& e) {
        std::string k = e.code;
        if (const auto* ids = std::get_if<std::vector<std::string>>(e.params.count("ids") ? &e.params.at("ids") : nullptr))
            for (const auto& i : *ids) k += "|" + i;
        if (const auto* n = e.stringParam("name")) k += "|" + *n;
        return k;
    };
    std::set<std::string> before;
    for (const auto& w : in.names.warnings) before.insert(key(w));
    for (const auto& w : out.names.warnings) {
        if (before.count(key(w))) continue;
        if (w.code == "NAMING_INTERNAL") c.error(Error("REGEN_KERNEL_FAILURE").with("detail", std::string("NAMING_INTERNAL")));
        else c.warning(w);
    }
}

EvalPtr finish(Ctx& c, StatePtr out, kernel::Shape tool) {
    auto r = std::make_shared<EvalResult>();
    r->messages = c.messages;
    r->stateOut = std::move(out);
    r->tool = tool;
    bool err = false, warn = false;
    for (const auto& m : c.messages) (m.error ? err : warn) = true;
    r->status = err ? FeatureStatus::Error : warn ? FeatureStatus::Warning : FeatureStatus::Ok;
    return r;
}

// Recompute names and order the bodies.
void rename(const kernel::Kernel& k, PartState& s) {
    if (!s.assemblyParts.empty()) return;  // virtual assembly state: names are rebuilt from the parts (assembly.md §7г.3)
    std::sort(s.bodies.begin(), s.bodies.end(), [](const Body& a, const Body& b) { return bodyIdLess(a.id, b.id); });
    std::vector<naming::NamedBody> nb;
    for (const auto& b : s.bodies) nb.push_back({b.id, b.shape, b.bases});
    s.names = naming::computeNames(k, nb);
}

// Smallest full face name of each body in the set (body key §2.4).
std::vector<std::string> bodyKeys(const kernel::Kernel& k, const std::vector<Body>& bodies) {
    std::vector<naming::NamedBody> nb;
    for (const auto& b : bodies) nb.push_back({b.id, b.shape, b.bases});
    const naming::NameTable t = naming::computeNames(k, nb);
    std::vector<std::string> keys(bodies.size());
    for (const auto& f : t.faces)
        if (keys[f.body].empty() || f.full < keys[f.body]) keys[f.body] = f.full;
    return keys;
}

// Split a shape into bodies with bases (components).
std::vector<Body> split(const kernel::Kernel& k, const kernel::Shape& s, const std::vector<std::string>& bases) {
    std::vector<Body> out;
    for (auto& [shape, map] : k.components(s)) {
        Body b;
        b.shape = shape;
        for (int fi : map) b.bases.push_back(fi >= 0 && fi < static_cast<int>(bases.size()) ? bases[fi] : std::string());
        out.push_back(b);
    }
    return out;
}

// Assign IDs to parts by the ordering key.
void assignIds(const kernel::Kernel& k, std::vector<Body>& parts, const std::string& prefix, bool sPrefix, bool singleKeepsPrefix) {
    if (parts.size() == 1 && singleKeepsPrefix) {
        parts[0].id = prefix;
        return;
    }
    const auto keys = bodyKeys(k, parts);
    std::vector<size_t> order(parts.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return keys[a] < keys[b]; });
    for (size_t r = 0; r < order.size(); ++r)
        parts[order[r]].id = prefix + (sPrefix ? ".s" : ".") + std::to_string(r + 1);
}

naming::InputNames inputOf(const std::vector<std::string>& bases) {
    naming::InputNames in;
    in.bases = bases;
    return in;
}


// Operation on bodies (§4.2 item 5).
bool uniteTool(Ctx& c, const std::vector<kernel::Shape>& shapes, const std::vector<std::vector<std::string>>& bases, ToolBuild& tb) {
    const size_t n = shapes.size();
    if (n == 1) {
        tb.shape = shapes[0];
        tb.bases = bases[0];
        return true;
    }
    std::vector<Error> nw;
    // Combination without a boolean; face bases carry over unchanged (naming.md §4.1, Combine).
    auto combine = [&](const std::vector<kernel::Shape>& part, const std::vector<std::vector<std::string>>& partBases,
                       std::vector<std::string>& outBases) {
        std::vector<naming::InputNames> ins;
        for (const auto& b : partBases) ins.push_back(inputOf(b));
        const kernel::OpResult comb = c.k.combine(part);
        outBases = naming::assignBases(naming::OpKind::Combine, c.f.id, comb, ins, &nw);
        return comb.shape;
    };
    // Groups — connected components of the graph "bounding boxes not separated" (§4.2 item 4); group number — by the first member.
    std::vector<BBox3> boxes;
    for (const auto& sh : shapes) boxes.push_back(c.k.bbox(sh));
    auto separated = [](const BBox3& a, const BBox3& b) {
        if (a.isEmpty() || b.isEmpty()) return false;
        for (int k = 0; k < 3; ++k)
            if (a.max()(k) + REGEN_TOOL_GAP < b.min()(k) || b.max()(k) + REGEN_TOOL_GAP < a.min()(k)) return true;
        return false;
    };
    std::vector<size_t> group(n);
    for (size_t i = 0; i < n; ++i) group[i] = i;
    std::function<size_t(size_t)> root = [&](size_t i) { return group[i] == i ? i : group[i] = root(group[i]); };
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            if (!separated(boxes[i], boxes[j])) {
                const size_t a = root(i), b = root(j);
                group[std::max(a, b)] = std::min(a, b);
            }
    std::vector<kernel::Shape> parts;
    std::vector<std::vector<std::string>> partBases;
    for (size_t g = 0; g < n; ++g) {
        if (root(g) != g) continue;
        std::vector<size_t> members;
        for (size_t i = g; i < n; ++i)
            if (root(i) == g) members.push_back(i);
        if (members.size() == 1) {
            parts.push_back(shapes[g]);
            partBases.push_back(bases[g]);
            continue;
        }
        // A group of several bodies — a single union of the first with a combination of the rest.
        std::vector<kernel::Shape> rest;
        std::vector<std::vector<std::string>> restIn;
        for (size_t k = 1; k < members.size(); ++k) rest.push_back(shapes[members[k]]), restIn.push_back(bases[members[k]]);
        std::vector<std::string> restBases;
        const kernel::Shape restShape = combine(rest, restIn, restBases);
        auto u = c.k.boolean(kernel::BoolKind::Union, shapes[g], restShape, c.cancel);
        if (!u) {
            c.kernelError(u.error());
            return false;
        }
        c.kernelWarnings(u->warnings);
        parts.push_back(u->shape);
        partBases.push_back(naming::assignBases(naming::OpKind::Boolean, c.f.id, *u, {inputOf(bases[g]), inputOf(restBases)}, &nw));
    }
    if (parts.size() == 1) {
        tb.shape = parts[0];
        tb.bases = partBases[0];
    } else {
        tb.shape = combine(parts, partBases, tb.bases);
    }
    for (auto& w : nw) c.warning(w);
    return true;
}

bool applyOperation(Ctx& c, PartState& st, const std::string& operation, const ToolBuild& tool) {
    if (!st.assemblyParts.empty()) return applyAssemblyOperation(c, st, operation, tool);  // assembly.md §7г.3
    const std::string fid = c.f.id;
    // «New body» is equivalent to «Add» (regen §4.2 item 5, vision item 17); «Add» with no bodies — new bodies.
    if ((operation == "new" || operation == "add") && st.bodies.empty()) {
        auto parts = split(c.k, tool.shape, tool.bases);
        assignIds(c.k, parts, "body:" + fid, false, true);
        for (auto& p : parts) st.bodies.push_back(p);
        return true;
    }
    if (st.bodies.empty()) {
        c.error(Error("REGEN_NO_TARGET").with("name", operation));
        return false;
    }
    if (operation == "add" || operation == "new") {
        const size_t toolComps = c.k.components(tool.shape).size();
        std::vector<size_t> touching;
        for (size_t i = 0; i < st.bodies.size(); ++i) {
            auto u = c.k.boolean(kernel::BoolKind::Union, st.bodies[i].shape, tool.shape, c.cancel);
            if (!u) {
                c.kernelError(u.error());
                return false;
            }
            if (c.k.components(u->shape).size() < 1 + toolComps) touching.push_back(i);
        }
        if (touching.empty()) {
            auto parts = split(c.k, tool.shape, tool.bases);
            assignIds(c.k, parts, "body:" + fid, false, true);
            for (auto& p : parts) st.bodies.push_back(p);
            return true;
        }
        std::vector<kernel::Shape> shapes;
        std::vector<naming::InputNames> ins;
        for (size_t i : touching) {
            shapes.push_back(st.bodies[i].shape);
            ins.push_back(inputOf(st.bodies[i].bases));
        }
        const kernel::OpResult comb = c.k.combine(shapes);
        std::vector<Error> nw;
        const auto combBases = naming::assignBases(naming::OpKind::Combine, fid, comb, ins, &nw);
        auto r = c.k.boolean(kernel::BoolKind::Union, comb.shape, tool.shape, c.cancel);
        if (!r) {
            c.kernelError(r.error());
            return false;
        }
        c.kernelWarnings(r->warnings);
        // "Add" without changing the bodies (tool entirely inside) — a warning, as for a cut (§4.2 item 5, S2-FEAT-001):
        // the union volume equals the volume of the touching bodies (face history is unreliable here — coincident faces are "modified").
        const double v0 = c.k.massProperties(comb.shape).volume, v1 = c.k.massProperties(r->shape).volume;
        if (std::abs(v1 - v0) <= ADD_NO_EFFECT_REL_TOL * std::max(1.0, v0)) c.warning(Error("REGEN_NO_EFFECT").with("name", operation));
        const auto rBases = naming::assignBases(naming::OpKind::Boolean, fid, *r, {inputOf(combBases), inputOf(tool.bases)}, &nw);
        for (auto& w : nw) c.warning(w);
        // The original body index of each face of the combination.
        std::vector<int> combOrigin(comb.history.faceOrigins.size(), -1);
        for (size_t f = 0; f < comb.history.faceOrigins.size(); ++f)
            for (const auto& o : comb.history.faceOrigins[f])
                if (o.kind == kernel::Origin::Kind::FromFace) combOrigin[f] = o.input.operand;
        std::vector<Body> keep;
        for (size_t i = 0; i < st.bodies.size(); ++i)
            if (std::find(touching.begin(), touching.end(), i) == touching.end()) keep.push_back(st.bodies[i]);
        std::vector<Body> fresh;
        for (auto& [shape, map] : c.k.components(r->shape)) {
            Body b;
            b.shape = shape;
            std::set<size_t> from;
            for (int fi : map) {
                b.bases.push_back(rBases.at(fi));
                for (const auto& o : r->history.faceOrigins.at(fi))
                    if (o.kind == kernel::Origin::Kind::FromFace && o.input.operand == 0 && combOrigin.at(o.input.face) >= 0)
                        from.insert(touching[combOrigin.at(o.input.face)]);
            }
            if (!from.empty()) {
                std::string best;
                for (size_t i : from)
                    if (best.empty() || bodyIdLess(st.bodies[i].id, best)) best = st.bodies[i].id;
                b.id = best;
                keep.push_back(b);
            } else {
                fresh.push_back(b);
            }
        }
        if (!fresh.empty()) assignIds(c.k, fresh, "body:" + fid, false, true);
        for (auto& b : fresh) keep.push_back(b);
        st.bodies = keep;
        return true;
    }
    // cut / intersect
    const kernel::BoolKind kind = operation == "cut" ? kernel::BoolKind::Subtract : kernel::BoolKind::Intersect;
    std::vector<Body> out;
    bool anyEffect = false;
    for (const Body& b : st.bodies) {
        auto r = c.k.boolean(kind, b.shape, tool.shape, c.cancel);
        if (!r) {
            c.kernelError(r.error());
            return false;
        }
        c.kernelWarnings(r->warnings);
        bool effect = false;
        for (const auto& d : r->history.deletedFaces) effect = effect || d.operand == 0;
        for (const auto& origins : r->history.faceOrigins)
            for (const auto& o : origins)
                if (!(o.kind == kernel::Origin::Kind::FromFace && o.input.operand == 0 && !o.modified)) effect = true;
        anyEffect = anyEffect || effect;
        std::vector<Error> nw;
        const auto bases = naming::assignBases(naming::OpKind::Boolean, fid, *r, {inputOf(b.bases), inputOf(tool.bases)}, &nw);
        for (auto& w : nw) c.warning(w);
        auto parts = split(c.k, r->shape, bases);
        if (parts.empty()) continue;
        assignIds(c.k, parts, b.id, true, true);
        for (auto& p : parts) out.push_back(p);
    }
    if (!anyEffect) c.warning(Error("REGEN_NO_EFFECT").with("name", operation));
    st.bodies = out;
    return true;
}

// ---------------------------------------------------------------- reference geometry (§4.6)

// Resolving a topology name to a single entity (naming policy §5.4: SPLIT/AMBIGUOUS — ambiguous).
std::optional<naming::EntityRef> resolveOne(Ctx& c, const PartState& in, const std::string& name, naming::EntityKind kind,
                                            const std::string& sub) {
    const auto rr = naming::resolve(name, kind, in.names);
    const std::vector<std::string> ids{name};
    switch (naming::refFate(rr, false)) {
    case naming::RefFate::Lost: c.error(Error("REGEN_REF_LOST").with("ids", ids), sub); return std::nullopt;
    case naming::RefFate::Ambiguous: c.error(Error("REGEN_REF_AMBIGUOUS").with("ids", ids), sub); return std::nullopt;
    case naming::RefFate::Resolved: break;
    }
    referenceWarnings(rr, name, c.messages);
    return rr.entities.front();
}

ResolvedRefs resolveMany(Ctx& c, const PartState& in, const std::vector<std::string>& names, naming::EntityKind kind,
                         const std::string& sub) {
    ResolvedRefs out;
    for (const auto& name : names) {
        const auto rr = naming::resolve(name, kind, in.names);
        const auto fate = naming::refFate(rr, true);
        if (fate == naming::RefFate::Lost) {
            out.lost.push_back(name);
            continue;
        }
        if (fate == naming::RefFate::Ambiguous) {
            out.ambiguous.push_back(name);
            continue;
        }
        if (rr.status == naming::ResolveStatus::Split)
            c.warning(Error("NAMING_REF_SPLIT").with("name", name).with("actual", double(rr.entities.size())));
        referenceWarnings(rr, name, c.messages);
        out.refs.push_back({name, rr.entities});
    }
    if (!out.lost.empty()) c.error(Error("REGEN_REF_LOST").with("ids", out.lost), sub);
    else if (!out.ambiguous.empty()) c.error(Error("REGEN_REF_AMBIGUOUS").with("ids", out.ambiguous), sub);
    return out;
}

Error refGeometry(const std::string& name, const std::string& detail) {
    return Error("REGEN_REF_GEOMETRY").with("name", name).with("detail", detail);
}

std::optional<Plane> refPlane(Ctx& c, const PartState& in, const doc::Json& r, const std::string& sub) {
    if (r.contains("datum")) return sketch::datumPlane(r["datum"].get<std::string>());
    if (r.contains("feature")) {
        auto it = in.planes.find(r["feature"].get<std::string>());
        if (it == in.planes.end()) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", r["feature"].get<std::string>()), sub + "/feature");
            return std::nullopt;
        }
        return it->second;
    }
    std::vector<Message> warnings;
    auto pl = sketchPlaneOnFace(r["face"].get<std::string>(), in, c.k, warnings);
    for (auto& w : warnings) c.messages.push_back(w);
    if (!pl) {
        c.error(pl.error(), sub + "/face");
        return std::nullopt;
    }
    return *pl;
}

std::optional<RefAxis> refLine(Ctx& c, const PartState& in, const doc::Json& r, const std::string& sub) {
    if (r.contains("datum")) {
        const std::string a = r["datum"];
        return RefAxis{Point3::Zero(), sketch::datumAxis(a).value_or(Vec3::UnitZ())};
    }
    if (r.contains("feature")) {
        auto it = in.axes.find(r["feature"].get<std::string>());
        if (it == in.axes.end()) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", r["feature"].get<std::string>()), sub + "/feature");
            return std::nullopt;
        }
        return it->second;
    }
    const std::string name = r["edge"];
    auto e = resolveOne(c, in, name, naming::EntityKind::Edge, sub + "/edge");
    if (!e) return std::nullopt;
    const auto& ee = in.names.edges.at(e->index);
    const auto cv = c.k.edgeCurve(in.bodies.at(ee.body).shape, ee.logical);
    if (cv.kind != kernel::CurveInfo::Kind::Line || (cv.p2 - cv.p1).norm() <= 0) {
        c.error(refGeometry(sub.substr(1), "edge not straight"), sub + "/edge");
        return std::nullopt;
    }
    return RefAxis{cv.p1, (cv.p2 - cv.p1).normalized()};
}

std::optional<Point3> refPoint(Ctx& c, const PartState& in, const doc::Json& r, const std::string& sub) {
    if (r.contains("datum")) return Point3::Zero();
    if (r.contains("vertex")) {
        auto v = resolveOne(c, in, r["vertex"].get<std::string>(), naming::EntityKind::Vertex, sub + "/vertex");
        if (!v) return std::nullopt;
        const auto& ve = in.names.vertices.at(v->index);
        return c.k.vertexPoint(in.bodies.at(ve.body).shape, ve.vertex);
    }
    const std::string sk = r["sketchPoint"]["sketch"], ent = r["sketchPoint"]["entity"];
    auto it = in.sketches.find(sk);
    if (it == in.sketches.end() || !it->second.result.state.points.count(ent)) {
        c.error(Error("REGEN_INPUT_ERROR").with("id", it == in.sketches.end() ? sk : ent), sub + "/sketchPoint");
        return std::nullopt;
    }
    return it->second.frame.toWorld(it->second.result.state.points.at(ent));
}

// Axis of a cylindrical or conical face (pattern AxisRef/DirRef {face}, §4.7 item 2).
std::optional<RefAxis> faceAxis(Ctx& c, const PartState& in, const std::string& face, const std::string& sub, const char* name) {
    auto fr = resolveOne(c, in, face, naming::EntityKind::Face, sub + "/face");
    if (!fr) return std::nullopt;
    const auto& fe = in.names.faces.at(fr->index);
    const auto si = c.k.faceSurface(in.bodies.at(fe.body).shape, fe.face);
    if (si.kind != kernel::SurfaceInfo::Kind::Cylinder && si.kind != kernel::SurfaceInfo::Kind::Cone) {
        c.error(refGeometry(name, "face has no axis"), sub + "/face");
        return std::nullopt;
    }
    return RefAxis{si.point, si.dir.normalized()};
}

// Axis of revolve feature id computed in state in (§4.3 axis field): reference axis, datum axis or a line of its sketch.
std::optional<RefAxis> revolveAxisOf(Ctx& c, const PartState& in, const std::string& id, const std::string& sub) {
    const doc::FeaturePtr f = c.d.active().find(id);
    const doc::RevolveData* rv = f ? f->revolve() : nullptr;
    auto missing = [&] {
        c.error(Error("REGEN_INPUT_ERROR").with("id", id), sub);
        return std::nullopt;
    };
    if (!rv) return missing();
    if (!rv->axisFeature.empty()) {
        auto it = in.axes.find(rv->axisFeature);
        if (it == in.axes.end()) return missing();
        return it->second;
    }
    if (!rv->axisDatum.empty())
        return RefAxis{Point3::Zero(), sketch::datumAxis(rv->axisDatum).value_or(Vec3::UnitZ())};
    const doc::FeaturePtr sk = c.d.active().find(rv->sketch);
    const sketch::Entity* line = sk && sk->sketchData() ? sk->sketchData()->findEntity(rv->axisSketchLine) : nullptr;
    auto se = in.sketches.find(rv->sketch);
    if (!line || se == in.sketches.end() || !se->second.result.state.points.count(line->p1) || !se->second.result.state.points.count(line->p2))
        return missing();
    const Point3 a = se->second.frame.toWorld(se->second.result.state.points.at(line->p1));
    const Point3 b = se->second.frame.toWorld(se->second.result.state.points.at(line->p2));
    if ((b - a).norm() < kernel::KERNEL_MIN_DIMENSION) return missing();
    return RefAxis{a, (b - a).normalized()};
}

// DirRef / AxisRef (§4.7 item 2, FX7-PT-007): LineRef, cylinder/cone face axis, plane normal, sketch line, revolve axis.
std::optional<RefAxis> refDirection(Ctx& c, const PartState& in, const doc::Json& r, const std::string& sub, const char* name) {
    if (r.contains("face")) return faceAxis(c, in, r["face"].get<std::string>(), sub, name);
    if (r.contains("normal")) {
        auto pl = refPlane(c, in, r["normal"], sub + "/normal");
        if (!pl) return std::nullopt;
        return RefAxis{pl->origin, pl->normal.normalized()};
    }
    if (r.contains("sketchLine")) {
        const std::string sk = r["sketchLine"]["sketch"], ent = r["sketchLine"]["entity"];
        const doc::FeaturePtr f = c.d.active().find(sk);
        auto it = in.sketches.find(sk);
        const sketch::Entity* e = f && f->sketchData() ? f->sketchData()->findEntity(ent) : nullptr;
        if (it == in.sketches.end() || !e) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", it == in.sketches.end() ? sk : ent), sub + "/sketchLine");
            return std::nullopt;
        }
        const auto& P = it->second.result.state.points;
        if (e->type != sketch::EntityType::Line || !P.count(e->p1) || !P.count(e->p2) ||
            (P.at(e->p2) - P.at(e->p1)).norm() < kernel::KERNEL_MIN_DIMENSION) {
            c.error(refGeometry(name, "not a sketch line"), sub + "/sketchLine");
            return std::nullopt;
        }
        const Point3 a = it->second.frame.toWorld(P.at(e->p1)), b = it->second.frame.toWorld(P.at(e->p2));
        return RefAxis{a, (b - a).normalized()};
    }
    if (r.contains("revolveAxis")) return revolveAxisOf(c, in, r["revolveAxis"].get<std::string>(), sub + "/revolveAxis");
    return refLine(c, in, r, sub);
}

// Source features of a pattern or mirror (§4.7 item 1): unique IDs in history order with their index in `features`.
std::vector<std::pair<std::string, size_t>> patternSourceIds(Ctx& c) {
    const doc::Json& d = c.f.pattern()->def;
    std::vector<std::pair<std::string, size_t>> ids;  // (ID, index in features)
    for (size_t i = 0; i < d["features"].size(); ++i) {
        const std::string id = d["features"][i];
        bool seen = false;
        for (const auto& x : ids) seen = seen || x.first == id;
        if (!seen) ids.push_back({id, i});
    }
    const auto& el = c.d.active();
    std::stable_sort(ids.begin(), ids.end(), [&](const auto& a, const auto& b) { return el.indexOf(a.first) < el.indexOf(b.first); });
    return ids;
}

// Instances of a pattern or mirror (§4.7 items 2–4, §4.8); nullopt — an error or no instances (already reported).
std::optional<std::vector<PatternInstance>> patternInstances(Ctx& c, const PartState& in) {
    const doc::Json& d = c.f.pattern()->def;
    const bool mirror = c.f.type == doc::FeatureType::Mirror;
    const std::string mode = mirror ? std::string("mirror") : d["mode"].get<std::string>();
    // 2–4. Instances.
    std::vector<PatternInstance> inst;
    auto count = [&](const doc::Json& j, const std::string& sub) -> std::optional<int> {
        const auto n = c.field(j.get<std::string>(), expr::DIMLESS, sub);
        if (!n) return std::nullopt;
        const double r = std::round(*n);
        if (std::abs(*n - r) > PATTERN_INT_TOL || r < 1 || r > PATTERN_MAX_INSTANCES) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("count")).with("actual", *n), sub);
            return std::nullopt;
        }
        return static_cast<int>(r);
    };
    if (mirror) {
        // Reflection about a plane: r = I − 2·n·nᵀ, t = 2·(n·p)·n.
        const auto pl = refPlane(c, in, d["plane"], "/plane");
        if (!pl) return std::nullopt;
        const Vec3 n = pl->normal.normalized();
        const Eigen::Matrix3d R = Eigen::Matrix3d::Identity() - 2 * n * n.transpose();
        PatternInstance x;
        for (int r = 0; r < 3; ++r)
            for (int q = 0; q < 3; ++q) x.m.r[r * 3 + q] = R(r, q);
        x.m.t = 2 * n.dot(pl->origin) * n;
        inst.push_back(x);
    } else if (mode == "linear") {
        const doc::Json& dirs = d["directions"];
        std::vector<Vec3> step;
        std::vector<int> n;
        for (size_t i = 0; i < dirs.size(); ++i) {
            const std::string sub = "/directions/" + std::to_string(i);
            const auto l = refDirection(c, in, dirs[i]["ref"], sub + "/ref", "ref");
            const auto cnt = count(dirs[i]["count"], sub + "/count");
            auto sp = c.field(dirs[i]["spacing"].get<std::string>(), expr::LENGTH, sub + "/spacing");
            if (sp && *sp <= 0) {
                c.error(Error("REGEN_BAD_VALUE").with("name", std::string("spacing")).with("actual", *sp), sub + "/spacing");
                sp.reset();
            }
            if (!l || !cnt || !sp) continue;
            const bool rev = dirs[i].contains("reverse") && dirs[i]["reverse"].get<bool>();
            step.push_back(l->dir * (rev ? -*sp : *sp));
            n.push_back(*cnt);
        }
        if (c.hasError()) return std::nullopt;
        const int n2 = n.size() > 1 ? n[1] : 1;
        for (int j = 0; j < n2; ++j)
            for (int i = 0; i < n[0]; ++i) {
                if (i == 0 && j == 0) continue;
                PatternInstance x;
                x.m.t = double(i) * step[0] + (n.size() > 1 ? Vec3(double(j) * step[1]) : Vec3::Zero());
                x.label = n.size() > 1 ? std::to_string(i) + "_" + std::to_string(j) : std::to_string(i);
                inst.push_back(x);
            }
    } else {
        const std::optional<RefAxis> ax = refDirection(c, in, d["axis"], "/axis", "axis");
        const auto cnt = count(d["count"], "/count");
        auto ang = c.field(d["angle"].get<std::string>(), expr::ANGLE, "/angle");
        if (ang && (*ang <= 0 || *ang > 2 * PI + PATTERN_INT_TOL)) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("angle")).with("actual", *ang), "/angle");
            ang.reset();
        }
        if (!ax || !cnt || !ang) return std::nullopt;
        const bool full = std::abs(*ang - 2 * PI) <= PATTERN_INT_TOL;
        const double delta = full ? *ang / *cnt : (*cnt > 1 ? *ang / (*cnt - 1) : 0.0);
        const bool rev = d.contains("reverse") && d["reverse"].get<bool>();
        const Vec3 k = ax->dir;
        for (int i = 1; i < *cnt; ++i) {
            const double th = (rev ? -1.0 : 1.0) * i * delta;
            // Rotation about an axis (point p, direction k) by Rodrigues: R = I·cos + [k]×·sin + (1 − cos)·k·kᵀ.
            Eigen::Matrix3d K;
            K << 0, -k.z(), k.y(), k.z(), 0, -k.x(), -k.y(), k.x(), 0;
            const Eigen::Matrix3d R = Eigen::Matrix3d::Identity() * std::cos(th) + K * std::sin(th) + (1 - std::cos(th)) * k * k.transpose();
            PatternInstance x;
            for (int r = 0; r < 3; ++r)
                for (int q = 0; q < 3; ++q) x.m.r[r * 3 + q] = R(r, q);
            x.m.t = ax->point - R * ax->point;
            x.label = std::to_string(i);
            inst.push_back(x);
        }
    }
    if (inst.empty()) {
        c.warning(Error("REGEN_NO_EFFECT").with("name", std::string("pattern")));
        return std::nullopt;
    }
    return inst;
}

// pattern (§4.7) and mirror (§4.8) evaluator: true — operations performed on st; preview — copy tools.
bool evalPattern(Ctx& c, const PartState& in, PartState& st, kernel::Shape& preview) {
    const bool mirror = c.f.type == doc::FeatureType::Mirror;
    const auto ids = patternSourceIds(c);
    std::vector<const ToolEntry*> sources;
    for (const auto& [id, i] : ids) {
        auto it = in.tools.find(id);
        if (it == in.tools.end()) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", id), "/features/" + std::to_string(i));
            continue;
        }
        sources.push_back(&it->second);
        // A pattern or mirror as a source (FX7-PT-008): its further tools are "<id>#2", "<id>#3", … (§4.7 item 6).
        for (int k = 2;; ++k) {
            auto more = in.tools.find(id + "#" + std::to_string(k));
            if (more == in.tools.end()) break;
            sources.push_back(&more->second);
        }
    }
    if (c.hasError()) return false;
    const auto instances = patternInstances(c, in);
    if (!instances) return false;
    const std::vector<PatternInstance>& inst = *instances;
    // 5. Tool of each source feature — union of the copies; the source feature's operation.
    std::vector<kernel::Shape> previews;
    for (size_t nth = 0; nth < sources.size(); ++nth) {
        const ToolEntry* src = sources[nth];
        ToolBuild tb;
        for (const PatternInstance& x : inst) {
            auto r = c.k.transform(src->shape, x.m);
            if (!r) {
                c.kernelError(r.error());
                return false;
            }
            std::vector<std::string> bases(r->history.faceOrigins.size());
            for (size_t fi = 0; fi < bases.size(); ++fi)
                for (const auto& o : r->history.faceOrigins[fi])
                    if (o.kind == kernel::Origin::Kind::FromFace && o.input.face < static_cast<int>(src->bases.size()))
                        bases[fi] = mirror ? naming::mirrorBase(c.f.id, src->bases[o.input.face])
                                           : naming::copyBase(c.f.id, x.label, src->bases[o.input.face]);
            if (tb.shape.null()) {
                tb.shape = r->shape;
                tb.bases = bases;
                continue;
            }
            auto u = c.k.boolean(kernel::BoolKind::Union, tb.shape, r->shape, c.cancel);
            if (!u) {
                c.kernelError(u.error());
                return false;
            }
            c.kernelWarnings(u->warnings);
            std::vector<Error> bw;
            tb.bases = naming::assignBases(naming::OpKind::Boolean, c.f.id, *u, {inputOf(tb.bases), inputOf(bases)}, &bw);
            for (auto& w : bw) c.warning(w);
            tb.shape = u->shape;
        }
        tb.ok = true;
        previews.push_back(tb.shape);
        if (!applyOperation(c, st, src->operation, tb)) return false;
        // Tools of this feature (FX7-PT-008): one per source tool, in order — "<F>", "<F>#2", …
        st.tools[nth == 0 ? c.f.id : c.f.id + "#" + std::to_string(nth + 1)] = {tb.shape, tb.bases, src->operation};
        // Threads of the source feature — into each instance (§4.7 item 5).
        std::vector<ThreadEntry> copies;
        for (const auto& t : in.threads)
            if (in.tools.count(t.feature) && &in.tools.at(t.feature) == src)
                for (const PatternInstance& x : inst) {
                    Eigen::Matrix3d R;
                    for (int r = 0; r < 3; ++r)
                        for (int q = 0; q < 3; ++q) R(r, q) = x.m.r[r * 3 + q];
                    ThreadEntry tc = t;
                    tc.feature = c.f.id;
                    tc.origin = R * t.origin + x.m.t;
                    tc.dir = R * t.dir;
                    copies.push_back(tc);
                }
        for (auto& tc : copies) st.threads.push_back(tc);
    }
    if (!mirror) {  // instances for assembly patterns by a part pattern (§4.7 item 7, FX7-ASM-007)
        auto& rec = st.patterns[c.f.id];
        rec.clear();
        for (const PatternInstance& x : inst) rec.push_back({x.label, x.m});
    }
    preview = previews.size() == 1 ? previews[0] : c.k.combine(previews).shape;
    return true;
}

// Offset plane "up to object" (§4.6, FX4-FEAT-001): offset s of the plane parallel to ref and passing through
// object `to` (vertex or point; straight edge or circle parallel to ref; planar face parallel to ref;
// tangent to a cylinder, cone or sphere), from ref along its normal. Two tangents — the one nearer to ref (smaller |s|; on
// a tie — larger s), `far` — the other one. nullopt — the error is already recorded.
std::optional<double> offsetToTarget(Ctx& c, const PartState& in, const Plane& ref, const doc::Json& r, bool far_) {
    const Vec3 n = ref.normal.normalized();
    auto s = [&](const Point3& p) { return (p - ref.origin).dot(n); };
    auto pickOf = [&](double a, double b) {
        const double nearS = std::abs(a) < std::abs(b) ? a : std::abs(b) < std::abs(a) ? b : std::max(a, b);
        const double farS = nearS == a ? b : a;
        return far_ ? farS : nearS;
    };
    if (r.contains("edge")) {
        auto e = resolveOne(c, in, r["edge"].get<std::string>(), naming::EntityKind::Edge, "/to/edge");
        if (!e) return std::nullopt;
        const auto& ee = in.names.edges.at(e->index);
        const auto cv = c.k.edgeCurve(in.bodies.at(ee.body).shape, ee.logical);
        if (cv.kind == kernel::CurveInfo::Kind::Line && (cv.p2 - cv.p1).norm() > 0) {
            if (std::abs((cv.p2 - cv.p1).normalized().dot(n)) > REF_PARALLEL_TOL) {
                c.error(refGeometry("to", "edge not parallel to plane"), "/to/edge");
                return std::nullopt;
            }
            return s(cv.p1);
        }
        if (cv.kind == kernel::CurveInfo::Kind::Circle) {
            if (cv.axis.normalized().cross(n).norm() > REF_PARALLEL_TOL) {
                c.error(refGeometry("to", "edge not parallel to plane"), "/to/edge");
                return std::nullopt;
            }
            return s(cv.center);
        }
        c.error(refGeometry("to", "unsupported edge"), "/to/edge");
        return std::nullopt;
    }
    if (r.contains("face")) {
        auto fr = resolveOne(c, in, r["face"].get<std::string>(), naming::EntityKind::Face, "/to/face");
        if (!fr) return std::nullopt;
        const auto& fe = in.names.faces.at(fr->index);
        const auto si = c.k.faceSurface(in.bodies.at(fe.body).shape, fe.face);
        using K = kernel::SurfaceInfo::Kind;
        switch (si.kind) {
            case K::Plane:
                if (si.dir.normalized().cross(n).norm() > REF_PARALLEL_TOL) {
                    c.error(refGeometry("to", "face not parallel to plane"), "/to/face");
                    return std::nullopt;
                }
                return s(si.point);
            case K::Cylinder:
                if (std::abs(si.dir.normalized().dot(n)) > REF_PARALLEL_TOL) {
                    c.error(refGeometry("to", "axis not parallel to plane"), "/to/face");
                    return std::nullopt;
                }
                return pickOf(s(si.point) - si.radius, s(si.point) + si.radius);
            case K::Sphere: return pickOf(s(si.point) - si.radius, s(si.point) + si.radius);
            case K::Cone: {
                // Axis parallel to ref: tangency along the face's largest circular rim — s(axis) ± the largest radius.
                if (std::abs(si.dir.normalized().dot(n)) <= REF_PARALLEL_TOL) {
                    const auto& sh = in.bodies.at(fe.body).shape;
                    const auto ct = kernel::canonicalTopology(c.k, sh);
                    double rMax = 0;
                    for (size_t le = 0; le < ct.logicalEdges.size(); ++le) {
                        const auto& L = ct.logicalEdges[le];
                        if (L.faces.first != fe.face && L.faces.second != fe.face) continue;
                        const auto cv = c.k.edgeCurve(sh, static_cast<int>(le));
                        if (cv.kind == kernel::CurveInfo::Kind::Circle) rMax = std::max(rMax, cv.radius);
                    }
                    if (rMax > 0) return pickOf(s(si.point) - rMax, s(si.point) + rMax);
                }
                // Otherwise a plane parallel to ref touches a cone along a generatrix only if the angle between the axis and ref equals
                // the cone half-angle: |dir·n| = sin(halfAngle); then it passes through the apex.
                if (std::abs(std::abs(si.dir.normalized().dot(n)) - std::sin(si.halfAngle)) > REF_PARALLEL_TOL) {
                    c.error(refGeometry("to", "no tangent plane parallel to plane"), "/to/face");
                    return std::nullopt;
                }
                return s(si.point);
            }
            default: c.error(refGeometry("to", "unsupported face"), "/to/face"); return std::nullopt;
        }
    }
    auto p = refPoint(c, in, r, "/to");
    if (!p) return std::nullopt;
    return s(*p);
}

// plane/axis evaluator: true — the entry was added to st.
bool evalRefGeometry(Ctx& c, const PartState& in, PartState& st, const doc::Feature& f) {
    const doc::Json& d = f.refGeom()->def;
    const std::string mode = d["mode"];
    if (f.type == doc::FeatureType::Plane) {
        if (mode == "offset") {
            auto ref = refPlane(c, in, d["ref"], "/ref");
            const auto dist = c.field(d["distance"].get<std::string>(), expr::LENGTH, "/distance");
            std::optional<double> to;  // up to object (FX4-FEAT-001): plane offset through the object from ref along the normal
            if (ref && d.contains("to")) to = offsetToTarget(c, in, *ref, d["to"], d.value("far", false));
            if (!ref || !dist || (d.contains("to") && !to)) return false;
            if (*dist < 0) {
                c.error(Error("REGEN_BAD_VALUE").with("name", std::string("distance")).with("actual", *dist), "/distance");
                return false;
            }
            Plane p = *ref;
            const bool flip = d.value("flip", false);
            if (to) {  // through the object and at a distance from it; normal and axes — as for ref (§4.6)
                p.origin = ref->origin + (*to + (flip ? -*dist : *dist)) * ref->normal;
                st.planes[f.id] = p;
                return true;
            }
            p.origin = ref->origin + (flip ? -*dist : *dist) * ref->normal;
            if (flip) p.normal = -ref->normal, p.y = -ref->y;
            st.planes[f.id] = p;
            return true;
        }
        if (mode == "angle") {
            auto ref = refPlane(c, in, d["ref"], "/ref");
            auto ax = refLine(c, in, d["axis"], "/axis");
            const auto a = c.field(d["angle"].get<std::string>(), expr::ANGLE, "/angle");
            if (!ref || !ax || !a) return false;
            if (std::abs(ax->dir.dot(ref->normal)) > REF_PARALLEL_TOL) {
                c.error(refGeometry("axis", "axis not parallel to plane"), "/axis");
                return false;
            }
            const Vec3 n0 = ref->normal;
            const Vec3 n = n0 * std::cos(*a) + ax->dir.cross(n0) * std::sin(*a);
            st.planes[f.id] = sketch::faceFrame(ax->point, n);
            return true;
        }
        if (mode == "threePoints") {
            std::vector<Point3> pts;
            for (int i = 0; i < 3; ++i) {
                auto p = refPoint(c, in, d["points"][size_t(i)], "/points/" + std::to_string(i));
                if (!p) return false;
                pts.push_back(*p);
            }
            const Vec3 n = (pts[1] - pts[0]).cross(pts[2] - pts[0]);
            if (n.norm() < REF_DEGENERATE_TOL) {
                c.error(refGeometry("points", "points collinear"), "/points");
                return false;
            }
            st.planes[f.id] = sketch::faceFrame(pts[0], n);
            return true;
        }
        // mid
        auto p1 = refPlane(c, in, d["refs"][0], "/refs/0");
        auto p2 = refPlane(c, in, d["refs"][1], "/refs/1");
        if (!p1 || !p2) return false;
        if (p1->normal.cross(p2->normal).norm() > REF_PARALLEL_TOL) {
            c.error(refGeometry("refs", "planes not parallel"), "/refs");
            return false;
        }
        const Point3 o = p1->origin + 0.5 * (p2->origin - p1->origin).dot(p1->normal) * p1->normal;
        st.planes[f.id] = sketch::faceFrame(o, p1->normal);
        return true;
    }
    // axis
    if (mode == "twoPoints") {
        auto a = refPoint(c, in, d["points"][0], "/points/0");
        auto b = refPoint(c, in, d["points"][1], "/points/1");
        if (!a || !b) return false;
        if ((*b - *a).norm() < kernel::KERNEL_MIN_DIMENSION) {
            c.error(refGeometry("points", "points coincide"), "/points");
            return false;
        }
        st.axes[f.id] = RefAxis{*a, (*b - *a).normalized()};
        return true;
    }
    if (mode == "edge") {
        auto l = refLine(c, in, doc::Json{{"edge", d["edge"]}}, "");
        if (!l) return false;
        st.axes[f.id] = *l;
        return true;
    }
    if (mode == "face") {
        auto fr = resolveOne(c, in, d["face"].get<std::string>(), naming::EntityKind::Face, "/face");
        if (!fr) return false;
        const auto& fe = in.names.faces.at(fr->index);
        const auto si = c.k.faceSurface(in.bodies.at(fe.body).shape, fe.face);
        if (si.kind != kernel::SurfaceInfo::Kind::Cylinder && si.kind != kernel::SurfaceInfo::Kind::Cone) {
            c.error(refGeometry("face", "face has no axis"), "/face");
            return false;
        }
        st.axes[f.id] = RefAxis{si.point, si.dir.normalized()};
        return true;
    }
    // planes
    auto p1 = refPlane(c, in, d["refs"][0], "/refs/0");
    auto p2 = refPlane(c, in, d["refs"][1], "/refs/1");
    if (!p1 || !p2) return false;
    const Vec3 dir = p1->normal.cross(p2->normal);
    if (dir.norm() <= REF_PARALLEL_TOL) {
        c.error(refGeometry("refs", "planes parallel"), "/refs");
        return false;
    }
    // Point of the intersection line closest to the origin: n1·x = n1·o1, n2·x = n2·o2, dir·x = 0.
    Eigen::Matrix3d m;
    m.row(0) = p1->normal.transpose();
    m.row(1) = p2->normal.transpose();
    m.row(2) = dir.normalized().transpose();
    const Eigen::Vector3d rhs(p1->normal.dot(p1->origin), p2->normal.dot(p2->origin), 0.0);
    st.axes[f.id] = RefAxis{Point3(m.colPivHouseholderQr().solve(rhs)), dir.normalized()};
    return true;
}

// Input sketch of a feature by ID; missing — REGEN_INPUT_ERROR.
const SketchEntry* sketchOf(Ctx& c, const PartState& input, const std::string& id) {
        auto it = input.sketches.find(id);
        if (it == input.sketches.end()) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", id), "/sketch");
            return nullptr;
        }
        return &it->second;
}

// Profile regions of a feature (§4.2 item 3): all outer ones or the listed ones; errors — feature messages.
std::vector<const sketch::Region*> selectRegions(Ctx& c, const SketchEntry& se, const doc::Profiles& p, const std::string& sketchId) {
        std::vector<const sketch::Region*> out;
        if (p.all) {
            for (const auto& r : se.result.profiles.regions)
                if (r.depth % 2 == 0) out.push_back(&r);
        } else {
            std::vector<std::string> missing;
            std::set<std::string> seen;
            for (const auto& id : p.regions) {
                if (!seen.insert(id).second) continue;
                const sketch::Region* r = se.result.profiles.find(id);
                if (!r) missing.push_back(id);
                else out.push_back(r);
            }
            if (!missing.empty()) {
                c.error(Error("REGEN_PROFILE_MISSING").with("id", sketchId).with("ids", missing), "/profiles");
                return std::vector<const sketch::Region*>{};
            }
            std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->regionId < b->regionId; });
        }
        if (out.empty() && !c.hasError()) c.error(Error("SKETCH_NO_REGIONS").with("id", sketchId), "/profiles");
        return out;
}

}  // namespace detail

using namespace detail;

void referenceWarnings(const naming::ResolveResult& rr, const std::string& name, std::vector<Message>& out) {
    if (rr.merged) out.push_back({false, Error("NAMING_REF_MERGED").with("name", name)});
    if (rr.ordinal) out.push_back({false, Error("NAMING_REF_ORDINAL").with("name", name)});
}

Result<Plane> sketchPlaneOnFace(const std::string& face, const PartState& state, const kernel::Kernel& k,
                                std::vector<Message>& warnings) {
    // In-context name "<path>/<name>" of a part in assembly context — in the in-context state (naming.md §5.3б).
    if (naming::hasPath(face) && (state.context || state.contextError)) {
        if (state.contextError) return Error(*state.contextError).with("ids", std::vector<std::string>{face});
        return sketchPlaneOnFace(face, *state.context, k, warnings);
    }
    const auto rr = naming::resolve(face, naming::EntityKind::Face, state.names);
    const std::vector<std::string> ids{face};
    if (const auto fate = naming::refFate(rr, true); fate != naming::RefFate::Resolved)
        return Error(fate == naming::RefFate::Lost ? "REGEN_REF_LOST" : "REGEN_REF_AMBIGUOUS").with("ids", ids);
    std::vector<kernel::SurfaceInfo> parts;
    for (const auto& ref : rr.entities) {
        const auto& fe = state.names.faces.at(ref.index);
        parts.push_back(k.faceSurface(state.bodies.at(fe.body).shape, fe.face));
        if (parts.back().kind != kernel::SurfaceInfo::Kind::Plane) {
            if (rr.status != naming::ResolveStatus::Split) return Error("REGEN_FACE_NOT_PLANAR").with("name", face);
            return Error("REGEN_REF_AMBIGUOUS").with("ids", ids);  // a SPLIT part is not planar
        }
    }
    if (rr.status == naming::ResolveStatus::Split) {
        // Parts of one plane: normals coincide within ANGULAR_TOL·10³, distance to the plane
        // of part #1 ≤ LINEAR_TOL·10² (naming.md §5.4) — otherwise ambiguous.
        const Vec3 n0 = parts.front().dir.normalized();
        for (const auto& p : parts) {
            const double ang = std::acos(std::min(1.0, std::abs(p.dir.normalized().dot(n0))));
            if (ang > ANGULAR_TOL * 1e3 || std::abs((p.point - parts.front().point).dot(n0)) > LINEAR_TOL * 1e2)
                return Error("REGEN_REF_AMBIGUOUS").with("ids", ids);
        }
        warnings.push_back({false, Error("NAMING_REF_SPLIT").with("name", face).with("actual", double(rr.entities.size()))});
    }
    referenceWarnings(rr, face, warnings);
    return sketch::faceFrame(parts.front().point, parts.front().dir);
}

namespace {

// Intersection points of a curve with plane (o, n) (sketch.md §2.5, piercing).
std::vector<Point3> crossPlane(const WorldCurve& cv, const Point3& o, const Vec3& n) {
    std::vector<Point3> out;
    if (cv.kind == WorldCurve::Kind::Line) {
        // An end on the plane counts within PIERCE_END_TOL: the solver leaves such an end a few ulps off the plane, and
        // the sign of that rounding differs between platforms (libm of macOS, S2-FEAT-006).
        constexpr double PIERCE_END_TOL = 1e-9;
        const auto snap = [](double d) { return std::abs(d) <= PIERCE_END_TOL ? 0.0 : d; };
        const double da = snap(n.dot(cv.a - o)), db = snap(n.dot(cv.b - o));
        if (std::abs(da - db) <= 1e-12 || da * db > 0) return out;
        out.push_back(cv.a + (da / (da - db)) * (cv.b - cv.a));
        return out;
    }
    const Vec3 m = cv.axis.normalized();
    if (m.cross(n).norm() <= 1e-12) return out;  // the curve's plane is parallel
    Vec3 u = cv.kind == WorldCurve::Kind::Arc ? Vec3((cv.a - cv.center).normalized()) : m.unitOrthogonal();
    const Vec3 v = m.cross(u);
    const double A = cv.radius * n.dot(u), B = cv.radius * n.dot(v), C = n.dot(cv.center - o), rho = std::hypot(A, B);
    if (rho <= 1e-12 || std::abs(C) > rho * (1 + 1e-12)) return out;
    double sweep = 2 * std::numbers::pi;
    if (cv.kind == WorldCurve::Kind::Arc) {
        const Vec3 ra = cv.a - cv.center, rb = cv.b - cv.center;
        sweep = std::atan2(m.dot(ra.cross(rb)), ra.dot(rb));
        if (sweep <= 0) sweep += 2 * std::numbers::pi;
    }
    const double phi = std::atan2(B, A), dth = std::acos(std::clamp(-C / rho, -1.0, 1.0));
    for (double th : {phi + dth, phi - dth}) {
        th = geom2::normalizeAngle0To2Pi(th);
        if (cv.kind == WorldCurve::Kind::Arc && th > sweep + 1e-12) continue;
        const Point3 p = cv.center + cv.radius * (std::cos(th) * u + std::sin(th) * v);
        bool dup = false;
        for (const auto& q : out) dup = dup || (q - p).norm() <= 1e-9;
        if (!dup) out.push_back(p);
    }
    return out;
}

}  // namespace

std::optional<sketch::Projection> bodyTargetGeometry(const std::string& kind, const kernel::Shape& shape, int face, const Plane& frame,
                                                     const kernel::Kernel& k) {
    using SK = kernel::SurfaceInfo::Kind;
    const kernel::SurfaceInfo si = k.faceSurface(shape, face);
    const bool line = si.kind == SK::Cylinder || si.kind == SK::Cone;
    const bool curve = si.kind == SK::Torus || (si.kind == SK::Other && si.otherKind == "revolution");
    if (!line && !curve) return std::nullopt;
    const bool silhouette = kind == "silhouette+" || kind == "silhouette-";
    if (silhouette != line) return std::nullopt;
    const Vec3 a = si.dir.normalized();
    if (std::abs(a.dot(frame.normal)) > SILHOUETTE_PARALLEL_TOL) return std::nullopt;
    // Axis direction in the sketch plane, canonical: x > 0, or x = 0 and y > 0 (the sides do not swap on a flip of the
    // surface's own axis direction).
    Point2 u(a.dot(frame.x), a.dot(frame.y));
    if (u.norm() <= 0) return std::nullopt;
    u.normalize();
    if (u.x() < -1e-12 || (std::abs(u.x()) <= 1e-12 && u.y() < 0)) u = -u;
    const Vec3 a3 = u.x() * frame.x + u.y() * frame.y;
    const double side = kind[kind.find_first_of("+-")] == '+' ? 1.0 : -1.0;
    const Vec3 h = side * (-u.y() * frame.x + u.x() * frame.y);
    kernel::CurveSet q;
    q.face = face;
    q.half = h;
    // The meridian must lie on the requested side of the axis: (P − A)·h > 0. If the kernel returns the opposite
    // half-plane (seen for surfaces of revolution whose parameter runs against the axis direction), the opposite half
    // is requested (sketch.md §2.5).
    {
        const auto probe = k.extremePoint(shape, q, a3);
        if (!probe) return std::nullopt;
        if ((*probe - si.point).dot(h) < 0) q.half = -h;
        const auto again = k.extremePoint(shape, q, a3);
        if (!again || (*again - si.point).dot(h) <= 0) return std::nullopt;
    }
    auto uv = [&](const Point3& p) { return Point2((p - frame.origin).dot(frame.x), (p - frame.origin).dot(frame.y)); };
    sketch::Projection g;
    if (silhouette) {
        const auto lo = k.extremePoint(shape, q, -a3), hi = k.extremePoint(shape, q, a3);
        if (!lo || !hi || (*hi - *lo).norm() <= kernel::KERNEL_EXTREME_TOL) return std::nullopt;
        const Point2 p1 = uv(*lo), p2 = uv(*hi);
        g.kind = sketch::Projection::Kind::Line;
        g.x1 = p1.x(), g.y1 = p1.y(), g.x2 = p2.x(), g.y2 = p2.y();
        return g;
    }
    const bool far = kind.find("far") != std::string::npos;
    const auto p = k.extremePoint(shape, q, far ? h : Vec3(-h));
    if (!p) return std::nullopt;
    const Point2 pp = uv(*p);
    g.kind = sketch::Projection::Kind::Point;
    g.px = pp.x(), g.py = pp.y();
    return g;
}

namespace {

// Sketch projection of a kernel 2D curve (sketch.md §2.5); nothing — a spline (not a sketch projection kind).
std::optional<sketch::Projection> projectionOfCurve(const kernel::Curve2& c) {
    sketch::Projection g;
    switch (c.kind) {
        case kernel::Curve2::Kind::Point: g.kind = sketch::Projection::Kind::Point, g.px = c.p.x(), g.py = c.p.y(); break;
        case kernel::Curve2::Kind::Line:
            g.kind = sketch::Projection::Kind::Line;
            g.x1 = c.p1.x(), g.y1 = c.p1.y(), g.x2 = c.p2.x(), g.y2 = c.p2.y();
            break;
        case kernel::Curve2::Kind::Circle: g.kind = sketch::Projection::Kind::Circle, g.cx = c.center.x(), g.cy = c.center.y(), g.r = c.radius; break;
        case kernel::Curve2::Kind::Arc:
            g.kind = sketch::Projection::Kind::Arc, g.cx = c.center.x(), g.cy = c.center.y(), g.r = c.radius;
            g.sx = c.start.x(), g.sy = c.start.y(), g.ex = c.end.x(), g.ey = c.end.y();
            break;
        case kernel::Curve2::Kind::Spline: return std::nullopt;
    }
    return g;
}

// Edge and vertex targets off the sketch plane (sketch.md §2.5 «Projections of edges and vertices off the plane», FX8-SK-010):
// ordered by depth (distance to the plane), equal depth — vertices, then edges, in table order (a vertex reference stays
// a point whatever the model change; a perpendicular edge's point projection may become a segment).
// World curve of an edge for piercing (sketch.md §2.5): a line, a circle or an arc (the arc ends — the chain's end
// vertices: edgeCurve gives no ends for a circle; the arc side by the edge's center of mass, regen §4.11 item 2);
// nothing — another curve.
std::optional<WorldCurve> edgeWorldCurve(const kernel::Shape& sh, const naming::EdgeEntry& ee, const std::string& id, const kernel::Kernel& k) {
    const auto cv = k.edgeCurve(sh, ee.logical);
    if (cv.kind == kernel::CurveInfo::Kind::Line) return WorldCurve{id, WorldCurve::Kind::Line, cv.p1, cv.p2, {}, {}, 0};
    if (cv.kind != kernel::CurveInfo::Kind::Circle) return std::nullopt;
    WorldCurve w{id, cv.closed ? WorldCurve::Kind::Circle : WorldCurve::Kind::Arc, cv.p1, cv.p2, cv.center, cv.axis.normalized(), cv.radius};
    if (!cv.closed) {
        if (ee.kernelEdges.empty()) return std::nullopt;
        w.a = k.vertexPoint(sh, k.edgeVertices(sh, ee.kernelEdges.front()).first);
        w.b = k.vertexPoint(sh, k.edgeVertices(sh, ee.kernelEdges.back()).second);
        const Point3 g = k.edgeCentroid(sh, ee.logical);
        const Vec3 ra = w.a - w.center, rb = w.b - w.center;
        double phi = std::atan2(w.axis.dot(ra.cross(rb)), ra.dot(rb));
        if (phi <= 0) phi += 2 * std::numbers::pi;
        if ((g - w.center).dot(Eigen::AngleAxisd(phi / 2, w.axis) * ra) < 0) w.axis = -w.axis;
    }
    return w;
}

}  // namespace

// The piercing point of a curve (sketch.md §2.5): of the crossings with the sketch plane — the nearest to the frame origin.
std::optional<Point3> piercePoint(const std::vector<WorldCurve>& curves, const Plane& frame) {
    std::optional<Point3> best;
    for (const auto& cv : curves)
        for (const auto& p : crossPlane(cv, frame.origin, frame.normal))
            if (!best || (p - frame.origin).norm() < (*best - frame.origin).norm()) best = p;
    return best;
}

namespace {

std::vector<BodyTarget> offPlaneTargets(const Plane& frame, const PartState& state, const kernel::Kernel& k) {
    struct Item {
        double depth;
        int order;
        BodyTarget target;
    };
    std::vector<Item> items;
    auto depthOf = [&](const Point3& p) { return std::abs((p - frame.origin).dot(frame.normal)); };
    auto validBody = [&](int b) { return b >= 0 && b < int(state.bodies.size()); };
    int order = 0;
    for (const auto& ve : state.names.vertices) {
        ++order;
        if (ve.full.empty() || !validBody(ve.body)) continue;
        const Point3 p = k.vertexPoint(state.bodies[ve.body].shape, ve.vertex);
        const double depth = depthOf(p);
        if (depth <= BODY_TARGET_PLANE_TOL) continue;
        sketch::Projection g;
        g.kind = sketch::Projection::Kind::Point;
        g.px = (p - frame.origin).dot(frame.x), g.py = (p - frame.origin).dot(frame.y);
        items.push_back({depth, order, {ve.full, g}});
    }
    for (const auto& ee : state.names.edges) {
        ++order;
        if (ee.full.empty() || !validBody(ee.body)) continue;
        const kernel::Shape& sh = state.bodies[ee.body].shape;
        // Depth and the "lies in the plane" test by the ends and middles of the chain's kernel edges.
        double depth = std::numeric_limits<double>::infinity(), far = 0;
        for (int ke : ee.kernelEdges) {
            const auto [v1, v2] = k.edgeVertices(sh, ke);
            for (const Point3& p : {k.vertexPoint(sh, v1), k.vertexPoint(sh, v2), k.edgeMidpoint(sh, ke)}) {
                const double d = depthOf(p);
                depth = std::min(depth, d), far = std::max(far, d);
            }
        }
        if (ee.kernelEdges.empty()) continue;
        const kernel::CurveInfo ci = k.edgeCurve(sh, ee.logical);
        bool inPlane = far <= BODY_TARGET_PLANE_TOL;
        if (ci.kind == kernel::CurveInfo::Kind::Line) inPlane = depthOf(ci.p1) <= BODY_TARGET_PLANE_TOL && depthOf(ci.p2) <= BODY_TARGET_PLANE_TOL;
        else if (ci.kind == kernel::CurveInfo::Kind::Circle)
            inPlane = depthOf(ci.center) <= BODY_TARGET_PLANE_TOL &&
                      std::abs(ci.axis.normalized().dot(frame.normal)) >= 1 - SILHOUETTE_PARALLEL_TOL;
        if (inPlane) continue;
        // An edge crossing the plane inside (not at an end on the plane): its piercing point is a point target ahead of
        // all others (depth 0), source "pierce:<edge>" — the same as «Pierce» gave (sketch.md §7, FX9-SK-008).
        if (const auto wc = edgeWorldCurve(sh, ee, ee.full, k))
            if (const auto pp = piercePoint({*wc}, frame);
                pp && (wc->kind == WorldCurve::Kind::Circle || ((*pp - wc->a).norm() > BODY_TARGET_PLANE_TOL && (*pp - wc->b).norm() > BODY_TARGET_PLANE_TOL))) {
                sketch::Projection g;
                g.kind = sketch::Projection::Kind::Point;
                g.px = (*pp - frame.origin).dot(frame.x), g.py = (*pp - frame.origin).dot(frame.y);
                items.push_back({0.0, order, {"pierce:" + ee.full, g}});
            }
        const auto c = k.projectEdge(sh, ee.logical, frame);
        if (!c) continue;  // ellipse, composite and other unsupported curves — no target
        if (auto g = projectionOfCurve(*c)) items.push_back({depth, order, {ee.full, *g}});
    }
    // Sorted through an index (no Eigen members are moved by the sort).
    std::vector<size_t> idx(items.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        return items[a].depth != items[b].depth ? items[a].depth < items[b].depth : items[a].order < items[b].order;
    });
    std::vector<BodyTarget> out;
    out.reserve(idx.size());
    for (size_t i : idx) out.push_back(items[i].target);
    return out;
}

}  // namespace

std::vector<BodyTarget> bodyTargets(const Plane& frame, const PartState& state, const kernel::Kernel& k) {
    // Piercing points of other sketches' curves (FX9-SK-008): "pierce:<sketch ID>" by the pierce rule, first.
    std::vector<BodyTarget> out;
    for (const auto& [id, se] : state.sketches)
        if (const auto pp = piercePoint(se.curves, frame)) {
            sketch::Projection g;
            g.kind = sketch::Projection::Kind::Point;
            g.px = (*pp - frame.origin).dot(frame.x), g.py = (*pp - frame.origin).dot(frame.y);
            out.push_back({"pierce:" + id, g});
        }
    for (auto& t : offPlaneTargets(frame, state, k)) out.push_back(std::move(t));
    for (const auto& fe : state.names.faces) {
        if (fe.full.empty() || fe.body < 0 || fe.body >= int(state.bodies.size())) continue;
        const kernel::Shape& sh = state.bodies[fe.body].shape;
        for (const char* p : sketch::PROJECTION_SOURCE_PREFIXES) {
            std::string kind(p);
            kind.pop_back();  // without ':'
            if (kind == "pierce") continue;
            if (auto g = bodyTargetGeometry(kind, sh, fe.face, frame, k)) out.push_back({kind + ":" + fe.full, *g, true});
        }
    }
    return out;
}

ProjectionResult resolveProjections(const sketch::SketchData& s, const Plane& frame, const PartState& state,
                                    const kernel::Kernel& k) {
    ProjectionResult out;
    std::vector<std::string> lost, ambiguous, cycle;
    auto uv = [&](const Point3& p) { return Point2((p - frame.origin).dot(frame.x), (p - frame.origin).dot(frame.y)); };
    // State where the name is resolved: in-context name of a part in assembly context — in state.context (naming.md §5.3б);
    // nullptr — context unavailable (a cycle — in cycle, otherwise — a loss).
    auto sourceState = [&](const std::string& name, const std::string& src) -> const PartState* {
        if (!naming::hasPath(name) || (!state.context && !state.contextError)) return &state;
        if (state.contextError) cycle.push_back(src);
        return state.context.get();
    };
    for (const auto& e : s.entities) {
        if (e.type != sketch::EntityType::Projected) continue;
        // Sketch axes (sketch.md §2.5, S1-SK-021): a segment along the local axis through the origin, without name resolution.
        if (e.source == "sketch:x" || e.source == "sketch:y") {
            sketch::Projection g;
            g.kind = sketch::Projection::Kind::Line;
            const bool x = e.source == "sketch:x";
            g.x1 = x ? -sketch::SKETCH_AXIS_HALF : 0, g.y1 = x ? 0 : -sketch::SKETCH_AXIS_HALF;
            g.x2 = x ? sketch::SKETCH_AXIS_HALF : 0, g.y2 = x ? 0 : sketch::SKETCH_AXIS_HALF;
            out.projections[e.id] = g;
            continue;
        }
        // Reference axis feature (sketch.md §2.5, FX9-SK-001): its line projected onto the plane, a point if perpendicular.
        if (sketch::isRefAxisSource(e.source)) {
            const auto ax = state.axes.find(sketch::refAxisFeature(e.source));
            if (ax == state.axes.end()) {
                lost.push_back(e.source);
                continue;
            }
            sketch::Projection g;
            const Point2 q = uv(ax->second.point);
            const Point2 w(ax->second.dir.dot(frame.x), ax->second.dir.dot(frame.y));
            if (w.norm() <= 1e-6) {
                g.kind = sketch::Projection::Kind::Point;
                g.px = q.x(), g.py = q.y();
            } else {
                const Point2 wn = w.normalized();
                const Point2 m = q - q.dot(wn) * wn;  // the point of the line nearest to the origin
                g.kind = sketch::Projection::Kind::Line;
                g.x1 = m.x() - sketch::SKETCH_AXIS_HALF * wn.x(), g.y1 = m.y() - sketch::SKETCH_AXIS_HALF * wn.y();
                g.x2 = m.x() + sketch::SKETCH_AXIS_HALF * wn.x(), g.y2 = m.y() + sketch::SKETCH_AXIS_HALF * wn.y();
            }
            out.projections[e.id] = g;
            continue;
        }
        // Piercing (sketch.md §2.5, S2-FEAT-006): intersection of sketch curves or an edge with the sketch plane.
        if (e.source.rfind("pierce:", 0) == 0) {
            const std::string ref = e.source.substr(7);
            std::vector<WorldCurve> curves;
            const std::string refLeaf = naming::splitPath(ref).leaf;
            if (!refLeaf.empty() && refLeaf[0] == 'E') {
                const PartState* S = sourceState(ref, e.source);
                if (!S) {
                    if (!state.contextError) lost.push_back(e.source);
                    continue;
                }
                const auto rr = naming::resolve(ref, naming::EntityKind::Edge, S->names);
                if (const auto fate = naming::refFate(rr, false); fate != naming::RefFate::Resolved) {
                    (fate == naming::RefFate::Lost ? lost : ambiguous).push_back(e.source);
                    continue;
                }
                referenceWarnings(rr, e.source, out.messages);
                const auto& ee = S->names.edges.at(rr.entities.front().index);
                const kernel::Shape& sh = S->bodies.at(ee.body).shape;
                if (const auto w = edgeWorldCurve(sh, ee, e.id, k)) {
                    curves.push_back(*w);
                } else {
                    out.messages.push_back({true, Error("REGEN_REF_GEOMETRY").with("name", std::string("source"))
                                                      .with("detail", std::string("unsupported curve")).with("id", e.id)});
                    continue;
                }
            } else {
                auto it = state.sketches.find(ref);
                if (it == state.sketches.end()) {
                    lost.push_back(e.source);
                    continue;
                }
                curves = it->second.curves;
            }
            const std::optional<Point3> best = piercePoint(curves, frame);
            if (!best) {
                out.messages.push_back({true, Error("REGEN_REF_GEOMETRY").with("name", std::string("source"))
                                                  .with("detail", std::string("curve does not cross sketch plane")).with("id", e.id)});
                continue;
            }
            sketch::Projection g;
            g.kind = sketch::Projection::Kind::Point;
            const Point2 q = uv(*best);
            g.px = q.x(), g.py = q.y();
            out.projections[e.id] = g;
            continue;
        }
        // Silhouette line / extreme point of a surface of revolution (sketch.md §2.5, FX8-SK-002): face name after the
        // kind; the geometry is recomputed from the face in this state.
        if (sketch::isSilhouetteSource(e.source) || sketch::isExtremeSource(e.source)) {
            const std::string ref = sketch::projectionSourceRef(e.source);
            const std::string kind = e.source.substr(0, e.source.size() - ref.size() - 1);
            const PartState* S = sourceState(ref, e.source);
            if (!S) {
                if (!state.contextError) lost.push_back(e.source);
                continue;
            }
            const auto rr = naming::resolve(ref, naming::EntityKind::Face, S->names);
            if (const auto fate = naming::refFate(rr, true); fate != naming::RefFate::Resolved) {
                (fate == naming::RefFate::Lost ? lost : ambiguous).push_back(e.source);
                continue;
            }
            referenceWarnings(rr, e.source, out.messages);
            // A split face (e.g. a cylinder cut in halves): the first part whose half-plane meets it.
            std::optional<sketch::Projection> g;
            for (const auto& ref1 : rr.entities) {
                const auto& fe = S->names.faces.at(ref1.index);
                if ((g = bodyTargetGeometry(kind, S->bodies.at(fe.body).shape, fe.face, frame, k))) break;
            }
            if (!g) {
                out.messages.push_back({true, Error("REGEN_REF_GEOMETRY").with("name", std::string("source"))
                                                  .with("detail", std::string("no silhouette or extreme in the sketch plane")).with("id", e.id)});
                continue;
            }
            out.projections[e.id] = *g;
            continue;
        }
        // Kind — by the first character of the name (for an assembly name "<path>/<name>" — after the last "/"); anything else gives
        // a kind error on resolution (naming §5).
        const std::string leaf = naming::splitPath(e.source).leaf;
        const naming::EntityKind kind = !leaf.empty() && leaf[0] == 'V' ? naming::EntityKind::Vertex : naming::EntityKind::Edge;
        const PartState* S = sourceState(e.source, e.source);
        if (!S) {
            if (!state.contextError) lost.push_back(e.source);
            continue;
        }
        const auto rr = naming::resolve(e.source, kind, S->names);
        // One sketch entity — one curve: SPLIT is ambiguous (naming §5.4).
        if (const auto fate = naming::refFate(rr, false); fate != naming::RefFate::Resolved) {
            (fate == naming::RefFate::Lost ? lost : ambiguous).push_back(e.source);
            continue;
        }
        referenceWarnings(rr, e.source, out.messages);
        sketch::Projection g;
        if (kind == naming::EntityKind::Vertex) {
            const auto& ve = S->names.vertices.at(rr.entities.front().index);
            const Point2 q = uv(k.vertexPoint(S->bodies.at(ve.body).shape, ve.vertex));
            g.kind = sketch::Projection::Kind::Point, g.px = q.x(), g.py = q.y();
        } else {
            const auto& ee = S->names.edges.at(rr.entities.front().index);
            auto c = k.projectEdge(S->bodies.at(ee.body).shape, ee.logical, frame);
            // A spline edge projection (kernel-interface §3.9) is not a sketch projection kind yet: as before, the
            // curve type is not supported.
            if (c && c->kind == kernel::Curve2::Kind::Spline) c = Error("KERNEL_NOT_SUPPORTED").with("name", std::string("curveType"));
            if (!c) {
                Error err = c.error();
                err.with("id", e.id);
                out.messages.push_back({true, err});
                continue;
            }
            g = *projectionOfCurve(*c);  // a spline was turned into an error above
        }
        out.projections[e.id] = g;
    }
    if (!lost.empty()) out.messages.push_back({true, Error("REGEN_REF_LOST").with("ids", lost)});
    if (!ambiguous.empty()) out.messages.push_back({true, Error("REGEN_REF_AMBIGUOUS").with("ids", ambiguous)});
    if (!cycle.empty()) out.messages.push_back({true, Error("REGEN_CONTEXT_CYCLE").with("ids", cycle)});
    return out;
}

EvalPtr Regenerator::evaluate(const doc::Document& d, const doc::Feature& f, int featureIndex, const StatePtr& input,
                              const expr::VariableTable& vars, const Cancel* cancel, bool& cancelledOut) {
    Ctx c{k_, d, f, "/elements/" + std::to_string(d.activeIndex()) + "/features/" + std::to_string(featureIndex), vars, documentUnits(d), cancel, {}, false};
    auto st = std::make_shared<PartState>(*input);

    try {
        if (f.refGeom()) {
            // §4.6
            evalRefGeometry(c, *input, *st, f);
            return finish(c, st);
        }
        if (f.type == doc::FeatureType::Shell || f.type == doc::FeatureType::Draft) return evalShellDraft(c, input, st, cancelledOut);
        if (f.type == doc::FeatureType::Pattern || f.type == doc::FeatureType::Mirror) {
            // §4.7, §4.8; in a sheet part — copies in the sheet model (§4.16 item 5в, FX8-SH-005)
            if (sheetIntact(*input)) return evalSheetPattern(c, input, st, cancelledOut);
            kernel::Shape preview;
            const bool ok = evalPattern(c, *input, *st, preview);
            if (c.cancelled) {
                cancelledOut = true;
                return nullptr;
            }
            if (!ok) return finish(c, input, preview);
            rename(k_, *st);
            nameTableMessages(c, *input, *st);
            return finish(c, st, preview);
        }
        if (const auto* s = f.sketchData()) {
            // §4.1
            std::optional<Plane> frame;
            if (s->plane.onFeature()) {
                auto it = input->planes.find(s->plane.feature);
                if (it == input->planes.end()) c.error(Error("REGEN_INPUT_ERROR").with("id", s->plane.feature), "/plane/feature");
                else frame = it->second;
            } else if (s->plane.frame) {  // explicit frame (document.md §2.7, S2-ASM-011)
                frame = planeFromFrame(*s->plane.frame);
                if (!frame) c.error(refGeometry("frame", "degenerate frame"), "/plane/frame");
            } else if (!s->plane.onFace()) {
                frame = sketch::datumPlane(s->plane.datum);
            } else {
                std::vector<Message> warnings;
                auto pl = sketchPlaneOnFace(s->plane.face, *input, k_, warnings);
                for (auto& w : warnings) c.messages.push_back(w);
                if (!pl) c.error(pl.error(), "/plane/face");
                else frame = *pl;
            }
            if (!frame) return finish(c, input);
            // §4.1 item 1а: projections in the in-context state (evaluator input).
            sketch::SketchData sd = *s;
            for (const auto& e : s->entities)
                if (e.type == sketch::EntityType::Projected) {
                    ProjectionResult pr = resolveProjections(*s, *frame, *input, k_);
                    for (auto& m : pr.messages) c.messages.push_back(m);
                    sd.projections = std::move(pr.projections);
                    break;
                }
            const sketch::Values values = dimensionValues(sd, vars, c.units, c.fpath);
            sketch::SketchResult sr = sketch::solve(sd, values);
            const bool err = sr.status == sketch::SketchStatus::Conflict || sr.status == sketch::SketchStatus::NotConverged ||
                             sr.status == sketch::SketchStatus::Error || !sr.errors.empty();
            for (const auto& e : sr.errors) c.error(e);
            for (const auto& m : sr.messages) (m.code == "SKETCH_REDUNDANT" ? c.messages.push_back({false, m}) : c.messages.push_back({err, m}));
            for (const auto& m : sr.profiles.messages) c.warning(m);
            std::vector<WorldCurve> curves;  // §2.1: world curves for piercing
            {
                const auto& P = sr.state.points;
                auto W = [&](const std::string& id) { return frame->toWorld(P.at(id)); };
                for (const auto& e : sd.entities) {
                    if (e.construction) continue;
                    if (e.type == sketch::EntityType::Line && P.count(e.p1) && P.count(e.p2))
                        curves.push_back({e.id, WorldCurve::Kind::Line, W(e.p1), W(e.p2), {}, frame->normal, 0});
                    else if (e.type == sketch::EntityType::Arc && P.count(e.start) && P.count(e.end) && P.count(e.center))
                        curves.push_back({e.id, WorldCurve::Kind::Arc, W(e.start), W(e.end), W(e.center), frame->normal,
                                          (P.at(e.start) - P.at(e.center)).norm()});
                    else if (e.type == sketch::EntityType::Circle && P.count(e.center)) {
                        const auto rit = sr.state.radii.find(e.id);
                        const double r = rit != sr.state.radii.end() ? rit->second : e.r;
                        curves.push_back({e.id, WorldCurve::Kind::Circle, {}, {}, W(e.center), frame->normal, r});
                    }
                }
            }
            st->sketches[f.id] = {sr, *frame, sd.projections, curves};
            return finish(c, st);
        }

        if (f.type == doc::FeatureType::SheetMetal || f.type == doc::FeatureType::SheetWall || f.type == doc::FeatureType::SheetFlange ||
            f.type == doc::FeatureType::SheetBend || f.type == doc::FeatureType::SheetCut)
            return evalSheet(c, input, st, cancelledOut);  // §4.16
        if (f.type == doc::FeatureType::Sweep) return evalSweep(c, input, st, cancelledOut);
        if (f.type == doc::FeatureType::Import) return evalImport(c, input, st, cancelledOut);
        if (f.type == doc::FeatureType::Thread) return evalThread(c, input, st, cancelledOut);
        if (f.type == doc::FeatureType::Hole) return evalHole(c, input, st, cancelledOut);
        if (f.type == doc::FeatureType::Gear) return evalGear(c, input, st, cancelledOut);
        if (f.type == doc::FeatureType::Coil) return evalCoil(c, input, st, cancelledOut);  // §4.20
        if (f.type == doc::FeatureType::Connection) return evalConnection(c, input, st, cancelledOut);  // solutions.md §7
        if (f.type == doc::FeatureType::ConnectionCut) return evalConnectionCut(c, input, st, cancelledOut);  // §4.19
        if (f.type == doc::FeatureType::Loft) return evalLoft(c, input, st, cancelledOut);
        if (const auto* x = f.extrude()) return evalExtrude(c, input, st, cancelledOut, x);

        if (const auto* rv = f.revolve()) return evalRevolve(c, input, st, cancelledOut, rv);

        if (const auto* ef = f.edgeFeature()) return evalEdgeFeature(c, input, st, cancelledOut, ef);
    } catch (const std::exception& e) {
        c.error(Error("REGEN_KERNEL_FAILURE").with("detail", std::string(e.what())));
        return finish(c, input);
    }
    return finish(c, input);
}

// ---------------------------------------------------------------- preview of reference geometry (selection.md §7.1)

RefPreview previewReference(const kernel::Kernel& k, const doc::Document& d, const PartState& in, const doc::Json& def,
                            const expr::VariableTable& vars) {
    RefPreview out;
    const std::string type = def.value("type", std::string());
    doc::Feature f;
    f.id = "f0";  // not a document ID: messages only
    f.type = type == "plane" ? doc::FeatureType::Plane : doc::FeatureType::Axis;
    f.data = doc::RefGeomData{def};
    Ctx c{k, d, f, std::string(), vars, documentUnits(d), nullptr, {}, false};
    try {
        if (type == "direction") {
            out.axis = refDirection(c, in, def["ref"], "/ref", "ref");
        } else if (type == "plane" || type == "axis") {
            PartState st;
            if (evalRefGeometry(c, in, st, f)) {
                if (auto it = st.planes.find(f.id); it != st.planes.end()) out.plane = it->second;
                if (auto it = st.axes.find(f.id); it != st.axes.end()) out.axis = it->second;
            }
        }
    } catch (const std::exception& e) {
        c.error(Error("REGEN_KERNEL_FAILURE").with("detail", std::string(e.what())));
    }
    out.messages = std::move(c.messages);
    if (!out.plane && !out.axis && out.messages.empty()) out.messages.push_back({true, Error("REGEN_INPUT_ERROR")});
    return out;
}

}  // namespace construe::regen
