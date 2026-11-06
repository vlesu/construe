// Part sections in the 3D view (spec/sections.md, S2-VIEW-002, S2-VIEW-005): the section's cut-away region, the cut
// body and its mesh with names of the original faces and edges. The part and the regeneration state are not changed.
#pragma once

#include "doc/document.h"
#include "io/stl.h"
#include "kernel/kernel.h"
#include "regen/regen.h"

#include <optional>
#include <vector>

namespace construe::api {

using Json = doc::Json;

inline constexpr double SECTION_MATCH_TOL = 1e-6;  // sections.md §6

// Region size L (sections.md §2) and the combined bounding box of the state's bodies.
struct SectionFrame {
    double L = 100;
    BBox3 all;
};
SectionFrame sectionFrame(const kernel::Kernel& k, const regen::PartState& st);

// Cut-away region of a section (§3) or an error status (§5). doc - for resolving references and the offset value.
Result<kernel::Shape> sectionTool(const kernel::Kernel& k, const regen::PartState& st, const doc::Document& d,
                                  const doc::Section& s, const SectionFrame& fr);

// Section plane as the plane after offset and flip (§3.1 items 1–3): normal points to the cut-away side.
Result<Plane> sectionPlane(const kernel::Kernel& k, const regen::PartState& st, const doc::Document& d, const doc::Section& s);

// Assembly sections (§7а): section plane in assembly coordinates and the cut-away region in assembly coordinates.
// v - virtual assembly state (assembly.md §7б): reference planes and sketches of the assembly.
Result<Plane> assemblySectionPlane(const kernel::Kernel& k, const regen::AssemblyState& st, const doc::Document& d, const doc::Section& s,
                                   const regen::PartState* v = nullptr);
SectionFrame assemblySectionFrame(const kernel::Kernel& k, const regen::AssemblyState& st);
Result<kernel::Shape> assemblySectionTool(const kernel::Kernel& k, const regen::AssemblyState& st, const doc::Document& d,
                                          const doc::Section& s, const SectionFrame& fr, const regen::PartState* v = nullptr);

// Mesh of body bodyIndex with regions tools subtracted in turn (§4); body cut away entirely - mesh without faces.
// failed - indices of tools on which the kernel returned an error (skipped).
Result<io::BodyMesh> sectionedBodyMesh(const kernel::Kernel& k, const regen::PartState& st, int bodyIndex,
                                       const std::vector<kernel::Shape>& tools, const kernel::TessParams& params,
                                       std::vector<int>* failed = nullptr);

// Installs the cutting of drawing view models by model sections (drawing.md §6.26, S7F-DRW-005;
// regen::setDrawingModelSections), once per process.
void installDrawingSectionCutter();

}  // namespace construe::api
