// Rendering of sheet primitives, shared by the client and export (spec/drawing.md §3, §3.1): dash patterns, texts as
// font outlines, arcs as Bezier curves.
#pragma once
#include "drawing/sheet.h"
#include "font/font.h"

#include <array>
#include <vector>

namespace construe::drawing {

// Default drawing font (§3.1). The only place for the name; choice by document standard — S3-FONT-001, S3-DRW-008.
inline constexpr const char* DRAWING_DEFAULT_FONT = "OpenGostTypeB-Regular.ttf";
inline constexpr double DOT_MM = 0.5;  // length of the "dot" in dash-dot lines (§3)

// Line dash pattern, mm: dash, gap, …; empty — solid (§3).
std::vector<double> dashPattern(LineType t);

// Text outlines in sheet coordinates (§3.1).
struct TextOutline {
    std::vector<std::vector<font::Curve2>> contours;
    std::vector<Error> warnings;
};
Result<TextOutline> textOutline(const Primitive& text);
// Width of a text of height height (§3.1): right edge of the glyphs; empty font — default drawing font.
Result<double> textWidth(const std::string& text, double height, const std::string& fontName = {},
                         const std::vector<font::StyleSpan>& styles = {});

// Counterclockwise arc from angle a0 to a1 as Bezier curve pieces of ≤ π/2: {P0, P1, P2, P3} (§3.1).
std::vector<std::array<Point2, 4>> arcBeziers(const Point2& center, double radius, double a0, double a1);
// Glyph outline arc (Curve2::Arc) as Bezier curves with exact endpoints a, b (§3.1).
std::vector<std::array<Point2, 4>> arcBeziers(const font::Curve2& arc);

// Glyph outline curve in traversal order (§3.1): from → to; the arc is stored counterclockwise, but in the traversal it
// may be passed clockwise (ccw = false).
struct OrientedCurve {
    bool arc = false;
    Point2 from{0, 0}, to{0, 0}, center{0, 0};
    bool ccw = true;
};
std::vector<OrientedCurve> orientContour(const std::vector<font::Curve2>& contour);
// Bezier curves of an oriented arc — from from to to (§3.1).
std::vector<std::array<Point2, 4>> arcBeziers(const OrientedCurve& arc);

}  // namespace construe::drawing
