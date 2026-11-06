// Numbering of sub-shapes of a BRep text as FreeCAD names them (FaceN, EdgeN; spec/fcstd.md §4.3).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace construe::fcstd {

struct BrepIndex {
    int faces = 0;                // number of distinct faces (FaceN, N = 1..faces)
    std::vector<int> edgeKernel;  // EdgeN − 1 → kernel edge index (degenerated edges are not kernel edges: −1)
};

// Distinct faces and edges in depth-first order from the root shape, a sub-shape counted once per (shape, composed
// location); nullopt — the text is not a BRep with Locations and TShapes sections.
std::optional<BrepIndex> brepIndex(const std::string& text);

}  // namespace construe::fcstd
