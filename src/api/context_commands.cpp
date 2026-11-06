// Designing a part in the context of an assembly (spec/assembly.md §7в, core-api.md §4.1в, §5; S2-ASM-011).
#include "api/session.h"

#include "api/errors.h"
#include "base/number_format.h"
#include "regen/naming.h"
#include "regen/regen.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace construe::api {

Result<Json> Session::insertPartInContext() {
    if (auto s = checkNoDrag(); !s) return s.error();
    if (!doc().active().isAssembly()) return Error("API_NOT_ASSEMBLY").with("id", doc().active().id);
    syncAssemblyPositions();
    doc::Document nd = doc();
    const std::string asmId = nd.active().id;
    doc::Element p;
    p.id = doc::allocate(nd, doc::IdKind::Element);
    p.name = doc::defaultElementName(nd, "part");
    doc::Instance in;
    in.id = doc::allocate(nd, doc::IdKind::Instance);
    in.name = p.name + " <1>";
    in.refElement = p.id;
    in.fixed = true;
    p.context = doc::PartContext{asmId, in.id, std::nullopt};
    const std::string pid = p.id, iid = in.id;
    nd.elements.push_back(std::move(p));
    nd.elements[size_t(nd.elementIndex(asmId))].instances.push_back(in);
    if (auto s = commit(std::move(nd), "undo.insertPartInContext", {}, pid); !s) return s.error();
    return Json{{"element", pid}, {"instance", iid}};
}

Status Session::setPartContext(const std::string& part, const Json& context) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    const int pi = nd.elementIndex(part);
    if (pi < 0 || nd.elements[size_t(pi)].isAssembly()) return notFound(part);
    if (!context.is_object() || !context.contains("assembly") || !context["assembly"].is_string() || !context.contains("instance") ||
        !context["instance"].is_string())
        return badArg("context");
    const std::string a = context["assembly"], i = context["instance"];
    const int ai = nd.elementIndex(a);
    if (ai < 0 || !nd.elements[size_t(ai)].isAssembly()) return badArg("context.assembly");
    const auto& ins = nd.elements[size_t(ai)].instances;
    const auto it = std::find_if(ins.begin(), ins.end(), [&](const doc::Instance& x) { return x.id == i; });
    if (it == ins.end() || it->refElement != part || !it->refFile.empty()) return badArg("context.instance");
    doc::Element& p = nd.elements[size_t(pi)];
    if (p.context && p.context->assembly == a && p.context->instance == i) return okStatus();
    if (p.context && !regen::contextRefs(p).empty()) return Error("API_CONTEXT_IN_USE").with("id", part);
    p.context = doc::PartContext{a, i, std::nullopt};
    // Entering the context of a part leaves the context of the others (S7F-ASM-005, assembly.md §7в.6): the other parts
    // of this assembly whose context holds no references and is not frozen lose the field.
    for (auto& e : nd.elements)
        if (e.id != part && e.context && e.context->assembly == a && !e.context->frozen && regen::contextRefs(e).empty()) e.context.reset();
    return commit(std::move(nd), "undo.partContext", {}, part);
}

Status Session::freezeContext(const std::string& part, bool frozen) {
    if (auto s = checkNoDrag(); !s) return s;
    syncAssemblyPositions();
    doc::Document nd = doc();
    const int pi = nd.elementIndex(part);
    if (pi < 0 || nd.elements[size_t(pi)].isAssembly()) return notFound(part);
    doc::Element& p = nd.elements[size_t(pi)];
    if (!p.context) return Error("API_NO_CONTEXT").with("id", part);
    if (!frozen) {
        p.context->frozen.reset();
        return commit(std::move(nd), "undo.freezeContext", {}, part);
    }
    if (p.context->frozen) return okStatus();
    // Snapshot of used instances (§7в.5): part description and position relative to the context instance.
    std::vector<doc::FrozenInstance> snap;
    const int ai = nd.elementIndex(p.context->assembly);
    if (ai >= 0 && nd.elements[size_t(ai)].isAssembly()) {
        const auto& ins = nd.elements[size_t(ai)].instances;
        const auto ctxIt = std::find_if(ins.begin(), ins.end(), [&](const doc::Instance& x) { return x.id == p.context->instance; });
        if (ctxIt != ins.end()) {
            const regen::Motion3 mc = regen::motionOf(ctxIt->position, ctxIt->rotation);
            const regen::Motion3 inv{mc.R.transpose(), -(mc.R.transpose() * mc.t)};
            for (const auto& path : regen::usedContextPaths(p)) {
                const auto it = std::find_if(ins.begin(), ins.end(), [&](const doc::Instance& x) { return x.id == path; });
                if (it == ins.end() || !it->refFile.empty()) continue;
                const int ei = nd.elementIndex(it->refElement);
                if (ei < 0 || nd.elements[size_t(ei)].isAssembly()) continue;
                doc::Element e = nd.elements[size_t(ei)];
                e.context.reset();
                const regen::Motion3 M = inv * regen::motionOf(it->position, it->rotation);
                const Eigen::Quaterniond q(M.R);
                doc::FrozenInstance fi;
                fi.instance = path;
                fi.position = {M.t.x(), M.t.y(), M.t.z()};
                fi.rotation = {q.w(), q.x(), q.y(), q.z()};
                fi.element = doc::elementToJsonValue(e);
                snap.push_back(std::move(fi));
            }
        }
    }
    p.context->frozen = std::move(snap);
    return commit(std::move(nd), "undo.freezeContext", {}, part);
}



Status Session::breakContext(const std::string& part) {
    if (auto s = checkNoDrag(); !s) return s;
    doc::Document nd = doc();
    const int pi = nd.elementIndex(part);
    if (pi < 0 || nd.elements[size_t(pi)].isAssembly()) return notFound(part);
    if (!nd.elements[size_t(pi)].context) return Error("API_NO_CONTEXT").with("id", part);
    // Current reference geometry - from the regeneration result of this part (active or evaluated separately).
    std::optional<regen::PartResult> own;
    PartResultPtr pr;
    if (doc().activeElement == part) pr = regenResult();
    if (!pr) {
        doc::Document pd = doc();
        pd.activeElement = part;
        regen::Regenerator rg(k_);
        own = rg.regenerate(pd, 0);
        if (!own) return Error("API_STATE_UNAVAILABLE");
    }
    const regen::PartResult& res = pr ? *pr : *own;
    doc::Element& p = nd.elements[size_t(pi)];
    auto hasSlash = [](const std::string& s) { return naming::hasPath(s); };
    for (auto& fp : p.features) {
        const auto* sd0 = fp->sketchData();
        if (!sd0) continue;
        // State after the sketch: system and projections (regen.md §4.1).
        regen::StatePtr after;
        if (auto a = res.after.find(fp->id); a != res.after.end()) after = a->second;
        const regen::SketchEntry* se = nullptr;
        if (after)
            if (auto it = after->sketches.find(fp->id); it != after->sketches.end()) se = &it->second;
        if (!se) continue;  // sketch not evaluated - references stay (lost)
        auto f = std::make_shared<doc::Feature>(*fp);
        auto* sd = std::get_if<sketch::SketchData>(&f->data);
        if (hasSlash(sd->plane.face)) {
            const Plane& fr = se->frame;
            sd->plane.face.clear();
            sd->plane.frame = std::array<double, 9>{fr.origin.x(), fr.origin.y(), fr.origin.z(), fr.x.x(), fr.x.y(), fr.x.z(),
                                                    fr.y.x(), fr.y.y(), fr.y.z()};
        }
        std::vector<sketch::Entity> extra;
        std::vector<sketch::Constraint> cons;
        auto newPoint = [&](double x, double y, bool constr) {
            sketch::Entity pt;
            pt.id = doc::allocate(nd, doc::IdKind::Entity);
            pt.type = sketch::EntityType::Point;
            pt.x = x, pt.y = y, pt.construction = constr;
            extra.push_back(pt);
            return pt.id;
        };
        auto fixPoint = [&](const std::string& id, double x, double y) {
            sketch::Constraint c;
            c.id = doc::allocate(nd, doc::IdKind::Constraint);
            c.type = "fix";
            c.refs = {id};
            c.target = {x, y};
            cons.push_back(c);
        };
        for (auto& e : sd->entities) {
            if (e.type != sketch::EntityType::Projected) continue;
            const std::string src = sketch::projectionSourceRef(e.source);
            if (!hasSlash(src)) continue;
            const auto g = se->projections.find(e.id);
            if (g == se->projections.end()) continue;  // not resolvable now - left unchanged
            const sketch::Projection& q = g->second;
            const bool constr = e.construction;
            e.source.clear();
            switch (q.kind) {
                case sketch::Projection::Kind::Point:
                    e.type = sketch::EntityType::Point;
                    e.x = q.px, e.y = q.py;
                    fixPoint(e.id, q.px, q.py);
                    break;
                case sketch::Projection::Kind::Line: {
                    e.type = sketch::EntityType::Line;
                    e.p1 = newPoint(q.x1, q.y1, constr), e.p2 = newPoint(q.x2, q.y2, constr);
                    fixPoint(e.p1, q.x1, q.y1);
                    fixPoint(e.p2, q.x2, q.y2);
                    break;
                }
                case sketch::Projection::Kind::Circle: {
                    e.type = sketch::EntityType::Circle;
                    e.center = newPoint(q.cx, q.cy, constr);
                    e.r = q.r;
                    sketch::Constraint c;
                    c.id = doc::allocate(nd, doc::IdKind::Constraint);
                    c.type = "fix";
                    c.refs = {e.id};
                    c.target = {q.cx, q.cy, q.r};
                    cons.push_back(c);
                    break;
                }
                case sketch::Projection::Kind::Arc: {
                    e.type = sketch::EntityType::Arc;
                    e.center = newPoint(q.cx, q.cy, constr);
                    e.start = newPoint(q.sx, q.sy, constr);
                    e.end = newPoint(q.ex, q.ey, constr);
                    fixPoint(e.center, q.cx, q.cy);
                    fixPoint(e.start, q.sx, q.sy);
                    sketch::Constraint c;  // chord - a driving dimension (§7в.5)
                    c.id = doc::allocate(nd, doc::IdKind::Constraint);
                    c.type = "distance";
                    c.refs = {e.start, e.end};
                    c.value = roundTrip(std::hypot(q.ex - q.sx, q.ey - q.sy)) + " mm";
                    c.driving = true;
                    cons.push_back(c);
                    break;
                }
            }
        }
        // New points - before the entities that reference them.
        sd->entities.insert(sd->entities.begin(), extra.begin(), extra.end());
        for (auto& c : cons) sd->constraints.push_back(std::move(c));
        fp = f;
    }
    p.context.reset();
    return commit(std::move(nd), "undo.breakContext", {}, part);
}

Result<std::optional<Session::ContextInfo>> Session::contextInfo(const std::string& part) const {
    const auto info = document();
    const int pi = info.document->elementIndex(part);
    if (pi < 0) return notFound(part);
    const doc::Element& p = info.document->elements[size_t(pi)];
    if (!p.context) return std::optional<ContextInfo>{};
    ContextInfo ci;
    ci.assembly = p.context->assembly;
    ci.instance = p.context->instance;
    ci.frozen = p.context->frozen.has_value();
    ci.used = regen::usedContextPaths(p);
    for (const auto& f : p.features) {
        doc::Element one;
        one.features.push_back(f);
        if (!regen::contextRefs(one).empty()) ci.features.push_back(f->id);
    }
    return std::optional<ContextInfo>{ci};
}

Result<std::vector<io::BodyMesh>> Session::contextTessellation(const kernel::TessParams& params, double acceptChordTol) {
    PartResultPtr pr = regenResult();
    if (!pr || !pr->initial) return Error("API_STATE_UNAVAILABLE");
    std::vector<io::BodyMesh> out;
    const regen::StatePtr ctx = pr->initial->context;
    if (!ctx) return out;
    if (!(params.chordTol >= kernel::KERNEL_MIN_CHORD && params.chordTol <= kernel::KERNEL_MAX_CHORD))
        return Error("KERNEL_INVALID_ARGUMENT").with("name", std::string("chordTol")).with("actual", params.chordTol);
    for (size_t i = 0; i < ctx->bodies.size(); ++i) {
        const TessCache::Key key{ctx, ctx->bodies[i].id, params.chordTol, params.angleTol};
        if (auto cached = tessCache_.get(key, acceptChordTol)) {
            out.push_back(*cached);
            continue;
        }
        auto m = bodyMesh(k_, *ctx, static_cast<int>(i), params);
        if (!m) return m.error();
        ++tessellations_;
        tessCache_.put(key, *m);
        out.push_back(*m);
    }
    return out;
}

}  // namespace construe::api
