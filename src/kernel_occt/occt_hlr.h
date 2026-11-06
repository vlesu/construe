// Hidden line removal projection on OCCT (spec/kernel-interface.md §3.17, §9.9б). Internal header of kernel_occt.
#pragma once

#include "kernel/kernel.h"

#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>

#include <utility>
#include <vector>

namespace construe::kernel::occt {

// Process-wide lazily created OCCT state used by the projection and sectioning algorithms, created once under the C++
// static initialization guard (spec/kernel-interface.md §9.11): BRepLib::Plane() — the shared plane carrying the
// pcurves of 2D edges (BRepLib_MakeEdge2d in HLRBRep_HLRToShape) — is created by OCCT on first use without
// synchronization, so concurrent first uses from the parallel view projections race on its handle (double free).
// Called by the kernel constructor and at the entry of hlrProject / sectionCutOcct; repeated calls are cheap.
void ensureSharedStatics();

// Inputs are already validated (§3.17 "Inputs and errors"); items: bodies (compounds of solids) and placements.
Result<HlrProjection> hlrProject(const std::vector<std::pair<TopoDS_Shape, gp_Trsf>>& items, const ViewDirection& view,
                                 const HlrParams& params, const Cancel* cancel);

// Sectioning bodies by a plane (§3.18, §9.9в): the remaining parts (body index in the input, shape in world coords) and
// the section figures. Inputs are already validated.
struct OcctSectionCut {
    std::vector<std::pair<int, TopoDS_Shape>> bodies;
    std::vector<SectionRegion> regions;
};
Result<OcctSectionCut> sectionCutOcct(const std::vector<std::pair<TopoDS_Shape, gp_Trsf>>& items, const ViewDirection& view,
                                      const Point3& origin, double chordTol, const Cancel* cancel);

}  // namespace construe::kernel::occt
