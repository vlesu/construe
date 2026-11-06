// Internal state of the export (spec/fcstd.md §12б): the objects being written, the regenerated part, references of our
// features to FreeCAD objects and sub-element names.
#pragma once

#include "expr/expr.h"
#include "fcstd/export.h"
#include "fcstd/import_internal.h"
#include "fcstd/writer.h"
#include "regen/regen.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace construe::fcstd::xp {

using Json = doc::Json;

// Shape written for a state (all bodies of the part): BRep text, the shape read back from it (kernel indices of
// FreeCAD's EdgeN/FaceN — spec/fcstd.md §4.3) and the numbering.
struct WrittenShape {
    std::string text;
    std::optional<kernel::Shape> shape;
    std::optional<BrepIndex> index;
};

// A feature that has no FreeCAD counterpart (§12г): it ends the attempt, everything up to it goes into the base body.
struct Unmapped {
    std::string reason;
};

struct Ctx {
    const doc::Document& d;
    const kernel::Kernel& k;
    expr::DocumentUnits units;
    expr::VariableTable vars;
    write::Names names;
    std::vector<write::Object> objects;
    std::map<std::string, std::string> parts;  // zip part → bytes
    std::vector<ReportEntry> report;
    std::vector<ExportedSketch> sketches;
    std::map<std::string, std::pair<double, double>> bodies;
    std::map<std::string, std::string> aliasOf;  // our variable name → spreadsheet alias (§12е)
    std::string sheet;                           // spreadsheet object name; empty — no variables
    std::map<std::string, std::string> objectOfElement;  // our element ID → its FreeCAD object (body, feature, assembly)
    std::vector<ExportedAssembly> assemblies;

    // Current part.
    const doc::Element* element = nullptr;
    std::optional<regen::PartResult> regen;
    std::map<std::string, std::string> objectOf;  // our feature ID → FreeCAD object name (current attempt)
    std::map<std::string, std::string> originOf;  // "axis:x", "plane:front", … → object name of the body origin
    std::string prevSolid;                       // FreeCAD object of the previous solid feature (base for refs)
    std::string prevFeature;                     // our feature whose state the previous solid object holds
    bool staticBase = false;  // the previous solid object is the base body: FreeCAD keeps our shape (§12г.3)
    bool needBase = false;    // a reference to faces or edges was asked of a shape FreeCAD rebuilds itself
    std::map<std::string, std::map<std::string, int>> sketchAxes;  // our sketch → construction line → FreeCAD "AxisN"
    std::map<std::string, WrittenShape> written;  // our feature ID → shape of the state after it
    std::vector<std::string> notes;              // reasons for the feature being converted (approximate)

    Ctx(const doc::Document& dd, const kernel::Kernel& kk) : d(dd), k(kk) {}

    // State after a feature of the current part (nullptr — none).
    regen::StatePtr stateAfter(const std::string& featureId) const;
    // Written shape of the state after the feature (empty text — no bodies).
    const WrittenShape& shapeAfter(const std::string& featureId);
    // Zip part with the shape of the state after the feature for object `object` (empty — no bodies).
    std::string shapePart(const std::string& object, const std::string& featureId);

    // FreeCAD sub-element names "EdgeN" / "FaceN" of the previous solid object for our stable names (§12г.3);
    // nullopt — not every name has its counterpart in the written shape.
    std::optional<std::vector<std::string>> edgeSubs(const std::vector<std::string>& ourNames);
    std::optional<std::vector<std::string>> faceSubs(const std::vector<std::string>& ourNames);

    // Value of an expression field in internal units (mm, rad, number); nullopt — not evaluable.
    std::optional<double> value(const std::string& text, std::optional<expr::Dim> expected);
    // FreeCAD expression for a field (§12е.2): nullopt — the field holds no variable reference or the expression has
    // no counterpart (then the note expressionFixed:<field> is added when it references variables).
    std::optional<std::string> binding(const std::string& text, std::optional<expr::Dim> expected, const std::string& field,
                                       bool inSheet = false);
    void note(const std::string& n);
};

// FreeCAD expression of ours (§12е.2): variables → `Variables.<alias>` (in the sheet itself — the bare alias);
// nullopt — no counterpart.
std::optional<std::string> translateExpression(const Ctx& c, const std::string& text, bool inSheet);

// Sketch (§12в): the Sketcher object of our sketch feature; adds the ExportedSketch record and notes.
write::Object exportSketch(Ctx& c, const doc::Feature& f, const std::string& name, bool holeSketch);

// Solid feature (§12г): the PartDesign object(s), or Unmapped. `name` — the object name to use.
std::variant<std::vector<write::Object>, Unmapped> exportFeature(Ctx& c, const doc::Feature& f, const std::string& name);
// FreeCAD type name base for object names ("Pad", "Pocket", …) of a mappable feature; empty — no counterpart.
std::string objectBase(const doc::Feature& f);

// Placement of a FreeCAD built-in object of the body origin (§12б.2).
write::Placement originPlacement(const std::string& role);
// FreeCAD origin role of a built-in plane (§12б.2): plane:front — XY_Plane, plane:top — XZ_Plane, otherwise YZ_Plane.
std::string datumPlaneRole(const std::string& datum);
// Origin of a body or an assembly (§12б.2): the App::Origin object and its seven built-in objects; fills c.originOf.
std::pair<write::Object, std::vector<write::Object>> makeOrigin(Ctx& c, const std::string& originName);

// Assembly (§12з): Assembly object `name`, its origin, joint group, links and joints; report entries.
void exportAssembly(Ctx& c, const doc::Element& el, const std::string& name);

}  // namespace construe::fcstd::xp
