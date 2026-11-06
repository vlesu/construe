// Assembly references (regen/assembly_ref.h, spec/assembly.md §3.1).
#include "regen/assembly_ref.h"

#include <algorithm>
#include <cctype>

namespace construe::regen {

bool isConstructionName(const std::string& name) {
    if (name == "origin" || name.rfind("plane:", 0) == 0 || name.rfind("axis:", 0) == 0) return true;
    // A feature ID (reference plane, axis, sketch) and "<sketch ID>:<entity ID>": no '.', '(' or '{' of stable names.
    if (name.empty() || name.find_first_of(".({}|~") != std::string::npos) return false;
    const size_t colon = name.find(':');
    if (colon != std::string::npos && (colon == 0 || colon + 1 == name.size() || name.find(':', colon + 1) != std::string::npos)) return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':'; });
}

AssemblyRef parseAssemblyRef(const AssemblyState& st, const std::string& ref) {
    AssemblyRef r;
    if (ref.find('/') == std::string::npos) {
        r.kind = ref.empty() ? AssemblyRef::Kind::Invalid : AssemblyRef::Kind::Own;
        r.leaf = r.kind == AssemblyRef::Kind::Own ? ref : std::string();
        return r;
    }
    // The longest prefix that is the path of a part entry with a state; the deepest existing entry for Invalid.
    const InstanceEntry* part = nullptr;
    const InstanceEntry* deepest = nullptr;
    size_t partEnd = 0;
    for (size_t pos = ref.find('/'); pos != std::string::npos; pos = ref.find('/', pos + 1)) {
        const InstanceEntry* e = st.find(ref.substr(0, pos));
        if (!e) continue;
        deepest = e;
        if (e->state && e->kind != "assembly") part = e, partEnd = pos;
    }
    if (part && partEnd + 1 < ref.size()) {
        r.kind = AssemblyRef::Kind::Part;
        r.entry = part;
        r.path = part->path;
        r.leaf = ref.substr(partEnd + 1);
        r.construction = isConstructionName(r.leaf);
        return r;
    }
    // Reference plane, axis or visible sketch of a nested assembly (§7.2, FX9-ASM-014): "<node>/<construction name>".
    const size_t sl = ref.rfind('/');
    if (const InstanceEntry* ne = st.find(ref.substr(0, sl)); ne && ne->kind == "assembly" && isConstructionName(ref.substr(sl + 1))) {
        r.kind = AssemblyRef::Kind::Nested;
        r.entry = ne;
        r.path = ne->path;
        r.leaf = ref.substr(sl + 1);
        return r;
    }
    r.entry = deepest;
    if (deepest) r.path = deepest->path;
    return r;
}

std::string refTopInstance(const std::string& ref) {
    const size_t sl = ref.find('/');
    return sl == std::string::npos ? std::string() : ref.substr(0, sl);
}

}  // namespace construe::regen
