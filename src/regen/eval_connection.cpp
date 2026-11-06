// Connection — an applied solution, an assembly feature (spec/solutions.md §7): inputs in the virtual assembly state,
// the frame, the pack, the rules (solutions module), cuts by parts and the report with placed parts.
#include "regen/eval_common.h"
#include "regen/hole_tables.h"
#include "sketch/data.h"

#include "base/number_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace construe::regen {

using namespace detail;

namespace {

kernel::Motion toKernel(const Motion3& M) {
    kernel::Motion km;
    for (int i = 0; i < 9; ++i) km.r[size_t(i)] = M.R(i / 3, i % 3);
    km.t = M.t;
    return km;
}

// World → frame coordinates.
Motion3 frameInverse(const ConnectionFrame& f) {
    Motion3 m;
    m.R.row(0) = f.X.transpose();
    m.R.row(1) = f.Y.transpose();
    m.R.row(2) = f.Z.transpose();
    m.t = -(m.R * f.O);
    return m;
}

// Part path of an assembly stable name "<path>/<name>": the longest part path that prefixes it (assembly.md §3).
std::string partOfName(const PartState& v, const std::string& name) {
    std::string best;
    for (const auto& p : v.assemblyParts)
        if (name.size() > p.path.size() && name.compare(0, p.path.size(), p.path) == 0 && name[p.path.size()] == '/' &&
            p.path.size() > best.size())
            best = p.path;
    return best;
}

struct Face {
    const kernel::Shape* shape = nullptr;
    int face = -1;
};

std::optional<Face> resolveFace(const PartState& v, const std::string& name, std::optional<Error>& err) {
    const auto rr = naming::resolve(name, naming::EntityKind::Face, v.names);
    const std::vector<std::string> ids{name};
    switch (naming::refFate(rr, false)) {
    case naming::RefFate::Lost: err = Error("REGEN_REF_LOST").with("ids", ids); return std::nullopt;
    case naming::RefFate::Ambiguous: err = Error("REGEN_REF_AMBIGUOUS").with("ids", ids); return std::nullopt;
    case naming::RefFate::Resolved: break;
    }
    const auto& fe = v.names.faces.at(rr.entities.front().index);
    return Face{&v.bodies.at(fe.body).shape, fe.face};
}

// A rectangle-free polygon profile in plane pl (vertices (u, v) in plane coordinates), counterclockwise.
kernel::Profile polygonProfile(const Plane& pl, std::vector<Point2> poly) {
    double area = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point2& a = poly[i];
        const Point2& b = poly[(i + 1) % poly.size()];
        area += a.x() * b.y() - b.x() * a.y();
    }
    if (area < 0) std::reverse(poly.begin(), poly.end());
    kernel::Profile prof;
    prof.plane = pl;
    for (size_t i = 0; i < poly.size(); ++i) {
        kernel::Segment3 s;
        s.id = "s" + std::to_string(i + 1);
        s.kind = kernel::Segment3::Kind::Line;
        s.p1 = pl.toWorld(poly[i]);
        s.p2 = pl.toWorld(poly[(i + 1) % poly.size()]);
        prof.outer.segments.push_back(s);
    }
    return prof;
}

// Revolved body of polygon (r, z) about the frame axis.
Result<kernel::OpResult> revolveTool(const kernel::Kernel& k, const ConnectionFrame& f, const std::vector<std::pair<double, double>>& rz) {
    Plane pl;
    pl.origin = f.O;
    pl.x = f.X;
    pl.y = f.Z;
    pl.normal = f.X.cross(f.Z);
    std::vector<Point2> poly;
    for (const auto& [r, z] : rz) poly.push_back(Point2(r, z));
    return k.revolve(polygonProfile(pl, poly), kernel::Axis{f.O, f.Z}, kernel::RevolveExtent{kernel::RevolveExtent::Kind::Full, 0});
}

Result<kernel::OpResult> boxTool(const kernel::Kernel& k, const ConnectionFrame& f, const std::array<double, 6>& b) {
    Plane pl;
    pl.origin = f.at(0, 0, b[4]);
    pl.x = f.X;
    pl.y = f.Y;
    pl.normal = f.Z;
    const std::vector<Point2> poly{Point2(b[0], b[2]), Point2(b[1], b[2]), Point2(b[1], b[3]), Point2(b[0], b[3])};
    kernel::Extent e;
    e.kind = kernel::Extent::Kind::Blind;
    e.depth = b[5] - b[4];
    return k.extrude(polygonProfile(pl, poly), e, false);
}

// Rigid motion applied to a frame; the inverse of a motion.
ConnectionFrame moved(const Motion3& M, const ConnectionFrame& f) {
    ConnectionFrame r;
    r.O = M.apply(f.O);
    r.X = M.applyDir(f.X), r.Y = M.applyDir(f.Y), r.Z = M.applyDir(f.Z);
    return r;
}
Motion3 inverted(const Motion3& M) { return {M.R.transpose(), -(M.R.transpose() * M.t)}; }

doc::Json vecJson(const Vec3& v) { return doc::Json::array({v.x(), v.y(), v.z()}); }
Vec3 vecOf(const doc::Json& j) { return Vec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()); }

// Face bases of a cut tool with the output position in side faces: F.side(s<k>/<tag>) (solutions.md §7.5, §7.10 item 3.2).
std::vector<std::string> taggedBases(std::vector<std::string> bases, const std::string& featureId, const std::string& tag) {
    const std::string key = featureId + ".side(";
    for (auto& b : bases)
        for (size_t pos = b.find(key); pos != std::string::npos; pos = b.find(key, pos + 1)) {
            const size_t close = b.find(')', pos + key.size());
            if (close != std::string::npos) b.insert(close, "/" + tag);
        }
    return bases;
}

}  // namespace

doc::Json frameJson(const ConnectionFrame& f) {
    return {{"origin", vecJson(f.O)}, {"x", vecJson(f.X)}, {"y", vecJson(f.Y)}, {"z", vecJson(f.Z)}};
}

ConnectionFrame frameFromJson(const doc::Json& j) {
    ConnectionFrame f;
    f.O = vecOf(j["origin"]);
    f.X = vecOf(j["x"]), f.Y = vecOf(j["y"]), f.Z = vecOf(j["z"]);
    return f;
}

std::string libraryCopyElement(const doc::LibraryPart& lp) {
    if (!lp.document) return {};
    for (const auto& el : lp.document->elements)
        if (el.kind == "part") return el.id;
    return {};
}

Motion3 connectionPartMotion(const ConnectionFrame& f, const solutions::PartPlan& p) {
    // The part's axis is the normal of its «Top» plane, part +Y (document.md §2.5, library.md §14.2).
    const Vec3 y = p.in ? f.Z : Vec3(-f.Z);
    const Vec3 x = f.X;
    const Vec3 z = x.cross(y);
    Motion3 m;
    m.R.col(0) = x;
    m.R.col(1) = y;
    m.R.col(2) = z;
    m.t = f.at(p.x, p.y, p.z);
    return m;
}

solutions::TableData libraryCopyTable(const doc::LibraryPart& lp) {
    solutions::TableData out;
    if (!lp.document) return out;
    const doc::Document& D = *lp.document;
    const doc::VariantTable* t = D.variantTable(lp.sizesTable);
    if (!t) return out;
    for (const auto& c : t->columns) {
        const doc::Variable* v = D.variable(c);
        out.columns.push_back(v ? v->name : c);
    }
    const expr::DocumentUnits units = documentUnits(D);
    for (const auto& r : t->variants) {
        doc::Document dr = D;
        for (auto& vt : dr.variantTables)
            if (vt.id == lp.sizesTable) vt.active = r.name;
        const expr::VariableTable vars = documentVariables(dr);
        solutions::TableRow row;
        row.name = r.name;
        row.texts = r.values;
        for (const auto& name : out.columns) {
            const auto* e = vars.find(name);
            row.values.push_back(e ? e->value : Result<expr::Quantity>(Error("EXPR_UNKNOWN_NAME")));
        }
        row.designation = expr::substitute(lp.designation, vars, units);
        out.rows.push_back(std::move(row));
    }
    return out;
}

solutions::TableProvider connectionTables(const doc::Document& d, const doc::SolutionCopy& copy, const doc::Json& tableMap) {
    const solutions::Definition def = solutions::parseDefinition(copy.definition);
    auto files = std::make_shared<std::map<std::string, solutions::DataTable>>(solutions::copyTables(copy));
    std::map<std::string, const doc::LibraryPart*> libs;
    for (const auto& t : def.tables)
        if (!t.library.empty() && tableMap.is_object() && tableMap.contains(t.id) && tableMap[t.id].is_string())
            libs[t.id] = d.libraryPart(tableMap[t.id].get<std::string>());
    auto defs = std::make_shared<std::vector<solutions::TableDef>>(def.tables);
    return [defs, files, libs](const std::string& id) -> std::optional<solutions::TableData> {
        for (const auto& t : *defs) {
            if (t.id != id) continue;
            if (!t.builtin.empty()) return solutions::holesTable(holeTables());
            if (!t.file.empty()) {
                auto it = files->find(id);
                return it == files->end() ? std::nullopt : std::optional(solutions::fileTable(it->second));
            }
            auto it = libs.find(id);
            if (it == libs.end() || !it->second) return std::nullopt;
            return libraryCopyTable(*it->second);
        }
        return std::nullopt;
    };
}

PackCache::Pack measurePack(const kernel::Kernel& k, const PartState& v, const ConnectionFrame& F, const kernel::Motion& toFrame,
                            double rho, const std::string& endPart, std::optional<double> endPlane, const Cancel* cancel);

ConnectionPlan planConnection(const kernel::Kernel& k, const solutions::Definition& def, const solutions::TableProvider& tables,
                              const doc::Json& inputs, const doc::Json& choices, const PartState& v, const Cancel* cancel,
                              PackCache* cache) {
    ConnectionPlan out;
    auto fail = [&](Error e) {
        out.plan.error = std::move(e);
        return out;
    };
    auto given = [&](const std::string& id) { return !id.empty() && inputs.is_object() && inputs.contains(id); };
    // §7.2: inputs.
    for (const auto& in : def.inputs)
        if (!in.optional && !given(in.id)) return fail(Error("SOL_INPUT_MISSING").with("name", in.id));
    std::vector<solutions::Value> measures;
    std::optional<Plane> face, packEnd;
    std::optional<Face> packEndFace;  // the pack end face itself (FX13-JNT-001)
    std::optional<std::pair<Point3, Vec3>> axis;
    for (const auto& in : def.inputs) {
        if (!given(in.id)) continue;
        const doc::Json& val = inputs[in.id];
        if (in.type == "point") {
            if (!val.is_array()) return fail(Error("SOL_INPUT_GEOMETRY").with("name", in.id));
            continue;
        }
        if (!val.is_string()) return fail(Error("SOL_INPUT_GEOMETRY").with("name", in.id));
        std::optional<Error> err;
        const auto fc = resolveFace(v, val.get<std::string>(), err);
        if (!fc) return fail(*err);
        out.inputParts[in.id] = partOfName(v, val.get<std::string>());
        const auto si = k.faceSurface(*fc->shape, fc->face);
        if (in.type == "face") {
            if (si.kind != kernel::SurfaceInfo::Kind::Plane) return fail(Error("SOL_INPUT_GEOMETRY").with("name", in.id));
            if (in.id == def.frameFace || in.id == def.framePackEnd) {
                Plane pl;
                pl.origin = k.faceCentroid(*fc->shape, fc->face);
                pl.normal = k.faceNormalAt(*fc->shape, fc->face, pl.origin).normalized();
                (in.id == def.frameFace ? face : packEnd) = pl;
                if (in.id == def.framePackEnd) packEndFace = *fc;
            }
        } else {
            if (si.kind != kernel::SurfaceInfo::Kind::Cylinder) return fail(Error("SOL_INPUT_GEOMETRY").with("name", in.id));
            const Vec3 a = si.dir.normalized();
            double len = 0;
            if (auto rg = kernel::axialRange(k, *fc->shape, fc->face, si.point, a)) len = rg->second - rg->first;
            measures.push_back({in.id + ".diameter", {2 * si.radius, expr::LENGTH}});
            measures.push_back({in.id + ".length", {len, expr::LENGTH}});
            if (in.id == def.frameAxis) axis = std::pair<Point3, Vec3>{si.point, a};
        }
    }
    // §7.3: the frame.
    if (!face) return fail(Error("SOL_INPUT_MISSING").with("name", def.frameFace));
    ConnectionFrame& F = out.frame;
    const Vec3 n = face->normal;
    if (axis && given(def.frameAxis)) {
        const auto& [q, a] = *axis;
        if (a.cross(n).norm() > solutions::SOL_PERP_TOL) return fail(Error("SOL_FRAME").with("name", def.frameAxis));
        F.O = q + ((face->origin - q).dot(n) / a.dot(n)) * a;
    } else if (given(def.framePoint)) {
        const doc::Json& p = inputs[def.framePoint];
        const Point3 P(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
        F.O = P - (P - face->origin).dot(n) * n;
    } else {
        return fail(Error("SOL_INPUT_MISSING").with("name", def.frameAxis.empty() ? def.framePoint : def.frameAxis));
    }
    F.Z = -n;
    Vec3 e = Vec3::UnitX();
    for (const Vec3& c : {Vec3(Vec3::UnitY()), Vec3(Vec3::UnitZ())})
        if (std::abs(c.dot(F.Z)) < std::abs(e.dot(F.Z))) e = c;
    F.X = (e - e.dot(F.Z) * F.Z).normalized();
    F.Y = F.Z.cross(F.X);
    // §7.4а: the pack end face (FX8-BLT-003) — parallel to the head face, beyond it along Z.
    std::string endPart;  // the part of the pack end face — the last part of the pack
    if (packEnd) {
        if (packEnd->normal.cross(F.Z).norm() > solutions::SOL_PERP_TOL || (packEnd->origin - F.O).dot(F.Z) <= solutions::SOL_PACK_GAP)
            return fail(Error("SOL_PACK_END").with("name", def.framePackEnd));
        endPart = out.inputParts[def.framePackEnd];
    }
    // §7.4а (FX13-JNT-001): the plane of the pack end face at endZ along the axis; the distance from the axis point on it to
    // the face itself — the axis passes through the face (or its hole) when not farther than the probe radius.
    double endZ = 0, endDist = 1e300;
    if (packEnd && packEndFace) {
        endZ = (packEnd->origin - F.O).dot(F.Z);
        endDist = k.distanceToFace(*packEndFace->shape, packEndFace->face, F.O + endZ * F.Z);
    }
    // Cache keys of the frame (FX8-BLT-001): the origin and the axis to 1e-9 mm.
    char frameKey[160];
    std::snprintf(frameKey, sizeof frameKey, "%.9f,%.9f,%.9f/%.9f,%.9f,%.9f", F.O.x(), F.O.y(), F.O.z(), F.Z.x(), F.Z.y(), F.Z.z());
    // §7.2: the depth of a face input's part along the frame axis (the largest frame z of its bodies).
    const kernel::Motion toFrame = toKernel(frameInverse(F));
    for (const auto& in : def.inputs) {
        if (in.type != "face" || !given(in.id)) continue;
        const std::string& path = out.inputParts[in.id];
        const std::string dkey = path + "|" + frameKey;
        double zmax = 0;
        if (cache && cache->depths.count(dkey)) {
            zmax = cache->depths.at(dkey);
        } else {
            for (const auto& body : v.bodies) {
                if (partOfName(v, body.id) != path) continue;
                auto t = k.transform(body.shape, toFrame);
                if (t) zmax = std::max(zmax, k.bbox(t->shape).max().z());
            }
            if (cache) cache->depths[dkey] = zmax;
        }
        measures.push_back({in.id + ".depth", {zmax, expr::LENGTH}});
    }
    // §7.4: the pack, measured with the probe radius from the rules' environment.
    solutions::PackMeasure pack = [&](double rho) -> Result<std::vector<solutions::Value>> {
        // The measurement depends on the state, the frame, the probe radius and the pack end only (FX8-BLT-001).
        char rk[96];
        std::snprintf(rk, sizeof rk, "|%.9f|%.9f|", rho, endZ);  // endZ: the pack end face plane (FX13-JNT-001)
        const std::string pkey = std::string(frameKey) + rk + endPart;
        if (cache) {
            if (auto it = cache->packs.find(pkey); it != cache->packs.end()) {
                if (it->second.error) return *it->second.error;
                out.pack = it->second.parts;
                return it->second.values;
            }
            if (!cache->measure) return Error("SOL_PACK_UNMEASURED");
            ++cache->probes;
        }
        const std::optional<double> endPlane = !endPart.empty() && endDist <= rho + solutions::SOL_PACK_GAP ? std::optional<double>(endZ) : std::nullopt;
        auto measured = measurePack(k, v, F, toFrame, rho, endPart, endPlane, cancel);
        if (measured.error && measured.error->code == "SOL_PACK_END") measured.error->with("name", def.framePackEnd);
        if (!measured.error || measured.error->code != "KERNEL_CANCELLED")
            if (cache) cache->packs[pkey] = measured;
        if (measured.error) return *measured.error;
        out.pack = measured.parts;
        return measured.values;
    };
    out.plan = solutions::evaluate(def, choices, measures, tables, pack);
    return out;
}

// §7.4: the pack along the frame axis — parts hit by the probe of radius rho; with the pack end (§7.4а) — the parts
// up to the part of the pack end face (endPart, not empty); endPlane — the axis passes through the pack end face: the pack
// ends at its plane (FX13-JNT-001): the extents are cut at it, parts beyond it are not in the pack.
PackCache::Pack measurePack(const kernel::Kernel& k, const PartState& v, const ConnectionFrame& F, const kernel::Motion& toFrame,
                            double rho, const std::string& endPart, std::optional<double> endPlane, const Cancel* cancel) {
    PackCache::Pack res;
    {
        BBox3 all;
        for (const auto& b : v.bodies) all.extend(k.bbox(b.shape));
        const double L = 2 * (all.isEmpty() ? 0.0 : (all.max() - all.min()).norm()) + 1;
        auto probe = revolveTool(k, F, {{0, -L}, {rho, -L}, {rho, L}, {0, L}});
        if (!probe) {
            res.error = probe.error();
            return res;
        }
        struct Hit {
            std::string path;
            double a, b;
            size_t order;
        };
        std::vector<Hit> hits;
        for (size_t pi = 0; pi < v.assemblyParts.size(); ++pi) {
            const std::string& path = v.assemblyParts[pi].path;
            double a = 1e300, b = -1e300;
            for (const auto& body : v.bodies) {
                if (body.id.compare(0, path.size() + 1, path + "/") != 0 || partOfName(v, body.id) != path) continue;
                auto x = k.boolean(kernel::BoolKind::Intersect, body.shape, probe->shape, cancel);
                if (!x) {
                    if (x.error().code == "KERNEL_CANCELLED") {
                        res.error = x.error();
                        return res;
                    }
                    continue;
                }
                if (k.massProperties(x->shape).volume <= solutions::SOL_PROBE_MIN_VOLUME) continue;
                auto t = k.transform(x->shape, toFrame);
                if (!t) continue;
                const BBox3 bb = k.bbox(t->shape);
                a = std::min(a, bb.min().z()), b = std::max(b, bb.max().z());
            }
            if (endPlane) {  // FX13-JNT-001: nothing beyond the plane of the pack end face
                if (a >= *endPlane - solutions::SOL_PACK_GAP) continue;
                b = std::min(b, *endPlane);
            }
            if (b >= a && a >= -solutions::SOL_PACK_GAP) hits.push_back({path, a, b, pi});
        }
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.a < y.a; });
        std::vector<Hit> chain;
        double reach = 0;
        for (auto h : hits) {
            if (!endPart.empty()) {  // §7.4а: every part up to the part of the end face, gaps allowed
                chain.push_back(h);
                reach = std::max(reach, h.b);
                if (h.path == endPart) break;
                continue;
            }
            if (chain.empty() ? h.a > solutions::SOL_PACK_GAP : h.a > reach + solutions::SOL_PACK_GAP) break;
            chain.push_back(h);
            reach = std::max(reach, h.b);
        }
        if (!endPart.empty() && (chain.empty() || chain.back().path != endPart)) {  // the axis misses the part of the end face
            res.error = Error("SOL_PACK_END");
            return res;
        }
        if (chain.empty()) {
            res.error = Error("SOL_PACK_EMPTY");
            return res;
        }
        for (const auto& h : chain) res.parts.push_back(h.path);
        if (endPlane) reach = *endPlane;  // the nut on the face plane (FX13-JNT-001)
        res.values = {{"pack.thickness", {reach, expr::LENGTH}},
                      {"pack.count", {double(chain.size()), expr::DIMLESS}},
                      {"pack.firstThickness", {chain.front().b - chain.front().a, expr::LENGTH}},
                      {"pack.lastThickness", {chain.back().b - chain.back().a, expr::LENGTH}},
                      {"pack.lastStart", {chain.back().a, expr::LENGTH}}};
    }
    return res;
}

Result<std::optional<std::vector<Point3>>> connectionSketchPoints(const doc::Element& el, const PartState& v,
                                                                  const solutions::Definition& def, const doc::Json& inputs) {
    std::optional<std::vector<Point3>> none;
    if (def.framePoint.empty() || !inputs.is_object() || !inputs.contains(def.framePoint)) return none;
    const doc::Json& in = inputs[def.framePoint];
    if (!in.is_object() || !in.contains("sketch") || !in["sketch"].is_string()) return none;
    const std::string id = in["sketch"];
    const auto f = el.find(id);
    const auto it = v.sketches.find(id);
    if (!f || !f->sketchData() || it == v.sketches.end()) return Error("REGEN_INPUT_ERROR").with("id", id);
    // regen.md §4.13 item 1: standalone points and construction vertices; coincident ones — one.
    std::vector<Point3> out;
    std::vector<Point2> taken;
    for (const auto& pid : sketch::holePointCandidates(*f->sketchData())) {
        const auto p = it->second.result.state.points.find(pid);
        if (p == it->second.result.state.points.end()) continue;
        bool dup = false;
        for (const auto& q : taken) dup = dup || (q - p->second).norm() < HOLE_POINT_MERGE_TOL;
        if (dup) continue;
        taken.push_back(p->second);
        out.push_back(it->second.frame.toWorld(p->second));
    }
    if (out.empty()) return Error("SOL_NO_POINTS").with("name", def.framePoint).with("id", id);
    return std::optional(std::move(out));
}

doc::Json inputsAtPoint(const solutions::Definition& def, const doc::Json& inputs, const Point3& p) {
    doc::Json out = inputs;
    out[def.framePoint] = doc::Json::array({p.x(), p.y(), p.z()});
    return out;
}

std::string pointOutput(const std::string& output, size_t k) { return k == 0 ? output : output + "_p" + std::to_string(k + 1); }

bool isAxisList(const doc::Json& v) {
    if (!v.is_array() || v.empty()) return false;
    return std::all_of(v.begin(), v.end(), [](const doc::Json& a) { return a.is_string() && !a.get<std::string>().empty(); });
}

Result<std::vector<doc::Json>> connectionPositions(const doc::Element& el, const PartState& v, const solutions::Definition& def,
                                                   const doc::Json& inputs, bool* byAxes) {
    if (byAxes) *byAxes = false;
    std::vector<doc::Json> out;
    if (!def.frameAxis.empty() && inputs.is_object() && inputs.contains(def.frameAxis) && isAxisList(inputs[def.frameAxis])) {
        for (const auto& a : inputs[def.frameAxis]) {
            doc::Json in = inputs;
            in[def.frameAxis] = a;
            out.push_back(std::move(in));
        }
        if (byAxes) *byAxes = out.size() > 1;
        return out;
    }
    auto pts = connectionSketchPoints(el, v, def, inputs);
    if (!pts) return pts.error();
    if (*pts)
        for (const auto& p : **pts) out.push_back(inputsAtPoint(def, inputs, p));
    else
        out.push_back(inputs);
    return out;
}

doc::Json mergePointReports(const std::vector<doc::Json>& reports, const char* countKey) {
    if (reports.empty()) return doc::Json::object();
    doc::Json out = reports.front();
    if (reports.size() == 1) return out;
    for (size_t k = 1; k < reports.size(); ++k)
        for (const char* key : {"cuts", "parts"})
            for (doc::Json e : reports[k][key]) {
                e["output"] = pointOutput(e["output"].get<std::string>(), k);
                out[key].push_back(std::move(e));
            }
    out[countKey] = double(reports.size());
    return out;
}

doc::Json connectionReport(const ConnectionPlan& p, const std::map<std::string, std::string>& libraries,
                           const std::vector<std::vector<std::string>>& changed, const PartState* partsOf) {
    doc::Json r = doc::Json::object();
    doc::Json values = doc::Json::array();
    for (const auto& v : p.plan.values)
        values.push_back({{"name", v.name}, {"value", v.q.value}, {"dim", expr::dimToString(v.q.dim)},
                          {"text", expr::format(v.q, expr::DocumentUnits{}, solutions::SOL_PREVIEW_PRECISION)}});
    r["values"] = values;
    doc::Json rows = doc::Json::array();
    for (const auto& x : p.plan.rows) rows.push_back({{"rule", x.rule}, {"table", x.table}, {"row", x.row}});
    r["rows"] = rows;
    r["pack"] = p.pack;
    doc::Json cuts = doc::Json::array();
    for (size_t i = 0; i < p.plan.cuts.size(); ++i) {
        const auto& c = p.plan.cuts[i];
        doc::Json j = {{"output", c.id}, {"title", c.title}};
        std::vector<std::string> parts;
        if (c.parts == "pack") parts = p.pack;
        else if (c.parts == "packFirst" && !p.pack.empty()) parts = {p.pack.front()};
        else if (c.parts == "packLast" && !p.pack.empty()) parts = {p.pack.back()};
        else if (c.parts == "packButLast" && p.pack.size() > 1) parts.assign(p.pack.begin(), p.pack.end() - 1);
        else if (auto it = p.inputParts.find(c.parts); it != p.inputParts.end()) parts = {it->second};
        j["parts"] = parts;
        j["changed"] = i < changed.size() ? changed[i] : std::vector<std::string>{};
        if (c.revolve) {
            double dmin = 0;
            for (const auto& [rr, z] : c.polygon)
                if (rr > 0 && (dmin == 0 || 2 * rr < dmin)) dmin = 2 * rr;
            j["diameter"] = dmin;
        }
        if (c.thread) j["thread"] = {{"d", (*c.thread)[0]}, {"pitch", (*c.thread)[1]}, {"length", (*c.thread)[2]}};
        if (!c.fit.empty()) j["fit"] = c.fit;
        if (partsOf) {  // §7.10 item 3.3: desired owned features by source, except library copies
            doc::Json owned = doc::Json::array();
            for (const auto& src : parts)
                for (const auto& ap : partsOf->assemblyParts)
                    if (ap.path == src && !doc::isLibraryRef(ap.file))
                        owned.push_back({{"source", src}, {"element", ap.element}, {"file", ap.file},
                                         {"frame", frameJson(moved(inverted(ap.world), p.frame))}});
            j["owned"] = owned;
            if (c.revolve) {
                doc::Json poly = doc::Json::array();
                for (const auto& v : c.polygon) poly.push_back(doc::Json::array({v.first, v.second}));
                j["tool"] = doc::Json::object();
                j["tool"]["revolve"] = poly;
            } else {
                doc::Json bx = doc::Json::object();
                bx["x"] = doc::Json::array({c.box[0], c.box[1]});
                bx["y"] = doc::Json::array({c.box[2], c.box[3]});
                bx["z"] = doc::Json::array({c.box[4], c.box[5]});
                j["tool"] = doc::Json::object();
                j["tool"]["box"] = bx;
            }
        }
        cuts.push_back(j);
    }
    r["cuts"] = cuts;
    doc::Json parts = doc::Json::array();
    for (const auto& x : p.plan.parts) {
        const Motion3 M = connectionPartMotion(p.frame, x);
        doc::Json w = doc::Json::array();
        for (int i = 0; i < 9; ++i) w.push_back(M.R(i / 3, i % 3));
        for (int i = 0; i < 3; ++i) w.push_back(M.t[i]);
        auto lib = libraries.find(x.table);
        parts.push_back({{"output", x.id}, {"title", x.title}, {"table", x.table}, {"library", lib != libraries.end() ? lib->second : std::string()},
                         {"row", x.row}, {"designation", x.designation}, {"world", w}});
    }
    r["parts"] = parts;
    r["cut"] = partsOf ? "parts" : "assembly";
    return r;
}

// Connection evaluator (solutions.md §7).
EvalPtr Regenerator::evalConnection(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut) {
    const doc::Json& d = c.f.pattern()->def;
    const doc::SolutionCopy* copy = c.d.solution(d["solution"].get<std::string>());
    if (!copy) {
        c.error(Error("SOL_NOT_FOUND").with("id", d["solution"].get<std::string>()), "/solution");
        return finish(c, input);
    }
    if (auto s = solutions::checkDefinition(copy->definition); !s) {
        c.error(s.error());
        return finish(c, input);
    }
    const solutions::Definition def = solutions::parseDefinition(copy->definition);
    const doc::Json tableMap = d.contains("tables") ? d["tables"] : doc::Json::object();
    // §7.12 (FX8-BLT-002): the point input given as a sketch — a bolt at each point of the sketch; §7.11 (FX10-WZ-005):
    // the frame axis input given as a list — a bolt on each axis.
    std::vector<doc::Json> perPoint;
    bool byAxes = false;
    {
        auto pos = connectionPositions(c.d.active(), *input, def, d["inputs"], &byAxes);
        if (!pos) {
            c.error(pos.error(), "/inputs/" + def.framePoint);
            return finish(c, input);
        }
        perPoint = std::move(*pos);
    }
    const solutions::TableProvider tables = connectionTables(c.d, *copy, tableMap);
    std::vector<ConnectionPlan> plans;
    for (const auto& in : perPoint) {
        plans.push_back(planConnection(k_, def, tables, in, d["choices"], *input, c.cancel));
        const ConnectionPlan& plan = plans.back();
        if (plan.plan.error) {
            if (plan.plan.error->code == "KERNEL_CANCELLED") {
                cancelledOut = true;
                return nullptr;
            }
            c.error(*plan.plan.error);
            return finish(c, input);
        }
    }
    std::map<std::string, std::string> libraries;
    if (tableMap.is_object())
        for (auto it = tableMap.begin(); it != tableMap.end(); ++it) libraries[it.key()] = it.value().get<std::string>();
    // Parts of placed outputs: a library copy must exist (§7.6).
    for (const auto& plan : plans)
        for (const auto& p : plan.plan.parts) {
            auto lib = libraries.find(p.table);
            const doc::LibraryPart* lp = lib != libraries.end() ? c.d.libraryPart(lib->second) : nullptr;
            if (!lp || libraryCopyElement(*lp).empty()) {
                c.error(Error("SOL_LIBRARY_NOT_FOUND").with("name", p.table));
                return finish(c, input);
            }
        }
    // §7.5: cuts by parts; in parts mode (§7.10 item 3.2) — in every part of a source's part.
    const bool partsMode = d.contains("cut") && d["cut"] == "parts";
    kernel::Shape preview;
    std::vector<doc::Json> reports;
    for (size_t pk = 0; pk < plans.size(); ++pk) {
        const ConnectionPlan& plan = plans[pk];
        const std::string pointTag = pk == 0 ? std::string() : "p" + std::to_string(pk + 1);  // §7.12: side names of the k-th point
        const doc::Json report0 = connectionReport(plan, libraries);
        std::vector<std::vector<std::string>> changed;
        for (size_t i = 0; i < plan.plan.cuts.size(); ++i) {
            const auto& cut = plan.plan.cuts[i];
            const auto targets = report0["cuts"][i]["parts"].get<std::vector<std::string>>();
            changed.emplace_back();
            if (targets.empty()) continue;
            if (partsMode) {
                // Jobs (part path, tool frame, side tag): a library copy — its part only; otherwise all parts of the part.
                struct Job {
                    std::string path;
                    ConnectionFrame frame;
                    std::string tag;
                };
                std::vector<Job> jobs;
                const std::vector<AssemblyPart> partsNow = st->assemblyParts;
                for (size_t n = 0; n < targets.size(); ++n) {
                    const std::string tag = "o" + std::to_string(cut.index) + pointTag + (n == 0 ? std::string() : "t" + std::to_string(n + 1));
                    const auto sp = std::find_if(partsNow.begin(), partsNow.end(), [&](const AssemblyPart& a) { return a.path == targets[n]; });
                    if (sp == partsNow.end()) continue;
                    if (doc::isLibraryRef(sp->file)) {
                        jobs.push_back({sp->path, plan.frame, tag});
                        continue;
                    }
                    const ConnectionFrame local = moved(inverted(sp->world), plan.frame);
                    for (const auto& q : partsNow)
                        if (q.element == sp->element && q.file == sp->file) jobs.push_back({q.path, moved(q.world, local), tag});
                }
                for (const auto& job : jobs) {
                    auto r = cut.revolve ? revolveTool(k_, job.frame, cut.polygon) : boxTool(k_, job.frame, cut.box);
                    if (!r) {
                        c.kernelError(r.error());
                        if (c.cancelled) {
                            cancelledOut = true;
                            return nullptr;
                        }
                        return finish(c, input);
                    }
                    std::vector<Error> nw;
                    auto bases = naming::assignBases(naming::OpKind::Generate, c.f.id, *r, {}, &nw);
                    for (auto& w : nw) c.warning(w);
                    ToolBuild tb;
                    tb.shape = r->shape;
                    tb.bases = taggedBases(std::move(bases), c.f.id, job.tag);
                    tb.ok = true;
                    if (preview.null()) preview = r->shape;
                    std::vector<std::string> ch;
                    if (!applyPartsOperation(c, *st, "cut", tb, [&](const std::string& path) { return path == job.path; }, ch)) {
                        if (c.cancelled) {
                            cancelledOut = true;
                            return nullptr;
                        }
                        return finish(c, input, preview);
                    }
                    for (const auto& x : ch)
                        if (std::find(changed.back().begin(), changed.back().end(), x) == changed.back().end()) changed.back().push_back(x);
                }
                continue;
            }
            auto r = cut.revolve ? revolveTool(k_, plan.frame, cut.polygon) : boxTool(k_, plan.frame, cut.box);
            if (!r) {
                c.kernelError(r.error());
                if (c.cancelled) {
                    cancelledOut = true;
                    return nullptr;
                }
                return finish(c, input);
            }
            std::vector<Error> nw;
            auto bases = naming::assignBases(naming::OpKind::Generate, c.f.id, *r, {}, &nw);
            for (auto& w : nw) c.warning(w);
            ToolBuild tb;
            tb.shape = r->shape;
            tb.bases = taggedBases(std::move(bases), c.f.id, "o" + std::to_string(cut.index) + pointTag);  // F.side(s<k>/o<index>) (§7.5)
            tb.ok = true;
            if (preview.null()) preview = r->shape;
            const std::set<std::string> set(targets.begin(), targets.end());
            if (!applyPartsOperation(c, *st, "cut", tb, [&](const std::string& path) { return set.count(path) > 0; }, changed.back())) {
                if (c.cancelled) {
                    cancelledOut = true;
                    return nullptr;
                }
                return finish(c, input, preview);
            }
        }
        reports.push_back(connectionReport(plan, libraries, changed, partsMode ? input.get() : nullptr));
    }
    st->connections[c.f.id] = mergePointReports(reports, byAxes ? "axes" : "points");
    nameTableMessages(c, *input, *st);
    return finish(c, st, preview);
}

// Cut owned by a connection — a part feature (regen.md §4.19, solutions.md §7.10).
EvalPtr Regenerator::evalConnectionCut(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut) {
    const doc::Json& d = c.f.pattern()->def;
    const ConnectionFrame F = frameFromJson(d["frame"]);
    std::vector<std::pair<double, double>> rz;
    if (d.contains("revolve"))
        for (const auto& v : d["revolve"]) rz.emplace_back(v[0].get<double>(), v[1].get<double>());
    std::array<double, 6> box{};
    if (d.contains("box"))
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 2; ++b) box[size_t(2 * a + b)] = d["box"][std::string(1, char('x' + a))][b].get<double>();
    auto r = d.contains("revolve") ? revolveTool(k_, F, rz) : boxTool(k_, F, box);
    if (!r) {
        c.kernelError(r.error());
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input);
    }
    std::vector<Error> nw;
    ToolBuild tb;
    tb.shape = r->shape;
    tb.bases = naming::assignBases(naming::OpKind::Generate, c.f.id, *r, {}, &nw);
    tb.ok = true;
    for (auto& w : nw) c.warning(w);
    if (!applyOperation(c, *st, "cut", tb)) {
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input, tb.shape);
    }
    rename(k_, *st);
    // §4.19 item 4: the thread over the wall faces of the hole radius.
    if (d.contains("thread") && d.contains("revolve")) {
        double radius = 0;
        if (d.contains("diameter")) radius = d["diameter"].get<double>() / 2;
        else
            for (const auto& v : rz)
                if (v.first > 0 && (radius == 0 || v.first < radius)) radius = v.first;
        const double td = d["thread"]["d"].get<double>(), pitch = d["thread"]["pitch"].get<double>();
        double t0 = 1e300, t1 = -1e300;
        const std::string wall = c.f.id + ".side(";
        for (const auto& fe : st->names.faces) {
            if (fe.full.rfind(wall, 0) != 0) continue;
            const auto& sh = st->bodies.at(fe.body).shape;
            const auto si = k_.faceSurface(sh, fe.face);
            if (si.kind != kernel::SurfaceInfo::Kind::Cylinder || std::abs(si.radius - radius) > 1e-6) continue;
            if (auto rg = kernel::axialRange(k_, sh, fe.face, F.O, F.Z)) t0 = std::min(t0, rg->first), t1 = std::max(t1, rg->second);
        }
        if (t1 > t0) {
            std::string des = "M" + canonicalNumber(td);
            const auto spec = threadSpec(des);
            if (!spec || std::abs((*spec)["pitch"].get<double>() - pitch) > 1e-9) des += "x" + canonicalNumber(pitch);
            st->threads.push_back({c.f.id, des, true, F.O + t0 * F.Z, F.Z, radius, td, pitch,
                                   std::min(d["thread"]["length"].get<double>(), t1 - t0)});
        }
    }
    nameTableMessages(c, *input, *st);
    return finish(c, st, tb.shape);
}

}  // namespace construe::regen
