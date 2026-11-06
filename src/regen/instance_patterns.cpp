// Instance patterns and the exploded view of an assembly (regen/instance_patterns.h, spec/assembly.md §7д, §7е).
#include "regen/instance_patterns.h"

#include "regen/assembly_ref.h"
#include "regen/mates.h"
#include "regen/regen.h"
#include "base/constants.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace construe::regen {

namespace {

Motion3 inverse(const Motion3& m) { return {m.R.transpose(), -(m.R.transpose() * m.t)}; }

Motion3 fromKernel(const kernel::Motion& m) {
    Motion3 r;
    for (int i = 0; i < 9; ++i) r.R(i / 3, i % 3) = m.r[size_t(i)];
    r.t = m.t;
    return r;
}

bool under(const std::string& path, const std::string& top) { return path == top || path.rfind(top + "/", 0) == 0; }

// World bounding box of the bodies of the entries under top (empty top — all entries with a state).
BBox3 worldBox(const kernel::Kernel& k, const AssemblyState& st, const std::string& top) {
    BBox3 bb;
    bb.setEmpty();
    for (const auto& e : st.instances) {
        if (!e.state || e.suppressed || e.error) continue;
        if (!top.empty() && (!under(e.path, top) || !e.pattern.empty() || !e.connection.empty())) continue;
        for (const auto& b : e.state->bodies) {
            const BBox3 x = k.bbox(b.shape);
            if (x.isEmpty()) continue;
            for (int c = 0; c < 8; ++c) bb.extend(Point3(e.world.apply(x.corner(BBox3::CornerType(c)))));
        }
    }
    return bb;
}

// Pattern count: dimensionless integer in [1, PATTERN_MAX_INSTANCES] (§7е.3 item 2).
Result<int> countValue(const doc::Json& j, const expr::VariableTable& vars, const expr::DocumentUnits& units) {
    if (!j.is_string()) return Error("ASM_PATTERN_BAD_VALUE").with("name", std::string("count"));
    auto q = expr::evaluateField(j.get<std::string>(), expr::Expected(expr::DIMLESS), vars.env(), units);
    if (!q) return q.error();
    const double n = q->value, r = std::round(n);
    if (std::abs(n - r) > PATTERN_INT_TOL || r < 1 || r > PATTERN_MAX_INSTANCES)
        return Error("ASM_PATTERN_BAD_VALUE").with("name", std::string("count")).with("actual", n);
    return static_cast<int>(r);
}

Result<double> positiveValue(const doc::Json& j, const char* name, expr::Expected expected, double max, const expr::VariableTable& vars,
                             const expr::DocumentUnits& units) {
    if (!j.is_string()) return Error("ASM_PATTERN_BAD_VALUE").with("name", std::string(name));
    auto q = expr::evaluateField(j.get<std::string>(), expected, vars.env(), units);
    if (!q) return q.error();
    if (!(q->value > 0) || q->value > max) return Error("ASM_PATTERN_BAD_VALUE").with("name", std::string(name)).with("actual", q->value);
    return q->value;
}

// Rotation about the axis (point p, unit direction a) by angle th (right-hand rule).
Motion3 rotationAbout(const Vec3& p, const Vec3& a, double th) {
    Motion3 m;
    m.R = Eigen::AngleAxisd(th, a).toRotationMatrix();
    m.t = p - m.R * p;
    return m;
}

// Motions G_k with labels (§7е.3 item 3) or the pattern error.
Result<std::vector<std::pair<std::string, Motion3>>> patternMotions(const kernel::Kernel& k, const doc::InstancePattern& p,
                                                                    const expr::VariableTable& vars, const expr::DocumentUnits& units,
                                                                    const AssemblyState& st) {
    const doc::Json& d = p.def;
    const std::string mode = p.mode();
    std::vector<std::pair<std::string, Motion3>> out;
    auto badRef = [](const std::string& ref) { return Error("ASM_PATTERN_BAD_REF").with("ref", ref); };
    if (mode == "linear") {
        const doc::Json& dirs = d["directions"];
        std::vector<Vec3> step;
        std::vector<int> n;
        for (const auto& dj : dirs) {
            const std::string ref = dj.value("ref", std::string());
            const auto prim = worldMatePrimitive(k, st, ref);
            if (!prim || prim->kind == MatePrimitive::Kind::Point) return badRef(ref);
            auto cnt = countValue(dj["count"], vars, units);
            if (!cnt) return cnt.error();
            auto sp = positiveValue(dj["spacing"], "spacing", expr::Expected(expr::LENGTH), 1e300, vars, units);
            if (!sp) return sp.error();
            const double sign = dj.value("reverse", false) ? -1.0 : 1.0;
            step.push_back(prim->w.normalized() * (sign * *sp));
            n.push_back(*cnt);
        }
        const int n2 = n.size() > 1 ? n[1] : 1;
        for (int j = 0; j < n2; ++j)
            for (int i = 0; i < n[0]; ++i) {
                if (i == 0 && j == 0) continue;
                Motion3 m;
                m.t = double(i) * step[0] + (n.size() > 1 ? Vec3(double(j) * step[1]) : Vec3::Zero());
                out.push_back({n.size() > 1 ? std::to_string(i) + "_" + std::to_string(j) : std::to_string(i), m});
            }
        return out;
    }
    if (mode == "circular") {
        const std::string ref = d.value("axis", std::string());
        const auto prim = worldMatePrimitive(k, st, ref);
        if (!prim || (prim->kind != MatePrimitive::Kind::Axis && prim->kind != MatePrimitive::Kind::Line)) return badRef(ref);
        auto cnt = countValue(d["count"], vars, units);
        if (!cnt) return cnt.error();
        auto ang = positiveValue(d["angle"], "angle", expr::Expected(expr::ANGLE), 2 * PI + PATTERN_INT_TOL, vars, units);
        if (!ang) return ang.error();
        const bool full = std::abs(*ang - 2 * PI) <= PATTERN_INT_TOL;
        const double delta = full ? *ang / *cnt : (*cnt > 1 ? *ang / (*cnt - 1) : 0.0);
        const double sign = d.value("reverse", false) ? -1.0 : 1.0;
        for (int i = 1; i < *cnt; ++i) out.push_back({std::to_string(i), rotationAbout(prim->p, prim->w.normalized(), sign * i * delta)});
        return out;
    }
    // feature: the instances of a part pattern feature, conjugated by the part instance's motion.
    const std::string path = d["feature"].value("instance", std::string());
    const std::string fid = d["feature"].value("feature", std::string());
    const InstanceEntry* fe = st.find(path);
    if (!fe || !fe->state || fe->error || fe->suppressed || fe->kind != "part") return badRef(path);
    const auto it = fe->state->patterns.find(fid);
    if (it == fe->state->patterns.end()) return badRef(fid);
    const Motion3 W = fe->world, Wi = inverse(W);
    for (const auto& [label, m] : it->second) out.push_back({label, W * fromKernel(m) * Wi});
    return out;
}

}  // namespace

void evaluateInstancePatterns(const kernel::Kernel& k, const doc::Element& asmEl, const expr::VariableTable& vars,
                              const expr::DocumentUnits& units, AssemblyState& st) {
    st.patterns.clear();
    const size_t originals = st.instances.size();
    for (const auto& p : asmEl.patterns) {
        PatternStatus ps;
        ps.id = p.id;
        if (p.suppressed()) {
            st.patterns.push_back(ps);
            continue;
        }
        const std::string src = p.source();
        const InstanceEntry* se = st.find(src);
        if (!se || se->depth != 0 || se->suppressed || se->error || !se->pattern.empty() || !se->connection.empty()) {
            ps.error = Error("ASM_PATTERN_SOURCE").with("id", src);
            st.patterns.push_back(ps);
            continue;
        }
        auto motions = patternMotions(k, p, vars, units, st);
        if (!motions) {
            ps.error = motions.error();
            st.patterns.push_back(ps);
            continue;
        }
        // The source and its nested entries (subassembly), as copies: push_back invalidates references.
        std::vector<InstanceEntry> copied;
        for (size_t i = 0; i < originals; ++i)
            if (under(st.instances[i].path, src)) copied.push_back(st.instances[i]);
        const std::string srcName = copied.front().name;
        for (const auto& [label, G] : *motions)
            for (const auto& c : copied) {
                InstanceEntry e = c;
                const std::string rest = c.path.substr(src.size());
                e.path = p.id + "/" + label + rest;
                e.instance = rest.empty() ? label : c.instance;
                e.name = rest.empty() ? srcName + " [" + label + "]" : c.name;
                e.depth = c.depth + 1;
                e.world = G * c.world;
                e.fixed = true;
                e.freeDof = -1;
                e.pattern = p.id;
                e.copyOf = c.path;
                st.instances.push_back(std::move(e));
            }
        ps.children = int(motions->size());
        st.patterns.push_back(ps);
    }
}

int patternChildCount(const doc::Element& asmEl, const doc::InstancePattern& p, const expr::VariableTable& vars,
                      const expr::DocumentUnits& units, const PatternPartLookup& part) {
    if (p.suppressed()) return 0;
    const auto& ins = asmEl.instances;
    const auto src = std::find_if(ins.begin(), ins.end(), [&](const doc::Instance& in) { return in.id == p.source(); });
    if (src == ins.end() || src->suppressed) return 0;
    const doc::Json& d = p.def;
    const std::string mode = p.mode();
    auto product = [](const doc::Json& def, const std::string& m, const expr::VariableTable& v, const expr::DocumentUnits& u) -> int {
        if (m == "linear") {
            int n = 1;
            for (const auto& dj : def["directions"]) {
                auto c = countValue(dj["count"], v, u);
                if (!c) return 0;
                n *= *c;
            }
            return n;
        }
        auto c = countValue(def["count"], v, u);
        return c ? *c : 0;
    };
    if (mode != "feature") return std::max(0, product(d, mode, vars, units) - 1);
    const auto [D, E] = part ? part(d["feature"].value("instance", std::string())) : std::make_pair(nullptr, nullptr);
    if (!D || !E) return 0;
    const auto f = E->find(d["feature"].value("feature", std::string()));
    if (!f || f->type != doc::FeatureType::Pattern || f->suppressed || !f->pattern()) return 0;
    const doc::Json& fd = f->pattern()->def;
    return std::max(0, product(fd, fd.value("mode", std::string()), documentVariables(*D), documentUnits(*D)) - 1);
}

bool isLibraryFastener(const InstanceEntry& e) {
    if (e.depth != 0 || e.kind != "part" || !e.connection.empty() || !doc::isLibraryRef(e.file)) return false;
    std::string pkg = e.file.substr(std::string(doc::LIBRARY_REF_PREFIX).size());
    pkg = pkg.substr(0, pkg.find('@'));
    const size_t dot = pkg.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string kind = pkg.substr(dot + 1);
    return std::any_of(std::begin(EXPLODE_FASTENER_KINDS), std::end(EXPLODE_FASTENER_KINDS), [&](const char* x) { return kind == x; });
}

std::vector<ExplodedFastener> explodedFasteners(const kernel::Kernel& k, const AssemblyState& st, const doc::Element* asmEl) {
    auto isFastener = [](const InstanceEntry& e) { return e.pattern.empty() && (!e.connection.empty() || isLibraryFastener(e)); };
    struct F {
        const InstanceEntry* e;
        Vec3 a;          // world axis: +Y of the part
        Point3 o;        // world origin of the part: its bearing point
        double below;    // extent of the part below its «Top» plane (−Y)
        double d;        // diameter
    };
    std::vector<F> fs;
    for (const auto& e : st.instances) {
        if (!e.state || e.suppressed || e.error || !isFastener(e)) continue;
        F f{&e, e.world.applyDir(Vec3::UnitY()).normalized(), e.world.t, 0, 0};
        BBox3 lb;
        lb.setEmpty();
        for (const auto& b : e.state->bodies)
            if (const BBox3 bb = k.bbox(b.shape); !bb.isEmpty()) lb.extend(bb);
        if (!lb.isEmpty()) f.below = std::max(0.0, -lb.min().y());
        // Diameter (§7д.5 item 2): the nominal diameter of its threads, else the smallest cylinder, else the box width.
        for (const auto& t : e.state->threads) f.d = std::max(f.d, t.d);
        if (f.d <= 0) {
            double r = 0;
            for (const auto& nf : e.state->names.faces) {
                const kernel::SurfaceInfo si = k.faceSurface(e.state->bodies.at(size_t(nf.body)).shape, nf.face);
                if (si.kind == kernel::SurfaceInfo::Kind::Cylinder && si.radius > 0 && (r == 0 || si.radius < r)) r = si.radius;
            }
            f.d = 2 * r;
        }
        if (f.d <= 0 && !lb.isEmpty()) f.d = std::min(lb.sizes().x(), lb.sizes().z());
        fs.push_back(f);
    }
    // Stacks (§7д.5 item 2): one axis line and direction, from the innermost (the smallest coordinate along the axis).
    std::vector<std::vector<size_t>> stacks;
    std::vector<bool> taken(fs.size(), false);
    for (size_t i = 0; i < fs.size(); ++i) {
        if (taken[i]) continue;
        std::vector<size_t> sk{i};
        taken[i] = true;
        for (size_t j = i + 1; j < fs.size(); ++j) {
            if (taken[j] || fs[i].a.dot(fs[j].a) < 1 - 1e-9) continue;
            const Vec3 v = fs[j].o - fs[i].o;
            if ((v - v.dot(fs[i].a) * fs[i].a).norm() > EXPLODE_FASTENER_TOL) continue;
            sk.push_back(j);
            taken[j] = true;
        }
        std::stable_sort(sk.begin(), sk.end(), [&](size_t x, size_t y) { return (fs[x].o - fs[i].o).dot(fs[i].a) < (fs[y].o - fs[i].o).dot(fs[i].a); });
        stacks.push_back(std::move(sk));
    }
    // Attachment (§7д.5 item 3): the part with a plane face through the bearing point, perpendicular to the axis, nearest
    // to the point (the face around the hole); else, for a library instance, the first mate partner that is no fastener.
    auto attachedOf = [&](const F& f) {
        std::string best;
        double bestDist = std::numeric_limits<double>::infinity();
        for (const auto& c : st.instances) {
            if (!c.state || c.suppressed || c.error || !c.pattern.empty() || !c.connection.empty() || c.kind != "part" || isFastener(c)) continue;
            BBox3 wb = worldBox(k, st, c.path);
            if (wb.isEmpty()) continue;
            const Vec3 grow = Vec3::Constant(f.d + EXPLODE_FASTENER_TOL);
            wb.extend(Point3(wb.min() - grow));
            wb.extend(Point3(wb.max() + grow));
            if (!wb.contains(f.o)) continue;
            const Motion3 inv = inverse(c.world);
            const Point3 ol = inv.apply(f.o);
            const Vec3 al = inv.applyDir(f.a);
            for (const auto& nf : c.state->names.faces) {
                const auto& sh = c.state->bodies.at(size_t(nf.body)).shape;
                const kernel::SurfaceInfo si = k.faceSurface(sh, nf.face);
                if (si.kind != kernel::SurfaceInfo::Kind::Plane) continue;
                const Vec3 n = si.dir.normalized();
                if (std::abs(n.dot(al)) < 1 - 1e-6 || std::abs((ol - si.point).dot(n)) > EXPLODE_FASTENER_TOL) continue;
                const double dist = k.distanceToFace(sh, nf.face, ol);
                if (dist <= EXPLODE_FASTENER_GAP * f.d && dist < bestDist) bestDist = dist, best = c.path.substr(0, c.path.find('/'));
            }
        }
        if (best.empty() && asmEl && f.e->depth == 0)
            for (const auto& m : asmEl->mates) {
                if (m.suppressed) continue;
                std::string own, other;
                for (const auto& r : m.refs) (refTopInstance(r) == f.e->path ? own : other) = refTopInstance(r);
                const InstanceEntry* o = other.empty() ? nullptr : st.find(other);
                if (!own.empty() && o && !isFastener(*o)) return other;
            }
        return best;
    };
    std::vector<ExplodedFastener> out;
    for (const auto& sk : stacks) {
        const std::string att = attachedOf(fs[sk.front()]);
        double acc = 0;  // the gaps of the inner fasteners of the stack
        for (size_t i : sk) {
            const F& f = fs[i];
            out.push_back({f.e->path, att, (f.below + EXPLODE_FASTENER_GAP * f.d + acc) * f.a});
            acc += EXPLODE_FASTENER_GAP * f.d;
        }
    }
    return out;
}

namespace {
// World displacement of a fastener following attachment att (§7д.5 item 4): its shift after the attachment's
// displacement X(att)·W(att)⁻¹ (identity without one); W — assembled, X — exploded placements of the top-level instances.
Motion3 followMove(const ExplodedFastener& f, const AssemblyState& assembled, const std::map<std::string, Motion3>& X) {
    Motion3 E;
    if (const auto it = X.find(f.attached); !f.attached.empty() && it != X.end())
        if (const InstanceEntry* t = assembled.find(f.attached)) E = it->second * inverse(t->world);
    return Motion3{Eigen::Matrix3d::Identity(), f.shift} * E;
}
}  // namespace

ExplodedStart autoExplode(const kernel::Kernel& k, const doc::Element& asmEl, const expr::VariableTable& vars,
                          const expr::DocumentUnits& units, const AssemblyState& st) {
    ExplodedStart out;
    // Top-level library fasteners follow their attachments (§7д.5): placed after the others.
    std::map<std::string, ExplodedFastener> follow;
    for (auto& f : explodedFasteners(k, st, &asmEl))
        if (const InstanceEntry* e = st.find(f.path); e && e->depth == 0) follow[f.path] = f;
    const BBox3 all = worldBox(k, st, {});
    const Vec3 C = all.isEmpty() ? Vec3::Zero() : Vec3(all.center());
    const double L = all.isEmpty() ? 1.0 : std::max(1.0, all.diagonal().norm());
    for (const auto& in : asmEl.instances) {
        const InstanceEntry* e = st.find(in.id);
        if (!e || e->depth != 0) continue;
        out.placements[in.id] = e->world;
        if (e->fixed || e->suppressed || e->error || follow.count(in.id)) continue;
        const BBox3 bb = worldBox(k, st, in.id);
        if (bb.isEmpty()) continue;
        const Vec3 c = bb.center();
        // Direction (§7д.3 item 2): an axis or line of its first mate, else a plane normal, else from the center.
        std::optional<Vec3> axis, normal;
        for (const auto& m : asmEl.mates) {
            if (m.suppressed) continue;
            const MateStatus* ms = st.mate(m.id);
            if (ms && ms->error) continue;
            int own = 0;
            std::optional<MatePrimitive> prim;
            for (const auto& r : m.refs) {
                auto mp = matePrimitive(k, st, r);
                if (mp && mp->first == in.id) {
                    ++own;
                    prim = mp->second;
                }
            }
            if (own != 1 || !prim) continue;
            const Vec3 w = e->world.applyDir(prim->w).normalized();
            if (!axis && (prim->kind == MatePrimitive::Kind::Axis || prim->kind == MatePrimitive::Kind::Line)) axis = w;
            if (!normal && prim->kind == MatePrimitive::Kind::Plane) normal = w;
        }
        Vec3 d = axis ? *axis : normal ? *normal : Vec3::UnitZ();
        if (!axis && !normal && (c - C).norm() > 1e-9 * L) d = (c - C).normalized();
        if ((c - C).dot(d) < 0) d = -d;
        const Vec3 delta = d * (EXPLODE_GAP * L + EXPLODE_SCALE * std::max(0.0, (c - C).dot(d)));
        out.placements[in.id] = Motion3{Eigen::Matrix3d::Identity(), delta} * e->world;
    }
    for (const auto& [path, f] : follow) out.placements[path] = followMove(f, st, out.placements) * st.find(path)->world;
    // Mates of the mode (§7д.3 item 5): those violated by the explosion.
    AssemblyState moved = st;
    moveTopInstances(moved, out.placements);
    std::map<std::string, double> res;
    solveMates(k, asmEl, vars, units, moved, std::nullopt, &res);
    for (const auto& m : asmEl.mates)
        if (auto it = res.find(m.id); it != res.end() && it->second > EXPLODE_RESIDUAL_TOL) out.suppressedMates.push_back(m.id);
    return out;
}

void moveTopInstances(AssemblyState& st, const std::map<std::string, Motion3>& X) {
    for (const auto& [top, Xt] : X) {
        const InstanceEntry* t = st.find(top);
        if (!t || t->depth != 0 || !t->pattern.empty() || !t->connection.empty()) continue;
        const Motion3 E = Xt * inverse(t->world);
        for (auto& e : st.instances)
            if (e.pattern.empty() && e.connection.empty() && under(e.path, top)) e.world = E * e.world;
    }
}

AssemblyState explodedSolve(const kernel::Kernel& k, const doc::Element& asmEl, const expr::VariableTable& vars,
                            const expr::DocumentUnits& units, const AssemblyState& st, bool& autoComputed,
                            std::vector<std::string>& autoSuppressed) {
    std::map<std::string, Motion3> X;
    std::vector<std::string> S;
    bool newPlaced = false;
    autoComputed = false;
    autoSuppressed.clear();
    if (!asmEl.exploded || !asmEl.exploded->placements) {
        ExplodedStart xs = autoExplode(k, asmEl, vars, units, st);
        X = std::move(xs.placements);
        S = xs.suppressedMates;
        autoComputed = true;
        autoSuppressed = S;
    } else {
        // Translation of the record with the assembled rotation: the exploded view only moves parts (§7д.2, FX8-ASM-006).
        for (const auto& p : *asmEl.exploded->placements) {
            Motion3 m = motionOf(p.position, p.rotation);
            if (const InstanceEntry* e = st.find(p.instance); e && e->depth == 0) m.R = e->world.R;
            X[p.instance] = m;
        }
        S = asmEl.exploded->suppressedMates;
        // Instances new to the saved view (§7д.2 item 2, FX12-ASM-002): all their mates were added after the view took the
        // mates into account (knownMates) — they are exploded by the automatic rule, and the mates of theirs the
        // automatic explosion relaxes join S. Without it a part inserted and mated later was carried by its mates with
        // the part it is mated to as one piece and could not be pulled off it.
        if (const auto& K = asmEl.exploded->knownMates) {
            std::map<std::string, bool> allNew;  // top-level instance → all its mates are new
            for (const auto& m : asmEl.mates) {
                if (m.suppressed) continue;
                const bool fresh = std::find(K->begin(), K->end(), m.id) == K->end();
                for (const auto& r : m.refs) {
                    const std::string top = refTopInstance(r);
                    if (top.empty()) continue;
                    auto [it, inserted] = allNew.emplace(top, fresh);
                    if (!inserted) it->second = it->second && fresh;
                }
            }
            std::set<std::string> fresh;
            for (const auto& [top, all] : allNew) {
                const InstanceEntry* e = st.find(top);
                if (all && e && e->depth == 0 && !e->fixed && !e->suppressed && !e->error && e->pattern.empty() && e->connection.empty() &&
                    !isLibraryFastener(*e))
                    fresh.insert(top);
            }
            if (!fresh.empty()) {
                ExplodedStart xs = autoExplode(k, asmEl, vars, units, st);
                for (const auto& t : fresh)
                    if (const auto it = xs.placements.find(t); it != xs.placements.end()) X[t] = it->second;
                for (const auto& id : xs.suppressedMates) {
                    if (std::find(S.begin(), S.end(), id) != S.end()) continue;
                    const auto m = std::find_if(asmEl.mates.begin(), asmEl.mates.end(), [&](const doc::Mate& x) { return x.id == id; });
                    if (m != asmEl.mates.end() && std::any_of(m->refs.begin(), m->refs.end(), [&](const std::string& r) { return fresh.count(refTopInstance(r)) > 0; }))
                        S.push_back(id);
                }
                newPlaced = true;
                autoSuppressed = S;
            }
        }
    }
    // Followers (§7д.5 item 4): top-level library fasteners without a record of their own (all — in the automatic
    // explosion); their mates do not take part in the solve, they are placed after it after their attachments.
    std::vector<ExplodedFastener> followers;
    for (auto& f : explodedFasteners(k, st, &asmEl)) {
        const InstanceEntry* e = st.find(f.path);
        if (!e || e->depth != 0) continue;
        const bool own = !autoComputed && asmEl.exploded && asmEl.exploded->placements &&
                         std::any_of(asmEl.exploded->placements->begin(), asmEl.exploded->placements->end(),
                                     [&](const doc::ExplodedPlacement& p) { return p.instance == f.path; });
        if (!own) followers.push_back(f);
    }
    AssemblyState c = st;
    moveTopInstances(c, X);
    doc::Element el = asmEl;
    if (!el.exploded) el.exploded.emplace();
    el.exploded->suppressedMates = S;
    // Mates of the library fasteners — followers and manually placed alike — are not solved in the mode (§7д.5 item 4):
    // a manual placement stays where it was put.
    el = explodedMateElement(el);
    for (auto& m : el.mates)
        for (const auto& r : m.refs)
            if (const InstanceEntry* e = st.find(refTopInstance(r)); e && isLibraryFastener(*e)) m.suppressed = true;
    MateSolveOptions o;
    o.translationOnly = true;  // §7д.2 item 3: no branch selection, rotations fixed
    solveMates(k, el, vars, units, c, std::nullopt, nullptr, o);
    std::map<std::string, Motion3> Xs;
    for (const auto& e : c.instances)
        if (e.depth == 0) Xs[e.path] = e.world;
    std::map<std::string, Motion3> Xf;
    for (const auto& f : followers) {
        Xf[f.path] = followMove(f, st, Xs) * st.find(f.path)->world;
        c.explodedFollow[f.path] = f.attached;
    }
    for (auto& e : c.instances)  // followers are out of the mode's mate system (§7д.5): no stale freeDof of the assembled one
        if (Xf.count(e.path)) e.freeDof = -1;
    moveTopInstances(c, Xf);
    c.explodedNew = newPlaced;
    return c;
}

AssemblyPtr applyExploded(const kernel::Kernel& k, const doc::Element& asmEl, const AssemblyState& final, const AssemblyState& solved,
                          bool autoComputed, const std::vector<std::string>& autoSuppressed) {
    auto a = std::make_shared<AssemblyState>(final);
    std::map<std::string, Motion3> E;  // world displacement of each top-level instance: X'(t) · W(t)⁻¹
    std::map<std::string, Motion3> Xs;  // exploded placements of the top-level instances
    for (const auto& e : final.instances)
        if (e.depth == 0 && e.pattern.empty() && e.connection.empty())
            if (const InstanceEntry* s = solved.find(e.path)) E[e.path] = s->world * inverse(e.world), Xs[e.path] = s->world;
    a->explodedFollow = solved.explodedFollow;
    // Derived connection fasteners (§7д.5): a record of their own (a manual move) — its translation with the assembled
    // rotation; else they follow their attachments.
    std::map<std::string, Motion3> C;
    for (const auto& f : explodedFasteners(k, final, &asmEl)) {
        const InstanceEntry* e = final.find(f.path);
        if (!e || e->connection.empty()) continue;
        const doc::ExplodedPlacement* own = nullptr;
        if (!autoComputed && asmEl.exploded && asmEl.exploded->placements)
            for (const auto& p : *asmEl.exploded->placements)
                if (p.instance == f.path) own = &p;
        if (own) {
            C[f.path] = Motion3{Eigen::Matrix3d::Identity(), Vec3(own->position[0], own->position[1], own->position[2]) - e->world.t};
        } else {
            C[f.path] = followMove(f, final, Xs);
            a->explodedFollow[f.path] = f.attached;
        }
    }
    auto topOf = [](const std::string& p) { return p.substr(0, p.find('/')); };
    auto place = [&](InstanceEntry& e) {
        if (!e.connection.empty()) {
            if (const auto it = C.find(e.path); it != C.end()) e.world = it->second * e.world;
            return;
        }
        if (!e.pattern.empty()) {
            const InstanceEntry* src = final.find(e.copyOf);
            const auto it = E.find(topOf(e.copyOf));
            if (!src || it == E.end()) return;
            const Motion3 G = e.world * inverse(src->world);
            e.world = G * it->second * src->world;
            return;
        }
        if (const auto it = E.find(topOf(e.path)); it != E.end()) e.world = it->second * e.world;
        if (e.depth == 0)
            if (const InstanceEntry* s = solved.find(e.path)) e.freeDof = s->freeDof;
    };
    for (auto& e : a->instances) {
        const Motion3 w0 = e.world;
        place(e);
        a->explodedMove[e.path] = e.world * inverse(w0);  // the displacement by the explosion (FX14-PMI-001)
    }
    // Mate statuses stay those of the assembled solve (S7F-ASM-004, §7д.2 item 4): mates describe the assembled view, the
    // exploded mode neither recomputes nor marks them; DOF and freeDof — of the mode (dragging in the mode).
    a->dof = solved.dof;
    a->exploded = true;
    a->explodedAuto = autoComputed;
    a->explodedNew = solved.explodedNew;
    a->explodedSuppressed = autoSuppressed;
    return a;
}

}  // namespace construe::regen
