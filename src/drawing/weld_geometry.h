// Weld seam geometry in the assembly virtual state (spec/weld.md §6а, §6б; FX8-WLD-001, FX8-WLD-002): the weld line —
// contact pieces of the chosen faces of different parts (or the chosen edges), and the weld body — the deposited metal
// shown in the assembly (a triangle of the leg for a fillet weld, a bead for a butt weld).
#pragma once

#include "base/error.h"
#include "base/math.h"
#include "doc/document.h"
#include "kernel/kernel.h"
#include "regen/regen.h"

#include <optional>
#include <string>
#include <vector>

namespace construe::drawing {

inline constexpr double WELD_CONTACT_TOL = 1e-3;        // mm: a point of an edge lies on a face (weld.md §8)
inline constexpr double WELD_CONTACT_STEP = 1.0;        // mm: sample step along an edge
inline constexpr int WELD_CONTACT_MIN_SAMPLES = 16;     // samples per edge at least
inline constexpr int WELD_CONTACT_REFINE = 30;          // bisection steps of a contact boundary
inline constexpr double WELD_REFINE_TOL = 1e-7;         // mm: over the distance at the sample on, while refining
inline constexpr double WELD_MIN_PIECE = 0.01;          // mm: shorter contact pieces (point contacts) are dropped
inline constexpr double WELD_PARALLEL_SIN = 0.17364817766693033;  // sin 10°: faces this parallel give a bead
inline constexpr double WELD_DISPLAY_LEG_MM = 4;        // leg of the shown body when the weld has no leg
inline constexpr double WELD_BEAD_HALF_MM = 3;          // bead half width
inline constexpr double WELD_BEAD_HEIGHT_MM = 1.5;      // bead height

// A piece of the weld line (weld.md §6а) in world coordinates.
struct WeldPiece {
    enum class Kind { Line, Arc, Polyline };
    Kind kind = Kind::Line;
    std::vector<Point3> points;  // the polyline (Line — its two ends; Arc — sampled from the start)
    Point3 center = Point3::Zero();
    Vec3 axis = Vec3::UnitZ();   // Arc: unit axis; the arc goes counterclockwise about it from points.front()
    double radius = 0, sweep = 0;  // Arc: radius, angle (2π — the full circle)
    int faceA = -1, faceB = -1;  // entries of the names table: A — the face carrying the piece, B — the face it lies on (-1 — none)
    double length = 0;
};

// The weld line (weld.md §6а): faces — contact pieces; edges — the resolved edges with their faces. error —
// WELD_FACE_LOST (ref), WELD_LINE_NOT_FOUND; edges that do not resolve are skipped (WELD_EDGE_LOST is weldLostEdge's).
struct WeldPath {
    std::vector<WeldPiece> pieces;
    std::optional<Error> error;
    double length() const;
};
WeldPath weldPath(const kernel::Kernel& k, const regen::PartState& s, const doc::Json& weld);

// The weld body (weld.md §6б): one solid per piece, combined; null — no piece gives a body.
struct WeldBody {
    kernel::Shape shape;
    double volume = 0;
};
// joint — of the weld type (weld.md §2.2), leg — the evaluated leg (mm) if any.
WeldBody weldBody(const kernel::Kernel& k, const regen::PartState& s, const WeldPath& path, const std::string& joint,
                  std::optional<double> leg);

}  // namespace construe::drawing
