// Imported body (spec/regen.md, import evaluator).
#include "regen/eval_common.h"

namespace construe::regen {

using namespace detail;

EvalPtr Regenerator::evalImport(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& /*cancelledOut*/) {
    [[maybe_unused]] const doc::Feature& f = c.f;
    // §4.15: shape from BREP text, face bases F.face(i + 1), bodies — as new bodies of the feature.
    const doc::Json& d = f.pattern()->def;
    auto shape = k_.shapeFromBrep(d.value("brep", std::string()));
    if (!shape) {
        c.error(shape.error(), "/brep");
        return finish(c, input);
    }
    const int nf = k_.counts(*shape).faces;
    std::vector<std::string> bases;
    for (int i = 0; i < nf; ++i) bases.push_back(f.id + ".face(" + std::to_string(i + 1) + ")");
    auto parts = split(k_, *shape, bases);
    assignIds(k_, parts, "body:" + f.id, false, true);
    for (auto& p : parts) st->bodies.push_back(p);
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st);
}

}  // namespace construe::regen
