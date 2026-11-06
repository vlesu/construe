// Serializing evaluator cache entries (spec/regen.md §5.4): layout — fields in the order of the tables in §2.1–§2.2.
#include "regen/state_io.h"

#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

namespace construe::regen {

namespace {

// ---------------------------------------------------------------- writing

struct Out {
    std::string b;
    void u8(uint8_t v) { b.push_back(char(v)); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(char((v >> (8 * i)) & 0xFF));
    }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) b.push_back(char((v >> (8 * i)) & 0xFF));
    }
    void f64(double v) {
        uint64_t u;
        std::memcpy(&u, &v, 8);
        u64(u);
    }
    void flag(bool v) { u8(v ? 1 : 0); }
    void str(std::string_view s) {
        u32(uint32_t(s.size()));
        b.append(s.data(), s.size());
    }
    void p2(const Point2& p) { f64(p.x()), f64(p.y()); }
    void p3(const Vec3& p) { f64(p.x()), f64(p.y()), f64(p.z()); }
    template <typename C>
    void strs(const C& c) {
        u32(uint32_t(c.size()));
        for (const auto& s : c) str(s);
    }
    void ints(const std::vector<int>& v) {
        u32(uint32_t(v.size()));
        for (int x : v) i32(x);
    }
    void plane(const Plane& p) { p3(p.origin), p3(p.x), p3(p.y), p3(p.normal); }
    void error(const Error& e) {
        str(e.code);
        u32(uint32_t(e.params.size()));
        for (const auto& [k, v] : e.params) {
            str(k);
            if (const auto* s = std::get_if<std::string>(&v)) u8(0), str(*s);
            else if (const auto* d = std::get_if<double>(&v)) u8(1), f64(*d);
            else u8(2), strs(std::get<std::vector<std::string>>(v));
        }
        flag(e.path.has_value());
        if (e.path) str(*e.path);
    }
    void errors(const std::vector<Error>& v) {
        u32(uint32_t(v.size()));
        for (const auto& e : v) error(e);
    }
};

class Encoder {
public:
    explicit Encoder(const kernel::Kernel& k) : k_(k) {}

    int32_t shape(const kernel::Shape& s) {
        if (s.null()) return -1;
        auto [it, fresh] = shapeIdx_.try_emplace(s.data(), int32_t(shapes_.size()));
        if (fresh) shapes_.push_back(s);
        return it->second;
    }
    uint32_t intern(std::string bytes) {
        if (auto it = poolIdx_.find(bytes); it != poolIdx_.end()) return it->second;
        const uint32_t i = uint32_t(pool_.size());
        poolIdx_.emplace(bytes, i);
        pool_.push_back(std::move(bytes));
        return i;
    }

    uint32_t table(const naming::NameTable& t) {
        Out o;
        o.u32(uint32_t(t.faces.size()));
        for (const auto& f : t.faces) {
            o.i32(f.body), o.i32(f.face), o.str(f.base), o.str(f.full), o.strs(f.nb), o.p3(f.key), o.f64(f.measure);
        }
        o.u32(uint32_t(t.edges.size()));
        for (const auto& e : t.edges) {
            o.i32(e.body), o.i32(e.logical), o.ints(e.kernelEdges), o.i32(e.f1), o.i32(e.f2), o.strs(e.ends), o.str(e.full);
            o.p3(e.key), o.f64(e.measure);
        }
        o.u32(uint32_t(t.vertices.size()));
        for (const auto& v : t.vertices) o.i32(v.body), o.i32(v.vertex), o.ints(v.faces), o.str(v.full), o.p3(v.key);
        o.u32(uint32_t(t.byName.size()));
        for (const auto& [n, r] : t.byName) o.str(n), o.u8(uint8_t(r.kind)), o.i32(r.index);
        o.errors(t.warnings);
        // Nested tables — sub-entries before this one.
        std::vector<std::pair<bool, uint32_t>> subIdx;
        for (const auto& [p, sub] : t.subs) subIdx.push_back({bool(sub.table), sub.table ? table(*sub.table) : 0});
        o.u32(uint32_t(t.subs.size()));
        size_t i = 0;
        for (const auto& [p, sub] : t.subs) {
            o.str(p), o.flag(subIdx[i].first), o.u32(subIdx[i].second), o.i32(sub.face0), o.i32(sub.edge0), o.i32(sub.vertex0);
            ++i;
        }
        return intern(std::move(o.b));
    }

    static void loop(Out& o, const sketch::Loop& l) {
        o.u32(uint32_t(l.segments.size()));
        for (const auto& s : l.segments) {
            o.str(s.id), o.u8(uint8_t(s.kind)), o.p2(s.p1), o.p2(s.p2), o.p2(s.center), o.f64(s.radius), o.f64(s.minorRadius);
            o.f64(s.angle), o.p2(s.start), o.p2(s.end), o.flag(s.reversed);
        }
    }

    uint32_t sketchEntry(const SketchEntry& e) {
        Out o;
        const sketch::SketchResult& r = e.result;
        o.u8(uint8_t(r.status));
        o.flag(r.dof.has_value());
        o.i32(r.dof.value_or(0));
        o.u32(uint32_t(r.entityState.size()));
        for (const auto& [id, st] : r.entityState) o.str(id), o.u8(uint8_t(st));
        o.u32(uint32_t(r.conflicts.size()));
        for (const auto& c : r.conflicts) o.strs(c.ids), o.str(c.suggested), o.flag(c.undetermined);
        o.strs(r.redundant);
        o.strs(r.unsatisfied);
        o.u32(uint32_t(r.state.points.size()));
        for (const auto& [id, p] : r.state.points) o.str(id), o.p2(p);
        o.u32(uint32_t(r.state.radii.size()));
        for (const auto& [id, v] : r.state.radii) o.str(id), o.f64(v);
        o.u32(uint32_t(r.state.ellipses.size()));
        for (const auto& [id, el] : r.state.ellipses) o.str(id), o.f64(el.rx), o.f64(el.ry), o.f64(el.angle);
        o.u32(uint32_t(r.reference.size()));
        for (const auto& [id, v] : r.reference) o.str(id), o.f64(v);
        o.errors(r.errors);
        o.errors(r.messages);
        o.u32(uint32_t(r.profiles.regions.size()));
        for (const auto& g : r.profiles.regions) {
            o.str(g.regionId), o.i32(g.depth);
            loop(o, g.outer);
            o.u32(uint32_t(g.holes.size()));
            for (const auto& h : g.holes) loop(o, h);
            o.f64(g.area), o.p2(g.interiorPoint);
        }
        o.errors(r.profiles.messages);
        o.plane(e.frame);
        o.u32(uint32_t(e.projections.size()));
        for (const auto& [id, p] : e.projections) {
            o.str(id), o.u8(uint8_t(p.kind));
            for (double v : {p.px, p.py, p.x1, p.y1, p.x2, p.y2, p.cx, p.cy, p.r, p.sx, p.sy, p.ex, p.ey}) o.f64(v);
        }
        o.u32(uint32_t(e.curves.size()));
        for (const auto& c : e.curves) o.str(c.id), o.u8(uint8_t(c.kind)), o.p3(c.a), o.p3(c.b), o.p3(c.center), o.p3(c.axis), o.f64(c.radius);
        return intern(std::move(o.b));
    }

    static void region(Out& o, const sheet::Region& r) {
        auto loopOut = [&](const sheet::Loop& l) {
            o.u32(uint32_t(l.size()));
            for (const auto& g : l) {
                o.u8(uint8_t(g.kind)), o.p2(g.a), o.p2(g.b), o.p2(g.c), o.f64(g.r), o.flag(g.rev), o.str(g.label);
                if (g.kind != sheet::Seg::Kind::Spline) continue;
                const spline::Curve& c = g.curve;
                o.i32(c.degree), o.u32(uint32_t(c.poles.size()));
                for (const auto& p : c.poles) o.p2(p);
                o.u32(uint32_t(c.weights.size()));
                for (double w : c.weights) o.f64(w);
                o.u32(uint32_t(c.knots.size()));
                for (double k : c.knots) o.f64(k);
                o.ints(c.mults);
            }
        };
        loopOut(r.outer);
        o.u32(uint32_t(r.holes.size()));
        for (const auto& h : r.holes) loopOut(h);
    }

    // Sheet metal entry (regen.md §2.1 `sheet`, sheet.md §2.3).
    void sheetEntry(Out& o, const SheetEntry& e) {
        const sheet::Model& m = *e.model;
        o.f64(m.params.thickness), o.f64(m.params.radius), o.f64(m.params.kFactor), o.u8(uint8_t(m.params.relief));
        o.f64(m.params.reliefWidth), o.f64(m.params.reliefDepth);
        for (int i = 0; i < 9; ++i) o.f64(m.root.linear()(i / 3, i % 3));
        o.p3(m.root.translation());
        o.u32(uint32_t(m.pieces.size()));
        for (const auto& p : m.pieces) {
            o.u8(uint8_t(p.kind)), o.str(p.feature), o.i32(p.parent);
            region(o, p.region);
            o.u32(uint32_t(p.ops.size()));
            for (const auto& op : p.ops) {
                o.u8(uint8_t(op.kind)), region(o, op.region);
                o.u32(uint32_t(op.minus.size()));
                for (const auto& r : op.minus) region(o, r);
            }
            o.str(p.faceA), o.str(p.faceB), o.p2(p.origin), o.p2(p.axis), o.p2(p.out);
            for (double v : {p.w0, p.w1, p.ba, p.angle, p.radius}) o.f64(v);
            o.flag(p.up);
            for (const auto* l : {&p.faceIn, &p.faceOut, &p.faceL, &p.faceR, &p.faceS, &p.faceE}) o.str(*l);
        }
        o.u32(uint32_t(m.cuts.size()));
        for (const auto& c : m.cuts) region(o, c.region), o.str(c.feature), o.i32(c.wall);
        o.u32(uint32_t(m.corners.size()));  // FX8-SH-004
        for (const auto& c : m.corners) {
            o.i32(c.piece), o.p2(c.at), o.u8(uint8_t(c.kind)), o.f64(c.size), o.f64(c.size2), o.f64(c.angle), o.flag(c.flip);
            o.p2(c.ref), o.str(c.label), o.str(c.edge), o.str(c.feature);
        }
        o.u32(uint32_t(m.flanges.size()));  // FX8-SH-005
        for (const auto& r : m.flanges) {
            const auto& f = r.spec;
            o.str(r.feature);
            for (double v : {f.length, f.angle, f.radius, f.offsetStart, f.offsetEnd, f.gap}) o.f64(v);
            o.u8(uint8_t(f.position)), o.flag(f.reverse), o.u8(uint8_t(f.relief));
            o.u32(uint32_t(r.edges.size()));
            for (const auto& e : r.edges) o.i32(e.piece), o.flag(e.onB), o.p2(e.q1), o.p2(e.q2), o.p2(e.out), o.str(e.name);
        }
        o.str(e.body), o.i32(shape(e.folded)), o.i32(shape(e.flat)), o.strs(e.flatBases), o.f64(e.overlap);
    }

    uint32_t state(const StatePtr& s) {
        if (auto it = stateIdx_.find(s.get()); it != stateIdx_.end()) return it->second;
        const uint32_t names = table(s->names);
        std::vector<uint32_t> sk;
        for (const auto& [id, e] : s->sketches) sk.push_back(sketchEntry(e));
        const bool hasContext = bool(s->context);
        const uint32_t ctx = hasContext ? state(s->context) : 0;
        std::vector<uint32_t> parts;  // assembly parts (assembly.md §7г.2)
        for (const auto& p : s->assemblyParts) parts.push_back(state(p.local));
        Out o;
        o.u32(uint32_t(s->bodies.size()));
        for (const auto& b : s->bodies) o.str(b.id), o.i32(shape(b.shape)), o.strs(b.bases);
        o.u32(names);
        o.u32(uint32_t(s->sketches.size()));
        size_t i = 0;
        for (const auto& [id, e] : s->sketches) o.str(id), o.u32(sk[i++]);
        o.u32(uint32_t(s->planes.size()));
        for (const auto& [id, p] : s->planes) o.str(id), o.plane(p);
        o.u32(uint32_t(s->axes.size()));
        for (const auto& [id, a] : s->axes) o.str(id), o.p3(a.point), o.p3(a.dir);
        o.u32(uint32_t(s->tools.size()));
        for (const auto& [id, t] : s->tools) o.str(id), o.i32(shape(t.shape)), o.strs(t.bases), o.str(t.operation);
        o.u32(uint32_t(s->threads.size()));
        for (const auto& t : s->threads) {
            o.str(t.feature), o.str(t.designation), o.flag(t.internal), o.p3(t.origin), o.p3(t.dir);
            o.f64(t.radius), o.f64(t.d), o.f64(t.pitch), o.f64(t.length), o.f64(t.height);
        }
        o.u32(uint32_t(s->gears.size()));
        for (const auto& g : s->gears) {
            o.str(g.feature), o.flag(g.internal), o.flag(g.sector), o.f64(g.module), o.f64(g.shift), o.i32(g.teeth), o.i32(g.formed);
            o.str(g.accuracy), o.p3(g.origin), o.p3(g.dir), o.p3(g.ref), o.f64(g.width), o.f64(g.start), o.f64(g.angle);
            o.f64(g.d), o.f64(g.da), o.f64(g.df), o.i32(shape(g.fill));
        }
        o.flag(hasContext), o.u32(ctx);
        o.flag(s->contextError.has_value());
        if (s->contextError) o.error(*s->contextError);
        o.flag(s->sheet.has_value());
        if (s->sheet) sheetEntry(o, *s->sheet);
        o.u32(uint32_t(s->assemblyParts.size()));
        for (size_t k = 0; k < parts.size(); ++k) {
            const auto& p = s->assemblyParts[k];
            o.str(p.path), o.str(p.element), o.str(p.file);
            for (int r = 0; r < 9; ++r) o.f64(p.world.R(r / 3, r % 3));
            o.p3(p.world.t), o.u32(parts[k]);
        }
        o.u32(uint32_t(s->connections.size()));  // connection reports (solutions.md §7.7)
        for (const auto& [id, r] : s->connections) o.str(id), o.str(r.dump());
        o.u32(uint32_t(s->patterns.size()));  // pattern instances (regen.md §4.7 item 7)
        for (const auto& [id, list] : s->patterns) {
            o.str(id), o.u32(uint32_t(list.size()));
            for (const auto& [label, m] : list) {
                o.str(label);
                for (double x : m.r) o.f64(x);
                o.p3(m.t);
            }
        }
        const uint32_t idx = intern(std::move(o.b));
        stateIdx_[s.get()] = idx;
        return idx;
    }

    uint32_t eval(const EvalPtr& r) {
        if (auto it = evalIdx_.find(r.get()); it != evalIdx_.end()) return it->second;
        const bool hasState = bool(r->stateOut);
        const uint32_t st = hasState ? state(r->stateOut) : 0;
        Out o;
        o.u8(uint8_t(r->status));
        o.u32(uint32_t(r->messages.size()));
        for (const auto& m : r->messages) o.flag(m.error), o.error(m.e);
        o.flag(hasState), o.u32(st);
        o.i32(shape(r->tool));
        const uint32_t idx = intern(std::move(o.b));
        evalIdx_[r.get()] = idx;
        return idx;
    }

    Result<std::string> finish(const std::vector<std::pair<uint64_t, uint32_t>>& entries) {
        Out o;
        o.u32(uint32_t(shapes_.size()));
        if (shapes_.empty()) {
            o.str({});
        } else {
            auto blob = k_.shapesToBlob(shapes_);
            if (!blob) return blob.error();
            o.str(*blob);
        }
        o.u32(uint32_t(pool_.size()));
        for (const auto& p : pool_) o.str(p);
        o.u32(uint32_t(entries.size()));
        for (const auto& [k, i] : entries) o.u64(k), o.u32(i);
        return std::move(o.b);
    }

private:
    const kernel::Kernel& k_;
    std::vector<kernel::Shape> shapes_;
    std::map<const void*, int32_t> shapeIdx_;
    std::vector<std::string> pool_;
    std::unordered_map<std::string, uint32_t> poolIdx_;
    std::map<const void*, uint32_t> stateIdx_, evalIdx_;
};

// ---------------------------------------------------------------- reading

struct Bad {
    std::string detail;
};

struct In {
    std::string_view b;
    size_t pos = 0;
    void need(size_t n) {
        if (n > b.size() - pos) throw Bad{"truncated"};
    }
    uint8_t u8() {
        need(1);
        return uint8_t(b[pos++]);
    }
    uint32_t u32() {
        need(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(b[pos + size_t(i)])) << (8 * i);
        pos += 4;
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    uint64_t u64() {
        need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(b[pos + size_t(i)])) << (8 * i);
        pos += 8;
        return v;
    }
    double f64() {
        const uint64_t u = u64();
        double v;
        std::memcpy(&v, &u, 8);
        return v;
    }
    bool flag() {
        const uint8_t v = u8();
        if (v > 1) throw Bad{"flag"};
        return v == 1;
    }
    std::string_view view() {
        const uint32_t n = u32();
        need(n);
        std::string_view s = b.substr(pos, n);
        pos += n;
        return s;
    }
    std::string str() { return std::string(view()); }
    // Collection element count: each element takes at least one byte — protection against huge allocations.
    uint32_t count() {
        const uint32_t n = u32();
        if (n > b.size() - pos) throw Bad{"count"};
        return n;
    }
    template <typename E>
    E enumv(int maxValue) {
        const uint8_t v = u8();
        if (v > maxValue) throw Bad{"enum"};
        return E(v);
    }
    Point2 p2() {
        const double x = f64();
        return {x, f64()};
    }
    Vec3 p3() {
        const double x = f64(), y = f64();
        return {x, y, f64()};
    }
    std::vector<std::string> strs() {
        std::vector<std::string> v(count());
        for (auto& s : v) s = str();
        return v;
    }
    std::set<std::string> strSet() {
        std::set<std::string> out;
        const uint32_t n = count();
        for (uint32_t i = 0; i < n; ++i) out.insert(str());
        return out;
    }
    std::vector<int> ints() {
        std::vector<int> v(count());
        for (auto& x : v) x = i32();
        return v;
    }
    Plane plane() {
        Plane p;
        p.origin = p3(), p.x = p3(), p.y = p3(), p.normal = p3();
        return p;
    }
    Error error() {
        Error e(str());
        const uint32_t n = count();
        for (uint32_t i = 0; i < n; ++i) {
            std::string k = str();
            const uint8_t t = u8();
            if (t == 0) e.params[k] = str();
            else if (t == 1) e.params[k] = f64();
            else if (t == 2) e.params[k] = strs();
            else throw Bad{"param type"};
        }
        if (flag()) e.path = str();
        return e;
    }
    std::vector<Error> errors() {
        std::vector<Error> v(count());
        for (auto& e : v) e = error();
        return v;
    }
    void end() {
        if (pos != b.size()) throw Bad{"trailing bytes"};
    }
};

class Decoder {
public:
    std::vector<kernel::Shape> shapes;
    std::vector<std::string_view> pool;

    kernel::Shape shape(int32_t i) {
        if (i == -1) return {};
        if (i < 0 || size_t(i) >= shapes.size()) throw Bad{"shape index"};
        return shapes[size_t(i)];
    }
    static void ref(uint32_t i, uint32_t self) {
        if (i >= self) throw Bad{"pool reference"};
    }

    std::shared_ptr<const naming::NameTable> table(uint32_t i) {
        if (i >= pool.size()) throw Bad{"pool index"};
        if (auto it = tables_.find(i); it != tables_.end()) return it->second;
        In in{pool[i]};
        auto t = std::make_shared<naming::NameTable>();
        t->faces.resize(in.count());
        for (auto& f : t->faces) {
            f.body = in.i32(), f.face = in.i32(), f.base = in.str(), f.full = in.str(), f.nb = in.strSet(), f.key = in.p3();
            f.measure = in.f64();
        }
        t->edges.resize(in.count());
        for (auto& e : t->edges) {
            e.body = in.i32(), e.logical = in.i32(), e.kernelEdges = in.ints(), e.f1 = in.i32(), e.f2 = in.i32(), e.ends = in.strSet();
            e.full = in.str(), e.key = in.p3(), e.measure = in.f64();
        }
        t->vertices.resize(in.count());
        for (auto& v : t->vertices) v.body = in.i32(), v.vertex = in.i32(), v.faces = in.ints(), v.full = in.str(), v.key = in.p3();
        const uint32_t nn = in.count();
        for (uint32_t k = 0; k < nn; ++k) {
            std::string n = in.str();
            naming::EntityRef r;
            r.kind = in.enumv<naming::EntityKind>(2);
            r.index = in.i32();
            t->byName.emplace(std::move(n), r);
        }
        t->warnings = in.errors();
        const uint32_t ns = in.count();
        for (uint32_t k = 0; k < ns; ++k) {
            std::string p = in.str();
            naming::NameTable::Sub sub;
            const bool has = in.flag();
            const uint32_t ti = in.u32();
            if (has) {
                ref(ti, i);
                sub.table = table(ti);
            }
            sub.face0 = in.i32(), sub.edge0 = in.i32(), sub.vertex0 = in.i32();
            t->subs.emplace(std::move(p), std::move(sub));
        }
        in.end();
        tables_[i] = t;
        return t;
    }

    static sketch::Loop loop(In& in) {
        sketch::Loop l;
        l.segments.resize(in.count());
        for (auto& s : l.segments) {
            s.id = in.str(), s.kind = in.enumv<sketch::Segment::Kind>(4), s.p1 = in.p2(), s.p2 = in.p2(), s.center = in.p2();
            s.radius = in.f64(), s.minorRadius = in.f64(), s.angle = in.f64(), s.start = in.p2(), s.end = in.p2();
            s.reversed = in.flag();
        }
        return l;
    }

    const SketchEntry& sketchEntry(uint32_t i) {
        if (i >= pool.size()) throw Bad{"pool index"};
        if (auto it = sketches_.find(i); it != sketches_.end()) return it->second;
        In in{pool[i]};
        SketchEntry e;
        sketch::SketchResult& r = e.result;
        r.status = in.enumv<sketch::SketchStatus>(5);
        const bool hasDof = in.flag();
        const int dof = in.i32();
        if (hasDof) r.dof = dof;
        const uint32_t ne = in.count();
        for (uint32_t k = 0; k < ne; ++k) {
            std::string id = in.str();
            r.entityState[id] = in.enumv<sketch::EntityState>(2);
        }
        r.conflicts.resize(in.count());
        for (auto& c : r.conflicts) c.ids = in.strs(), c.suggested = in.str(), c.undetermined = in.flag();
        r.redundant = in.strs();
        r.unsatisfied = in.strs();
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            r.state.points[id] = in.p2();
        }
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            r.state.radii[id] = in.f64();
        }
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            sketch::SolvedState::EllipseShape el;
            el.rx = in.f64(), el.ry = in.f64(), el.angle = in.f64();
            r.state.ellipses[id] = el;
        }
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            r.reference[id] = in.f64();
        }
        r.errors = in.errors();
        r.messages = in.errors();
        r.profiles.regions.resize(in.count());
        for (auto& g : r.profiles.regions) {
            g.regionId = in.str(), g.depth = in.i32();
            g.outer = loop(in);
            g.holes.resize(in.count());
            for (auto& h : g.holes) h = loop(in);
            g.area = in.f64(), g.interiorPoint = in.p2();
        }
        r.profiles.messages = in.errors();
        e.frame = in.plane();
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            sketch::Projection p;
            p.kind = in.enumv<sketch::Projection::Kind>(3);
            for (double* v : {&p.px, &p.py, &p.x1, &p.y1, &p.x2, &p.y2, &p.cx, &p.cy, &p.r, &p.sx, &p.sy, &p.ex, &p.ey}) *v = in.f64();
            e.projections[id] = p;
        }
        e.curves.resize(in.count());
        for (auto& c : e.curves) {
            c.id = in.str(), c.kind = in.enumv<WorldCurve::Kind>(2), c.a = in.p3(), c.b = in.p3(), c.center = in.p3(), c.axis = in.p3();
            c.radius = in.f64();
        }
        in.end();
        return sketches_.emplace(i, std::move(e)).first->second;
    }

    static sheet::Region region(In& in) {
        auto loopIn = [&] {
            sheet::Loop l(in.count());
            for (auto& g : l) {
                g.kind = in.enumv<sheet::Seg::Kind>(3), g.a = in.p2(), g.b = in.p2(), g.c = in.p2(), g.r = in.f64();
                g.rev = in.flag(), g.label = in.str();
                if (g.kind != sheet::Seg::Kind::Spline) continue;
                spline::Curve& c = g.curve;
                c.degree = in.i32();
                c.poles.resize(in.count());
                for (auto& p : c.poles) p = in.p2();
                c.weights.resize(in.count());
                for (auto& w : c.weights) w = in.f64();
                c.knots.resize(in.count());
                for (auto& k : c.knots) k = in.f64();
                c.mults = in.ints();
            }
            return l;
        };
        sheet::Region r;
        r.outer = loopIn();
        r.holes.resize(in.count());
        for (auto& h : r.holes) h = loopIn();
        return r;
    }

    SheetEntry sheetEntry(In& in) {
        auto m = std::make_shared<sheet::Model>();
        m->params.thickness = in.f64(), m->params.radius = in.f64(), m->params.kFactor = in.f64();
        m->params.relief = in.enumv<sheet::Relief>(2);
        m->params.reliefWidth = in.f64(), m->params.reliefDepth = in.f64();
        for (int k = 0; k < 9; ++k) m->root.linear()(k / 3, k % 3) = in.f64();
        m->root.translation() = in.p3();
        m->pieces.resize(in.count());
        for (auto& p : m->pieces) {
            p.kind = in.enumv<sheet::Piece::Kind>(1), p.feature = in.str(), p.parent = in.i32();
            p.region = region(in);
            p.ops.resize(in.count());
            for (auto& op : p.ops) {
                op.kind = in.enumv<sheet::PieceOp::Kind>(2), op.region = region(in);
                op.minus.resize(in.count());
                for (auto& r : op.minus) r = region(in);
            }
            p.faceA = in.str(), p.faceB = in.str(), p.origin = in.p2(), p.axis = in.p2(), p.out = in.p2();
            for (double* v : {&p.w0, &p.w1, &p.ba, &p.angle, &p.radius}) *v = in.f64();
            p.up = in.flag();
            for (auto* l : {&p.faceIn, &p.faceOut, &p.faceL, &p.faceR, &p.faceS, &p.faceE}) *l = in.str();
        }
        m->cuts.resize(in.count());
        for (auto& c : m->cuts) c.region = region(in), c.feature = in.str(), c.wall = in.i32();
        m->corners.resize(in.count());
        for (auto& c : m->corners) {
            c.piece = in.i32(), c.at = in.p2(), c.kind = in.enumv<sheet::Corner::Kind>(1);
            c.size = in.f64(), c.size2 = in.f64(), c.angle = in.f64(), c.flip = in.flag();
            c.ref = in.p2(), c.label = in.str(), c.edge = in.str(), c.feature = in.str();
        }
        m->flanges.resize(in.count());
        for (auto& r : m->flanges) {
            auto& f = r.spec;
            r.feature = in.str();
            for (double* v : {&f.length, &f.angle, &f.radius, &f.offsetStart, &f.offsetEnd, &f.gap}) *v = in.f64();
            f.position = in.enumv<sheet::Position>(2), f.reverse = in.flag(), f.relief = in.enumv<sheet::Relief>(2);
            r.edges.resize(in.count());
            for (auto& e : r.edges) e.piece = in.i32(), e.onB = in.flag(), e.q1 = in.p2(), e.q2 = in.p2(), e.out = in.p2(), e.name = in.str();
        }
        SheetEntry e;
        e.model = m;
        e.body = in.str(), e.folded = shape(in.i32()), e.flat = shape(in.i32()), e.flatBases = in.strs(), e.overlap = in.f64();
        return e;
    }

    StatePtr state(uint32_t i) {
        if (i >= pool.size()) throw Bad{"pool index"};
        if (auto it = states_.find(i); it != states_.end()) return it->second;
        In in{pool[i]};
        auto s = std::make_shared<PartState>();
        s->bodies.resize(in.count());
        for (auto& b : s->bodies) b.id = in.str(), b.shape = shape(in.i32()), b.bases = in.strs();
        const uint32_t names = in.u32();
        ref(names, i);
        s->names = *table(names);
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            const uint32_t si = in.u32();
            ref(si, i);
            s->sketches[id] = sketchEntry(si);
        }
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            s->planes[id] = in.plane();
        }
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            RefAxis a;
            a.point = in.p3(), a.dir = in.p3();
            s->axes[id] = a;
        }
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            ToolEntry t;
            t.shape = shape(in.i32()), t.bases = in.strs(), t.operation = in.str();
            s->tools[id] = std::move(t);
        }
        s->threads.resize(in.count());
        for (auto& t : s->threads) {
            t.feature = in.str(), t.designation = in.str(), t.internal = in.flag(), t.origin = in.p3(), t.dir = in.p3();
            t.radius = in.f64(), t.d = in.f64(), t.pitch = in.f64(), t.length = in.f64(), t.height = in.f64();
        }
        s->gears.resize(in.count());
        for (auto& g : s->gears) {
            g.feature = in.str(), g.internal = in.flag(), g.sector = in.flag(), g.module = in.f64(), g.shift = in.f64();
            g.teeth = in.i32(), g.formed = in.i32();
            g.accuracy = in.str(), g.origin = in.p3(), g.dir = in.p3(), g.ref = in.p3(), g.width = in.f64(), g.start = in.f64();
            g.angle = in.f64(), g.d = in.f64(), g.da = in.f64(), g.df = in.f64(), g.fill = shape(in.i32());
        }
        const bool hasContext = in.flag();
        const uint32_t ctx = in.u32();
        if (hasContext) {
            ref(ctx, i);
            s->context = state(ctx);
        }
        if (in.flag()) s->contextError = in.error();
        if (in.flag()) s->sheet = sheetEntry(in);
        s->assemblyParts.resize(in.count());
        for (auto& p : s->assemblyParts) {
            p.path = in.str();
            p.element = in.str();
            p.file = in.str();
            for (int r = 0; r < 9; ++r) p.world.R(r / 3, r % 3) = in.f64();
            p.world.t = in.p3();
            const uint32_t li = in.u32();
            ref(li, i);
            p.local = state(li);
        }
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            s->connections[id] = doc::Json::parse(in.str());
        }
        for (uint32_t k = 0, n = in.count(); k < n; ++k) {
            std::string id = in.str();
            auto& list = s->patterns[id];
            list.resize(in.count());
            for (auto& [label, m] : list) {
                label = in.str();
                for (double& x : m.r) x = in.f64();
                m.t = in.p3();
            }
        }
        in.end();
        states_[i] = s;
        return s;
    }

    EvalPtr eval(uint32_t i) {
        if (i >= pool.size()) throw Bad{"pool index"};
        if (auto it = evals_.find(i); it != evals_.end()) return it->second;
        In in{pool[i]};
        auto r = std::make_shared<EvalResult>();
        r->status = in.enumv<FeatureStatus>(4);
        r->messages.resize(in.count());
        for (auto& m : r->messages) m.error = in.flag(), m.e = in.error();
        const bool hasState = in.flag();
        const uint32_t st = in.u32();
        if (hasState) {
            ref(st, i);
            r->stateOut = state(st);
        }
        r->tool = shape(in.i32());
        in.end();
        evals_[i] = r;
        return r;
    }

private:
    std::map<uint32_t, std::shared_ptr<const naming::NameTable>> tables_;
    std::map<uint32_t, SketchEntry> sketches_;
    std::map<uint32_t, StatePtr> states_;
    std::map<uint32_t, EvalPtr> evals_;
};

}  // namespace

Result<std::string> writeCacheEntries(const kernel::Kernel& k, const CacheEntries& entries) {
    Encoder enc(k);
    std::vector<std::pair<uint64_t, uint32_t>> list;
    for (const auto& [key, r] : entries)
        if (r) list.push_back({key, enc.eval(r)});
    return enc.finish(list);
}

Result<CacheEntries> readCacheEntries(const kernel::Kernel& k, std::string_view bytes) {
    try {
        In in{bytes};
        Decoder dec;
        const uint32_t nShapes = in.u32();
        const std::string_view blob = in.view();
        if (nShapes > 0) {
            auto shapes = k.shapesFromBlob(std::string(blob));
            if (!shapes) return Error("REGEN_CACHE_INVALID").with("detail", "shapes: " + shapes.error().code);
            if (shapes->size() != nShapes) return Error("REGEN_CACHE_INVALID").with("detail", std::string("shape count"));
            dec.shapes = std::move(*shapes);
        }
        dec.pool.resize(in.count());
        for (auto& p : dec.pool) p = in.view();
        CacheEntries out(in.count());
        for (auto& [key, r] : out) {
            key = in.u64();
            r = dec.eval(in.u32());
        }
        in.end();
        return out;
    } catch (const Bad& b) {
        return Error("REGEN_CACHE_INVALID").with("detail", b.detail);
    }
}

}  // namespace construe::regen
