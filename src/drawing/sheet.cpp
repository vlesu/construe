// Sheet formats and sheet build (spec/drawing.md §2.1, §2.2, §4).
#include "drawing/sheet.h"
#include "drawing/sheet_parts.h"
#include "drawing/weld_annotations.h"

#include "drawing/cut_plan.h"
#include "drawing/standard.h"

#include <utility>

namespace construe::drawing {

namespace {
struct FormatRow {
    const char* name;
    double shortSide, longSide;
};
// Table §2.1 (GOST 2.301).
constexpr FormatRow FORMATS[] = {
    {"A0", 841, 1189},    {"A1", 594, 841},     {"A2", 420, 594},     {"A3", 297, 420},     {"A4", 210, 297},
    {"A0x2", 1189, 1682}, {"A0x3", 1189, 2523}, {"A1x3", 841, 1783},  {"A1x4", 841, 2378},  {"A2x3", 594, 1261},
    {"A2x4", 594, 1682},  {"A2x5", 594, 2102},  {"A3x3", 420, 891},   {"A3x4", 420, 1189},  {"A3x5", 420, 1486},
    {"A3x6", 420, 1783},  {"A3x7", 420, 2080},  {"A4x3", 297, 630},   {"A4x4", 297, 841},   {"A4x5", 297, 1051},
    {"A4x6", 297, 1261},  {"A4x7", 297, 1471},  {"A4x8", 297, 1682},  {"A4x9", 297, 1892},
};
}  // namespace

std::optional<SheetSize> sheetSize(const std::string& format, const std::string& orientation) {
    if (orientation != "portrait" && orientation != "landscape") return std::nullopt;
    for (const auto& f : FORMATS)
        if (format == f.name)
            return orientation == "portrait" ? SheetSize{f.shortSide, f.longSide} : SheetSize{f.longSide, f.shortSide};
    return std::nullopt;
}

std::optional<SheetSize> sheetSize(const doc::Element& drawing, const doc::Sheet& sheet) {
    // GOST: A4 — portrait only (GOST R 2.104-2023 п. Б.5); the stored orientation does not apply (§2.1).
    if (sheet.format == "A4" && drawingStandard(drawing) == font::DrawingStandard::Gost) return sheetSize(sheet.format, "portrait");
    return sheetSize(sheet.format, sheet.orientation);
}

const std::vector<std::string>& sheetFormats() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> v;
        for (const auto& f : FORMATS) v.emplace_back(f.name);
        return v;
    }();
    return list;
}

const std::vector<std::string>& standardScales() {
    static const std::vector<std::string> list = {"1:1",   "1:2",   "1:2.5", "1:4",   "1:5",    "1:10",   "1:15",  "1:20",
                                                  "1:25",  "1:40",  "1:50",  "1:75",  "1:100",  "1:200",  "1:400", "1:500",
                                                  "1:800", "1:1000", "2:1",  "2.5:1", "4:1",    "5:1",    "10:1",  "20:1",
                                                  "40:1",  "50:1",  "100:1"};
    return list;
}

SheetGraphics composeSheet(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models) {
    if (isCutPlan(drawing)) return composeCutSheet(d, drawing, sheet, models);  // cut plan (§2.7)
    SheetGraphics g;
    // Drawing font by standard (§3.2): text widths during the build and font of the result texts.
    const std::string font = drawingFont(drawing);
    const DrawingFontScope fontScope(font);
    const auto size = sheetSize(drawing, sheet).value_or(SheetSize{});
    g.width = size.width;
    g.height = size.height;
    // 1. Sheet trim.
    Primitive trim;
    trim.kind = PrimitiveKind::Polyline;
    trim.points = {Point2(0, 0), Point2(g.width, 0), Point2(g.width, g.height), Point2(0, g.height)};
    trim.closed = true;
    trim.role = "trim";
    g.primitives.push_back(std::move(trim));
    // 2. Frame and title block; 3. Views, gear tables; 4. Technical requirement annotations, weld seams; 5. Dimensions;
    // 6. Texts.
    appendFrame(d, drawing, sheet, models, g);
    appendViews(d, drawing, sheet, models, g);
    appendGearTables(d, drawing, sheet, models, g);  // §6.20.3
    appendAnnotations(d, drawing, sheet, models, g);
    appendWelds(d, drawing, sheet, models, g);
    appendBalloons(d, drawing, sheet, models, g);    // §6.27, FX14-DRW-002
    appendPartsLists(d, drawing, sheet, models, g);  // §6.28, FX14-DRW-003
    appendConstructions(d, drawing, sheet, models, g);  // §6.29, FX14-DRW-006
    appendDimensions(d, drawing, sheet, models, g);
    appendTexts(sheet, g);  // 6. Texts of the sheet (§6.23)
    applyDrawingFont(g.primitives, font);
    return g;
}

}  // namespace construe::drawing
