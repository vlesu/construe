// Linked sketch groups: pattern generators and rebuild by roles (spec/sketch.md §2.6).
// Pure functions over sketch data; evaluation of parameter expressions and input validation are up to the caller (api).
#pragma once

#include "sketch/data.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace construe::sketch {

inline constexpr int PATTERN_MAX_INSTANCES = 1000;  // §7: instances including the source
inline constexpr double PATTERN_FULL_TOL = 1e-9;    // §7: circular pattern angle — full circle, rad
inline constexpr double PATTERN_COUNT_TOL = 1e-9;   // §7: an instance count expression value is an integer (S4-SKT-006)

// Generation item (§2.6.2): entity or constraint with a role; references are persistent IDs or "@<role>".
struct GroupItem {
    std::string role;
    std::optional<Entity> entity;
    std::optional<Constraint> constraint;
};

// Linear pattern (§2.6.3): numbers are evaluated parameters (mm, rad).
struct LinearPattern {
    int count1 = 1, count2 = 1;
    double step1 = 0, angle1 = 0, step2 = 0, angle2 = 0;
    std::string line;  // axis segment (S4-SKT-006): angles are measured from its direction; empty — from the x axis
    // Segment of the second axis (FX12-FEAT-001): angle2 is measured from its direction and the second axis follows it;
    // empty — as the first axis.
    std::string line2;
};
// Circular pattern (§2.6.3): center — point ID or "origin"; angle — full angle, rad.
struct CircularPattern {
    std::string center = "origin";
    int count = 2;
    double angle = 0;
    double step() const;  // φ per §2.6.3
};

// Generators (§2.6.3). Sources are validated by the caller: segments, circles, arcs, standalone points.
std::vector<GroupItem> linearPatternItems(const SketchData& s, const std::vector<std::string>& sources, const LinearPattern& p);
std::vector<GroupItem> circularPatternItems(const SketchData& s, const std::vector<std::string>& sources, const CircularPattern& p);

// Rebuild (§2.6.2 items 2–4): group `group` (new sources and params already written, members — the old ones) gets
// items; IDs — by role, new ones — allocate(true — entity, false — constraint). A group whose id is not in s.groups
// is appended to the end of groups. Returns the new sketch data.
SketchData applyGroupItems(SketchData s, Group group, const std::vector<GroupItem>& items,
                           const std::function<std::string(bool entity)>& allocate);
// Group deletion (§2.6.2): keepMembers — «Развязать» ("Unlink", members remain), otherwise members are deleted along
// with the constraints referencing them.
SketchData removeGroup(SketchData s, const std::string& groupId, bool keepMembers);
// Points of a curve (p1, p2 / center / center, start, end; text — anchor).
std::vector<std::string> ownedPoints(const Entity& e);

}  // namespace construe::sketch
