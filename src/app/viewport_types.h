// 3D view types shared by the view, the renderer and the tools: pick target, body and region to draw.
#pragma once

#include <map>
#include <optional>
#include "io/stl.h"

#include <QColor>
#include <QVector3D>

#include <string>
#include <tuple>
#include <vector>

namespace construe::app {

// What is picked in the 3D view: faces, edges, planes — via the ID buffer; body vertices, the origin
// and axes — geometrically (client.md §4.3а).
struct PickTarget {
    // SketchEntity — a point, line or circle of a visible sketch outside sketch mode (selection.md §2.2, FX7-SEL-001).
    enum class Kind { None, Face, Edge, Plane, Vertex, Origin, Axis, Region, Weld, SketchEntity };
    Kind kind = Kind::None;
    std::string body;  // for Face/Edge/Vertex; for Region and SketchEntity — the sketch feature ID
    std::string name;  // stable name, plane ID (plane:front …), "origin", axis ID (axis:x …), weld ID (w<N>), entity ID
    bool operator==(const PickTarget&) const = default;
    bool operator<(const PickTarget& o) const {
        return std::tie(kind, body, name) < std::tie(o.kind, o.body, o.name);
    }
};

struct DrawBody {
    std::string key;     // unique key (state + body + parameters) — for buffer reuse
    io::BodyMesh mesh;
    QColor color;
    float alpha = 1.0f;
    bool pickable = true;
    std::optional<QColor> edgeColor;  // line color instead of COLOR_EDGE (conventional thread representation, §3.4)
    std::map<std::string, QColor> faceColors;  // face color override by full name (§5.10, S2-PART-004)
    // The whole body is picked as this target (weld body, app-ux §8а, S7F-WLD-001): in the ID buffer even when not
    // pickable; selected / hovered — highlighted as a face.
    std::optional<PickTarget> pickAs;
    double diag = 0;  // body bounding box diagonal, mm (chord rule §11.6); 0 — unknown
};

// A plane in the view (app-ux §3.4): origin (ID plane:*) or reference (feature ID); a square around center.
struct DrawPlane {
    std::string id;
    QString label;
    Plane frame;
    QVector3D center;
};

// Reference axis (app-ux §3.4, S2-REF-004): feature ID, point and unit direction.
struct DrawAxis {
    std::string id;
    QString label;
    QVector3D point, dir;
};

// Reference geometry and visible sketches of nested subassemblies in the view of the assembly (app-ux §3.4, FX9-ASM-008):
// drawn only, not picked; world coordinates; IDs "<node path>/<feature ID>".
struct DrawNestedReference {
    std::vector<DrawPlane> planes;
    std::vector<DrawAxis> axes;
    std::vector<std::vector<QVector3D>> sketchLines;
};

// Assembly weld seam line (app-ux §8а, S5-WELD-002): weld ID and polylines of its edges in world coordinates.
struct DrawWeld {
    std::string id;
    std::vector<std::vector<QVector3D>> polylines;
};

// Pickable element of a visible sketch outside sketch mode (app-ux §3.4, FX7-SEL-001): a point, a line segment or a
// circle/arc polyline in world coordinates.
struct DrawSketchPick {
    std::string sketch, entity;
    enum class Kind { Point, Line, Curve } kind = Kind::Point;
    bool construction = false;
    std::vector<QVector3D> points;  // Point — 1, Line — 2, Curve — polyline
};

// Sketch region for fill and picking (app-ux §3.4): triangles in world coordinates.
struct DrawRegion {
    std::string sketch, regionId;
    std::vector<QVector3D> triangles;  // 3 vertices each
};

}  // namespace construe::app
