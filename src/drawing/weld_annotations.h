// Assembly weld seams on drawings and their status (spec/weld.md §6, spec/drawing.md §6.19): value evaluation,
// edge resolution in the assembly virtual state, weld images and designations on sheet views.
#pragma once
#include "drawing/sheet.h"
#include "pmi/weld.h"

#include <optional>
#include <string>
#include <vector>

namespace construe::drawing {

// Values of the weld expressions (weld.md §6 item 1), mm; error — WELD_BAD_VALUE with name.
struct WeldValues {
    std::optional<double> leg, length, pitch;
    std::optional<Error> error;
};
WeldValues weldValues(const doc::Document& d, const doc::Json& weld);
// Label number texts from the values (weld.md §4): formatNumber(v, WELD_DECIMALS).
pmi::WeldTexts weldTexts(const WeldValues& v);

// First weld edge that does not resolve in the names table (weld.md §6 item 2); nullopt — all resolve.
std::optional<std::string> weldLostEdge(const naming::NameTable& names, const doc::Json& weld);
// World polylines of the resolved weld edges of state s (drawing.md §6.19 "Модели и рёбра"), edge order; lost edges skipped.
std::vector<std::vector<Point3>> weldPolylines(const kernel::Kernel& k, const regen::PartState& s, const doc::Json& weld);
// World polylines of edges of the names table of s (entries — indices into names.edges), as the weld edges (§6.19).
std::vector<std::vector<Point3>> edgePolylines(const kernel::Kernel& k, const regen::PartState& s, const std::vector<int>& entries);

// Weld images and designations on the views of the sheet (drawing.md §6.19).
void appendWelds(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                 SheetGraphics& out);

inline constexpr double WELD_TEXT_MM = 3.5;  // drawing.md §6.19
inline constexpr double WELD_LEADER_MM = 10;
inline constexpr double WELD_ARROW_HALF_MM = 1;
inline constexpr double WELD_TEXT_GAP_MM = 1;
inline constexpr double WELD_SAMPLE_MM = 0.5;
inline constexpr double WELD_MATCH_MM = 0.05;
inline constexpr double WELD_FACING_RATIO = 0.5;
inline constexpr int WELD_CIRCLE_SEGMENTS = 64;
inline constexpr double WELD_TESS_CHORD = 0.05;  // model mm

}  // namespace construe::drawing
