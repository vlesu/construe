// Thread and hole (spec/regen.md, thread and hole evaluators).
#include "regen/eval_common.h"
#include "regen/hole_tables.h"

#include "base/number_format.h"

namespace construe::regen {

using namespace detail;

EvalPtr Regenerator::evalThread(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& /*cancelledOut*/) {
    [[maybe_unused]] const doc::Feature& f = c.f;
    // §4.14
    const doc::Json& d = f.pattern()->def;
    const std::string name = d["face"];
    const auto fr = resolveOne(c, *input, name, naming::EntityKind::Face, "/face");
    if (!fr) return finish(c, input);
    const auto& fe = input->names.faces.at(fr->index);
    const kernel::Shape& sh = input->bodies.at(fe.body).shape;
    const auto si = k_.faceSurface(sh, fe.face);
    if (si.kind != kernel::SurfaceInfo::Kind::Cylinder) {
        c.error(refGeometry("face", "face not cylindrical"), "/face");
        return finish(c, input);
    }
    // Designation from the table or diameter and pitch by expressions (S6-LIB-008).
    double nd = 0, P = 0;
    std::string designation;
    if (d.contains("diameter")) {
        const auto dv = c.field(d["diameter"].get<std::string>(), expr::LENGTH, "/diameter");
        if (!dv) return finish(c, input);
        if (*dv <= 0) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("diameter")).with("actual", *dv), "/diameter");
            return finish(c, input);
        }
        nd = *dv;
        if (d.contains("pitch")) {
            const auto pv = c.field(d["pitch"].get<std::string>(), expr::LENGTH, "/pitch");
            if (!pv) return finish(c, input);
            if (*pv <= 0) {
                c.error(Error("REGEN_BAD_VALUE").with("name", std::string("pitch")).with("actual", *pv), "/pitch");
                return finish(c, input);
            }
            P = *pv;
        } else {
            const auto coarse = threadCoarsePitch(nd);
            if (!coarse) {
                c.error(Error("REGEN_THREAD_DESIGNATION"), "/diameter");
                return finish(c, input);
            }
            P = *coarse;
        }
        designation = threadDesignation(nd, P);
    } else {
        const auto spec = threadSpec(d["designation"].get<std::string>());
        if (!spec) {
            c.error(Error("REGEN_THREAD_DESIGNATION"), "/designation");
            return finish(c, input);
        }
        nd = (*spec)["d"].get<double>();
        P = (*spec)["pitch"].get<double>();
        designation = d["designation"];
    }
    const std::string sizePath = d.contains("diameter") ? "/diameter" : "/designation";
    // Free thread (FX7-PT-003): own profile height, (0, P].
    double height = 0;
    if (d.contains("height")) {
        const auto hv = c.field(d["height"].get<std::string>(), expr::LENGTH, "/height");
        if (!hv) return finish(c, input);
        if (*hv <= 0 || *hv > P) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("height")).with("actual", *hv), "/height");
            return finish(c, input);
        }
        height = *hv;
    }
    const Vec3 a = si.dir.normalized();
    const Vec3 u = a.unitOrthogonal();
    const double r = si.radius;
    const bool internal = k_.faceNormalAt(sh, fe.face, si.point + r * u).dot(u) < 0;
    const double D = 2 * r;
    // Internal — by tap drill diameter or by nominal (a hole modeled at nominal, FX1-FEAT-004).
    if (threadDiameterFit(nd, P, D, internal) == ThreadDiameterFit::None) {
        c.error(Error("REGEN_THREAD_DIAMETER").with("actual", D).with("expected", nd), sizePath);
        return finish(c, input);
    }
    const auto range = kernel::axialRange(k_, sh, fe.face, si.point, a);
    if (!range) {
        c.error(refGeometry("face", "face not cylindrical"), "/face");
        return finish(c, input);
    }
    const double t0 = range->first, t1 = range->second;
    double L = t1 - t0;
    if (d.contains("length")) {
        const auto v = c.field(d["length"].get<std::string>(), expr::LENGTH, "/length");
        if (!v) return finish(c, input);
        if (*v <= 0 || *v > L + SWEEP_JOIN_TOL) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("length")).with("actual", *v), "/length");
            return finish(c, input);
        }
        L = *v;
    }
    const bool rev = d.contains("reverse") && d["reverse"].get<bool>();
    const Point3 origin = si.point + (rev ? t0 : t1) * a;
    st->threads.push_back({f.id, designation, internal, origin, rev ? a : Vec3(-a), r, nd, P, L, height});
    return finish(c, st);
}

EvalPtr Regenerator::evalHole(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut) {
    [[maybe_unused]] const doc::Feature& f = c.f;
    [[maybe_unused]] const Cancel* cancel = c.cancel;
    auto sketchOf = [&](const std::string& id) { return detail::sketchOf(c, *input, id); };
    // §4.13
    const doc::Json& d = f.pattern()->def;
    const std::string skId = d["sketch"];
    const SketchEntry* se = sketchOf(skId);
    const auto skf = c.d.active().find(skId);
    if (!se || !skf || !skf->sketchData()) return finish(c, input);
    // 1. Points: standalone ones and construction geometry vertices; coincident ones — one (FX1-FEAT-002).
    std::vector<std::pair<std::string, Point3>> points;
    {
        std::vector<Point2> taken;
        for (const auto& id : sketch::holePointCandidates(*skf->sketchData())) {
            const auto it = se->result.state.points.find(id);
            if (it == se->result.state.points.end()) continue;
            bool dup = false;
            for (const auto& q : taken) dup = dup || (q - it->second).norm() < HOLE_POINT_MERGE_TOL;
            if (dup) continue;
            taken.push_back(it->second);
            points.push_back({id, se->frame.toWorld(it->second)});
        }
    }
    if (points.empty()) {
        c.error(Error("REGEN_HOLE_NO_POINTS").with("id", skId), "/sketch");
        return finish(c, input);
    }
    const bool rev = d.contains("reverse") && d["reverse"].get<bool>();
    const Vec3 dir = se->frame.normal.normalized() * (rev ? 1.0 : -1.0);
    // 2. Sizes from the table.
    const auto row = holeSize(d["size"].get<std::string>());
    if (!row) {
        c.error(Error("REGEN_HOLE_SIZE").with("name", std::string("size")), "/size");
        return finish(c, input);
    }
    const std::string kind = d["kind"];
    double dh = 0;
    if (kind == "tapped") {
        const doc::Json& pitches = (*row)["pitches"];
        const doc::Json* hit = nullptr;
        if (!d.contains("pitch")) hit = &pitches[0];
        else
            for (const auto& p : pitches)
                if (std::abs(p["pitch"].get<double>() - d["pitch"].get<double>()) < 1e-9) hit = &p;
        if (!hit) {
            c.error(Error("REGEN_HOLE_SIZE").with("name", std::string("pitch")), "/pitch");
            return finish(c, input);
        }
        dh = (*hit)["drill"];
    } else {
        dh = (*row)["clearance"][d.value("fit", std::string("normal"))];
    }
    if (input->sheet && !input->sheet->body.empty()) return evalSheetHole(c, input, st, cancelledOut, points, dir, dh);  // §4.16
    const double rh = dh / 2;
    double Rtop = rh, shoulder = 0;  // radius at the surface; counterbore/countersink depth
    if (kind == "counterbore") Rtop = (*row)["counterbore"]["d"].get<double>() / 2, shoulder = (*row)["counterbore"]["t"];
    if (kind == "countersink") Rtop = (*row)["countersink"]["d"].get<double>() / 2, shoulder = Rtop - rh;
    // 3. Depth.
    const bool through = d["end"]["type"] == "throughAll";
    double depth = 0;
    if (through) {
        const std::vector<kernel::Shape> shapes = operationBounds(c, *st);  // assembly: the targets (assembly.md §7г.3)
        if (shapes.empty()) {
            c.error(Error("REGEN_THROUGH_ALL_NO_BODIES"), "/end");
            return finish(c, input);
        }
        const kernel::Shape bounds = k_.combine(shapes).shape;
        for (const auto& [id, P] : points) depth = std::max(depth, kernel::throughAllDepth(k_, bounds, P, dir));
    } else {
        const auto v = c.field(d["end"]["depth"].get<std::string>(), expr::LENGTH, "/end/depth");
        if (!v) return finish(c, input);
        if (*v <= 0 || *v <= shoulder) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("end.depth")).with("actual", *v), "/end/depth");
            return finish(c, input);
        }
        depth = *v;
    }
    // 4. Tool for each point: revolved body of the half-profile.
    ToolBuild total;
    std::vector<std::pair<double, double>> rz;  // the half-profile (r, z) — the owned cut "in the parts" (§4.13 item 6)
    std::vector<ConnectionFrame> frames;          // per point: O = P, X — the profile direction, Z = dir
    const double tipH = rh / std::tan(59.0 * PI / 180);
    for (const auto& [pid, P] : points) {
        Plane pl;
        pl.origin = P;
        pl.x = dir.unitOrthogonal();
        pl.y = dir;
        pl.normal = pl.x.cross(pl.y);
        std::vector<std::pair<Point2, std::string>> poly;  // vertex and ID of the segment going FROM it
        poly.push_back({Point2(0, 0), "s6"});
        if (kind == "counterbore") {
            poly.push_back({Point2(Rtop, 0), "s3"});
            poly.push_back({Point2(Rtop, shoulder), "s4"});
            poly.push_back({Point2(rh, shoulder), "s1"});
        } else if (kind == "countersink") {
            poly.push_back({Point2(Rtop, 0), "s5"});
            poly.push_back({Point2(rh, shoulder), "s1"});
        } else {
            poly.push_back({Point2(rh, 0), "s1"});
        }
        poly.push_back({Point2(rh, depth), "s2"});
        poly.push_back({Point2(0, depth + (through ? 0.0 : tipH)), "s7"});
        if (rz.empty())
            for (const auto& v : poly) rz.emplace_back(v.first.x(), v.first.y());
        frames.push_back({P, pl.x, dir.cross(pl.x), dir});
        kernel::Profile prof;
        prof.plane = pl;
        for (size_t i = 0; i < poly.size(); ++i) {
            kernel::Segment3 s;
            s.id = poly[i].second;
            s.kind = kernel::Segment3::Kind::Line;
            s.p1 = pl.toWorld(poly[i].first);
            s.p2 = pl.toWorld(poly[(i + 1) % poly.size()].first);
            prof.outer.segments.push_back(s);
        }
        auto r = k_.revolve(prof, kernel::Axis{P, dir}, kernel::RevolveExtent{kernel::RevolveExtent::Kind::Full, 0});
        if (!r) {
            c.kernelError(r.error());
            return finish(c, input);
        }
        std::vector<Error> nw;
        auto bases = naming::assignBases(naming::OpKind::Generate, f.id, *r, {}, &nw);
        for (auto& w : nw) c.warning(w);
        const std::string key = f.id + ".side(";
        for (auto& b : bases)
            for (size_t pos = b.find(key); pos != std::string::npos; pos = b.find(key, pos + 1)) {
                const size_t close = b.find(')', pos + key.size());
                if (close != std::string::npos) b.insert(close, "/" + pid);
            }
        if (total.shape.null()) {
            total.shape = r->shape;
            total.bases = bases;
            continue;
        }
        auto u = k_.boolean(kernel::BoolKind::Union, total.shape, r->shape, cancel);
        if (!u) {
            c.kernelError(u.error());
            if (c.cancelled) {
                cancelledOut = true;
                return nullptr;
            }
            return finish(c, input);
        }
        std::vector<Error> bw;
        total.bases = naming::assignBases(naming::OpKind::Boolean, f.id, *u, {inputOf(total.bases), inputOf(bases)}, &bw);
        for (auto& w : bw) c.warning(w);
        total.shape = u->shape;
    }
    total.ok = true;
    // "In the parts" (assembly.md §7г.6, FX8-ASM-007): the same parts get the cut everywhere in the assembly and the
    // hole becomes owned features of the part documents (materialized by api, as the cuts of a connection).
    const bool partsMode = !st->assemblyParts.empty() && d.contains("cut") && d["cut"] == "parts";
    std::vector<std::string> sources, changedAll;
    if (!(partsMode ? applyPartsHoleOperation(c, *st, total, sources, changedAll) : applyOperation(c, *st, "cut", total))) {
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input, total.shape);
    }
    if (partsMode && !c.hasError()) {
        // The frame of a point in the coordinates of a part with world motion W.
        auto movedFrame = [](const Motion3& W, const ConnectionFrame& f) {
            const Motion3 inv{W.R.transpose(), -(W.R.transpose() * W.t)};
            ConnectionFrame r;
            r.O = inv.apply(f.O);
            r.X = inv.applyDir(f.X), r.Y = inv.applyDir(f.Y), r.Z = inv.applyDir(f.Z);
            return r;
        };
        doc::Json cuts = doc::Json::array();
        doc::Json poly = doc::Json::array();
        for (const auto& [r, z] : rz) poly.push_back(doc::Json::array({r, z}));
        for (size_t k = 0; k < points.size(); ++k) {
            doc::Json cut = {{"output", points[k].first}, {"title", std::string("Отверстие")}, {"parts", sources}, {"changed", changedAll},
                             {"diameter", dh}};
            if (kind == "tapped") {
                const doc::Json& pitches = (*row)["pitches"];
                const double pitch = d.contains("pitch") ? d["pitch"].get<double>() : pitches[0]["pitch"].get<double>();
                const auto spec = threadSpec(d["size"].get<std::string>());
                cut["thread"] = {{"d", spec ? (*spec)["d"].get<double>() : dh}, {"pitch", pitch}, {"length", depth}};
            }
            doc::Json owned = doc::Json::array();
            for (const auto& src : sources)
                for (const auto& ap : input->assemblyParts)
                    if (ap.path == src && !doc::isLibraryRef(ap.file))
                        owned.push_back({{"source", src}, {"element", ap.element}, {"file", ap.file},
                                         {"frame", frameJson(movedFrame(ap.world, frames[k]))}});
            cut["owned"] = owned;
            cut["tool"] = {{"revolve", poly}};
            cuts.push_back(cut);
        }
        st->connections[f.id] = {{"cuts", cuts}, {"parts", doc::Json::array()}, {"cut", "parts"}};
    }
    if (!c.hasError()) {
        st->tools[f.id] = {total.shape, total.bases, "cut"};
        if (kind == "tapped") {  // hole thread (§4.13 item 5, S2-FEAT-009)
            const doc::Json& pitches = (*row)["pitches"];
            const double pitch = d.contains("pitch") ? d["pitch"].get<double>() : pitches[0]["pitch"].get<double>();
            const bool coarse = std::abs(pitch - pitches[0]["pitch"].get<double>()) < 1e-9;
            const std::string des = d["size"].get<std::string>() + (coarse ? std::string() : "x" + canonicalNumber(pitch));
            const auto spec = threadSpec(des);
            rename(k_, *st);
            for (const auto& [pid, P] : points) {
                // Extent — over the hole wall faces of this point (F.side(s1/p)).
                double t0 = 1e300, t1 = -1e300;
                const std::string wall = f.id + ".side(s1/" + pid + ")";
                for (const auto& fe : st->names.faces)
                    if (fe.full.rfind(wall, 0) == 0)
                        if (auto rg = kernel::axialRange(k_, st->bodies.at(fe.body).shape, fe.face, P, dir))
                            t0 = std::min(t0, rg->first), t1 = std::max(t1, rg->second);
                if (!(t1 > t0)) continue;
                st->threads.push_back({f.id, des, true, P + t0 * dir, dir, rh, spec ? (*spec)["d"].get<double>() : dh,
                                       spec ? (*spec)["pitch"].get<double>() : pitch, t1 - t0});
            }
        }
    }
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st, total.shape);
}

}  // namespace construe::regen
