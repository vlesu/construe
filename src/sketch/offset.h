// Sketch offset as a linked group (spec/sketch.md §2.6.5, S7F-SK-005): chains of sources and their sides, the group
// generator, the closed contour of a curve and the contour of body edges seen from the sketch plane. Pure functions;
// expression evaluation is up to the caller (api).
#pragma once

#include "base/math.h"
#include "sketch/groups.h"

#include <string>
#include <utility>
#include <vector>

namespace construe::sketch {

inline constexpr double OFFSET_JOINT_TOL = 1e-6;  // §7: curve ends meet by position, mm
inline constexpr double OFFSET_MIN = 1e-9;        // §7: smallest offset distance and offset radius, mm

// Chains of offset sources (§2.6.5). error — empty when the sources are valid, otherwise "sources" (errorId — the
// offending source, may be empty for a branching joint).
struct OffsetChains {
    struct Joint {
        size_t i = 0, j = 0;           // source indices
        bool iEnd = false, jEnd = false;  // which end: false — the start (p1 / start), true — the end (p2 / end)
    };
    std::string error, errorId;
    std::vector<int> sigma;     // per source: side for distance > 0 relative to its own direction (+1 — left)
    std::vector<Joint> joints;  // in chain traversal order
};
OffsetChains offsetChains(const SketchData& s, const std::vector<std::string>& sources, const std::string& selfGroup = {});

// Sign of the group distance at which source curve is offset to side (relative to its own direction, +1 — left);
// 0 — curve is not a source or the sources are invalid (core-api §4.6.4 offsetSign).
int offsetSign(const SketchData& s, const std::vector<std::string>& sources, const std::string& curve, int side);

// Generator of an `offset` group (§2.6.5): distance — the evaluated signed value, valueText — the positive form of its
// text (value of the member dimensions). error — "sources" or "distance" (a radius vanishes); items — otherwise.
struct OffsetItems {
    std::string error, errorId;
    std::vector<GroupItem> items;
};
OffsetItems offsetItems(const SketchData& s, const std::vector<std::string>& sources, double distance, const std::string& valueText,
                        const std::string& selfGroup = {});

// Closed contour of a curve (core-api §4.6.4 contourOf): lines, arcs and projected lines/arcs of the same construction
// kind chained through joints, without branches, in traversal order from curve; a circle is a contour by itself.
// A dimension of an offset group other than its lead `dim0` (sketch.md §2.6.5, FX12-SK-001): derived from the group
// (same value as the lead, changed only through it) — not shown in the sketch and not transferred to drawings.
bool derivedOffsetDimension(const SketchData& s, const std::string& constraintId);

std::vector<std::string> contourOf(const SketchData& s, const std::string& curve);

// Contour of body edges seen from the sketch plane (§2.6.5 «Контур рёбер тела» ("Body edge contour")): curves — edge projections in the order
// of the name table; start — the clicked edge. Empty — not in a closed contour.
std::vector<std::string> edgeContour(const std::vector<std::pair<std::string, Projection>>& curves, const std::string& start);

}  // namespace construe::sketch
