// Assembly cut and hole (spec/assembly.md §7г): the operation by parts of the virtual assembly state — each target
// instance's part state is changed in part coordinates; part documents are not touched.
#include "regen/eval_common.h"

#include "base/hash.h"

namespace construe::regen {

namespace {

kernel::Motion kernelMotion(const Motion3& M) {
    kernel::Motion km;
    for (int i = 0; i < 9; ++i) km.r[size_t(i)] = M.R(i / 3, i % 3);
    km.t = M.t;
    return km;
}

Motion3 inverse(const Motion3& M) { return {M.R.transpose(), -(M.R.transpose() * M.t)}; }

// Feature targets (§7г.1): instance paths; empty — "all".
std::vector<std::string> featureTargets(const doc::Feature& f) {
    if (const auto* x = f.extrude()) return x->targets;
    std::vector<std::string> out;
    if (const auto* p = f.pattern(); p && p->def.contains("targets") && p->def["targets"].is_array())
        for (const auto& t : p->def["targets"])
            if (t.is_string()) out.push_back(t.get<std::string>());
    return out;
}

bool covers(const std::string& target, const std::string& path) {
    return path == target || (path.size() > target.size() && path.compare(0, target.size(), target) == 0 && path[target.size()] == '/');
}

bool isTarget(const std::vector<std::string>& targets, const std::string& path) {
    if (targets.empty()) return true;
    for (const auto& t : targets)
        if (covers(t, path)) return true;
    return false;
}

bool separated(const BBox3& a, const BBox3& b) {
    if (a.isEmpty() || b.isEmpty()) return true;
    for (int k = 0; k < 3; ++k)
        if (a.max()(k) + REGEN_TOOL_GAP < b.min()(k) || b.max()(k) + REGEN_TOOL_GAP < a.min()(k)) return true;
    return false;
}

}  // namespace

void rebuildAssemblyState(const kernel::Kernel& k, PartState& v, const std::map<std::string, kernel::Shape>& world) {
    v.bodies.clear();
    v.names = naming::NameTable{};
    for (const auto& p : v.assemblyParts) appendInstanceState(k, v, p.path, *p.local, p.world, &world);
}

AssemblyPtr assemblyWithFeatures(const AssemblyPtr& a, const PartState& final, uint64_t finalKey) {
    if (!a) return a;
    std::shared_ptr<AssemblyState> out;
    for (size_t i = 0; i < a->instances.size(); ++i) {
        const InstanceEntry& e = a->instances[i];
        if (!e.state) continue;
        for (const auto& p : final.assemblyParts) {
            if (p.path != e.path || p.local == e.state) continue;
            if (!out) out = std::make_shared<AssemblyState>(*a);
            out->instances[i].state = p.local;
            out->instances[i].stateKey = construe::hashCombine(e.stateKey, finalKey);
        }
    }
    return out ? AssemblyPtr(out) : a;
}

namespace detail {

// §7г.3 item 3: targets without a part — warnings (once per evaluation).
void targetWarnings(Ctx& c, const PartState& st, const std::vector<std::string>& targets) {
    for (const auto& m : c.messages)
        if (m.e.code == "ASM_TARGET_NOT_FOUND") return;
    for (const auto& t : targets) {
        bool found = false;
        for (const auto& p : st.assemblyParts) found = found || covers(t, p.path);
        if (!found) c.warning(Error("ASM_TARGET_NOT_FOUND").with("ref", t));
    }
}

std::vector<kernel::Shape> operationBounds(Ctx& c, const PartState& st) {
    std::vector<kernel::Shape> out;
    if (st.assemblyParts.empty()) {
        for (const auto& b : st.bodies) out.push_back(b.shape);
        return out;
    }
    const auto targets = featureTargets(c.f);
    targetWarnings(c, st, targets);
    for (const auto& b : st.bodies)
        if (isTarget(targets, b.id.substr(0, b.id.rfind('/')))) out.push_back(b.shape);
    return out;
}

bool applyAssemblyOperation(Ctx& c, PartState& st, const std::string& operation, const ToolBuild& tool) {
    const auto targets = featureTargets(c.f);
    targetWarnings(c, st, targets);
    std::vector<std::string> changed;
    if (!applyPartsOperation(c, st, operation, tool, [&](const std::string& path) { return isTarget(targets, path); }, changed)) return false;
    if (changed.empty()) c.warning(Error("REGEN_NO_EFFECT").with("name", operation));
    return true;
}

bool applyPartsOperation(Ctx& c, PartState& st, const std::string& operation, const ToolBuild& tool,
                         const std::function<bool(const std::string&)>& target, std::vector<std::string>& changedOut) {
    std::set<std::string> changed;
    for (auto& part : st.assemblyParts) {
        if (!target(part.path)) continue;
        auto tl = c.k.transform(tool.shape, kernelMotion(inverse(part.world)));
        if (!tl) {
            c.kernelError(tl.error());
            return false;
        }
        BBox3 box;
        for (const auto& b : part.local->bodies) box.extend(c.k.bbox(b.shape));
        if (separated(c.k.bbox(tl->shape), box)) continue;
        // The part feature operation (regen.md §4.2 item 5) in part coordinates; "no effect" of a single part is not a message.
        auto local = std::make_shared<PartState>(*part.local);
        Ctx sub{c.k, c.d, c.f, c.fpath, c.vars, c.units, c.cancel, {}, false};
        ToolBuild tb;
        tb.shape = tl->shape;
        tb.bases = tool.bases;
        tb.ok = true;
        const bool ok = applyOperation(sub, *local, operation, tb);
        bool noEffect = false;
        for (auto& m : sub.messages) {
            if (m.e.code == "REGEN_NO_EFFECT" || m.e.code == "REGEN_NO_TARGET") {
                noEffect = true;
                continue;
            }
            c.messages.push_back(std::move(m));
        }
        if (sub.cancelled) {
            c.cancelled = true;
            return false;
        }
        if (!ok && !noEffect) return false;
        if (noEffect) continue;
        rename(c.k, *local);
        nameTableMessages(c, *part.local, *local);
        part.local = local;
        changed.insert(part.path);
        changedOut.push_back(part.path);
    }
    // Bodies of unchanged parts keep their world shapes (§7г.2).
    std::map<std::string, kernel::Shape> world;
    for (const auto& b : st.bodies)
        if (!changed.count(b.id.substr(0, b.id.rfind('/')))) world[b.id] = b.shape;
    rebuildAssemblyState(c.k, st, world);
    return true;
}

bool applyPartsHoleOperation(Ctx& c, PartState& st, const ToolBuild& tool, std::vector<std::string>& sources,
                             std::vector<std::string>& changedAll) {
    const auto targets = featureTargets(c.f);
    targetWarnings(c, st, targets);
    if (!applyPartsOperation(c, st, "cut", tool, [&](const std::string& path) { return isTarget(targets, path); }, sources)) return false;
    if (sources.empty()) c.warning(Error("REGEN_NO_EFFECT").with("name", std::string("cut")));
    changedAll = sources;
    // The same part elsewhere in the assembly (§7г.6): its hole is a feature of the part — all its instances have it.
    const std::vector<AssemblyPart> parts = st.assemblyParts;
    for (const auto& src : sources) {
        const auto sp = std::find_if(parts.begin(), parts.end(), [&](const AssemblyPart& a) { return a.path == src; });
        if (sp == parts.end() || doc::isLibraryRef(sp->file)) continue;
        for (const auto& q : parts) {
            if (q.element != sp->element || q.file != sp->file) continue;
            if (std::find(changedAll.begin(), changedAll.end(), q.path) != changedAll.end()) continue;
            auto moved = c.k.transform(tool.shape, kernelMotion(q.world * inverse(sp->world)));
            if (!moved) {
                c.kernelError(moved.error());
                return false;
            }
            ToolBuild tb = tool;
            tb.shape = moved->shape;
            std::vector<std::string> ch;
            if (!applyPartsOperation(c, st, "cut", tb, [&](const std::string& path) { return path == q.path; }, ch)) return false;
            for (const auto& x : ch) changedAll.push_back(x);
        }
    }
    return true;
}

}  // namespace detail

}  // namespace construe::regen
