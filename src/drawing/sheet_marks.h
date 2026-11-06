// Sheet marks (spec/drawing.md §6.25, FX9-DRW-008, FX9-DRW-004): placing model annotations, rest roughness and welds
// onto the sheet views as stored objects, the mark key, the nearest point of a view image to the sign.
#pragma once
#include "drawing/sheet.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace construe::drawing {

inline constexpr double MARK_MIN_LEADER_MM = 5;  // §6.25

// Model element of a mark: the main view of the chain of its view, this document (empty ref.file); none — empty.
std::string markModel(const doc::Sheet& sheet, const doc::SheetMark& m);
// Key (kind, model, source) of the mark (§6.25) — as a dismissed record.
doc::DismissedMark markKey(const doc::Sheet& sheet, const doc::SheetMark& m);

// Marks of the command «Place all additional information» (§6.25),
// without id: annotations, rest roughness, welds; only those not actually on the drawing (FX12-DRW-005). views — the
// selected views (FX14-DRW-005, §6.18 «Выбранный вид и место»): annotations go onto them first; IDs not on the sheet
// are ignored.
std::vector<doc::SheetMark> modelMarks(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                       const ModelProvider& models, const std::vector<std::string>& views = {});

// A model mark not on the drawing with its image on the sheet («Show undimensioned», §6.25, FX12-DRW-004).
struct MarkPreview {
    doc::SheetMark mark;  // as modelMarks gives it (without id)
    std::vector<Primitive> primitives;
};

// Nearest point of the polylines to p (§6.25): the point and the polyline index (the first at a tie); none — nullopt.
std::optional<std::pair<Point2, size_t>> nearestOnPolylines(const std::vector<std::vector<Point2>>& polys, const Point2& p);

// Room of a built sheet for the view choice of a mark (§6.18 «Выбранный вид и место», FX14-DRW-005): the boxes of
// what stands on it — dimensions, marks, views (by their named geometry), the title block — and the drawing field.
struct SheetBox {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    bool empty() const { return x1 < x0; }
};
struct MarkRoom {
    SheetGraphics graphics;  // the sheet as built before the command
    struct Owned {
        std::string view;  // the view of a view box; empty — a dimension, mark or the title block
        SheetBox box;
    };
    std::vector<Owned> boxes;
    SheetBox field;  // empty — sheet size unknown (no field check)
};
MarkRoom markRoom(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models);
// Box of primitives as the room check of dimensions takes it (§6а.10 item 5в: texts — as rectangles of item 7а).
SheetBox primitivesExtent(const std::vector<Primitive>& prims);
// Number of interferences of a box on view `view` (§6.18 «Помехи»): room boxes (except that view's own box) and extra
// boxes met deeper than MODEL_DIM_OVERLAP_TOL, plus 1 when the box leaves the field.
int markInterference(const MarkRoom& room, const SheetBox& b, const std::string& view, const std::vector<SheetBox>& extra);

// Candidates of the command by kind (view_pmi.cpp, weld_annotations.cpp): marks chosen onto this sheet.
std::vector<doc::SheetMark> annotationMarks(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                            const ModelProvider& models, const std::vector<std::string>& views = {});
std::vector<doc::SheetMark> weldMarks(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                      const ModelProvider& models);

}  // namespace construe::drawing
