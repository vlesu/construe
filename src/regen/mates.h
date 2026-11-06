// Assembly mates (spec/assembly.md §7): reference geometry, equations, solving with the common solver.
#pragma once

#include "doc/document.h"
#include "expr/expr.h"
#include "kernel/kernel.h"
#include "regen/assembly.h"
#include "regen/assembly_ref.h"

#include <map>
#include <optional>
#include <set>
#include <string>

namespace construe::regen {

inline constexpr double MATE_TOL = 1e-6;  // assembly.md §8
inline constexpr double ASM_SPLIT_TOL = 1e-6;  // parts of a split face give one primitive (§7.2, FX10-LIB-009)

// "Pull" (assembly.md §7.6б): point `grab` of top-level instance `instance` (instance frame) is pulled towards the world
// point `target` while all mates stay satisfied.
struct MatePull {
    std::string instance;
    Vec3 grab = Vec3::Zero();
    Vec3 target = Vec3::Zero();
};

struct MateSolveOptions {
    bool translationOnly = false;  // exploded view (§7.4 "Решение в разнесённом виде"): no branch selection, rotations fixed
    std::optional<MatePull> pull;
    // Underdetermined nested assemblies (top-level paths) solved as rigid wholes in their current form (§2б: the
    // manipulator of a subassembly as a whole).
    std::set<std::string> rigid;
};

// Solve the mates of assembly asmEl: updates the world placements of instances in st, mate statuses and DOF.
// drag — dragging (§7.6): a top-level instance and its initial placement for the solve. residuals — instead of solving,
// the maximum absolute dimensionless residual of each mate with equations at the current placements (§7д.3 item 5).
void solveMates(const kernel::Kernel& k, const doc::Element& asmEl, const expr::VariableTable& vars, const expr::DocumentUnits& units,
                AssemblyState& st, const std::optional<std::pair<std::string, Motion3>>& drag = std::nullopt,
                std::map<std::string, double>* residuals = nullptr, const MateSolveOptions& opts = {});


// Elements of an assembly as evaluated in its exploded view (§7д.2 item 3): mates of exploded.suppressedMates suppressed.
doc::Element explodedMateElement(const doc::Element& asmEl);

// Face index of a mate reference name in part state st (§7.2): one face, or a face split by a later cut (SPLIT) whose
// parts all give the same primitive — then its first part; nullopt — otherwise.
std::optional<int> mateFace(const kernel::Kernel& k, const PartState& st, const std::string& name);
// Primitive of stable name `name` in part state st, part coordinates; nullopt — the name does not resolve to one entity
// or its geometry has no primitive.
std::optional<MatePrimitive> partPrimitive(const kernel::Kernel& k, const PartState& st, const std::string& name);
// Primitive of mate reference `ref` in the frame of its top-level instance and that instance's path (empty — source or
// reference geometry of the assembly, world frame); nullopt — not resolved.
std::optional<std::pair<std::string, MatePrimitive>> matePrimitive(const kernel::Kernel& k, const AssemblyState& st, const std::string& ref);

// Relative placement M_A⁻¹·M_B of the reference instances (lock, §7.4): [qw, qx, qy, qz, tx, ty, tz]; nullopt — reference not resolved.
std::optional<std::array<double, 7>> mateRelative(const kernel::Kernel& k, const AssemblyState& st, const std::string& refA,
                                                  const std::string& refB);

// Rim angles [α_A, α_B] of the gear mate (assembly.md §7.4, FX10-GEAR-004) between the pitch cylinders refA, refB at the
// current placements, rad in (−π, π]; nullopt — not resolved or not two pitch cylinders of rims (not both internal).
std::optional<std::array<double, 2>> mateGearAngles(const kernel::Kernel& k, const AssemblyState& st, const std::string& refA, const std::string& refB);

// "Turn" of a mate (assembly.md §7.4а, FX9-ASM-005): the mate has a side — both references are planes, axes or lines
// for coincident / distance / concentric, or directed for angle / parallel.
bool mateHasSide(const kernel::Kernel& k, const AssemblyState& st, const std::string& type, const std::array<std::string, 2>& refs);
// Current value of a distance or angle mate between the references at the current placements (assembly.md §7.4,
// FX9-ASM-002: the fallback type keeps the parts where they are): length in mm, angle in rad, signed as the solver's
// equation of that pair needs; nullopt — not resolved or the pair has no such mate.
std::optional<double> mateCurrentValue(const kernel::Kernel& k, const AssemblyState& st, const std::string& type,
                                       const std::array<std::string, 2>& refs);
// The free top-level instance of the mate (of the second reference, else of the first) turned once by 180° about the
// line through its reference point perpendicular to its reference direction: its path and new world placement;
// nullopt — no side or no free instance.
std::optional<std::pair<std::string, Motion3>> mateTurn(const kernel::Kernel& k, const AssemblyState& st, const std::string& type,
                                                        const std::array<std::string, 2>& refs);

// Body of the mate system carrying entry `path` (assembly.md §2б): its top-level instance or, inside an underdetermined
// nested assembly, the free unit of that assembly with the longest path that is a prefix of `path`.
std::string mateUnitOf(const AssemblyState& st, const std::string& path, const std::set<std::string>* rigid = nullptr);

// World primitive of mate reference ref in assembly state st (assembly.md §7.2, §7е.2); nullopt — not resolved.
std::optional<MatePrimitive> worldMatePrimitive(const kernel::Kernel& k, const AssemblyState& st, const std::string& ref);

}  // namespace construe::regen
