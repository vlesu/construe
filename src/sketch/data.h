// Sketch data as stored in the document (spec/sketch.md §2, spec/document.md §2.7).
#pragma once

#include "font/font.h"

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::sketch {

// Half-length of a sketch axis projection (source "sketch:x" / "sketch:y", sketch.md §2.5), mm.
inline constexpr double SKETCH_AXIS_HALF = 1000.0;

// Ellipse — S4-SKT-004, EllipseArc — S4-SKT-007, Bezier, BSpline — S4A-SPL-001
enum class EntityType { Point, Line, Circle, Arc, Projected, Text, Ellipse, EllipseArc, Bezier, BSpline };
// Ellipse or ellipse arc (shape fields rx, ry, angle; §2.1).
inline bool isEllipseType(EntityType t) { return t == EntityType::Ellipse || t == EntityType::EllipseArc; }
// Bezier curve or B-spline / NURBS (fields poles, weights; B-spline — degree, knots, mults, periodic; §2.1, §2.7).
inline bool isSplineType(EntityType t) { return t == EntityType::Bezier || t == EntityType::BSpline; }

// Implicit body targets of a sketch projection (sketch.md §2.5 «Силуэты и экстремумы» ("Silhouettes and extrema"), FX8-SK-002): the source is
// "<kind>:<face name>"; kinds — silhouette lines of a cylinder / cone ("silhouette+", "silhouette-": side of the axis)
// and extreme points of a meridian of a surface of revolution ("extreme+far", "extreme-far", "extreme+near",
// "extreme-near"). Also "pierce:<ref>". projectionSourceRef — the referenced name (or the source itself).
inline const char* const PROJECTION_SOURCE_PREFIXES[] = {"pierce:", "silhouette+:", "silhouette-:", "extreme+far:", "extreme-far:",
                                                         "extreme+near:", "extreme-near:"};
inline std::string projectionSourceRef(const std::string& source) {
    for (const char* p : PROJECTION_SOURCE_PREFIXES)
        if (source.rfind(p, 0) == 0) return source.substr(std::char_traits<char>::length(p));
    return source;
}
inline bool isSilhouetteSource(const std::string& s) { return s.rfind("silhouette+:", 0) == 0 || s.rfind("silhouette-:", 0) == 0; }
inline bool isExtremeSource(const std::string& s) {
    for (const char* p : {"extreme+far:", "extreme-far:", "extreme+near:", "extreme-near:"})
        if (s.rfind(p, 0) == 0) return true;
    return false;
}
// Projection of a reference axis feature onto the sketch plane: "refaxis:<axis feature ID>" (sketch.md §2.5, FX9-SK-001).
inline constexpr const char REFAXIS_SOURCE_PREFIX[] = "refaxis:";
inline bool isRefAxisSource(const std::string& s) { return s.size() > 8 && s.rfind(REFAXIS_SOURCE_PREFIX, 0) == 0; }
inline std::string refAxisFeature(const std::string& s) { return isRefAxisSource(s) ? s.substr(8) : std::string(); }

// Number of code points of a UTF-8 string (text style fragments index code points, FX8-SK-007).
inline int codePointCount(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

struct Entity {
    std::string id;
    EntityType type = EntityType::Point;
    double x = 0, y = 0;                  // point
    std::string p1, p2;                   // line
    std::string center;                   // circle, arc, ellipse; text — anchor point (file key: anchor, sketch.md §2.1)
    double r = 0;                         // circle
    double rx = 0, ry = 0;                // ellipse: semi-axes along its own axis (angle) and across it (S4-SKT-004)
    std::string start, end;               // arc, ellipse arc (S4-SKT-007)
    std::string source;                   // projected (1б)
    std::string text, font;               // text (S2-SK-013)
    double height = 0, angle = 0;         // text: cap height (mm), rotation (rad); ellipse: rx axis rotation (rad)
    std::vector<font::StyleSpan> styles;  // text: bold / italic / underline fragments (file key: styles, FX8-SK-007)
    std::vector<std::string> poles;       // bezier, bspline: control point IDs (S4A-SPL-001)
    std::vector<double> weights;          // bezier, bspline: pole weights (empty — all 1)
    int degree = 0;                       // bspline
    std::vector<double> knots;            // bspline: distinct knots, ascending
    std::vector<int> mults;               // bspline: knot multiplicities
    bool periodic = false;                // bspline: closed periodic curve
    bool construction = false;
};

// Solution branch in storage form (spec/solver.md §4.2.20).
struct Branch {
    enum class Kind { Side, Contact, Angle };
    Kind kind = Kind::Side;
    int side = 1;
    std::string contact = "External";  // External | Internal1 | Internal2
    int sign = 1;
    bool supplementary = false;
    bool operator==(const Branch&) const = default;
};

struct Constraint {
    std::string id;
    std::string type;                     // type name from spec/sketch.md §2.2
    std::vector<std::string> refs;        // entity IDs or "origin"
    std::vector<double> target;           // fix
    std::optional<std::array<double, 2>> vector;  // translate: copy offset, mm (§2.2, S4-SKT-001)
    std::optional<double> rotation;               // rotate: copy rotation angle, rad (in the file — angle; S4-SKT-002)
    std::optional<std::string> value;     // dimensions: expression
    std::optional<bool> driving;          // dimensions
    std::optional<std::string> arc;       // distance to a circle/arc: "near" | "far" (§2.2)
    bool diametral = false;               // point–segment distance: the value is twice the distance (Ø, S2-SK-003)
    std::optional<Branch> branch;
    // Dimension label: offset of the label center from the dimension anchor, mm (sketch.md §2.2 «labelOffset»,
    // FX8-SK-004) — the label moves with its points.
    std::optional<std::array<double, 2>> labelOffset;
    bool automatic = false;  // "auto": created by a hint while drawing, yields to explicit ones (S1-SK-016)
    // "yielded" (dimensions only): auto constraints removed when the dimension was placed; restored when the
    // dimension is deleted (spec/sketch.md §2.2, core-api §4.6, FX4-SK-003). Does not affect the solution.
    std::vector<Constraint> yielded;
};

// Parameter value of a linked group (§2.6.1): number, expression string or flag.
struct GroupParam {
    enum class Kind { Number, Text, Flag };
    Kind kind = Kind::Text;
    double number = 0;
    std::string text;
    bool flag = false;
    static GroupParam num(double v) { return {Kind::Number, v, {}, false}; }
    static GroupParam str(std::string s) { return {Kind::Text, 0, std::move(s), false}; }
    static GroupParam boolean(bool b) { return {Kind::Flag, 0, {}, b}; }
    bool operator==(const GroupParam&) const = default;
};

struct GroupMember {
    std::string role;
    std::string id;  // entity or constraint ID
    bool operator==(const GroupMember&) const = default;
};

// Linked sketch group (spec/sketch.md §2.6): pattern, primitive.
struct Group {
    std::string id;    // g<N>
    std::string type;  // linearPattern | circularPattern | (primitives S4-SKT-003)
    std::vector<std::string> sources;
    std::map<std::string, GroupParam> params;
    std::vector<GroupMember> members;
    bool operator==(const Group&) const = default;

    const GroupMember* member(const std::string& role) const {
        for (const auto& m : members)
            if (m.role == role) return &m;
        return nullptr;
    }
    bool hasMember(const std::string& objectId) const {
        for (const auto& m : members)
            if (m.id == objectId) return true;
        return false;
    }
};

struct Plane {
    std::string datum;  // plane:front | plane:top | plane:right
    std::string face;   // 1б: stable face name
    std::string feature;  // reference plane — ID of a plane feature (document.md §2.7, S2-REF-005)
    std::optional<std::array<double, 9>> frame;  // explicit frame: origin, x, y (document.md §2.7, S2-ASM-011)
    bool onFace() const { return !face.empty(); }
    bool onFeature() const { return !feature.empty(); }
};

// Geometry of a projected entity (spec/sketch.md §2.5), in sketch local coordinates. Not saved to the document.
struct Projection {
    enum class Kind { Point, Line, Arc, Circle };
    Kind kind = Kind::Point;
    double px = 0, py = 0;                      // Point
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;      // Line
    double cx = 0, cy = 0, r = 0;               // Arc, Circle
    double sx = 0, sy = 0, ex = 0, ey = 0;      // Arc: counterclockwise from start to end
};

struct SketchData {
    Plane plane;
    std::vector<Entity> entities;
    std::vector<Constraint> constraints;
    std::map<std::string, Projection> projections;  // projected entity ID → geometry (§2.5)
    std::vector<Group> groups;                      // linked groups (§2.6)
    std::vector<std::string> weakAnchors;           // weak dimension anchor points chosen by the user, by priority (§4.4, S4-WEAK-006)

    const Entity* findEntity(const std::string& id) const {
        for (const auto& e : entities)
            if (e.id == id) return &e;
        return nullptr;
    }
    const Constraint* findConstraint(const std::string& id) const {
        for (const auto& c : constraints)
            if (c.id == id) return &c;
        return nullptr;
    }
    const Group* findGroup(const std::string& id) const {
        for (const auto& g : groups)
            if (g.id == id) return &g;
        return nullptr;
    }
    // Group whose member the entity or constraint is (§2.6.1); nullptr — not a member.
    const Group* groupOfMember(const std::string& id) const {
        for (const auto& g : groups)
            if (g.hasMember(id)) return &g;
        return nullptr;
    }
};

const char* entityTypeName(EntityType t);
// Open spline ends — its first and last poles (§2.7.1); none for a periodic spline or another entity.
std::vector<std::string> splineEnds(const Entity& e);
bool isDimensionType(const std::string& type);
// Angle dimension (value is an angle, unit is the document angular unit): angle, arcAngle (S2-SK-005).
bool isAngleDimension(const std::string& t);
// Dimensionless dimension (value — a number without unit): weight of a spline pole (S4A-SPL-002).
bool isNumberDimension(const std::string& t);
// Points owned by curves (ends, centers, poles, text anchors); the other points are standalone (§2.1, FX9-SK-007).
std::set<std::string> ownedPoints(const SketchData& sketch);
// Hole candidate points (regen.md §4.13 item 1, FX1-FEAT-002): standalone points and ends of construction segments
// and arcs, by compareIds; coincident positions are merged by the caller.
std::vector<std::string> holePointCandidates(const SketchData& sketch);

}  // namespace construe::sketch
