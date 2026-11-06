// Sketch primitives — polygon and star linked groups (spec/sketch.md §2.6.4, S4-SKT-003). Pure generation functions;
// validation and parameter evaluation are up to the caller (api).
#pragma once

#include "sketch/groups.h"

#include <optional>
#include <string>
#include <vector>

namespace construe::sketch {

inline constexpr int PRIMITIVE_MAX_SIDES = 40;  // §7

// Primitive placement: center, size (circle radius for a polygon; unused for a star), direction.
struct PrimitivePlacement {
    double x = 0, y = 0, size = 0, angle = 0;
};

struct PolygonParams {
    int sides = 6;
    bool inscribed = true;
    PrimitivePlacement start;  // initial placement (only on creation, §2.6.4)
};

struct StarParams {
    int rays = 5;
    double d1 = 0, d2 = 0;          // evaluated diameters, mm
    std::string d1Text, d2Text;     // expressions — values of member dimensions d1, d2
    PrimitivePlacement start;
};

// Current placement from the members of the previous group (§2.6.4); none if the members are not in the sketch.
std::optional<PrimitivePlacement> polygonPlacement(const SketchData& s, const Group& old);
std::optional<PrimitivePlacement> starPlacement(const SketchData& s, const Group& old);

std::vector<GroupItem> polygonItems(const SketchData& s, const Group* old, const PolygonParams& p);
std::vector<GroupItem> starItems(const SketchData& s, const Group* old, const StarParams& p);

}  // namespace construe::sketch
