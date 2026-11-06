// Internal state of the import (spec/fcstd.md §5): the document being built, regeneration of the current part,
// geometric matching against the shapes stored in the file.
#pragma once

#include "fcstd/brep_index.h"
#include "fcstd/import.h"
#include "regen/regen.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace construe::fcstd::detail {

using Json = doc::Json;

// Imported sketch: our feature ID, the frame of our plane and the map of FreeCAD geometry to our entities.
struct SketchMap {
    std::string feature;
    Plane frame;                      // our plane frame (part coordinates)
    Vec3 fcNormal = Vec3::UnitZ();    // FreeCAD sketch normal (part coordinates)
    std::map<int, std::string> curve;  // GeoId → curve entity (line, circle, arc, ellipse, ellipse arc)
    std::map<std::pair<int, int>, std::string> point;  // (GeoId, PointPos) → point entity
    std::set<int> circlesAsPoints;    // GeoIds of circles turned into hole points (§6.1)
    int points = 0;                   // standalone points of the sketch (hole centers, §7.1)
};

// Failure of a feature mapping: the feature goes into the base body (§8).
struct Unmapped {
    std::string reason;
};

struct Ctx {
    const FcDocument& fc;
    const kernel::Kernel& k;
    regen::Regenerator regen;
    uint64_t revision = 1;

    doc::Document base;   // document with variables and finished elements
    Json element;         // the part being built
    std::vector<ReportEntry> report;
    std::map<std::string, std::string> variableOf;  // "Sheet.Alias" / "Sheet.A1" → variable name (§10)
    std::map<std::string, SketchMap> sketches;      // FreeCAD sketch name → imported sketch (current part)
    std::map<std::string, std::string> featureOf;   // FreeCAD object name → our feature ID (current part)
    std::map<std::string, std::string> planeOf;     // FreeCAD datum plane → our plane feature ID
    std::map<std::string, std::string> elementOf;   // FreeCAD object name → our element ID (bodies, solids)
    std::vector<std::string> notes;                 // reasons collected for the object being converted

    Ctx(const FcDocument& d, const kernel::Kernel& kk) : fc(d), k(kk), regen(kk) {}

    std::string allocate(doc::IdKind kind) { return doc::allocate(base, kind); }

    // Regeneration of the current part with its features (nullopt — the document is invalid).
    std::optional<regen::PartResult> regenerate(std::string* error = nullptr);
    // State after the last feature of the current part (nullptr — no features yet or invalid).
    regen::StatePtr state();
    regen::StatePtr state_;
    bool stateValid_ = false;
    void touch() { stateValid_ = false; }

    // Shape stored in the file for a property (Shape of a feature), via the kernel; nullopt — no shape.
    std::optional<kernel::Shape> fcShape(const Object& o, const std::string& property = "Shape");
    std::optional<BrepIndex> fcIndex(const Object& o, const std::string& property = "Shape");

    // Our stable names of FreeCAD sub-elements "EdgeN" / "FaceN" of object o's shape, matched geometrically in the
    // current state (§4.4). nullopt — some element did not match.
    std::optional<std::vector<std::string>> edgeNames(const Object& o, const std::vector<std::string>& subs);
    std::optional<std::vector<std::string>> faceNames(const Object& o, const std::vector<std::string>& subs);

    // Placement of an object in part (body) coordinates.
    Placement placementOf(const Object& o) const;

    // Expression conversion (§10.2): our expression text or nullopt (no counterpart — the value is fixed).
    std::optional<std::string> convertExpression(const std::string& fcExpr, const std::string& ownerSheet = {});
    // Expression bound to property `path` of object o, converted; nullopt — none or not convertible (note added).
    std::optional<std::string> boundExpression(const Object& o, const std::string& path);
};

// Our stable name of sub-element `sub` ("FaceN", "EdgeN", "VertexN") of FreeCAD shape `shape` with numbering `index`,
// matched geometrically in state st after mapping the FreeCAD geometry by G (§4.4, §7а.3); nullopt — no match.
std::optional<std::string> matchSubElement(const kernel::Kernel& k, const regen::PartState& st, const kernel::Shape& shape,
                                           const BrepIndex& index, const std::string& sub, const Placement& G);

// Assembly (§7а): our assembly element appended to the document with its report entries.
void importAssembly(Ctx& c, const Object& assembly);

// Number in our notation: rounded to FCSTD_VALUE_DIGITS decimals (§12), with a unit.
std::string mm(double v);
std::string deg(double radians);
std::string degValue(double degrees);
double roundValue(double v);

// Sketch (§6): our sketch feature JSON for FreeCAD sketch `sk` in the current part; fills ctx.sketches[sk.name].
Result<Json> convertSketch(Ctx& c, const Object& sk, const std::set<std::string>& holeSketches);

// Solid feature or datum (§7): our feature JSON(s) appended to the current part, or Unmapped.
std::variant<std::vector<Json>, Unmapped> convertFeature(Ctx& c, const Object& o);

// World geometry helpers.
bool parallel(const Vec3& a, const Vec3& b, double tol = 1e-9);
// Built-in datum plane of our part equal (as a plane, any orientation) to the plane (point, normal); empty — none.
std::string datumPlaneFor(const Point3& p, const Vec3& n);
// Built-in datum axis equal (as a line) to (point, dir); empty — none. reversed — dir opposite to the axis.
std::string datumAxisFor(const Point3& p, const Vec3& dir, bool* reversed);
// Built-in datum axis parallel to dir (position ignored); empty — none.
std::string datumAxisParallel(const Vec3& dir, bool* reversed);

}  // namespace construe::fcstd::detail
