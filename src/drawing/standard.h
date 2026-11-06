// GOST or ISO drawing standard (spec/drawing.md §2.5, §3.2, §5а, §6.2; S3-DRW-008): effective projection method,
// drawing font by standard, moving projected views on method change, ISO frame and title block.
#pragma once
#include "drawing/sheet.h"
#include "font/font.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace construe::drawing {

enum class ProjectionMethod { First, Third };

// Drawing standard (§2.5): field standard, absent — GOST.
font::DrawingStandard drawingStandard(const doc::Element& drawing);
// Effective projection method (§2.5): GOST — always first angle; ISO — field projection (absent — first angle).
ProjectionMethod projectionMethod(const doc::Element& drawing);

// Preferred drawing font of the standard (§3.2) — process-wide; empty — no preference.
void setPreferredDrawingFont(font::DrawingStandard s, std::string name);
std::string preferredDrawingFont(font::DrawingStandard s);
// Drawing font of the standard (§3.2): the preferred one if present in the library; else defaultDrawingFont; else DRAWING_DEFAULT_FONT.
std::string drawingFontFor(font::DrawingStandard s);
std::string drawingFont(const doc::Element& drawing);

// Font for texts with empty font during the sheet build (§3.2) in the current thread; a nested scope restores the previous one.
class DrawingFontScope {
public:
    explicit DrawingFontScope(std::string font);
    ~DrawingFontScope();
    DrawingFontScope(const DrawingFontScope&) = delete;
    DrawingFontScope& operator=(const DrawingFontScope&) = delete;

private:
    std::optional<std::string> prev_;
};
// Font for empty font: from the current thread's DrawingFontScope or DRAWING_DEFAULT_FONT.
std::string scopedDrawingFont();
// Write font into texts with empty font if it differs from DRAWING_DEFAULT_FONT (§3.2).
void applyDrawingFont(std::vector<Primitive>& prims, const std::string& font);

// Moving projected views to the other side of their parents on projection method change (§6.2).
void mirrorProjectedViews(doc::Element& drawing);

// Three views (§6.2а, FX1-DRW-001): positions of the top view and the side view relative to the main view at pos by the
// model bounding box box (world), scale k and projection method.
struct ThreeViewsLayout {
    Point2 top{0, 0}, side{0, 0};
    std::string topDirection, sideDirection;
};
inline constexpr double THREE_VIEWS_GAP_MM = 20;  // sheet mm between view bounding boxes
ThreeViewsLayout threeViewsLayout(const BBox3& box, double k, ProjectionMethod method, const Point2& pos);

// Free field of a sheet (§6.2б, FX10-DRW-001): the frame interior shrunk by THREE_VIEWS_MARGIN_MM and the rectangles
// views must not cover (title block, field 26), grown by the same margin; sheet mm.
struct Rect2 {
    Point2 min{0, 0}, max{0, 0};
    bool contains(const Rect2& o) const;
    bool overlaps(const Rect2& o) const;
};
struct SheetField {
    Rect2 frame;
    std::vector<Rect2> blocked;
};
inline constexpr double THREE_VIEWS_MARGIN_MM = 10;  // sheet mm between the views and the frame / title block
SheetField sheetField(const doc::Element& drawing, const doc::Sheet& sheet);
// Fitting three views into the sheet field (§6.2б): the main view position nearest to want at which the three view
// rectangles lie in the field; with autoScale the sheet scale goes down the standard series (§2.2) from scale until
// they fit. fits = false — no position at the last scale tried (the views stay at want).
struct ThreeViewsFit {
    std::string scale;  // scale "a:b" used
    Point2 pos{0, 0};   // main view position
    bool fits = false;
};
ThreeViewsFit fitThreeViews(const BBox3& box, const std::string& scale, bool autoScale, ProjectionMethod method,
                            const Point2& want, const SheetField& field);

// ISO 5457 frame and ISO 7200 title block (§5а).
inline constexpr double ISO_FIELD_LEFT = 20, ISO_FIELD = 10;
inline constexpr double ISO_CENTRING_OVERSHOOT_MM = 5;
inline constexpr double ISO_BLOCK_W = 180, ISO_BLOCK_H = 36;
inline constexpr double ISO_LABEL_H = 1.8;
inline constexpr double PROJECTION_SYMBOL_H = 3;

// Keys of ISO-only fields (§5а.4) in specification order.
const std::vector<std::string>& isoTitleBlockKeys();
// Frame, centering marks, lines, method symbol and title block texts of an ISO sheet W × H by field values (§5а.1–§5а.4).
void appendIsoTitleFrame(double W, double H, ProjectionMethod method, const std::map<std::string, std::string>& values,
                         SheetGraphics& g);

}  // namespace construe::drawing
