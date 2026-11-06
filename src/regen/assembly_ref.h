// Assembly references (spec/assembly.md §3.1, S7F-RF-010): the one parser of "<path>/<name>" references to objects of
// an assembly and of its instances.
#pragma once

#include "regen/assembly.h"

#include <string>

namespace construe::regen {

// Parsed assembly reference (assembly.md §3.1).
struct AssemblyRef {
    enum class Kind {
        Own,     // no "/": source, reference geometry or a sketch of the assembly itself; leaf — the whole reference
        Part,    // object of a part instance: entry — the part entry, leaf — stable or construction name
        Nested,  // reference plane, axis or sketch of a nested assembly: entry — the assembly node, leaf — construction name
        Invalid  // anything else; entry/path — the deepest existing entry among the prefixes (for captions), may be empty
    };
    Kind kind = Kind::Invalid;
    std::string path;                     // entry path (empty for Own)
    std::string leaf;                     // name after the path (Own — the whole reference, Invalid — empty)
    const InstanceEntry* entry = nullptr;
    bool construction = false;            // Part: leaf is a construction element of the part (isConstructionName)
};

// Parse ref against the evaluated assembly st (assembly.md §3.1 "Разбор по состоянию").
AssemblyRef parseAssemblyRef(const AssemblyState& st, const std::string& ref);

// Top-level instance of a reference: the part before the first "/"; empty — no "/" (own geometry of the assembly).
std::string refTopInstance(const std::string& ref);

// The name after an instance path is a construction element of the part (assembly.md §7.1, FX9-ASM-001): origin,
// plane:*, axis:*, a feature ID or "<sketch ID>:<entity ID>" — not a stable name of the body.
bool isConstructionName(const std::string& name);

}  // namespace construe::regen
