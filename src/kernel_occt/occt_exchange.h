// Exchange on OCCT: BREP text and STEP via XCAF (spec/kernel-interface.md §3.16, §9.9а). Internal header of kernel_occt.
#pragma once

#include "base/error.h"

#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace construe::kernel::occt {

struct OccStepProduct {
    std::string name;
    std::optional<std::string> color;                                 // #RRGGBB
    TopoDS_Shape shape;                                               // part (empty means assembly)
    std::vector<std::pair<TopoDS_Shape, std::string>> faceColors;     // face → #RRGGBB
    std::vector<std::tuple<int, gp_Trsf, std::string>> components;    // product, placement, occurrence name
};

Result<std::string> brepWrite(const TopoDS_Shape& s);
Result<TopoDS_Shape> brepRead(const std::string& text);
Status stepWrite(const std::string& path, const std::vector<OccStepProduct>& products, int root);
Result<std::pair<std::vector<OccStepProduct>, int>> stepRead(const std::string& path);

}  // namespace construe::kernel::occt
