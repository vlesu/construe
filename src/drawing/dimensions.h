// Drawing dimensions (spec/drawing.md §6а, GOST 2.307): associative dimensions by stable names of edges and vertices
// of the view model, view geometry picking and placement preview for the client.
#pragma once
#include "drawing/sheet.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::drawing {

// Constants of §6а.8.
inline constexpr double DIM_TEXT_HEIGHT = 3.5;    // mm
inline constexpr double DIM_TEXT_GAP = 1.0;       // mm
inline constexpr double DIM_ARROW_LEN = 3.0;      // mm
inline constexpr double DIM_ARROW_ANGLE_DEG = 20;  // arrowhead opening angle
inline constexpr double DIM_ARROW_GAP = 1.0;      // mm
inline constexpr double DIM_EXT_OVERSHOOT = 2.0;  // mm
inline constexpr int DIM_DECIMALS = 2;
inline constexpr double DIM_EPS = 1e-6;           // mm
inline constexpr double DIM_PARALLEL_TOL = 1e-9;
inline constexpr double DIM_MIN_OFFSET = 1.0;     // sheet mm
inline constexpr double DIM_PICK_TIE = 0.01;      // sheet mm
inline constexpr double DIM_TEXT_PAD = 0.5;       // sheet mm: clearance around the number in the "fits" check (§6а.4)
inline constexpr double DIM_TEXT_SHIFT_GAP = 1.0; // sheet mm: number shifted beyond the extension line — from the arrow tip
inline constexpr double DIM_SHELF_LEADER_MM = 5;  // sheet mm: leader line to the shelf
inline constexpr double DIM_SHELF_PAD = 1.0;      // sheet mm: shelf extends beyond the number on each side
inline constexpr double DIM_BREAK_OVERRUN = 4.0;  // sheet mm: broken dimension line runs past the middle (axis)
inline constexpr double DIM_ISO_ARROW_ANGLE_DEG = 30;  // ISO 129-1 arrowhead opening angle
inline constexpr double DIM_ISO_EXT_GAP = 1.0;    // sheet mm: gap between the outline and the extension line (ISO 129-1)
inline constexpr double DIM_FACE_TOL = 1e-5;      // model mm: silhouette and cut point lies on the face
inline constexpr double DIM_CURVE_DEFLECTION = 0.01;  // model mm: curve polyline for picking and extents (§6а.7, FX7-DRW-010)

// Named view model geometry on the sheet for click picking (§6а.7); coordinates — sheet mm.
struct ViewEntity {
    enum class Kind { Point, Line, Circle, Arc, Curve };
    std::string ref;  // edge | vertex | silhouette | cut (FX3-DRW-002) | axis | center (FX9-DRW-001) | construction (FX14-DRW-006)
    std::string name;
    // Face whose line this entity is (FX9-DRW-009: an annotation set in the drawing goes onto it): silhouette, cut, axis,
    // center — the face itself; edge — of its two faces the plane seen edge-on (normal ⊥ view direction), else the first;
    // vertex — none.
    std::string face;
    int side = 0;     // silhouette, cut: generatrix side +1 / −1
    Kind kind = Kind::Point;
    Point2 a{0, 0}, b{0, 0};     // line (point — a)
    Point2 center{0, 0};         // circle, arc
    double radius = 0;
    double start = 0, end = 0;   // arc: rad, counterclockwise from start to end
    std::vector<std::vector<Point2>> polylines;  // curve: its polylines (FX7-DRW-010)
    double depth = 0;            // depth δ: larger — closer to the viewer
};
Result<std::vector<ViewEntity>> viewEntities(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                             const std::string& viewId, const ModelProvider& models);

// Dimension construction without writing to the document (§6а.7); cursor — sheet point: placement fields are derived from it.
struct DimensionPreview {
    doc::SheetDimension dimension;  // with placement fields
    std::vector<Primitive> primitives;
    std::string text;
    double value = 0;  // mm or degrees
    bool modelTolerance = false;  // the dimension has a tolerance from the model (§6а.3): tolerance display choice is available
    // Linear: measurement direction d, offset direction m, edges of the measured geometry along m and the extent of the
    // dimension line with the number along d (sheet mm) — row layout (§6а.10 item 7).
    struct Layout {
        bool valid = false;
        Point2 d{1, 0}, m{0, 1};
        double hmax = 0, hmin = 0, lo = 0, hi = 0;
    } layout;
};
Result<DimensionPreview> previewDimension(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                          const doc::SheetDimension& a, const std::optional<Point2>& cursor, const ModelProvider& models);

// Sheet constructions (§6.29, FX14-DRW-006, constructions.cpp): the build of one construction without writing to the
// document — primitives (role cons:<id>), effective points on the sheet and the binding error (the construction is drawn
// by its stored points); built — the views already built (placements). Errors: DRAWING_CONS_VIEW_NOT_FOUND,
// DRAWING_CONS_DEGENERATE, view errors (§6.7), VIEW_PENDING.
struct ConstructionPreview {
    doc::SheetConstruction construction;
    std::vector<Primitive> primitives;
    std::vector<Point2> points;
    std::optional<Error> error;
};
Result<ConstructionPreview> previewConstruction(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                                const doc::SheetConstruction& c, const ModelProvider& models,
                                                const SheetGraphics* built = nullptr);
// Line type of a construction without the line field (§6.29): pitch circle and axis — dashDot, otherwise solid.
const std::string& defaultConstructionLine(const std::string& kind);
inline constexpr double CONSTRUCTION_AXIS_OVERSHOOT_MM = 3;   // sheet mm: an axis line runs past its points (§6.29)
inline constexpr double CONSTRUCTION_COLLINEAR_TOL = 1e-12;   // pitch circle: points on one line (§6.29)
inline constexpr size_t CONSTRUCTION_MAX_POINTS = 64;

// Dimensions from the model (§6а.10, FX1-DRW-003, FX2-DRW-001): for a part — sketch and feature dimensions, for an assembly —
// "distance" and "angle" mates and overall reference dimensions; matched to model edges and vertices and distributed over
// suitable views, each once per whole drawing; without IDs (the command assigns them).
inline constexpr double MODEL_DIM_MATCH_TOL = 1e-5;   // model mm: coincidence of sketch points and topology
inline constexpr double MODEL_DIM_VALUE_TOL = 1e-4;   // model mm or degrees: dimension value equals the feature value
inline constexpr size_t MODEL_DIM_WITNESSES = 2;      // max number of substitute references for a sketch point (vertex and edges)
inline constexpr double MODEL_DIM_OFFSET_MM = 10;     // sheet mm: first dimension line from the outline
inline constexpr double MODEL_DIM_STEP_MM = 7;        // sheet mm: step of subsequent dimensions on the view
inline constexpr double MODEL_DIM_ANGLE_RADIUS_MM = 15;  // sheet mm: arc radius of the first angular dimension
inline constexpr double MODEL_DIM_CIRCLE_ANGLE = 0.7853981633974483;  // rad (45°): leader direction of diameter and radius
// Spreading of overlapping dimensions after the row layout (§6а.10 item 7а, FX13-DRW-002).
inline constexpr int MODEL_DIM_SPREAD_TRIES = 16;          // shifted positions tried for one dimension
inline constexpr double MODEL_DIM_SPREAD_ANGLE = 0.5235987755982988;  // rad (30°): leader turn step of a diameter
inline constexpr double MODEL_DIM_OVERLAP_TOL = 0.1;       // sheet mm: text boxes and dimension lines overlap by more
// Room for a dimension on a view (§6а.10 item 5в, FX13-DRW-003): the field inside the frame and the title block of form 1.
inline constexpr double MODEL_DIM_FIELD_LEFT_MM = 20, MODEL_DIM_FIELD_MM = 5;  // sheet mm: frame margins (GOST 2.301)
inline constexpr double MODEL_DIM_TITLE_W_MM = 185, MODEL_DIM_TITLE_H_MM = 55;  // sheet mm: title block of form 1 (GOST 2.104)
// Model dimension that no view of its model shows (§6а.10 item 8, FX8-DRW-002): the source feature, its name, the sketch
// dimension ID ("" — a feature dimension) and the value expression ("" — none).
struct UnplacedDimension {
    std::string feature, featureName, constraint, value;
};
// onlyViews (FX7-DRW-009): dimensions go only onto these views (main and projected views of any chain of their models);
// nullptr — onto the connected views of each model (§6а.10 item 1). unplaced — the sources shown on no view (item 8).
std::vector<doc::SheetDimension> modelDimensions(const doc::Document& d, const doc::Element& drawing, const doc::Sheet& sheet,
                                                 const ModelProvider& models, const std::set<std::string>* onlyViews = nullptr,
                                                 std::vector<UnplacedDimension>* unplaced = nullptr);

// Value formatting (§6а.3): length — up to DIM_DECIMALS digits, decimal comma; angle — degrees and minutes.
std::string formatDimensionLength(double mm);
std::string formatDimensionAngle(double deg);

}  // namespace construe::drawing
