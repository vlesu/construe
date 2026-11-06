// Item numbers (balloons) and parts lists on assembly drawing sheets (spec/drawing.md §6.27, §6.28; FX14-DRW-002,
// FX14-DRW-003).
#pragma once
#include "drawing/sheet.h"
#include "drawing/specification.h"

#include <optional>
#include <string>
#include <vector>

namespace construe::drawing {

// Constants (§6.27, §6.28).
inline constexpr double BALLOON_TEXT_H = 5;
inline constexpr double BALLOON_DOT_R_MM = 0.6;
inline constexpr double BALLOON_LEADER_MM = 12;
inline constexpr double BALLOON_SHELF_GAP_MM = 1;
inline constexpr double BALLOON_TEXT_GAP_MM = 1;
inline constexpr double PARTS_LIST_BOTTOM_MM = 60;

// Specification rows of an assembly of this document for its drawing (§6.27 item 3): its first specification, else one
// without overrides and manual rows; the connection reports — of the model state of the assembly (none — no fasteners).
SpecContent assemblySpecContent(const doc::Document& d, const std::string& assembly, const ModelProvider& models);
// Item number of the part whose face is `face` ("<instance path>/<face>"): the position of the row with its top-level
// instance (a pattern child — its source); none — nullopt.
std::optional<std::string> balloonNumber(const doc::Element& assembly, const SpecContent& c, const std::string& face);
// Index of the specification row of the part whose face is `face` (§6.27 item 3): the row with its top-level instance (a
// pattern child — its source) or, for a connection fastener, the derived instance path "<feature>/<output>" that the
// face path starts with; none — nullopt.
std::optional<size_t> balloonItem(const doc::Element& assembly, const SpecContent& c, const std::string& face);

// Item numbers not on the drawing (§6.27 «Непоставленные номера позиций», FX14-DRW-004): for each assembly of this
// document whose views are on the sheet, a balloon {kind "balloon", view, source} (without id and position) for each
// numbered specification row of which no balloon stands on any sheet of the drawing and whose part is drawn on a view of
// this sheet; the view — the selected views first, then the main view, then the others in sheet order.
std::vector<doc::SheetMark> balloonMarks(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                         const ModelProvider& models, const std::vector<std::string>& views = {});

// Step 4 of composeSheet (§4): the balloons of the sheet (§6.27), then its parts lists (§6.28).
void appendBalloons(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                    SheetGraphics& out);
void appendPartsLists(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                      SheetGraphics& out);

}  // namespace construe::drawing
