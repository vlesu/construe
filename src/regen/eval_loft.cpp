// Loft (spec/regen.md, loft evaluator).
#include "regen/eval_common.h"

namespace construe::regen {

using namespace detail;

EvalPtr Regenerator::evalLoft(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut) {
    [[maybe_unused]] const doc::Feature& f = c.f;
    [[maybe_unused]] const Cancel* cancel = c.cancel;
    // §4.12
    const doc::Json& d = f.pattern()->def;
    std::vector<kernel::Profile> profiles;
    for (size_t i = 0; i < d["sections"].size(); ++i) {
        const doc::Json& sec = d["sections"][i];
        const std::string sid = sec["sketch"];
        const std::string sub = "/sections/" + std::to_string(i);
        auto it = input->sketches.find(sid);
        if (it == input->sketches.end()) {
            c.error(Error("REGEN_INPUT_ERROR").with("id", sid), sub + "/sketch");
            continue;
        }
        const sketch::Region* reg = nullptr;
        if (sec.contains("region")) {
            reg = it->second.result.profiles.find(sec["region"].get<std::string>());
            if (!reg) {
                c.error(Error("REGEN_PROFILE_MISSING").with("id", sid).with("ids", std::vector<std::string>{sec["region"]}), sub + "/region");
                continue;
            }
        } else {
            int n = 0;
            for (const auto& r : it->second.result.profiles.regions)
                if (r.depth % 2 == 0) ++n, reg = &r;
            if (n != 1) {
                c.error(Error("REGEN_LOFT_SECTION").with("id", sid).with("actual", double(n)), sub);
                continue;
            }
        }
        if (!reg->holes.empty()) {
            c.error(Error("REGEN_LOFT_SECTION").with("id", sid).with("detail", std::string("holes")), sub);
            continue;
        }
        profiles.push_back(toKernelProfile(*reg, it->second.frame));
        if (sec.contains("start") && sec["start"].is_string()) {  // the section's start point (FX10-PT-004)
            const auto& P = it->second.result.state.points;
            const auto pt = P.find(sec["start"].get<std::string>());
            if (pt == P.end()) {
                c.error(Error("REGEN_LOFT_SECTION").with("id", sid).with("detail", std::string("start")), sub + "/start");
                continue;
            }
            profiles.back().start = it->second.frame.toWorld(pt->second);
        }
    }
    if (c.hasError()) return finish(c, input);
    auto lr = k_.loft(profiles, cancel);
    if (!lr) {
        c.kernelError(lr.error());
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input);
    }
    c.kernelWarnings(lr->warnings);
    ToolBuild tool;
    std::vector<Error> nw;
    tool.bases = naming::assignBases(naming::OpKind::Generate, f.id, *lr, {}, &nw);
    for (auto& w : nw) c.warning(w);
    tool.shape = lr->shape;
    tool.ok = true;
    const std::string op = d["operation"];
    if (!applyOperation(c, *st, op, tool)) {
        if (c.cancelled) {
            cancelledOut = true;
            return nullptr;
        }
        return finish(c, input, tool.shape);
    }
    if (!c.hasError()) st->tools[f.id] = {tool.shape, tool.bases, op == "new" ? "add" : op};
    rename(k_, *st);
    nameTableMessages(c, *input, *st);
    return finish(c, st, tool.shape);
}

}  // namespace construe::regen
