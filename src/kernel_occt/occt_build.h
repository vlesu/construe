// Building solids from a face description and from a mesh on OCCT (spec/kernel-interface.md §3.22, §3.23, §9.6ж).
// Internal header of kernel_occt.
#pragma once

#include "base/cancel.h"
#include "base/error.h"
#include "kernel/kernel.h"

#include <TopoDS_Shape.hxx>

#include <optional>

namespace construe::kernel::occt {

Result<TopoDS_Shape> buildSolidFromFaces(const SolidSpec& spec, const Cancel* cancel);
Result<TopoDS_Shape> buildSolidFromMesh(const MeshSpec& mesh, const Cancel* cancel);
// Shape healing (§3.24, §9.6з; S4-REC-013): a healed copy; the caller checks the result. OCCT failure: nullopt.
std::optional<TopoDS_Shape> healShape(const TopoDS_Shape& in, double maxTolerance);

}  // namespace construe::kernel::occt
