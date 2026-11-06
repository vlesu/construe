// Shared by feature evaluators (spec/regen.md §4): evaluation context and helpers. Internal header of the regen
// module: evaluators are split into eval_*.cpp files (one file per feature group, stage 2А).
#pragma once
#include "regen/regen.h"
#include "base/constants.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace construe::regen::detail {

struct Ctx {
    const kernel::Kernel& k;
    const doc::Document& d;
    const doc::Feature& f;
    std::string fpath;  // /elements/<active>/features/<i>
    const expr::VariableTable& vars;
    expr::DocumentUnits units;
    const Cancel* cancel;
    std::vector<Message> messages;
    bool cancelled = false;

    void error(Error e, const std::string& sub = {}) {
        if (!sub.empty()) e.at(fpath + sub);
        messages.push_back({true, std::move(e)});
    }
    void warning(Error e) { messages.push_back({false, std::move(e)}); }
    bool hasError() const {
        for (const auto& m : messages)
            if (m.error) return true;
        return false;
    }

    std::optional<double> field(const std::string& text, expr::Expected expected, const std::string& sub) {
        auto q = expr::evaluateField(text, expected, vars.env(), units);
        if (!q) {
            error(q.error(), sub);
            return std::nullopt;
        }
        return q->value;
    }

    // Kernel error → feature message (§4, general rules). false — evaluation stops.
    void kernelError(const Error& e) {
        if (e.code == "KERNEL_CANCELLED") {
            cancelled = true;
            return;
        }
        if (e.code == "KERNEL_INVALID_INDEX" || e.code == "KERNEL_INTERNAL") {
            Error r("REGEN_KERNEL_FAILURE");
            r.with("detail", e.code);
            error(r);
            return;
        }
        error(e);
    }
    void kernelWarnings(const std::vector<Error>& ws) {
        for (const auto& w : ws)
            if (w.code != "KERNEL_HISTORY_INCOMPLETE") warning(w);
    }
};

// Name table warnings of state out that were not present in the input (naming.md §7).
void nameTableMessages(Ctx& c, const PartState& in, const PartState& out);
// Evaluator result: state out, context messages, preview tool.
EvalPtr finish(Ctx& c, StatePtr out, kernel::Shape tool = {});
void rename(const kernel::Kernel& k, PartState& s);
std::vector<std::string> bodyKeys(const kernel::Kernel& k, const std::vector<Body>& bodies);
std::vector<Body> split(const kernel::Kernel& k, const kernel::Shape& s, const std::vector<std::string>& bases);
void assignIds(const kernel::Kernel& k, std::vector<Body>& parts, const std::string& prefix, bool sPrefix, bool singleKeepsPrefix);
naming::InputNames inputOf(const std::vector<std::string>& bases);

struct ToolBuild {
    kernel::Shape shape;
    std::vector<std::string> bases;
    bool ok = false;
};

// Union of region bodies into a tool in one operation (§4.2 item 4, FX3-PERF-001): no boolean if the bounding boxes are
// pairwise separated, otherwise a single union of the first body with a combination of the rest. bases[i] — face bases of shapes[i].
bool uniteTool(Ctx& c, const std::vector<kernel::Shape>& shapes, const std::vector<std::vector<std::string>>& bases, ToolBuild& tb);

// Build the tool from regions (extrude/revolve) §4.2 item 4.
template <class MakeOne>
ToolBuild buildTool(Ctx& c, const std::vector<const sketch::Region*>& regions, MakeOne makeOne) {
    ToolBuild tb;
    std::vector<kernel::Shape> shapes;
    std::vector<std::vector<std::string>> bases;
    for (const sketch::Region* r : regions) {
        auto res = makeOne(*r);
        if (!res) {
            c.kernelError(res.error());
            return tb;
        }
        c.kernelWarnings(res->warnings);
        std::vector<Error> nw;
        bases.push_back(naming::assignBases(naming::OpKind::Generate, c.f.id, *res, {}, &nw));
        for (auto& w : nw) c.warning(w);
        shapes.push_back(res->shape);
    }
    if (shapes.empty() || !uniteTool(c, shapes, bases, tb)) return tb;
    tb.ok = !tb.shape.null();
    return tb;
}

bool applyOperation(Ctx& c, PartState& st, const std::string& operation, const ToolBuild& tool);
// Assembly features (assembly.md §7г): the operation by parts of the virtual assembly state; the "through all" bound —
// the bodies of the feature's targets (all bodies of a part state).
bool applyAssemblyOperation(Ctx& c, PartState& st, const std::string& operation, const ToolBuild& tool);
std::vector<kernel::Shape> operationBounds(Ctx& c, const PartState& st);
// The assembly hole "in the parts" (assembly.md §7г.6, FX8-ASM-007): the cut by the targets (sources — the changed
// parts), then in every other part of the same element and file (not a library copy) with the tool moved by
// M_q·M_p⁻¹; changedAll — all changed parts.
bool applyPartsHoleOperation(Ctx& c, PartState& st, const ToolBuild& tool, std::vector<std::string>& sources,
                             std::vector<std::string>& changedAll);
// The operation by parts (assembly.md §7г.3 item 2) over the parts for which target(path) holds; changedOut — paths of the
// changed parts in part order; no warnings for unchanged parts (connections, solutions.md §7.5).
bool applyPartsOperation(Ctx& c, PartState& st, const std::string& operation, const ToolBuild& tool,
                         const std::function<bool(const std::string&)>& target, std::vector<std::string>& changedOut);
std::optional<naming::EntityRef> resolveOne(Ctx& c, const PartState& in, const std::string& name, naming::EntityKind kind,
                                            const std::string& sub);
// A list of references of one field (naming.md §5.4 "Общие правила"; fillet/chamfer edges, shell/draft faces): each name
// is resolved in document order, SPLIT gives all parts with the warning NAMING_REF_SPLIT, then referenceWarnings — both
// into c.messages, name by name. Any LOST — error REGEN_REF_LOST (ids — all lost names) at sub; otherwise any AMBIGUOUS —
// REGEN_REF_AMBIGUOUS (ids — all such names) at sub. refs — the resolved names with their entities, in input order.
struct ResolvedRef {
    std::string name;
    std::vector<naming::EntityRef> entities;
};
struct ResolvedRefs {
    std::vector<ResolvedRef> refs;
    std::vector<std::string> lost, ambiguous;
};
ResolvedRefs resolveMany(Ctx& c, const PartState& in, const std::vector<std::string>& names, naming::EntityKind kind,
                         const std::string& sub);
Error refGeometry(const std::string& name, const std::string& detail);
std::optional<Plane> refPlane(Ctx& c, const PartState& in, const doc::Json& r, const std::string& sub);
std::optional<RefAxis> refLine(Ctx& c, const PartState& in, const doc::Json& r, const std::string& sub);
std::optional<Point3> refPoint(Ctx& c, const PartState& in, const doc::Json& r, const std::string& sub);
bool evalPattern(Ctx& c, const PartState& in, PartState& st, kernel::Shape& preview);
// Pattern instance (§4.7 item 4): label and motion (a reflection for the mirror, §4.8).
struct PatternInstance {
    std::string label;
    kernel::Motion m;
};
std::vector<std::pair<std::string, size_t>> patternSourceIds(Ctx& c);
std::optional<std::vector<PatternInstance>> patternInstances(Ctx& c, const PartState& in);
bool evalRefGeometry(Ctx& c, const PartState& in, PartState& st, const doc::Feature& f);
const SketchEntry* sketchOf(Ctx& c, const PartState& input, const std::string& id);
std::vector<const sketch::Region*> selectRegions(Ctx& c, const SketchEntry& se, const doc::Profiles& p, const std::string& sketchId);

}  // namespace construe::regen::detail
