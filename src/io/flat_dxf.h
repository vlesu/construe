// Export of a sheet metal flat pattern to DXF for laser cutting (spec/io.md §2д; S5-DXF-001): closed contours from
// the section curves, side-by-side layout of several parts, cut and bend line layers. Pure functions plus an atomic
// file write; the flat pattern itself comes from the sheet module through api (sheet.md §4.8).
#pragma once

#include "base/error.h"
#include "kernel/kernel.h"

#include <string>
#include <vector>

namespace construe::io {

// Constants (io.md §2д.4).
inline constexpr double FLAT_JOIN_TOL = 1e-4;        // mm: chaining the ends of section curves
inline constexpr double FLAT_MERGE_TOL = 1e-6;       // mm: merging adjacent pieces of one edge
inline constexpr double FLAT_FULL_TURN_TOL = 1e-12;  // rad: an arc of a full turn is a circle
inline constexpr double FLAT_LAYOUT_GAP = 10;        // mm: gap between parts in the layout
inline constexpr double FLAT_SPLINE_TOL = 1e-3;      // mm: chord deviation of a flattened spline (gear teeth, FX7-SH-004)

struct FlatVertex {
    Point2 p = Point2::Zero();
    double bulge = 0;  // of the curve from this vertex to the next: 0 — line, tan(θ/4) — arc (> 0 counterclockwise)
};

// Closed contour of the flat pattern (§2д.1): a circle or a closed polyline with arcs.
struct FlatLoop {
    bool circle = false;
    Point2 center = Point2::Zero();
    double radius = 0;
    std::vector<FlatVertex> vertices;
    int depth = 0;    // number of other contours containing this one
    double area = 0;  // signed: > 0 — counterclockwise
};

struct FlatBend {
    Point2 a = Point2::Zero(), b = Point2::Zero();
    bool up = true;
};

struct FlatDxfPart {
    std::string name;
    std::vector<FlatLoop> loops;
    std::vector<FlatBend> bends;
};

// Placement of the parts (§2д.2): shift of each part and the overall extents [0, 0] … [width, height].
struct FlatLayout {
    std::vector<Point2> shifts;
    double width = 0, height = 0;
};

// Contours from the section curves (§2д.1); EXPORT_FLAT_OPEN — a contour does not close.
Result<std::vector<FlatLoop>> flatLoops(const std::vector<kernel::Curve2>& curves);
// Flat pattern area by the contours: outer contours minus holes (§2д.1).
double flatArea(const std::vector<FlatLoop>& loops);
FlatLayout flatLayout(const std::vector<FlatDxfPart>& parts);
std::string flatDxf(const std::vector<FlatDxfPart>& parts, bool bendLines);
Status writeFlatDxf(const std::string& path, const std::vector<FlatDxfPart>& parts, bool bendLines);

}  // namespace construe::io
