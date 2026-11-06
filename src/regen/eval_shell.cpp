// Shell and draft (spec/regen.md §4.9, §4.10).
#include "regen/eval_common.h"

namespace construe::regen {

using namespace detail;

EvalPtr Regenerator::evalShellDraft(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut) {
    [[maybe_unused]] const doc::Feature& f = c.f;
    [[maybe_unused]] const Cancel* cancel = c.cancel;
    // §4.9, §4.10: faces by bodies.
    const doc::Json& d = f.pattern()->def;
    std::map<int, std::vector<int>> perBody;
    const auto resolved = resolveMany(c, *input, d["faces"].get<std::vector<std::string>>(), naming::EntityKind::Face, "/faces");
    for (const auto& ref : resolved.refs)
        for (const auto& e : ref.entities) {
            const auto& fe = input->names.faces.at(e.index);
            auto& lst = perBody[fe.body];
            if (std::find(lst.begin(), lst.end(), fe.face) == lst.end()) lst.push_back(fe.face);
        }
    if (c.hasError()) return finish(c, input);
    // Operation on a single body (draft or shell); nullopt — value error, the message is already recorded.
    std::function<Result<kernel::OpResult>(const Body&, const std::vector<int>&)> op;
    if (f.type == doc::FeatureType::Draft) {
        const auto pl = refPlane(c, *input, d["neutral"], "/neutral");
        const auto a = c.field(d["angle"].get<std::string>(), expr::ANGLE, "/angle");
        if (a && !(*a > 0 && *a < PI / 2)) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("angle")).with("actual", *a), "/angle");
            return finish(c, input);
        }
        if (!pl || !a) return finish(c, input);
        const Vec3 pull = pl->normal.normalized() * (d.contains("reverse") && d["reverse"].get<bool>() ? -1.0 : 1.0);
        const Point3 p0 = pl->origin;
        const double ang = *a;
        op = [this, pull, p0, ang, cancel](const Body& b, const std::vector<int>& faces) {
            return k_.draft(b.shape, faces, p0, pull, ang, cancel);
        };
    }
    if (!op) {
        const auto t = c.field(d["thickness"].get<std::string>(), expr::LENGTH, "/thickness");
        if (!t) return finish(c, input);
        if (*t <= 0) {
            c.error(Error("REGEN_BAD_VALUE").with("name", std::string("thickness")).with("actual", *t), "/thickness");
            return finish(c, input);
        }
        const bool outward = d.contains("outward") && d["outward"].get<bool>();
        const double th = *t;
        op = [this, th, outward, cancel](const Body& b, const std::vector<int>& faces) {
            return k_.shell(b.shape, faces, th, outward, cancel);
        };
    }
    for (auto& [bi, faces] : perBody) {
        Body& b = st->bodies.at(bi);
        auto r = op(b, faces);
        if (!r) {
            c.kernelError(r.error());
            if (c.cancelled) {
                cancelledOut = true;
                return nullptr;
            }
            return finish(c, input);
        }
        c.kernelWarnings(r->warnings);
        // Names of input edges and vertices — for wall fillet faces at concave edges (FromEdge, FromVertex; naming.md §4.1).
        naming::InputNames in = inputOf(b.bases);
        for (const auto& ee : input->names.edges)
            if (ee.body == bi) {
                if (static_cast<int>(in.edgeNames.size()) <= ee.logical) in.edgeNames.resize(ee.logical + 1);
                in.edgeNames[ee.logical] = ee.full;
            }
        for (const auto& ve : input->names.vertices)
            if (ve.body == bi) in.vertexNames[ve.vertex] = ve.full;
        std::vector<Error> nw;
        b.bases = naming::assignBases(naming::OpKind::Boolean, f.id, *r, {in}, &nw);
        for (auto& w : nw) c.warning(w);
        b.shape = r->shape;
    }
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st);
}

}  // namespace construe::regen
