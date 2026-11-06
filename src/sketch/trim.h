// Sketch curve trimming (spec/sketch.md §4.3, S1-SK-004).
#pragma once

#include "base/error.h"
#include "base/math.h"
#include "base/spline.h"
#include "sketch/data.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace construe::sketch {

inline constexpr double TRIM_TOL = 1e-6;  // mm (§7)

struct TrimPiece {
    enum class Kind { Line, Arc, Circle, Ellipse, EllipseArc, Spline };  // Ellipse, EllipseArc — S4-SKT-007, Spline — S4A-SPL-003
    Kind kind = Kind::Line;
    Point2 a = Point2::Zero(), b = Point2::Zero();  // segment
    Point2 center = Point2::Zero();                 // arc, circle, ellipse, ellipse arc
    Point2 start = Point2::Zero(), end = Point2::Zero();  // arc, ellipse arc: counterclockwise from start to end
    double r = 0;                                   // circle
    double rx = 0, ry = 0, angle = 0;               // ellipse, ellipse arc
    spline::Curve curve;                            // spline: the removed piece (open) or the whole curve
};

struct Cut {
    Point2 point = Point2::Zero();
    double param = 0;
    std::string cutter;
    std::optional<std::string> cutterPoint;
};

struct TrimPlan {
    // EllipseToArc — S4-SKT-007; PeriodicToOpen — a periodic spline becomes open (S4A-SPL-003)
    enum class Action { Delete, TrimStart, TrimEnd, Split, CircleToArc, EllipseToArc, PeriodicToOpen };
    std::string curve;
    Action action = Action::Delete;
    TrimPiece removed;
    std::optional<Cut> lo, hi;
};

struct TrimOutcome {
    SketchData sketch;
    std::vector<std::string> removedConstraints;
    std::vector<std::string> created;
};

const char* trimActionName(TrimPlan::Action a);

Result<TrimPlan> trimPlan(const SketchData& s, const std::string& curve, const Point2& pick);
// allocate(true) — entity ID, allocate(false) — constraint ID.
Result<TrimOutcome> trim(const SketchData& s, const std::string& curve, const Point2& pick,
                         const std::function<std::string(bool entity)>& allocate);

}  // namespace construe::sketch
