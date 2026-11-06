// Snaps and hit testing in a sketch (app-ux §6.4, §3.5): pure functions on top of SketchGeometry. Pixel thresholds are
// theme.h constants; snap geometry is in app/snap.
#pragma once

#include "app/sketch_annotations.h"
#include "app/sketch_geometry.h"
#include "app/viewport.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace construe::app {

// Position snap for cursor screen (§6.4): to a sketch point → to a body vertex in the sketch plane →
// to a segment midpoint → onto a sketch curve (incl. a projection curve) → onto a body edge in the sketch plane;
// without a position snap — alignment with points by x/y. suppress (Ctrl) — only the point under the cursor.
// exclude — IDs of entities not to snap to (the element being drawn).
// Point of sketch curve `curve` (segment, circle, arc, projection curve) on the line "coordinate axis = value"
// (axis: 0 — x, 1 — y), nearest to near; no intersection — empty (§6.4, S1-SK-015).
std::optional<Point2> curveAxisPoint(const SketchGeometry& g, const std::string& curve, int axis, double value, const Point2& near);

// Segment a–b is parallel to a sketch axis within ALIGN_MID_AXIS_TOL_DEG — only then its midpoint is an alignment
// target of snap 7 (app-ux §6.4, FX8-SK-003).
bool alignMidAllowed(const Point2& a, const Point2& b);

// bodies — shown bodies: their vertices and edges lying in the sketch plane (SNAP_PLANE_TOL) are targets.
// extra — implicit body targets in the sketch plane (bodyTargetsBodies: projections of edges and vertices off the plane —
// FX8-SK-010, then silhouette lines and meridian extremes — FX8-SK-002): edges and vertices named by their projection
// source, treated as body geometry in the plane at any view direction, ranked after the shown bodies in list order
// (app-ux §6.4 «Body target priority»).
Snap snapAt(const SketchGeometry& g, const std::vector<DrawBody>& bodies, const QPointF& screen, bool suppress,
            const std::set<std::string>& exclude = {}, const std::vector<DrawBody>* extra = nullptr);
// Implicit body targets as bodies for snapping, in world coordinates of the sketch plane, named by the source:
// [0] — projections of edges and vertices off the plane (FX8-SK-010), [1] — silhouettes and extremes (FX8-SK-002).
// A point target — a vertex; a segment — a straight edge of two points; an arc or a circle — a polyline.
std::vector<DrawBody> bodyTargetsBodies(const std::vector<regen::BodyTarget>& targets, const Plane& frame);
// Object under the cursor (§3.5): dimension label or icon → point (including projection point and origin) → curve.
Hit hitAt(const SketchGeometry& g, const SketchAnnotations& a, const QPointF& screen);
// The object a click of «Dimension» or «Constraint» takes under the cursor (app-ux §6.8, §6.9): one choice for the
// hover highlight and the click (FX9-SK-006). Candidates are asked lazily, in priority order: the sketch object (a
// text — its anchor point); else a body edge / vertex in the view ("srce:"/"srcv:" + name, view — the view object to
// highlight); else a silhouette or extreme point (FX8-SK-002); else a reference axis feature (FX9-SK-001), else a
// sketch axis (S1-SK-021). Body targets and axes are not taken by «Dimension» with two picks; axes — only as its first
// pick or the second after a point (otherwise a click near the axis places the dimension).
struct ToolTarget {
    Hit hit;
    PickTarget view;
};
struct ToolTargetSources {
    std::function<Hit()> sketch;                                       // sketch object under the cursor (hitAt)
    std::function<const sketch::Entity*(const std::string&)> entity;   // sketch entity by ID; none or no data — nullptr
    std::function<PickTarget()> view;                                  // body edge / vertex in the view
    std::function<std::optional<std::pair<std::string, bool>>()> body; // silhouette / extreme: source and «is a point»
    std::function<std::string()> refAxis;                              // reference axis feature ID; none — empty
    std::function<std::string()> axis;                                 // sketch axis source; none — empty
};
ToolTarget toolTargetAt(bool dimension, const std::vector<std::string>& picked, const ToolTargetSources& src);
// The ID the hover highlights and a click picks (a point or a curve); otherwise empty.
std::string toolTargetId(const ToolTarget& t);
// Screen point of an entity for tests (app-ux §12): a point — itself; a curve — the first candidate point from the
// middle towards the ends, a click on which selects exactly this curve.
std::optional<QPointF> screenPosOfEntity(const SketchGeometry& g, const SketchAnnotations& a, const std::string& id);

}  // namespace construe::app
