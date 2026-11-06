// Captions of objects (spec/app-ux.md §5.5г, S7F-RF-005, S7F-RF-009): the built-in geometry of a part and every picked
// object — the single table for the tree, the view, the panels, the candidates of a selection set and the messages.
#pragma once

#include "api/session.h"
#include "app/viewport_types.h"

#include <QString>

#include <optional>
#include <string>

namespace construe::app {

// Origin planes: ID and label (app-ux §3.4, §4.2).
struct DatumPlaneDef {
    const char* id;
    const char* label;  // text key (app-ux §9.5)
};
inline constexpr DatumPlaneDef DATUM_PLANES[] = {{"plane:front", "ui.common.front"}, {"plane:top", "ui.common.top"}, {"plane:right", "ui.common.right"}};
// Origin axes: ID and label.
inline constexpr DatumPlaneDef DATUM_AXES[] = {{"axis:x", "ui.common.xAxis"}, {"axis:y", "ui.common.yAxis"}, {"axis:z", "ui.common.zAxis"}};

// Caption of a built-in plane ("Front", "Top", "Right"); empty — not a built-in plane ID.
QString datumCaption(const std::string& id);
// Caption of a built-in axis: "X axis" (capital) or "x axis" inside a sentence; empty — not a built-in axis ID.
QString axisCaption(const std::string& id, bool capital = true);

// Kind of a picked object (app-ux §5.5г): one table of captions for every style.
enum class ObjectKind {
    Object, Origin, Vertex, Edge, CircularEdge, Face, PlanarFace, CylindricalFace, ConicalFace, SphericalFace,
    Element, Point, Line, Centerline, Arc, Circle
};
// Caption of the kind inside a sentence: «planar face», «centerline», «object» …
QString objectKindCaption(ObjectKind k);
// Kind of a face by its surface; of an edge by its curve; of a sketch entity (nullptr — an unknown element).
ObjectKind faceKind(std::optional<kernel::SurfaceInfo::Kind> surface);
ObjectKind edgeKind(const std::optional<kernel::CurveInfo>& curve);
ObjectKind sketchEntityKind(const sketch::Entity* e);

// Captions of one picked object, by where it is shown (app-ux §5.5г):
// - List — the rows and the header of a selection field of a part panel: «Front», «X axis», «Sketch1», «Edge N»,
//   «Face N», «Sketch1: line»;
// - Candidate — inside a candidate label of a selection set: ««Top»», «x axis», ««Sketch1»: centerline», «face»;
// - Assembly — a mate, weld or instance pattern object: «“Front” plane», «X axis», «Part1 <1>: planar face».
enum class CaptionStyle { List, Candidate, Assembly };

// Caption of a reference (selection.md §7.1: plane:* / axis:* / origin / feature ID / stable name / "<sketch>:<entity>";
// assembly: "<path>/<name>") in the state `state`. kind — the view object kind of an instance reference (face, edge,
// vertex) when the caller knows it (Assembly style); a face kind is then asked from the session.
QString objectCaption(const api::Session& s, const std::string& ref, CaptionStyle style,
                      const api::StateRef& state = api::StateRef::final(), std::optional<PickTarget::Kind> kind = std::nullopt);
// «Edge N» / «Face N» (app-ux §5.5): N is the ordinal among all names of that kind of the final state, sorted
// bytewise; an unknown name — the name itself.
QString topoCaption(const api::Session& s, const std::string& name, bool edge);

}  // namespace construe::app
