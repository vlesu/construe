// Cut plan — the third drawing standard (spec/drawing.md §2.7, FX11-CUT-001…006): flat sheet metal parts at 1:1 on a
// growing sheet without a frame, a label of material and thickness under each part, free sketches as engraving.
#pragma once
#include "drawing/sheet.h"

#include <optional>
#include <string>
#include <vector>

namespace construe::drawing {

inline constexpr double CUT_SHEET_W = 2500, CUT_SHEET_H = 1250;  // mm: the smallest sheet (a common rolled sheet)
inline constexpr double CUT_SHEET_MARGIN_MM = 20;               // margin from the parts to the edge of a grown sheet
inline constexpr double CUT_LAYOUT_GAP_MM = 20;                 // gap between parts laid out in a row
inline constexpr double CUT_LABEL_MM = 5, CUT_LABEL_GAP_MM = 5;  // label size (GOST 2.304 size 5) and gap below the part
inline constexpr double CUT_THICKNESS_TOL = 1e-6;               // mm: equal thicknesses
inline constexpr double CUT_PLANE_TOL = 1e-4;                   // mm: a sketch lies on a wall face (§2.7.1)
inline constexpr const char* CUT_COLOR_CONTOUR = "#000000";
inline constexpr const char* CUT_COLOR_ENGRAVE = "#FF0000";
inline constexpr const char* CUT_COLOR_LABEL = "#0000FF";

// The drawing is a cut plan (§2.7): standard "cut".
bool isCutPlan(const doc::Element& drawing);

// Label format (§2.7): templates with {material} and {thickness} (without a material — {thickness} only) and the decimal
// separator; process-wide, set by the client in the interface language.
void setCutLabelFormat(std::string withMaterial, std::string withoutMaterial, std::string decimalSeparator);
// Label of a part: material name (empty — none) and thickness T.
std::string cutLabel(const std::string& material, double thickness);

// A sheet metal part as drawn on a cut plan (§2.7 items 4, 5): contour and engraving curves in flat coordinates, their
// bounding box, the thickness.
struct CutPart {
    double thickness = 0;
    std::vector<Primitive> contour;   // role and color not set; flat coordinates
    std::vector<Primitive> engraving;
    Point2 min{0, 0}, max{0, 0};      // bounding box of contour and engraving
};
// Cut part of a model state (cached per state object); not a sheet metal part — DRAWING_CUT_NOT_SHEET_METAL with
// element (and file). el — the part element (for the free sketches, §2.7.1); nullptr — no engraving.
Result<CutPart> cutPart(const kernel::Kernel& k, const regen::StatePtr& st, const doc::Element* el, const std::string& element,
                        const std::string& file);
// Thickness of the parts of a sheet (§2.7 item 3): of the first part without an error; none — no such part.
std::optional<double> cutSheetThickness(const doc::Document& d, const doc::Sheet& sheet, const ModelProvider& models);
// Position of a new part w × h laid out in a row on the sheet (§2.7 «Добавление детали»), by the built sheet g.
Point2 cutRowPosition(const doc::Sheet& sheet, const SheetGraphics& g, double w, double h);

// Sheet build of a cut plan (§2.7) — instead of the steps of §4.
SheetGraphics composeCutSheet(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models);

}  // namespace construe::drawing
