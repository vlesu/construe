// Instance patterns and the exploded view of an assembly (spec/assembly.md §7д, §7е; FX7-ASM-005, FX7-ASM-007).
#pragma once

#include "doc/document.h"
#include "expr/expr.h"
#include "kernel/kernel.h"
#include "regen/assembly.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace construe::regen {

inline constexpr double EXPLODE_GAP = 0.2;            // fraction of the assembly diagonal (assembly.md §8)
inline constexpr double EXPLODE_SCALE = 1.0;          // extra distance per distance from the center (§8)
inline constexpr double EXPLODE_RESIDUAL_TOL = 1e-6;  // dimensionless mate residual (§8)
inline constexpr double EXPLODE_FASTENER_GAP = 2.0;   // fastener gap in its diameters (§7д.5, FX10-ASM-004)
inline constexpr double EXPLODE_FASTENER_TOL = 0.01;  // mm: one axis line of a stack, bearing on a face plane (§7д.5)
// Kinds of library fasteners: the last segment of the package id after '.' (library.md §14.1; §7д.5).
inline constexpr const char* EXPLODE_FASTENER_KINDS[] = {"bolt", "screw", "nut", "washer", "stud"};

// Child instances of the instance patterns of assembly asmEl (§7е.3): appended to st.instances after the mates are
// solved; statuses — st.patterns.
void evaluateInstancePatterns(const kernel::Kernel& k, const doc::Element& asmEl, const expr::VariableTable& vars,
                              const expr::DocumentUnits& units, AssemblyState& st);

// Number of child instances of pattern p without geometry (§7е.3, for the specification): partDoc — the document and
// element of the part instance `feature.instance` (feature mode), resolved by the caller; nullptr — not found.
using PatternPartLookup = std::function<std::pair<const doc::Document*, const doc::Element*>(const std::string& instancePath)>;
int patternChildCount(const doc::Element& asmEl, const doc::InstancePattern& p, const expr::VariableTable& vars,
                      const expr::DocumentUnits& units, const PatternPartLookup& part);

// Automatic explosion (§7д.3) of the assembled state st (after the mates): top-level placements X and the mates
// suppressed in the mode S.
struct ExplodedStart {
    std::map<std::string, Motion3> placements;
    std::vector<std::string> suppressedMates;
};
ExplodedStart autoExplode(const kernel::Kernel& k, const doc::Element& asmEl, const expr::VariableTable& vars,
                          const expr::DocumentUnits& units, const AssemblyState& st);

// Fasteners of the exploded view (§7д.5, FX10-ASM-004): derived connection instances and top-level library instances of
// fastener packages; each is moved along its axis (+Y of the part) by its shift, following the displacement of the
// instance it bears on.
struct ExplodedFastener {
    std::string path;      // entry path
    std::string attached;  // top-level instance it bears on (§7д.5 item 3); empty — none
    Vec3 shift = Vec3::Zero();  // world translation (§7д.5 item 2)
};
bool isLibraryFastener(const InstanceEntry& e);
// The fasteners of state st (assembled placements) with their attachments and shifts; asmEl — for the mate fallback of
// the attachment (nullptr — none).
std::vector<ExplodedFastener> explodedFasteners(const kernel::Kernel& k, const AssemblyState& st, const doc::Element* asmEl);

// Rigid move of the top-level instances (with their nested entries) of st into placements X (§7д.2 item 3); children
// of instance patterns and derived connection instances are not moved.
void moveTopInstances(AssemblyState& st, const std::map<std::string, Motion3>& X);

// The exploded solve (§7д.2 items 2–3) on the assembled state st (before assembly features): the solved state of the
// mode — top-level placements X' and the mode's mate statuses, DOF and freeDof.
AssemblyState explodedSolve(const kernel::Kernel& k, const doc::Element& asmEl, const expr::VariableTable& vars,
                            const expr::DocumentUnits& units, const AssemblyState& st, bool& autoComputed,
                            std::vector<std::string>& autoSuppressed);

// The final exploded state (§7д.2 item 4, §7д.5): final — the assembled state after the assembly features, solved — the
// result of explodedSolve; the derived connection fasteners follow their attachments or keep their records.
AssemblyPtr applyExploded(const kernel::Kernel& k, const doc::Element& asmEl, const AssemblyState& final, const AssemblyState& solved,
                          bool autoComputed, const std::vector<std::string>& autoSuppressed);

}  // namespace construe::regen
