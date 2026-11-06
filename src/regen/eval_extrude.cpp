// Extrude and revolve (spec/regen.md §4.2, §4.3).
#include "regen/eval_common.h"

namespace construe::regen {

using namespace detail;

// ---------------------------------------------------------------- "up to" end (§4.2а, S7F-FEAT-001)

namespace {

constexpr double UP_TO_TOL = 1e-6;          // mm: a face lies on the sketch plane
constexpr double UP_TO_ANGLE_TOL = 1e-7;    // sine of an angle: parallelism
constexpr double UP_TO_PLANAR_TOL = 1e-4;   // mm: points of a curve lie in one plane
constexpr double UP_TO_CURVE_DEFLECTION = 0.01;  // mm: polyline of a curve edge

// The end target (§4.2а item 1): a plane {q, m} or a surface — the solids s with face bases.
struct UpToTarget {
    bool plane = false;
    Point3 q = Point3::Zero();
    Vec3 m = Vec3::UnitZ();
    kernel::Shape surface;
    std::vector<std::string> bases;
};

struct RefLine {
    Point3 p;
    Vec3 u;
};

std::optional<UpToTarget> planeTarget(const Point3& q, const Vec3& m) {
    UpToTarget t;
    t.plane = true;
    t.q = q;
    t.m = m.normalized();
    return t;
}

// Straight edge of an UpToRef (§4.2а item 1): nothing (with an error) — not straight.
std::optional<RefLine> upToLine(Ctx& c, const PartState& in, const std::string& name, const std::string& sub) {
    auto e = resolveOne(c, in, name, naming::EntityKind::Edge, sub + "/edge");
    if (!e) return std::nullopt;
    const auto& ee = in.names.edges.at(e->index);
    const auto cv = c.k.edgeCurve(in.bodies.at(ee.body).shape, ee.logical);
    if (cv.kind != kernel::CurveInfo::Kind::Line || (cv.p2 - cv.p1).norm() <= 0) {
        c.error(refGeometry("to", "edge not straight"), sub + "/edge");
        return std::nullopt;
    }
    return RefLine{cv.p1, (cv.p2 - cv.p1).normalized()};
}

// Plane of a planar curve edge (§4.2а item 1): circle — {center, axis}; another curve — by its polyline points.
std::optional<UpToTarget> curvePlane(Ctx& c, const PartState& in, const naming::EdgeEntry& ee, const std::string& sub) {
    const kernel::Shape& sh = in.bodies.at(ee.body).shape;
    const auto cv = c.k.edgeCurve(sh, ee.logical);
    if (cv.kind == kernel::CurveInfo::Kind::Circle) return planeTarget(cv.center, cv.axis);
    kernel::CurveSet q;
    q.edge = ee.logical;
    auto lines = c.k.curvePoints(sh, q, UP_TO_CURVE_DEFLECTION);
    std::vector<Point3> pts;
    if (lines)
        for (const auto& l : *lines) pts.insert(pts.end(), l.begin(), l.end());
    auto notPlanar = [&] {
        c.error(refGeometry("to", "curve not planar"), sub + "/edge");
        return std::nullopt;
    };
    if (pts.size() < 3) return notPlanar();
    const Point3 p0 = pts.front();
    Point3 p1 = p0, p2 = p0;
    for (const auto& p : pts)
        if ((p - p0).norm() > (p1 - p0).norm()) p1 = p;
    if ((p1 - p0).norm() <= UP_TO_PLANAR_TOL) return notPlanar();
    const Vec3 u = (p1 - p0).normalized();
    auto offLine = [&](const Point3& p) { return ((p - p0) - (p - p0).dot(u) * u).norm(); };
    for (const auto& p : pts)
        if (offLine(p) > offLine(p2)) p2 = p;
    const Vec3 m = (p1 - p0).cross(p2 - p0);
    if (offLine(p2) <= UP_TO_PLANAR_TOL || m.norm() <= 0) return notPlanar();
    const Vec3 mn = m.normalized();
    for (const auto& p : pts)
        if (std::abs((p - p0).dot(mn)) > UP_TO_PLANAR_TOL) return notPlanar();
    return planeTarget(p0, mn);
}

std::optional<UpToTarget> upToTarget(Ctx& c, const PartState& in, const doc::ExtrudeData& x, const Vec3& n,
                                     const std::vector<kernel::Shape>& bounds) {
    if (x.endType == "upToNext") {
        if (bounds.empty()) {
            c.error(Error("REGEN_UP_TO_NO_BODIES"), "/end");
            return std::nullopt;
        }
        // All bodies of the bound with their face bases (Combine keeps the bases, naming.md §4.1).
        std::vector<naming::InputNames> ins;
        std::vector<kernel::Shape> shapes;
        for (const auto& b : in.bodies) {
            shapes.push_back(b.shape);
            ins.push_back(inputOf(b.bases));
        }
        UpToTarget t;
        if (shapes.size() != bounds.size()) {  // assembly: the bound is not the body list — no bases
            t.surface = c.k.combine(bounds).shape;
            return t;
        }
        const kernel::OpResult comb = c.k.combine(shapes);
        std::vector<Error> nw;
        t.bases = naming::assignBases(naming::OpKind::Combine, c.f.id, comb, ins, &nw);
        t.surface = comb.shape;
        return t;
    }
    const doc::Json& to = x.upTo;
    auto geom = [&](const std::string& detail, const std::string& sub) {
        c.error(refGeometry("to", detail), sub);
        return std::nullopt;
    };
    if (!to.is_array() || to.empty() || to.size() > 2) return geom("refs do not define a plane", "/end/to");
    if (to.size() == 1) {
        const doc::Json& r = to[0];
        const std::string sub = "/end/to/0";
        if (r.contains("face")) {
            auto f = resolveOne(c, in, r["face"].get<std::string>(), naming::EntityKind::Face, sub + "/face");
            if (!f) return std::nullopt;
            const auto& fe = in.names.faces.at(f->index);
            const auto& body = in.bodies.at(fe.body);
            const auto si = c.k.faceSurface(body.shape, fe.face);
            if (si.kind == kernel::SurfaceInfo::Kind::Plane) return planeTarget(si.point, si.dir);
            UpToTarget t;  // a curved surface: the first face of its body along the direction
            t.surface = body.shape;
            t.bases = body.bases;
            return t;
        }
        if (r.contains("feature") || (r.contains("datum") && r["datum"] != "origin")) {
            auto pl = refPlane(c, in, r, sub);
            if (!pl) return std::nullopt;
            return planeTarget(pl->origin, pl->normal);
        }
        if (r.contains("vertex") || r.contains("datum")) {
            auto p = refPoint(c, in, r, sub);
            if (!p) return std::nullopt;
            return planeTarget(*p, n);
        }
        if (r.contains("edge")) {
            auto e = resolveOne(c, in, r["edge"].get<std::string>(), naming::EntityKind::Edge, sub + "/edge");
            if (!e) return std::nullopt;
            const auto& ee = in.names.edges.at(e->index);
            const auto cv = c.k.edgeCurve(in.bodies.at(ee.body).shape, ee.logical);
            if (cv.kind != kernel::CurveInfo::Kind::Line) return curvePlane(c, in, ee, sub);
            const Vec3 u = cv.p2 - cv.p1;
            if (u.norm() <= 0 || std::abs(u.normalized().dot(n)) > UP_TO_ANGLE_TOL) return geom("edge not parallel to sketch", sub + "/edge");
            return planeTarget(cv.p1, n);
        }
        return geom("refs do not define a plane", sub);
    }
    // Two references: two straight edges or an edge and a vertex.
    std::vector<RefLine> lines;
    std::vector<Point3> points;
    for (size_t i = 0; i < 2; ++i) {
        const doc::Json& r = to[i];
        const std::string sub = "/end/to/" + std::to_string(i);
        if (r.contains("edge")) {
            auto l = upToLine(c, in, r["edge"].get<std::string>(), sub);
            if (!l) return std::nullopt;
            lines.push_back(*l);
        } else if (r.contains("vertex") || (r.contains("datum") && r["datum"] == "origin")) {
            auto p = refPoint(c, in, r, sub);
            if (!p) return std::nullopt;
            points.push_back(*p);
        } else {
            return geom("refs do not define a plane", sub);
        }
    }
    if (lines.empty()) return geom("refs do not define a plane", "/end/to");
    const RefLine& a = lines[0];
    Vec3 m;
    if (lines.size() == 2) {
        const RefLine& b = lines[1];
        m = a.u.cross(b.u);
        if (m.norm() <= UP_TO_ANGLE_TOL) m = a.u.cross(b.p - a.p);  // parallel edges
        else if (std::abs((b.p - a.p).dot(m.normalized())) > UP_TO_TOL) return geom("refs do not define a plane", "/end/to");  // skew
    } else {
        m = a.u.cross(points[0] - a.p);
    }
    if (m.norm() <= UP_TO_ANGLE_TOL) return geom("refs do not define a plane", "/end/to");
    return planeTarget(a.p, m);
}

// Start components of a boolean result (§4.2а item 4): those with a planar face on the sketch plane; combined into a
// tool with the bases carried over. endDepth > 0 — a start component that reaches the prism end (a face parallel to the
// sketch at endDepth) gives REGEN_UP_TO_NOT_REACHED.
bool startComponents(Ctx& c, const kernel::OpResult& r, const std::vector<std::string>& bases, const Plane& F, const Vec3& d,
                     double endDepth, ToolBuild& out, bool& reachedEnd) {
    const Vec3 n = F.normal.normalized();
    std::vector<kernel::Shape> shapes;
    std::vector<naming::InputNames> ins;
    reachedEnd = false;
    for (const auto& b : split(c.k, r.shape, bases)) {
        bool start = false, end = false;
        const int nf = c.k.counts(b.shape).faces;
        for (int f = 0; f < nf; ++f) {
            const auto si = c.k.faceSurface(b.shape, f);
            if (si.kind != kernel::SurfaceInfo::Kind::Plane || si.dir.normalized().cross(n).norm() > UP_TO_ANGLE_TOL) continue;
            const double h = (si.point - F.origin).dot(d);
            if (std::abs(h) <= UP_TO_TOL) start = true;
            if (endDepth > 0 && std::abs(h - endDepth) <= UP_TO_TOL) end = true;
        }
        if (!start) continue;
        reachedEnd = reachedEnd || end;
        shapes.push_back(b.shape);
        ins.push_back(inputOf(b.bases));
    }
    if (shapes.empty()) return false;
    if (shapes.size() == 1) {
        out.shape = shapes[0];
        out.bases = ins[0].bases;
    } else {
        const kernel::OpResult comb = c.k.combine(shapes);
        std::vector<Error> nw;
        out.bases = naming::assignBases(naming::OpKind::Combine, c.f.id, comb, ins, &nw);
        out.shape = comb.shape;
    }
    out.ok = !out.shape.null();
    return true;
}

// Boolean of the prism with the target and its face bases (Boolean history, naming.md §4.1).
std::optional<kernel::OpResult> upToBoolean(Ctx& c, kernel::BoolKind kind, const ToolBuild& prism, const kernel::Shape& target,
                                            const std::vector<std::string>& targetBases, std::vector<std::string>& bases) {
    auto r = c.k.boolean(kind, prism.shape, target, nullptr);
    if (!r) {
        c.kernelError(r.error());
        return std::nullopt;
    }
    c.kernelWarnings(r->warnings);
    std::vector<Error> nw;
    bases = naming::assignBases(naming::OpKind::Boolean, c.f.id, *r, {inputOf(prism.bases), inputOf(targetBases)}, &nw);
    for (auto& w : nw) c.warning(w);
    return *r;
}

}  // namespace

// The tool of an "up to" extrusion (§4.2а): false — an error was reported (or cancelled).
template <class Build>
static bool upToTool(Ctx& c, const PartState& in, const PartState& st, const doc::ExtrudeData& x, const Plane& F, Build build,
                     ToolBuild& tool) {
    const Vec3 n = F.normal.normalized();
    const Vec3 d = x.reverse ? Vec3(-n) : n;
    const std::vector<kernel::Shape> bounds = operationBounds(c, st);
    auto target = upToTarget(c, in, x, n, bounds);
    if (!target) return false;
    double offset = 0;
    if (x.endType == "upTo" && !x.upToOffset.empty()) {
        const auto o = c.field(x.upToOffset, expr::LENGTH, "/end/offset");
        if (!o) return false;
        offset = *o;
    }
    auto prismOf = [&](double L) {
        kernel::Extent e;
        e.kind = kernel::Extent::Kind::Blind;
        e.depth = L;
        return build(e);
    };
    auto behind = [&] {
        c.error(Error("REGEN_UP_TO_BEHIND"), "/end");
        return false;
    };
    bool reachedEnd = false;
    if (target->plane) {
        const Vec3 m = target->m;
        const double md = m.dot(d);
        if (std::abs(md) <= UP_TO_ANGLE_TOL) {
            c.error(refGeometry("to", "plane parallel to direction"), "/end/to");
            return false;
        }
        const Vec3 mp = md > 0 ? m : Vec3(-m);
        const Point3 q = target->q - offset * mp;
        if (m.cross(n).norm() <= UP_TO_ANGLE_TOL) {  // parallel to the sketch: an ordinary depth
            const double D = (q - F.origin).dot(d);
            if (D <= 0) return behind();
            tool = prismOf(D);
            return tool.ok;
        }
        // Inclined plane: a prism past it, minus the half-space beyond it.
        const ToolBuild probe = prismOf(1.0);
        if (!probe.ok) return false;
        const BBox3 pb = c.k.bbox(probe.shape);
        double tMax = -1e300;
        for (int k = 0; k < 8; ++k) {
            const Point3 ck = pb.corner(static_cast<BBox3::CornerType>(k));
            tMax = std::max(tMax, (q - ck).dot(m) / d.dot(m));
        }
        if (tMax <= 0) return behind();
        const double L = tMax + std::max(1.0, 0.1 * tMax);
        const ToolBuild prism = prismOf(L);
        if (!prism.ok) return false;
        const BBox3 b = c.k.bbox(prism.shape);
        const double R = 2 * (b.max() - b.min()).norm() + 1;  // covers the prism around its center
        const Point3 bc = b.center();
        Plane hp;
        hp.normal = mp;
        hp.origin = bc - (bc - q).dot(mp) * mp;  // the prism center projected onto the plane
        hp.x = (std::abs(mp.x()) < 0.9 ? Vec3::UnitX() : Vec3::UnitY()).cross(mp).normalized();
        hp.y = mp.cross(hp.x).normalized();
        kernel::Profile hprof;
        hprof.plane = hp;
        const Point2 sq[4] = {{-R, -R}, {R, -R}, {R, R}, {-R, R}};
        for (int i = 0; i < 4; ++i) {
            kernel::Segment3 s;
            s.id = "upto" + std::to_string(i);
            s.kind = kernel::Segment3::Kind::Line;
            s.p1 = hp.toWorld(sq[i]);
            s.p2 = hp.toWorld(sq[(i + 1) % 4]);
            hprof.outer.segments.push_back(s);
        }
        kernel::Extent he;
        he.kind = kernel::Extent::Kind::Blind;
        he.depth = 2 * R;
        auto h = c.k.extrude(hprof, he, false);
        if (!h) {
            c.kernelError(h.error());
            return false;
        }
        // Bases of the half-space (§4.2а item 2): its face on the plane replaces the prism end cap.
        std::vector<std::string> hb;
        for (const auto& origins : h->history.faceOrigins) {
            bool cap = false;
            for (const auto& o : origins) cap = cap || (o.kind == kernel::Origin::Kind::Profile && o.role == kernel::ProfileRole::CapStart);
            hb.push_back(c.f.id + (cap ? ".cap(end)" : ".side(upto)"));
        }
        std::vector<std::string> rb;
        auto r = upToBoolean(c, kernel::BoolKind::Subtract, prism, h->shape, hb, rb);
        if (!r) return false;
        if (!startComponents(c, *r, rb, F, d, 0, tool, reachedEnd)) return behind();
        return true;
    }
    // A surface (§4.2а item 3): the first face of the target bodies along the direction.
    if (offset != 0) {
        c.error(Error("REGEN_BAD_VALUE").with("name", std::string("end.offset")).with("actual", offset), "/end/offset");
        return false;
    }
    const double L = kernel::throughAllDepth(c.k, target->surface, F.origin, d);
    const ToolBuild prism = prismOf(L);
    if (!prism.ok) return false;
    std::vector<std::string> rb;
    auto r = upToBoolean(c, kernel::BoolKind::Subtract, prism, target->surface, target->bases, rb);
    if (!r) return false;
    if (!startComponents(c, *r, rb, F, d, L, tool, reachedEnd)) {
        r = upToBoolean(c, kernel::BoolKind::Intersect, prism, target->surface, target->bases, rb);  // the sketch inside material
        if (!r) return false;
        if (!startComponents(c, *r, rb, F, d, L, tool, reachedEnd)) return behind();
    }
    if (reachedEnd) {
        c.error(Error("REGEN_UP_TO_NOT_REACHED"), "/end");
        return false;
    }
    return true;
}

EvalPtr Regenerator::evalExtrude(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut, const doc::ExtrudeData* x) {
    [[maybe_unused]] const doc::Feature& f = c.f;
    auto sketchOf = [&](const std::string& id) { return detail::sketchOf(c, *input, id); };
    auto selectRegions = [&](const SketchEntry& se, const doc::Profiles& p, const std::string& sketchId) {
        return detail::selectRegions(c, se, p, sketchId);
    };
    const SketchEntry* se = sketchOf(x->sketch);
    if (!se) return finish(c, input);
    const auto regions = selectRegions(*se, x->profiles, x->sketch);
    if (regions.empty()) return finish(c, input);
    // Sheet part (§4.16 item 5): the extrusion goes into the sheet model when it is a cut through walls or a tab.
    const bool upTo = x->endType == "upTo" || x->endType == "upToNext";  // §4.2а: never into the sheet model
    const bool sheetPart = !upTo && input->sheet && !input->sheet->body.empty() && x->targets.empty() && st->assemblyParts.empty();
    if (sheetPart) {
        bool handled = false;
        auto r = evalSheetExtrude(c, input, st, cancelledOut, x, *se, regions, handled);
        if (handled || cancelledOut) return r;
    }
    auto makeTool = [&](const kernel::Extent& e) {
        return buildTool(c, regions, [&](const sketch::Region& r) { return k_.extrude(toKernelProfile(r, se->frame), e, x->reverse); });
    };
    ToolBuild tool;
    kernel::Extent ext;
    if (upTo) {
        if (!upToTool(c, *input, *st, *x, se->frame, makeTool, tool)) {
            if (c.cancelled) {
                cancelledOut = true;
                return nullptr;
            }
            return finish(c, input, tool.shape);
        }
    } else if (x->endType == "throughAllBoth" && x->operation != "cut" && x->operation != "intersect") {
        // §4.2 item 3: both directions — cut only.
        c.error(Error("REGEN_BAD_VALUE").with("name", std::string("end.type")), "/end/type");
        return finish(c, input);
    }
    if (x->endType == "throughAll" || x->endType == "throughAllBoth") {
        // §4.2 item 3: bound — all input bodies (assembly: bodies of the targets, assembly.md §7г.3).
        const std::vector<kernel::Shape> shapes = operationBounds(c, *st);
        if (shapes.empty()) {
            c.error(Error("REGEN_THROUGH_ALL_NO_BODIES"), "/end");
            return finish(c, input);
        }
        ext.kind = x->endType == "throughAll" ? kernel::Extent::Kind::ThroughAll : kernel::Extent::Kind::ThroughAllBoth;
        ext.bounds = k_.combine(shapes).shape;
    } else {
        const auto depth = c.field(x->depth, expr::LENGTH, "/end/depth");
        if (!depth) return finish(c, input);
        if (*depth <= 0) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("end.depth")).with("actual", *depth), "/end/depth");
            return finish(c, input);
        }
        ext.kind = x->endType == "symmetric" ? kernel::Extent::Kind::Symmetric : kernel::Extent::Kind::Blind;
        ext.depth = *depth;
    }
    if (!upTo) tool = makeTool(ext);
    if (c.cancelled) {
        cancelledOut = true;
        return nullptr;
    }
    if (!tool.ok || !applyOperation(c, *st, x->operation, tool)) {
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input, tool.shape);
    }
    if (!c.hasError()) st->tools[f.id] = {tool.shape, tool.bases, x->operation == "new" ? "add" : x->operation};  // §4.2 item 7
    // An ordinary extrusion of the sheet body: the flat pattern no longer follows the part (§4.16 item 5).
    if (sheetPart && !c.hasError())
        for (const auto& b : input->bodies)
            if (b.id == input->sheet->body && b.shape == input->sheet->folded) c.warning(Error("SHEET_NOT_IN_FLAT").with("id", f.id));
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st, tool.shape);
}

EvalPtr Regenerator::evalRevolve(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut, const doc::RevolveData* rv) {
    [[maybe_unused]] const doc::Feature& f = c.f;
    [[maybe_unused]] const doc::Document& d = c.d;
    auto sketchOf = [&](const std::string& id) { return detail::sketchOf(c, *input, id); };
    auto selectRegions = [&](const SketchEntry& se, const doc::Profiles& p, const std::string& sketchId) {
        return detail::selectRegions(c, se, p, sketchId);
    };
    const SketchEntry* se = sketchOf(rv->sketch);
    if (!se) return finish(c, input);
    kernel::Axis axis;
    if (!rv->axisFeature.empty()) {
        auto it = input->axes.find(rv->axisFeature);
        if (it == input->axes.end()) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", rv->axisFeature), "/axis/feature");
            return finish(c, input);
        }
        axis.origin = it->second.point;
        axis.direction = it->second.dir;
    } else if (!rv->axisDatum.empty()) {
        axis.origin = Point3::Zero();
        axis.direction = sketch::datumAxis(rv->axisDatum).value_or(Vec3::UnitZ());
    } else {
        const doc::Feature* skf = nullptr;
        for (const auto& ff : d.active().features)
            if (ff->id == rv->sketch) skf = ff.get();
        const sketch::Entity* line = skf && skf->sketchData() ? skf->sketchData()->findEntity(rv->axisSketchLine) : nullptr;
        if (!line) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", rv->axisSketchLine), "/axis/sketchLine");
            return finish(c, input);
        }
        const auto& pts = se->result.state.points;
        const Point3 a = se->frame.toWorld(pts.at(line->p1)), b = se->frame.toWorld(pts.at(line->p2));
        axis.origin = a;
        axis.direction = (b - a).norm() > 0 ? Vec3((b - a).normalized()) : Vec3::Zero();
    }
    const auto regions = selectRegions(*se, rv->profiles, rv->sketch);
    if (regions.empty()) return finish(c, input);
    kernel::RevolveExtent ext;
    if (rv->angleType == "full") {
        ext.kind = kernel::RevolveExtent::Kind::Full;
    } else {
        const auto a = c.field(rv->angleValue, expr::ANGLE, "/angle/value");
        if (!a) return finish(c, input);
        if (*a <= 0 || *a > 2 * PI) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("angle.value")).with("actual", *a), "/angle/value");
            return finish(c, input);
        }
        ext.kind = rv->angleType == "symmetric" ? kernel::RevolveExtent::Kind::Symmetric : kernel::RevolveExtent::Kind::Angle;
        ext.value = *a;
    }
    const ToolBuild tool = buildTool(c, regions, [&](const sketch::Region& r) {
        return k_.revolve(toKernelProfile(r, se->frame), axis, ext);
    });
    if (c.cancelled) {
        cancelledOut = true;
        return nullptr;
    }
    if (!tool.ok || !applyOperation(c, *st, rv->operation, tool)) {
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input, tool.shape);
    }
    if (!c.hasError()) st->tools[f.id] = {tool.shape, tool.bases, rv->operation == "new" ? "add" : rv->operation};  // §4.2 item 7
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st, tool.shape);
}

}  // namespace construe::regen
