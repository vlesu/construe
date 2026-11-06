// Sheet primitives and drawing sheet build (spec/drawing.md §2.1, §3, §4).
#pragma once
#include "base/error.h"
#include "base/math.h"
#include "doc/document.h"
#include "kernel/kernel.h"
#include "regen/regen.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace construe::drawing {

// Sheet size, mm (§2.1).
struct SheetSize {
    double width = 0, height = 0;
};
// GOST 2.301 format and orientation → size; nullopt — unknown format or orientation.
std::optional<SheetSize> sheetSize(const std::string& format, const std::string& orientation);
// Effective sheet size of a drawing (§2.1): a GOST drawing's A4 sheet is always portrait (the field does not apply).
std::optional<SheetSize> sheetSize(const doc::Element& drawing, const doc::Sheet& sheet);
// All formats in the order of the §2.1 table (basic, then additional).
const std::vector<std::string>& sheetFormats();
// GOST 2.302 scale series for selection in the client (§2.2).
const std::vector<std::string>& standardScales();

enum class PrimitiveKind { Line, Polyline, Arc, Circle, Text, Image };
enum class LineType { Solid, Dashed, DashDot, DashDotDot, Wavy };
enum class Weight { Thick, Thin };

// Raster image of a shaded view (§3, §6.22): width × height pixels, RGBA 8 bits per channel, rows from the top,
// non-premultiplied alpha (0 — transparent).
struct RasterImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// Sheet primitive (§3). Coordinates — mm from the lower-left corner of the sheet, Y up.
struct Primitive {
    PrimitiveKind kind = PrimitiveKind::Line;
    std::vector<Point2> points;  // line — 2 points, polyline — vertices, image — lower-left and upper-right corners
    bool closed = false;         // polyline
    bool filled = false;         // polyline: closed one is filled with the line color (dimension arrows, §6а.5)
    std::shared_ptr<const RasterImage> image;  // image (§6.22)
    bool penOnly = false;  // drawn only by pen outputs (SVG, DXF) — instead of an image there (§3, §6.22)
    Point2 center{0, 0};         // arc, circle
    double radius = 0;
    double start = 0, end = 0;   // arc: angles, rad, counterclockwise from start to end
    Point2 position{0, 0};       // text
    double height = 0, angle = 0;
    std::string text, font;
    std::string align = "left bottom";  // text: left|center|right + bottom|middle|top
    std::vector<font::StyleSpan> styles;  // text: bold / italic / underline fragments, code points (§6.23, FX9-DRW-006)
    LineType lineType = LineType::Solid;
    Weight weight = Weight::Thin;
    std::string role;
    bool local = false;  // set in the drawing itself (FX9-DRW-009): on screen — COLOR_DRAWING_LOCAL; outputs ignore it
    std::string color;   // "#RRGGBB" of a cut plan primitive (§2.7, FX11-CUT-002); empty — black
};

struct ViewError {
    std::string view;
    Error error;
};

// Placement of a built view (§3, §6.5): view point p → sheet point offset + scale·p.
struct ViewPlacement {
    std::string view;
    Point2 offset{0, 0};
    double scale = 1;
};

// Drawn sheet mark (§3, §6.25): its ID, the sign anchor on the sheet (as position) and the roles of its primitives.
struct MarkPlacement {
    std::string mark;
    Point2 anchor{0, 0};
    std::vector<std::string> roles;
};

struct SheetGraphics {
    double width = 0, height = 0;
    std::vector<Primitive> primitives;
    std::vector<ViewError> errors;
    std::vector<ViewPlacement> placements;  // built views (§3)
    std::vector<ViewError> warnings;   // view built with a warning (§6.7)
    std::vector<std::string> pending;  // view model or projection not computed yet (§6.8)
    std::vector<MarkPlacement> marks;  // drawn sheet marks (§6.25)
};

// "Not computed yet" code of the model source (§6.8): the view goes to pending, not to errors.
inline constexpr const char* VIEW_PENDING = "DRAWING_VIEW_PENDING";

// Model source for views (§6.3): state of the part, body or assembly referenced by the view (empty file — this document).
class ModelProvider {
public:
    virtual ~ModelProvider() = default;
    virtual Result<regen::StatePtr> model(const std::string& file, const std::string& element) const = 0;
    virtual const kernel::Kernel& kernel() const = 0;
    // Document of the linked file ref.file of the view (assembly.md §5а) — for model dimensions of other files (§6а.10); none — nullptr.
    virtual const doc::Document* linkedDocument(const std::string& /*file*/) const { return nullptr; }
    // Model bodies in world positions (§6.3); by default — bodies of model() with identity placement.
    virtual Result<std::vector<kernel::PlacedShape>> modelBodies(const std::string& file, const std::string& element) const;
    // Hidden-line-removal projection (§6.3); by default — by the kernel, without cache.
    virtual Result<std::shared_ptr<const kernel::HlrProjection>> projection(const std::vector<kernel::PlacedShape>& bodies,
                                                                          const kernel::ViewDirection& view,
                                                                          const kernel::HlrParams& params) const;
    // Part colors of the model bodies (§6.22): "#RRGGBB" per body of modelBodies, "" — default; by default — none.
    virtual std::vector<std::string> modelColors(const std::string& /*file*/, const std::string& /*element*/) const { return {}; }
    // Planar cut faces of a variant with model sections (§6.26, FX10-DRW-002); none — not cut or not known.
    virtual std::vector<regen::DrawingCutFace> modelCutFaces(const std::string& /*file*/, const std::string& /*element*/) const { return {}; }
    // Body sectioning for a section view (§6.11); by default — by the kernel, without cache.
    virtual Result<std::shared_ptr<const kernel::SectionCut>> sectionCut(const std::vector<kernel::PlacedShape>& bodies,
                                                                        const kernel::ViewDirection& view, const Point3& origin,
                                                                        const kernel::HlrParams& params) const;
};

// Model of a main view (§6.3, §6.21): the state and the bodies of ref; for a flat-pattern view (flat, FX7-DRW-008) —
// the flat pattern state of the sheet metal part (cached per model state, so the bodies stay the same objects); a part
// without a flat pattern — DRAWING_VIEW_NOT_SHEET_METAL.
Result<regen::StatePtr> viewModel(const ModelProvider& models, const doc::DrawingView& base);
Result<std::vector<kernel::PlacedShape>> viewModelBodies(const ModelProvider& models, const doc::DrawingView& base,
                                                         const std::optional<std::vector<std::string>>& sections);
// Bodies of the whole model of a main view (without its sections): the model center (§6.3, §6.26).
Result<std::vector<kernel::PlacedShape>> wholeModelBodies(const ModelProvider& models, const doc::DrawingView& base);

// Sheet build (§4): trim, frame and title block (§5), views (§6).
SheetGraphics composeSheet(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models);

// Parts of the sheet build — by stream files: frame and title block (§5, title_block.cpp), views (§6, views.cpp).
void appendFrame(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                 SheetGraphics& out);
void appendViews(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                 SheetGraphics& out);

// Effective position of a sheet view (§6.2); nullopt — no view, no parent or a cycle.
std::optional<Point2> effectiveViewPosition(const doc::Sheet& sheet, const std::string& viewId);

// View frame and placement for dimensions (§6а.2) — as in the build (§6.1–§6.5, §6.11, §6.12): world point P → view point
// (P·r, P·u) → sheet offset + scale·p (same as placements, §3). View error — as in the build (§6.7); model not
// computed — VIEW_PENDING.
struct ViewMapping {
    const doc::DrawingView* base = nullptr;  // main view of the chain (model)
    Vec3 n = Vec3::UnitZ(), u = Vec3::UnitY(), r = Vec3::UnitX();
    Point2 offset{0, 0};
    double scale = 1;
    std::optional<Point3> cutOrigin;  // section: point of the cutting plane; the part (X − O)·n > 0 is removed (§6.11)
    Point2 toView(const Point3& p) const { return Point2(p.dot(r), p.dot(u)); }
    Point2 toSheet(const Point2& q) const { return offset + scale * q; }
};
Result<ViewMapping> viewMapping(const doc::Sheet& sheet, const std::string& viewId, const ModelProvider& models);
// View frame without a model (§6.1, §6.2, §6.11) — for choosing the annotation view (§6.18); chain error — as in the build.
struct ViewTriad {
    const doc::DrawingView* base = nullptr;  // main view of the chain (model)
    Vec3 n = Vec3::UnitZ(), u = Vec3::UnitY(), r = Vec3::UnitX();
    std::optional<Point3> cutOrigin;  // section: point of the cutting plane (the part (X − O)·n > 0 is removed)
    bool sectionOnly = false;
};
Result<ViewTriad> viewTriad(const doc::Sheet& sheet, const std::string& viewId);
// Technical requirement annotations of view models (§6.18, view_pmi.cpp).
void appendAnnotations(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                       SheetGraphics& out);
// Constructions of the sheet (§6.29, FX14-DRW-006, constructions.cpp): thin lines on views, before the dimensions.
void appendConstructions(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                         SheetGraphics& out);
// Sheet dimensions (§6а, dimensions.cpp).
void appendDimensions(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                      SheetGraphics& out);

// Texts of the sheet (§6.23, FX7-DRW-006, sheet_texts.cpp): primitives role text:<id>, a primitive per line.
void appendTexts(const doc::Sheet& sheet, SheetGraphics& out);
// Lines of a sheet text ('\n' separates them, '\r' is dropped) and the font of the text (empty — the drawing font).
std::vector<std::string> sheetTextLines(const std::string& text);
std::string sheetTextFont(const doc::SheetText& t);
// Lines of a sheet text as drawn (§6.23): explicit breaks, then — with a frame (boxWidth > 0) — wrapping by words to the
// frame width by the metrics of the drawing font (FX9-DRW-007); styles of each line in its own code points.
struct SheetTextLine {
    std::string text;
    std::vector<font::StyleSpan> styles;
    std::string align = "left";  // of its paragraph: "left" | "center" | "right" (S7F-DRW-002)
};
std::vector<SheetTextLine> sheetTextLayoutLines(const doc::SheetText& t);
// Frame of a text with boxWidth > 0 and lineCount drawn lines (§6.23, FX9-DRW-007): corners on the sheet — top left,
// top right, bottom right, bottom left (rotated with the text); the height is not less than the content.
std::optional<std::array<Point2, 4>> sheetTextFrame(const doc::SheetText& t, size_t lineCount);
inline constexpr double SHEET_TEXT_FRAME_TOP = 1.35;  // frame top above the first baseline, in font sizes (FX9-DRW-007)
inline constexpr double SHEET_TEXT_DEFAULT_MM = 5;  // size of a new text (GOST 2.304 size 5)
inline constexpr double SHEET_TEXT_LINE_STEP = 1.7;  // distance between baselines, in font sizes (GOST 2.304, type B: 17/10 h)

// Gear rim parameter tables (§6.20.3, gear_view.cpp): in the top right corner of the sheet where the model is shown first.
void appendGearTables(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models,
                      SheetGraphics& out);
// Width of the sheet's gear tables, mm (0 — none): the remaining-surfaces roughness sign moves left of them (§6.18а).
double gearTablesWidth(const doc::Element& drawing, const doc::Sheet& sheet, const ModelProvider& models);

// Designations of sections and detail views (§6.13): view ID → letter in the order of drawing sheets and views.
std::map<std::string, std::string> viewLetters(const doc::Element& drawing);
// Letter number i (from 0) of the GOST 2.316 series (§6.13).
std::string designationLetter(int i);
// Scale of a new detail view (§6.12): the smallest of standardScales() not less than 2 × parentScale, else the largest.
std::string detailScale(double parentScale);

inline constexpr double DRAWING_CURVE_TOL = 0.01;  // sheet mm: polyline tolerance of views (§6.9)

// Sections, detail views, view decoration (§6.9, §6.11–6.16).
inline constexpr double DRAWING_MIN_LINE = 1e-6;            // model mm
inline constexpr double SECTION_STROKE_MM = 10;
inline constexpr double SECTION_ARROW_OFFSET_MM = 2.5;
inline constexpr double SECTION_ARROW_LEN_MM = 8;
inline constexpr double ARROW_HEAD_LEN_MM = 4, ARROW_HEAD_HALF_MM = 0.75;
inline constexpr double DESIGNATION_TEXT_MM = 7;
inline constexpr double LABEL_GAP_MM = 5;
inline constexpr double DETAIL_LEADER_MM = 5;
inline constexpr double HATCH_DIVISOR = 15;
inline constexpr double HATCH_MIN_MM = 1, HATCH_MAX_MM = 10;
inline constexpr double CUT_FACE_MIN_COS = 0.01;  // a model section cut face is hatched when turned to the viewer (§6.26)
inline constexpr double CENTERLINE_OVERSHOOT_MM = 3;
inline constexpr double CENTERLINE_SOLID_DIAMETER_MM = 12;
inline constexpr double CENTERLINE_MIN_TURN = 0.75 * 3.14159265358979323846;
inline constexpr double AXIS_PARALLEL_TOL = 1e-9;
inline constexpr double SECTION_BIND_OVERSHOOT_MM = 5;  // sheet mm: a section line bound to an axis runs past its ends (FX9-DRW-002)
inline constexpr double DRAWING_MERGE_TOL = 1e-4;           // sheet mm
inline constexpr double THREAD_MINOR_FACTOR = 1.0825318;
inline constexpr double THREAD_ARC_START = 100 * 3.14159265358979323846 / 180;
inline constexpr double THREAD_MATCH_TOL = 1e-3;            // model mm

// Gear rims (§6.20).
inline constexpr double GEAR_TABLE_W = 110, GEAR_TABLE_NAME_W = 65, GEAR_TABLE_SYMBOL_W = 10;  // sheet mm (GOST 2.403)
inline constexpr double GEAR_TABLE_ROW = 8, GEAR_TABLE_TEXT = 3.5, GEAR_TABLE_PAD = 1;     // sheet mm
inline constexpr int GEAR_TABLE_DECIMALS = 3;
inline constexpr size_t GEAR_FILL_CACHE = 64;  // conventional bodies kept (§6.20.1)
inline constexpr size_t FLAT_VIEW_CACHE = 16;  // flat pattern states kept for flat-pattern views (§6.21)

// Shaded display of a view (§6.22).
inline constexpr double RENDER_CHORD_MM = 0.2;      // sheet mm: chord tolerance of the tessellation
inline constexpr double RENDER_ANGLE_TOL = 0.35;    // rad: angular tolerance of the tessellation
inline constexpr double RENDER_DPI = 200;           // image pixels per inch of the sheet
inline constexpr int RENDER_SUPERSAMPLE = 2;        // samples per pixel side (anti-aliasing)
inline constexpr int RENDER_MAX_PX = 2400;          // image side limit, pixels (the resolution drops beyond)
inline constexpr double RENDER_AMBIENT = 0.35;      // ambient share of the shading (as the 3D view, app-ux §11.5)
inline constexpr const char* RENDER_DEFAULT_COLOR = "#C3CCD6";  // part without a color (COLOR_BODY of the 3D view)
inline constexpr size_t RENDER_CACHE = 8;  // shaded images kept (§6.22): views of a sheet with the render display

// GOST 2.303 line drawing constants (§3), mm.
inline constexpr double LINE_THICK_MM = 0.7;
inline constexpr double LINE_THIN_MM = 0.35;
inline constexpr double DASH_STROKE_MM = 4, DASH_GAP_MM = 1.5;
inline constexpr double DASHDOT_STROKE_MM = 15, DASHDOT_GAP_MM = 3;
inline constexpr double DASHDOTDOT_STROKE_MM = 20, DASHDOTDOT_GAP_MM = 4;

}  // namespace construe::drawing
