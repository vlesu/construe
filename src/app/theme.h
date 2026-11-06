// UX constants (spec/app-ux.md §11). Names match the constant names of the specification.
#pragma once

#include <QColor>
#include <QString>

namespace construe::app {

// §11.1
inline constexpr int WINDOW_DEFAULT_W = 1600, WINDOW_DEFAULT_H = 980;  // fits 1920 × 1040 with a frame up to 60 px (FX14-UI-005)
inline constexpr int WINDOW_MIN_W = 1024, WINDOW_MIN_H = 700;
inline constexpr int TEST_WINDOW_W = 1280, TEST_WINDOW_H = 800;
// The window frame before it is shown — for placing the window inside the available screen area (FX14-UI-002). Not less
// than the real Windows 10/11 frame (45 px vertically measured on the user's machine) with a margin: 44 + 8 = 52 px
// (FX14-UI-005, spec/app-ux.md §11.1).
inline constexpr int WINDOW_FRAME_ALLOWANCE_SIDE = 8, WINDOW_FRAME_ALLOWANCE_TOP = 44;
inline constexpr int RIBBON_MIN_H = 72;  // ribbon height follows its contents, not less than this (app-ux §2.3)
inline constexpr int RIBBON_ICON_PX = 24;
inline constexpr int QUICK_ICON_PX = RIBBON_ICON_PX;  // buttons above the ribbon, the same as ribbon icons (app-ux §2.3, FX7-UI-002)
inline constexpr int RIBBON_TAB_MIN_H = QUICK_ICON_PX + 8;  // tab bar height: the buttons above the ribbon fit (FX7-UI-002)
inline constexpr int LEFT_PANEL_W = 300, LEFT_PANEL_MIN_W = 220;
inline constexpr double PROPS_HEIGHT_FRACTION = 0.6;
inline constexpr int RIGHT_PANEL_W = 360;
inline constexpr int PANEL_CLOSE_PX = 28;  // the close cross of a hideable panel (app-ux §2.1в, FX8-UI-008)
inline constexpr int STATUSBAR_H = 24;
// Long operations (app-ux §9.3, FX7-UI-006): the window appears after the delay; the worker is polled at the interval.
inline constexpr int LONG_OP_SHOW_DELAY_MS = 400, LONG_OP_POLL_MS = 30;
// Long work indicator (app-ux §9.4, FX8-UI-001): a running stripe at the top of the window after the delay.
inline constexpr int BUSY_SHOW_DELAY_MS = 1000, BUSY_BAR_H = 3, BUSY_PERIOD_MS = 1400, BUSY_TRACK_ALPHA = 60;
inline constexpr double BUSY_SEGMENT_FRACTION = 0.3;
inline constexpr int BUSY_POLL_MS = 100, BUSY_FRAME_MS = 40;
inline constexpr int CONFIRM_CORNER_BTN_PX = 32, CONFIRM_CORNER_MARGIN_PX = 12;
inline constexpr int CONFLICT_PANEL_W_PX = 320;
inline constexpr int TOAST_W_PX = 360, TOAST_MARGIN_PX = 16, TOAST_MAX = 3;
// Text of a notification higher than this scrolls (S7F-UI-004): any message is read to its end.
inline constexpr int TOAST_TEXT_MAX_H_PX = 200;
inline constexpr int DIM_FIELD_W_PX = 120;
inline constexpr int DIM_TOL_FIELD_W_PX = 100;  // tolerance field next to the dimension value field (FX12-PMI-004; list arrow FX13-TOL-001)
// §11.3: sketch pattern (§6.3) — default spacing: source extent × factor; without an extent — mm.
inline constexpr double PATTERN_STEP_FACTOR = 1.5, PATTERN_STEP_DEFAULT_MM = 10;

// §11.2
inline constexpr int DRAG_THRESHOLD_PX = 4;
inline constexpr int BOX_FILL_ALPHA = 40;  // assembly instance selection box fill (0…255, §8а, FX13-ASM-002)
inline constexpr double VIEW_PICK_MARGIN_MM = 2;  // drawing view margin for picking and the frame (§8б)
inline constexpr int PICK_WINDOW_PX = 10;
inline constexpr int PICK_POINT_PX = 8;
inline constexpr int PICK_CURVE_PX = 6;
inline constexpr int SNAP_POINT_PX = 10;
inline constexpr int SNAP_CURVE_PX = 8;
inline constexpr double SNAP_PLANE_TOL = 1e-4;  // mm: body geometry in the sketch plane (§6.4)
inline constexpr int SNAP_ALIGN_PX = 6;
inline constexpr double SNAP_TIE_PX = 1;  // screen distances closer than this are equal for target levels (app-ux §6.4, FX8-SK-010)
inline constexpr int TARGET_CIRCLE_SEGMENTS = 64;  // polyline of a projected circle snap target (FX8-SK-010)
inline constexpr int TARGET_ARC_MIN_SEGMENTS = 8;  // fewest segments of a projected arc snap target
inline constexpr double SNAP_ANGLE_DEG = 4;
// Tangency at a joint with an arc (app-ux §6.4 "tangency at a joint", FX10-SK-004): the tangent of an arc drawn by
// inexact clicks (third point, center) is off by several degrees.
inline constexpr double SNAP_TANGENT_DEG = 20;
// A placement collapses a curve if its length / radius falls below this share of its size before (app-ux §6.4,
// FX10-SK-004): the hints of the placement are then dropped one by one, the weakest first.
inline constexpr double AUTO_COLLAPSE_RATIO = 1e-3;
// Alignment with a segment midpoint (snap 7) only for segments parallel to a sketch axis within this angle (FX8-SK-003).
inline constexpr double ALIGN_MID_AXIS_TOL_DEG = 1;
inline constexpr double PARALLEL_DIM_TOL_DEG = 0.5;
inline constexpr int MIN_ELEMENT_PX = 3;
inline constexpr int WAKE_HOVER_MS = 400;
inline constexpr int MANIP_ARROW_PX = 60, MANIP_SNAP_PX = 20;
inline constexpr int FREE_MANIP_PX = 28;  // free-degree manipulator icon and grab radius (§8а, FX8-ASM-003)
inline constexpr double FREE_MANIP_RING_FRACTION = 0.75;  // a press farther from the centre grabs the ring
inline constexpr double FREE_MANIP_CENTER_FRACTION = 0.25;  // a press nearer the centre — a free pull; between — an arrow (S7F-ASM-003)
inline constexpr double ROTATE_DEG_PER_PX = 0.4;
inline constexpr double ZOOM_STEP_FACTOR = 1.2;
inline constexpr double ZOOM_MIN_HEIGHT_MM = 0.01, ZOOM_MAX_HEIGHT_MM = 100000;
inline constexpr double KEY_ROTATE_DEG = 15, KEY_PAN_FRACTION = 0.1;
inline constexpr double FIT_MARGIN_FRACTION = 0.1;
inline constexpr double SELECTION_FRAME_FRACTION = 0.3;  // centering on a selected object (§5.5а, S7F-UI-001)
inline constexpr int SLOT_NODE_PAD_PX = 4;  // caption of a places tree node from its left edge (§5.5б, FX10-WZ-001)
inline constexpr double EXTRUDE_DEFAULT_DEPTH = 10;
inline constexpr double PLANE_MIN_HALF_MM = 50, PLANE_BBOX_FACTOR = 0.6;
// Decimal places of numbers shown in the interface (fields, properties, messages; app-ux §5.6). Not the drawing
// dimension precision (drawing::DIM_DECIMALS, spec/drawing.md).
inline constexpr int UI_DECIMALS = 3;
inline constexpr int DIM_DECIMALS = UI_DECIMALS;  // former name, kept until parallel branches switch to UI_DECIMALS
inline constexpr double STL_COARSE_FACTOR = 10, STL_COARSE_ANGLE_DEG = 17;
inline constexpr double STL_FINE_FACTOR = 0.1, STL_FINE_ANGLE_DEG = 3;

// §11.3
inline constexpr int ANIM_VIEW_MS = 400;
inline constexpr int TOOLTIP_DELAY_MS = 500;
inline constexpr int BUTTON_TOOLTIP_DELAY_MS = 300;  // button tooltips (§2.1, FX9-UI-001)
inline constexpr int CONTEXT_BAR_DELAY_MS = 250;
inline constexpr int CONTEXT_BAR_OFFSET_X_PX = 16, CONTEXT_BAR_OFFSET_Y_PX = -16;  // §11.4
inline constexpr int CONTEXT_BAR_ICON_PX = 24, CONTEXT_BAR_MAX_ICONS = 10;
inline constexpr int CONTEXT_BAR_FADE_NEAR_PX = 40, CONTEXT_BAR_FADE_FAR_PX = 160;
inline constexpr int PREVIEW_DEBOUNCE_MS = 150;
inline constexpr int TARGETS_LIST_MAX_H = 120;  // px: «Instances» list of an assembly feature panel (§8а)
inline constexpr int EXPR_CHECK_DEBOUNCE_MS = 300;
inline constexpr int REGEN_INDICATOR_DELAY_MS = 300;
inline constexpr int DIM_PREVIEW_DELAY_MS = 400;  // dimension preview after the mouse pauses (§6.9, FX2-SK-002)
inline constexpr int DIM_PREVIEW_MOVE_PX = 3;     // mouse jitter below this does not remove the preview
inline constexpr int DIM_PREVIEW_ALPHA = 150;     // preview opacity (0…255)
inline constexpr int WEAK_LAYOUT_ROWS = 4;        // rows of layout candidates for weak dimension labels (§6.6а)
inline constexpr int TOAST_INFO_MS = 4000, TOAST_WARNING_MS = 8000;

// §11.4
inline constexpr double EDGE_WIDTH_PX = 1.5;
inline constexpr double EDGE_HOVER_WIDTH_PX = 3, EDGE_SELECTED_WIDTH_PX = 5;  // FX7-PT-001: the selected edge is thicker
inline constexpr double EDGE_ID_WIDTH_PX = 3;  // body edges in the ID buffer (client.md §4.3)
inline constexpr double CANDIDATE_ALPHA = 0.25;  // selection candidate plane fill (selection.md §10, FX7-SEL-001)
inline constexpr double SK_LINE_PX = 2;
inline constexpr int SK_POINT_PX = 5, SK_CENTER_PX = 7;
// A standalone point (not a curve end): a filled circle with a contrasting outline (app-ux §6.5, FX9-SK-007).
inline constexpr int SK_STANDALONE_POINT_PX = 9;
inline constexpr double SK_POINT_OUTLINE_PX = 1.5;
inline constexpr int VERTEX_HOVER_PX = 8, VERTEX_SELECTED_PX = 8;
inline constexpr int ORIGIN_TRIAD_PX = 40;
inline constexpr int GLYPH_PX = 16, GLYPH_OFFSET_PX = 12;
// Snap hints while drawing (app-ux §6.4, FX8-SK-006): symbol font size (bold), position mark half-size and line width,
// width of the highlighted sample element of an equality.
inline constexpr int SNAP_HINT_PX = 24;
inline constexpr int SNAP_MARK_PX = 9;
inline constexpr int SNAP_MARK_WIDTH_PX = 3;
inline constexpr int SNAP_SAMPLE_WIDTH_PX = 4;
inline constexpr int DIM_ARROW_PX = 8, DIM_TEXT_PX = 12;
// Shorter dimension lines get their arrows outside, with the line continued beyond them (app-ux §6.6, FX10-IO-007).
inline constexpr int DIM_ARROWS_INSIDE_MIN_PX = 24, DIM_ARROW_STUB_PX = 16;
inline constexpr int DOF_DOT_PX = 10;
inline constexpr int ROLLBACK_BAR_H_PX = 3;
inline constexpr int TREE_STATUS_COLUMN_PX = 24;  // right column of tree icons (app-ux §4.2а)
inline constexpr double AMBIENT = 0.35;
inline constexpr double FACE_HOVER_ALPHA = 0.35, FACE_SELECTED_ALPHA = 0.45, REGION_HOVER_ALPHA = 0.25;
inline constexpr double LIST_HOVER_ALPHA = 0.75;  // a selection list row hovered: its object in the view (§5.5а, FX10-WZ-002)
inline constexpr double PREVIEW_ALPHA = 0.5;
inline constexpr double INSTANCE_TRANSPARENT_ALPHA = 0.35;  // assembly.md §5
inline constexpr float HIDDEN_DASH_ON_PX = 6, HIDDEN_DASH_OFF_PX = 4;  // HIDDEN_DASH (§3.4а)
// Technical requirement annotations in 3D (app-ux §3.4, S3-PMI-001…003).
inline constexpr int PMI_TEXT_PX = 12, PMI_LEADER_PX = 48, PMI_STACK_PX = 36, PMI_ARROW_PX = 8;
inline constexpr double PMI_NORMAL_MIN = 0.2;
// Lift of the annotation anchor along the face normal for the depth test (FX12-PMI-001): the larger of PX on screen
// and MM in the model — clears the face's own tessellation (chord sag of curved faces) without reaching through walls.
inline constexpr double PMI_DEPTH_LIFT_PX = 3.0, PMI_DEPTH_LIFT_MM = 0.1;

// §11.5 — palette. Token — the constant name (for the test mode displayState).
struct ColorToken {
    const char* name;
    QColor color;
};
#define CONSTRUE_COLOR(NAME, HEX) inline const ColorToken NAME{#NAME, QColor(HEX)};
CONSTRUE_COLOR(COLOR_BG_TOP, "#D7DEE6")
CONSTRUE_COLOR(COLOR_BG_BOTTOM, "#F5F7FA")
CONSTRUE_COLOR(COLOR_BODY, "#C3CCD6")
CONSTRUE_COLOR(COLOR_EDGE, "#20262E")
CONSTRUE_COLOR(COLOR_HOVER, "#F28C28")
CONSTRUE_COLOR(COLOR_LIST_HOVER, "#FF6A00")  // bright orange: the object of a hovered selection list row, over the selection (§5.5а, FX10-WZ-002)
CONSTRUE_COLOR(COLOR_SELECTED, "#1BA34A")
CONSTRUE_COLOR(COLOR_SNAP, "#F28C28")
CONSTRUE_COLOR(COLOR_SK_UNDER, "#1F5FD6")
CONSTRUE_COLOR(COLOR_SK_FULL, "#141414")
CONSTRUE_COLOR(COLOR_SK_CONFLICT, "#D62828")
CONSTRUE_COLOR(COLOR_SK_POINT_OUTLINE, "#FFFFFF")  // outline of a standalone sketch point (FX9-SK-007)
CONSTRUE_COLOR(COLOR_WARNING, "#E0A800")
CONSTRUE_COLOR(COLOR_ERROR, "#D62828")
CONSTRUE_COLOR(COLOR_ERROR_TEXT, "#B71C1C")
CONSTRUE_COLOR(COLOR_INFO, "#2B6CB0")
CONSTRUE_COLOR(COLOR_DISABLED_TEXT, "#9AA0A6")
CONSTRUE_COLOR(COLOR_ERROR_BANNER_BG, "#FDECEA")
CONSTRUE_COLOR(COLOR_WARNING_BANNER_BG, "#FFF6D6")
CONSTRUE_COLOR(COLOR_DIM, "#1A1A1A")
CONSTRUE_COLOR(COLOR_DIM_DRIVEN, "#6B7280")
CONSTRUE_COLOR(COLOR_DIM_WEAK, "#AAB4C3")
CONSTRUE_COLOR(COLOR_GLYPH_BG, "#E6E8EB")
CONSTRUE_COLOR(COLOR_GLYPH_FG, "#1A1A1A")
CONSTRUE_COLOR(COLOR_ICON, "#3A3F47")
CONSTRUE_COLOR(COLOR_ICON_ACCENT, "#1F6FEB")
CONSTRUE_COLOR(COLOR_ICON_SOLID, "#9AA6B2")  // faces of parts on icons (S7F-UI-005)
CONSTRUE_COLOR(COLOR_PREVIEW_ADD, "#F2C300")
CONSTRUE_COLOR(COLOR_PREVIEW_CUT, "#E04A3A")
CONSTRUE_COLOR(COLOR_PREVIEW_INTERSECT, "#8E6CEF")
CONSTRUE_COLOR(COLOR_MANIP, "#2B6CB0")
CONSTRUE_COLOR(COLOR_ROLLBACK, "#2B6CB0")
CONSTRUE_COLOR(COLOR_PLANE_FILL, "#6FA8DC")
CONSTRUE_COLOR(COLOR_PLANE_BORDER, "#4A7FB0")
CONSTRUE_COLOR(COLOR_AXIS_X, "#D33A3A")
CONSTRUE_COLOR(COLOR_AXIS_Y, "#2E9E3E")
CONSTRUE_COLOR(COLOR_AXIS_Z, "#2F5FD0")
CONSTRUE_COLOR(COLOR_AXIS_REF, "#7A4FB0")  // reference axes (app-ux §3.4, S2-REF-004)
CONSTRUE_COLOR(COLOR_HLR_FACE, "#FFFFFF")     // faces of the «hidden lines» styles (app-ux §3.4а)
CONSTRUE_COLOR(COLOR_EDGE_HIDDEN, "#8A939E")  // hidden edges of the hiddenDashed style (app-ux §3.4а)
CONSTRUE_COLOR(COLOR_INTERFERENCE, "#D62828")  // a pair of interfering parts (app-ux §8а)
CONSTRUE_COLOR(COLOR_WELD_BODY, "#C62020")  // weld bodies — the deposited metal (app-ux §8а, FX8-WLD-002)
CONSTRUE_COLOR(COLOR_FASTENER, "#D9B1AB")      // fasteners of the connection wizard in the assembly view, slightly reddish (app-ux §8а, FX13-JNT-004)
CONSTRUE_COLOR(COLOR_LIBRARY_PART, "#AFCBEA")  // library parts without a color of their own, slightly bluish (app-ux §8а, FX13-LIB-001)
CONSTRUE_COLOR(COLOR_BASE_BODY, "#C98A3A")     // base body of a recovered part when shown for comparison (app-ux §3.4, S4-REC-009)
CONSTRUE_COLOR(COLOR_CONTEXT, "#8A96A3")       // context bodies of a part in assembly context (app-ux §8а, S2-ASM-011)
CONSTRUE_COLOR(COLOR_THREAD, "#B07A1F")    // conventional thread representation (app-ux §3.4, S2-FEAT-009)
CONSTRUE_COLOR(COLOR_PMI, "#1D4E89")       // technical requirement annotations (app-ux §3.4, S3-PMI-*)
CONSTRUE_COLOR(COLOR_ORIGIN_POINT, "#D33A3A")
CONSTRUE_COLOR(COLOR_FIELD_ACTIVE_BG, "#E3F0FF")
CONSTRUE_COLOR(COLOR_FIELD_ACTIVE_BORDER, "#1F6FEB")
CONSTRUE_COLOR(COLOR_OK, "#1BA34A")
CONSTRUE_COLOR(COLOR_CANCEL, "#D62828")
CONSTRUE_COLOR(COLOR_DRAWING_LOCAL, "#8E3BB5")  // drawing: annotations and tolerances set in the drawing itself (FX9-DRW-009), screen only
#undef CONSTRUE_COLOR
inline constexpr double PLANE_FILL_OPACITY = 0.15;

// §11.6
inline constexpr double VIEW_CHORD_PX = 0.5;
inline constexpr double VIEW_CHORD_BASE_MM = 1;
inline constexpr double VIEW_CHORD_STEP = 1.4142135623730951;   // √2: chord level step (FX3-PERF-002)
inline constexpr double VIEW_CHORD_REUSE = 1.4142135623730951;  // √2: chord of a ready mesh ≤ REUSE · t (FX3-PERF-002)
inline constexpr double VIEW_MIN_CHORD_REL = 1e-4, VIEW_MAX_CHORD_REL = 1e-2;
inline constexpr double VIEW_ANGLE_TOL = 0.25;
inline constexpr int VIEW_RETESS_DEBOUNCE_MS = 200;
inline constexpr int THREAD_MAX_RINGS = 200;  // conventional thread representation (app-ux §3.4, S2-FEAT-009)
inline constexpr int THREAD_RING_SEGMENTS = 48;     // sides of a thread ring (app-ux §3.4)
inline constexpr double THREAD_RING_GAP = 0.002;  // ring clearance from the face mesh, share of the radius (§3.4, FX10-HOLE-001)

// §11.7
inline constexpr int TEST_IDLE_TIMEOUT_MS = 30000;
inline constexpr int TEST_IDLE_TIMEOUT_HEAVY_MS = 60000;
inline constexpr int TEST_IDLE_WALL_FACTOR = 4;  // hard wall-clock cap of idle: limit × factor (S4-QA-003)

// Text fields (§6.11в «Text field», FX9-UI-002): height grows with the lines between the minimum and the
// fraction of the window, then scrolls; the resize corner; the minimum width of the sketch text box field.
inline constexpr int TEXT_EDIT_MIN_LINES = 6;
inline constexpr double TEXT_EDIT_MAX_FRACTION = 0.5;
inline constexpr int TEXT_EDIT_GRIP_PX = 12;
inline constexpr int TEXT_EDIT_MIN_WIDTH_PX = 320;
inline constexpr int SHEET_TEXT_EDITOR_MIN_W_PX = 260;  // the in-place editor of a sheet text (§8б)
inline constexpr int TEXT_FRAME_HANDLE_PX = 8;          // corner handles of a sheet text frame (§8б, FX9-DRW-007)

}  // namespace construe::app
