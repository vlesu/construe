#include "sketch/data.h"

#include "base/ids.h"

#include <algorithm>
#include <set>

namespace construe::sketch {

const char* entityTypeName(EntityType t) {
    switch (t) {
        case EntityType::Point: return "point";
        case EntityType::Line: return "line";
        case EntityType::Circle: return "circle";
        case EntityType::Arc: return "arc";
        case EntityType::Projected: return "projected";
        case EntityType::Text: return "text";
        case EntityType::Ellipse: return "ellipse";
        case EntityType::EllipseArc: return "ellipseArc";
        case EntityType::Bezier: return "bezier";
        case EntityType::BSpline: return "bspline";
    }
    return "?";
}

std::vector<std::string> splineEnds(const Entity& e) {
    if (!isSplineType(e.type) || e.periodic || e.poles.empty()) return {};
    return {e.poles.front(), e.poles.back()};
}

bool isDimensionType(const std::string& t) {
    return t == "distance" || t == "distanceX" || t == "distanceY" || t == "length" || t == "radius" ||
           t == "diameter" || t == "angle" || t == "arcLength" || t == "arcAngle" || t == "pathLength" ||
           t == "semiMajor" || t == "semiMinor" || t == "ellipseAngle" || t == "weight";
}

bool isAngleDimension(const std::string& t) { return t == "angle" || t == "arcAngle" || t == "ellipseAngle"; }

bool isNumberDimension(const std::string& t) { return t == "weight"; }

std::set<std::string> ownedPoints(const SketchData& sketch) {
    std::set<std::string> owned;
    for (const auto& e : sketch.entities) {
        for (const std::string* r : {&e.p1, &e.p2, &e.center, &e.start, &e.end})
            if (!r->empty()) owned.insert(*r);
        for (const auto& q : e.poles) owned.insert(q);
    }
    return owned;
}

std::vector<std::string> holePointCandidates(const SketchData& sketch) {
    const std::set<std::string> owned = ownedPoints(sketch);
    std::set<std::string> vertices;
    for (const auto& e : sketch.entities) {
        if (!e.construction) continue;
        if (e.type == EntityType::Line) vertices.insert(e.p1), vertices.insert(e.p2);
        if (e.type == EntityType::Arc) vertices.insert(e.start), vertices.insert(e.end);
    }
    std::vector<std::string> out;
    for (const auto& e : sketch.entities)
        if (e.type == EntityType::Point && (!owned.count(e.id) || vertices.count(e.id))) out.push_back(e.id);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return compareIds(a, b) < 0; });
    return out;
}

}  // namespace construe::sketch
