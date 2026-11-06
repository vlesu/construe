// Edge fillet and chamfer (spec/regen.md, edge feature evaluator).
#include "regen/eval_common.h"

namespace construe::regen {

using namespace detail;

namespace {

// Inner wall edge of a shell paired with a finished outer edge (§4.4 item 3a, FX5-FEAT-001).
struct WallEdge {
    kernel::EdgeRef ref;  // inner logical edge with its own size; label — name of the outer edge
};

// Q lies opposite face f across the wall: the nearest point C of f is a foot of the perpendicular (Q − C = −t·n(C)).
// Returns t and the outward normal n(C), or nullopt.
std::optional<std::pair<double, Vec3>> wallFoot(const kernel::Kernel& k, const kernel::Shape& s, int face, const Point3& q) {
    const Point3 c = k.closestPointOnFace(s, face, q);
    const double t = (q - c).norm();
    if (t < kernel::KERNEL_MIN_DIMENSION) return std::nullopt;
    const Vec3 n = k.faceNormalAt(s, face, c).normalized();
    if (((q - c) + t * n).norm() > REGEN_WALL_TOL * std::max(1.0, t)) return std::nullopt;
    return std::make_pair(t, n);
}

struct Wall {
    std::vector<WallEdge> pairs;             // inner edges paired with the refs, with their sizes
    std::map<int, std::string> innerEdgeOf;  // every inner wall edge with an outer partner → full name of the partner
};

// Inner wall edges of body bi (input state) and their pairs among the refs (§4.4 item 3a).
Wall wallEdges(const kernel::Kernel& k, const naming::NameTable& names, int bi, const kernel::Shape& shape,
               const std::vector<kernel::EdgeRef>& refs, double size, bool isFillet) {
    Wall w;
    std::map<int, std::string> outerBase;  // inner face (index into names.faces) → outer base
    for (size_t i = 0; i < names.faces.size(); ++i)
        if (names.faces[i].body == bi)
            if (auto o = naming::outerBaseOfInner(names.faces[i].base)) outerBase[static_cast<int>(i)] = *o;
    if (outerBase.empty()) return w;
    // Partner of each inner edge: the outer edge between faces with the outer bases of its faces, opposite across the
    // wall; several — the smallest wall.
    struct Partner {
        const naming::EdgeEntry* outer = nullptr;
        double t = 0, cosPhi = 0;
    };
    std::map<int, Partner> partner;  // inner logical edge → partner
    for (const auto& ie : names.edges) {
        if (ie.body != bi || ie.kernelEdges.empty() || !outerBase.count(ie.f1) || !outerBase.count(ie.f2)) continue;
        const std::string& oa = outerBase.at(ie.f1);
        const std::string& ob = outerBase.at(ie.f2);
        const Point3 q = k.edgeMidpoint(shape, ie.kernelEdges[ie.kernelEdges.size() / 2]);
        Partner best;
        for (const auto& oe : names.edges) {
            if (oe.body != bi) continue;
            const auto& A = names.faces.at(oe.f1);
            const auto& B = names.faces.at(oe.f2);
            if (!((A.base == oa && B.base == ob) || (A.base == ob && B.base == oa))) continue;
            const auto fa = wallFoot(k, shape, A.face, q), fb = wallFoot(k, shape, B.face, q);
            if (!fa || !fb || std::abs(fa->first - fb->first) > REGEN_WALL_TOL * std::max(1.0, fa->first)) continue;
            const double t = (fa->first + fb->first) / 2;
            if (!best.outer || t < best.t) best = {&oe, t, std::clamp(fa->second.dot(fb->second), -1.0, 1.0)};
        }
        if (best.outer) {
            partner[ie.logical] = best;
            w.innerEdgeOf[ie.logical] = best.outer->full;
        }
    }
    std::set<int> taken;
    for (const auto& r : refs) taken.insert(r.edge);
    for (const auto& r : refs) {
        // The pair of the ref — its inner partner edge not referenced and not taken (the smallest wall).
        int bestInner = -1;
        for (const auto& [ie, p] : partner)
            if (p.outer->logical == r.edge && !taken.count(ie) && (bestInner < 0 || p.t < partner.at(bestInner).t)) bestInner = ie;
        if (bestInner < 0) continue;
        taken.insert(bestInner);
        const Partner& p = partner.at(bestInner);
        // Fillet: concentric inner radius R − t. Chamfer: the inner face is the outer chamfer face offset by t; φ — angle
        // between the outward normals of the outer faces: d' = d + (t − t / cos(φ/2)) / sin(φ/2) (90°: d − (2 − √2)·t).
        double inner = size - p.t;
        if (!isFillet) {
            const double half = std::acos(p.cosPhi) / 2;
            if (std::sin(half) < 1e-6) continue;  // tangent faces — not a corner
            inner = size + (p.t - p.t / std::cos(half)) / std::sin(half);
        }
        if (inner < kernel::KERNEL_MIN_DIMENSION) continue;  // the inner corner stays sharp
        w.pairs.push_back({{bestInner, r.label, inner}});
    }
    return w;
}

}  // namespace

EvalPtr Regenerator::evalEdgeFeature(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut, const doc::EdgeFeatureData* ef) {
    [[maybe_unused]] const doc::Feature& f = c.f;
    [[maybe_unused]] const Cancel* cancel = c.cancel;
    // §4.4 (1б)
    const bool isFillet = f.type == doc::FeatureType::Fillet;
    std::map<int, std::vector<kernel::EdgeRef>> perBody;
    std::set<std::pair<int, int>> seen;
    const auto resolved = resolveMany(c, *input, ef->edges, naming::EntityKind::Edge, "/edges");
    for (const auto& ref : resolved.refs)
        for (const auto& e : ref.entities) {
            const auto& ee = input->names.edges.at(e.index);
            if (seen.insert({ee.body, ee.logical}).second) perBody[ee.body].push_back({ee.logical, ref.name});
        }
    if (c.hasError()) return finish(c, input);
    const auto size = c.field(ef->size, expr::LENGTH, isFillet ? "/radius" : "/distance");
    if (!size) return finish(c, input);
    if (*size <= 0) {
        c.error(Error("REGEN_BAD_VALUE").with("name", std::string(isFillet ? "radius" : "distance")).with("actual", *size),
                isFillet ? "/radius" : "/distance");
        return finish(c, input);
    }
    // Chamfer by a distance and an angle or by two distances (§4.4 item 2а, FX7-PT-010).
    double size2 = 0, angle = 0;
    if (!isFillet && !ef->mode.empty()) {
        const bool byAngle = ef->mode == "distanceAngle";
        const std::string sub = byAngle ? "/angle" : "/distance2";
        const auto v = c.field(ef->size2, byAngle ? expr::Expected(expr::ANGLE) : expr::Expected(expr::LENGTH), sub);
        if (!v) return finish(c, input);
        if (byAngle ? !(*v > 0 && *v < PI / 2) : *v <= 0) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string(byAngle ? "angle" : "distance2")).with("actual", *v), sub);
            return finish(c, input);
        }
        (byAngle ? angle : size2) = *v;
    }
    // Sheet part: corners through the thickness of the sheet body (§4.16 item 5б, FX8-SH-004).
    if (sheetIntact(*input)) {
        int sheetBody = -1;
        for (size_t i = 0; i < input->bodies.size(); ++i)
            if (input->bodies[i].id == input->sheet->body) sheetBody = int(i);
        if (perBody.count(sheetBody)) {
            std::vector<SheetEdgeRef> edges;
            for (const auto& [bi, refs] : perBody)
                for (const auto& r : refs) {
                    if (bi != sheetBody) {
                        c.error(Error("SHEET_EDGE_NOT_THROUGH").with("name", r.label), "/edges");
                        return finish(c, input);
                    }
                    edges.push_back({bi, r.edge, r.label, "/edges"});
                }
            return evalSheetEdge(c, input, st, cancelledOut, edges, *size, size2, angle, ef->flip);
        }
    }
    std::vector<Body> bodies = st->bodies;
    for (auto& [bi, refs] : perBody) {
        Body& b = bodies.at(bi);
        for (auto& r : refs) r.size2 = size2, r.angle = angle, r.flip = ef->flip;
        // §4.4 item 3a: shell wall — inner edges paired with the outer ones get their own size in the same operation
        // (an equal-distance chamfer only).
        const Wall wall = size2 > 0 || angle > 0 ? Wall{} : wallEdges(k_, input->names, bi, b.shape, refs, *size, isFillet);
        std::vector<kernel::EdgeRef> all = refs;
        for (const auto& w : wall.pairs) all.push_back(w.ref);
        auto op = [&](const std::vector<kernel::EdgeRef>& e) {
            return isFillet ? k_.fillet(b.shape, e, *size, cancel) : k_.chamfer(b.shape, e, *size, cancel);
        };
        auto r = op(all);
        bool wallKept = !wall.pairs.empty();
        if (!r && wallKept && r.error().code != "KERNEL_CANCELLED") {
            // The kernel could not finish the inner edges — only the outer ones, with a warning (the wall is thinner).
            r = op(refs);
            wallKept = false;
            if (r) {
                std::vector<std::string> ids;
                for (const auto& w : wall.pairs)
                    if (std::find(ids.begin(), ids.end(), w.ref.label) == ids.end()) ids.push_back(w.ref.label);
                c.warning(Error("REGEN_WALL_NOT_KEPT").with("ids", ids));
            }
        }
        if (!r) {
            c.kernelError(r.error());
            if (c.cancelled) {
                cancelledOut = true;
                return nullptr;
            }
            return finish(c, input);
        }
        c.kernelWarnings(r->warnings);
        naming::InputNames in;
        in.bases = b.bases;
        for (const auto& ee : input->names.edges)
            if (ee.body == bi) {
                if (static_cast<int>(in.edgeNames.size()) <= ee.logical) in.edgeNames.resize(ee.logical + 1);
                in.edgeNames[ee.logical] = ee.full;
            }
        for (const auto& ve : input->names.vertices)
            if (ve.body == bi) in.vertexNames[ve.vertex] = ve.full;
        if (wallKept) in.innerEdgeOf = wall.innerEdgeOf;
        std::vector<Error> nw;
        const auto bases = naming::assignBases(isFillet ? naming::OpKind::Fillet : naming::OpKind::Chamfer, f.id, *r, {in}, &nw);
        for (auto& w : nw) c.warning(w);
        auto parts = split(k_, r->shape, bases);
        assignIds(k_, parts, b.id, true, true);
        b = parts.front();
        for (size_t pi = 1; pi < parts.size(); ++pi) bodies.push_back(parts[pi]);
    }
    st->bodies = bodies;
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st);
}

}  // namespace construe::regen
