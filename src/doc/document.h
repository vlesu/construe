// Document model (spec/document.md). A document is a value; features are shared between snapshots.
#pragma once

#include "base/error.h"
#include "base/ids.h"
#include "doc/json_text.h"
#include "sketch/data.h"

#include <functional>
#include <map>
#include <memory>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace construe::doc {

inline constexpr int CURRENT_SCHEMA_VERSION = 5;
// Frame of an owned connection cut (§2.10о, §7): unit and orthogonal axes within this tolerance.
inline constexpr double DOC_FRAME_TOL = 1e-6;
inline constexpr double VIEW_AXIS_TOL = 1e-6;  // view orientation "plane": unit and orthogonal normal, up (§2.4г, FX7-DRW-007)

enum class FeatureType { Sketch, Extrude, Revolve, Fillet, Chamfer, Plane, Axis, Pattern, Mirror, Shell, Draft, Sweep, Loft, Hole, Thread, Import,
                         SheetMetal, SheetWall, SheetFlange, SheetBend, SheetCut,  // sheet metal — §2.10л
                         Gear,                                                     // gear rim — §2.10м
                         Connection,                                               // solution connection — §2.10н
                         ConnectionCut,                                            // cut owned by a connection — §2.10о
                         Coil };                                                   // spring / custom thread — §2.10п
const char* featureTypeName(FeatureType t);
// A feature with a tool — a source of patterns and mirrors (§2.10б, FX7-PT-008): extrude, revolve, sweep, loft, hole,
// gear rim, coil, pattern and mirror.
bool isToolFeature(FeatureType t);

struct Profiles {
    bool all = true;
    std::vector<std::string> regions;  // regionId, if not all
};

struct ExtrudeData {
    std::string sketch;
    Profiles profiles;
    bool reverse = false;              // direction: normal | reverse
    std::string endType = "blind";     // blind | symmetric | throughAll
    std::string depth = "10 mm";       // for blind/symmetric
    std::string operation = "new";     // new | add | cut | intersect
    std::vector<std::string> targets;  // assembly cut: instance paths (assembly.md §7г.1); empty — "all"
    std::string tabLeft, tabRight;     // tab extensions in a sheet part (§2.8, FX7-SH-003); empty — absent
    // "Up to" end (§2.8, S7F-FEAT-001): upTo — 1…2 UpToRef objects of end.to; upToOffset — end.offset (empty — absent).
    Json upTo = Json::array();
    std::string upToOffset;
};

struct RevolveData {
    std::string sketch;
    Profiles profiles;
    std::string axisSketchLine;        // or
    std::string axisDatum;             // axis:x | axis:y | axis:z
    std::string axisFeature;           // reference axis — ID of an axis feature (S2-REF-004)
    std::string angleType = "full";    // full | angle | symmetric
    std::string angleValue;            // for angle/symmetric
    std::string operation = "new";
};

struct EdgeFeatureData {
    std::vector<std::string> edges;
    std::string size;                  // fillet: radius; chamfer: distance
    // Chamfer (§2.10, FX7-PT-010): "" — equal distances; "distanceAngle" — size and angle; "twoDistances" — size and size2.
    std::string mode;
    std::string size2;                 // distanceAngle: angle; twoDistances: second distance
    bool flip = false;                 // the distance is measured on the other face
};

// Reference geometry (document.md §2.10а): definition fields after the common ones (mode, mode fields) — as in file.
struct RefGeomData {
    Json def;
};

// Pattern/mirror/shell/draft/sweep/loft/hole, sheet metal and gear features (document.md §2.10б–§2.10з, §2.10л, §2.10м;
// S2-FEAT-002…008, S5-SHEET-*, S6-GEAR-002…004): def fields after common, as in file.
struct PatternData {
    Json def;
};

using FeatureData = std::variant<sketch::SketchData, ExtrudeData, RevolveData, EdgeFeatureData, RefGeomData, PatternData>;

// Reference-geometry definition refs to features: (ID, expected type "plane"/"axis"/"sketch", JSON path from feature).
struct FeatureRefInDef {
    std::string id;
    std::string expected;
    std::string path;
};
std::vector<FeatureRefInDef> refGeomFeatureRefs(const Json& def);

struct Feature {
    std::string id;
    FeatureType type = FeatureType::Sketch;
    std::string name;
    bool suppressed = false;
    FeatureData data;
    // Dimension tolerances (§2.6, FX12-PMI-007): parameter path → tolerance {field, showDeviations?} | {upper, lower};
    // none — no tolerances.
    std::optional<Json> tolerances;

    const sketch::SketchData* sketchData() const { return std::get_if<sketch::SketchData>(&data); }
    const ExtrudeData* extrude() const { return std::get_if<ExtrudeData>(&data); }
    const RevolveData* revolve() const { return std::get_if<RevolveData>(&data); }
    const EdgeFeatureData* edgeFeature() const { return std::get_if<EdgeFeatureData>(&data); }
    const RefGeomData* refGeom() const { return std::get_if<RefGeomData>(&data); }
    const PatternData* pattern() const { return std::get_if<PatternData>(&data); }
};

using FeaturePtr = std::shared_ptr<const Feature>;

// Feature expression fields (document.md §2.11): traversal with optional text replacement; driving is false only for
// a reference (driven) sketch dimension.
enum class ExprDim { Length, Angle, Number };
void forEachExpression(Feature& feature, const std::function<void(std::string& text, ExprDim dim, bool driving)>& fn);
struct ExprField {
    std::string text;
    ExprDim dim;
    bool driving;
};
std::vector<ExprField> expressionFields(const Feature& feature);
// Tolerance of a feature dimension (§2.6, FX12-PMI-007): param — parameter path ("end.depth", "radius", …); none — nullptr.
const Json* featureTolerance(const Feature& feature, const std::string& param);

// Pattern/mirror refs to features (document.md §2.10б, §2.10в): (ID, expected type "tool"/"axis"/"plane", JSON path).
std::vector<FeatureRefInDef> patternFeatureRefs(const Json& def);

// Feature dependencies (document.md §2.6, S2-FEAT-010): feature IDs from ID refs and base names of stored stable names.
std::set<std::string, IdLess> featureDependencies(const Feature& feature);
// Feature IDs of base names in a stable name (naming.md §2.3): "f<digits>" then ".", no letter/digit/"_" before it.
void featureIdsInName(const std::string& name, std::set<std::string, IdLess>& out);

struct Rollback {
    enum class Kind { End, Start, After };
    Kind kind = Kind::End;
    std::string feature;  // for After
    bool operator==(const Rollback&) const = default;
};

// Section (document.md §2.4а, S2-VIEW-002): by a plane (plane + offset) or by a sketch (sketch).
struct Section {
    std::string id;
    std::string name;
    std::optional<Json> plane;   // PlaneRef
    std::string offset;          // length expression (only with plane)
    std::string sketch;          // ID of a sketch feature (stepped section)
    bool flip = false;
    bool hidden = false;
    std::vector<std::string> exclude;  // assembly: instances that the section does not cut (sections.md §7а)
    bool operator==(const Section&) const = default;
};

// Assembly instance (document.md §2.4б, assembly.md).
struct Instance {
    std::string id;
    std::string name;
    std::string refElement;
    std::string refFile;  // object in another file: path relative to this file's dir (assembly.md §5а); empty: this file
    std::array<double, 3> position{0, 0, 0};
    std::array<double, 4> rotation{1, 0, 0, 0};  // quaternion [w, x, y, z]
    bool fixed = false, suppressed = false, hidden = false, transparent = false;
    // Render color of the instance in this assembly (§2.4б, FX12-ASM-003): #RRGGBB over the part color; none — by the part.
    std::optional<std::string> color;
    // Instance variables (§2.4б, S6-PARAM-004): chosen variant rows and formulas.
    struct VariantRow {
        std::string table;
        std::string row;
        bool operator==(const VariantRow&) const = default;
    };
    struct Override {
        std::string variable;
        std::string expr;
        bool operator==(const Override&) const = default;
    };
    std::vector<VariantRow> variantRows;
    std::vector<Override> overrides;
    // Form of an underdetermined subassembly instance (§2.4б, FX9-ASM-010): placements of its nested instances by path
    // relative to the subassembly ("i7", deeper — "i3/i2"), each in the frame of its own assembly; empty — the file's ones.
    struct FormPlacement {
        std::string path;
        std::array<double, 3> position{0, 0, 0};
        std::array<double, 4> rotation{1, 0, 0, 0};
        bool operator==(const FormPlacement&) const = default;
    };
    std::vector<FormPlacement> form;
    bool operator==(const Instance&) const = default;
};

// Drawing view (§2.4г, drawing.md §2.3).
struct DrawingView {
    std::string id;
    std::string name;
    std::string kind = "base";  // base | projected | section | detail
    std::string refElement;     // base: model
    std::string refFile;        // base: object of another file; empty — this file
    std::string orientation;    // base: front | back | top | bottom | left | right | iso
    std::string parent;         // projected, section, detail: parent view
    std::string direction;      // projected: top | bottom | left | right
    std::array<std::array<double, 2>, 2> line{};  // section: section line in parent coordinates (drawing.md §6.11)
    // section: binding of the line to parent elements by stable names (FX9-DRW-002, drawing.md §6.11; schema
    // sectionLineBind): {"axis": face} or {"points": [{vertex|center|mid|edge: name, at?, t}, …]}.
    std::optional<Json> lineBind;
    bool flip = false, sectionOnly = false;        // section
    std::array<double, 2> center{0, 0};            // detail: center in parent coordinates (drawing.md §6.12)
    double radius = 0;                             // detail
    std::array<double, 2> position{0, 0};
    std::optional<std::string> scale;
    bool hiddenLines = false, tangentEdges = false;
    bool centerLines = true;  // false — without axis and center lines (drawing.md §6.15)
    bool pmi = true;          // false — without the model's technical requirement annotations (drawing.md §6.18)
    // base, orientation "plane" (FX7-DRW-007, drawing.md §6.1): projection plane of the model (PlaneRef JSON) and the
    // view triad resolved from it — toward the viewer and up (world unit vectors).
    std::optional<Json> plane;
    std::optional<std::array<double, 3>> normal, up;
    std::string display;  // base, projected: "render" — shaded image (FX7-DRW-007, drawing.md §6.22); empty — by the standard
    bool flat = false;    // base: flat pattern of a sheet metal part (FX7-DRW-008, drawing.md §6.21)
    bool exploded = false;  // base: assembly in its exploded view (FX7-ASM-005, drawing.md §6.24)
    // base: model sections the view is cut with (S7F-DRW-005, drawing.md §6.26); none — those enabled in the model.
    std::optional<std::vector<std::string>> sections;
    bool operator==(const DrawingView&) const = default;
};

// Text block on a sheet (FX7-DRW-006, §2.4г, drawing.md §6.23): lines separated by '\n'.
struct SheetText {
    std::string id;
    std::array<double, 2> position{0, 0};  // sheet mm: start of the baseline of the first line
    std::string text;
    double height = 5;                     // mm: GOST 2.304 font size (capital letter height)
    double angle = 0;                      // degrees, counterclockwise
    std::string font;                      // "gost" | "iso" | font file name (FX10-DRW-004); empty — the drawing font (drawing.md §3.2)
    std::vector<font::StyleSpan> styles;   // bold / italic / underline fragments, code points (FX9-DRW-006)
    double boxWidth = 0, boxHeight = 0;    // mm: the frame (0 — none); width wraps lines by words (FX9-DRW-007)
    // Alignment of paragraphs (between '\n') by index: "left" | "center" | "right"; missing — "left" (S7F-DRW-002).
    std::vector<std::string> aligns;
    bool operator==(const SheetText&) const = default;
};

// Dimension ref to the view's model geometry (§2.4г): edge or vertex by stable name; a face line without a name —
// a silhouette (contour line) or a cut line on a section by face name (FX3-DRW-002).
struct DimensionRef {
    std::string kind = "edge";  // edge | vertex | silhouette | cut | axis | center (FX9-DRW-001) | construction (FX14-DRW-006)
    std::string name;
    int side = 0;               // silhouette, cut: side of the generatrix +1 / −1 (0 — no side)
    std::string arc;            // edge for linear: "near" | "far" — to the near / far point of the circle (FX3-DRW-004)
    bool operator==(const DimensionRef&) const = default;
};

// Drawing sheet dimension (§2.4г, drawing.md §6а).
struct SheetDimension {
    std::string id;
    std::string kind = "linear";     // linear | diameter | radius | angle | arcLength | pathLength
    std::string orientation;         // linear: horizontal | vertical | aligned
    std::string view;
    std::vector<DimensionRef> refs;
    double offset = 0;
    double angle = 0;                // diameter, radius, angle, pathLength
    std::optional<std::string> prefix, text, suffix;
    std::optional<std::string> tolerance;  // model tolerance display: hide|field|fieldDeviations|deviations (FX3-DRW-001)
    bool brk = false;                      // "break": dimension line with a break (linear, diameter; FX3-DRW-003)
    // Tolerance set in the drawing itself (FX9-DRW-009): {field?, upper?, lower?} instead of the model tolerance.
    std::optional<Json> fit;
    // Feature dimension placed from the model (§2.4г, FX12-PMI-007): {feature, param} — its tolerance is the feature's
    // tolerance of this parameter (drawing.md §6а.3).
    std::optional<Json> source;
    bool operator==(const SheetDimension&) const = default;
};

// Sheet mark (§2.4г, drawing.md §6.25, FX9-DRW-008): a reference to a model object (annotation, weld, rest roughness)
// and to a view; values come from the model at every build (local — from def, FX9-DRW-009).
struct SheetMark {
    std::string id;
    std::string kind = "annotation";  // annotation | weld | restRoughness | balloon (FX14-DRW-002) | partsList (FX14-DRW-003)
    std::string view;                 // empty — none (partsList)
    std::string source;               // annotation / weld ID, balloon — face name; empty — none (restRoughness, partsList, local)
    std::optional<std::array<double, 2>> position;  // sheet mm: the sign anchor; none — automatic
    bool local = false;               // FX9-DRW-009: set in the drawing, values from def
    std::optional<Json> def;          // with local: annotation record as in §2.4д without id
    bool operator==(const SheetMark&) const = default;
};

// Model mark deleted by the user (§2.4г, FX9-DRW-008): the command does not bring it back.
struct DismissedMark {
    std::string kind;
    std::string model;   // model element ID
    std::string source;  // empty — none (restRoughness)
    bool operator==(const DismissedMark&) const = default;
};

// Construction point on a view (§2.4г, drawing.md §6.29, FX14-DRW-006): view coordinates and an optional binding to the
// model of the view by a stable name.
struct ConstructionPoint {
    std::array<double, 2> xy{0, 0};  // view coordinates, model mm: a free point; a bound one — its position when bound
    std::string bind;                // "" — free | vertex | center (face axis seen end-on) | edge
    std::string name;                // stable name of the bound model entity
    std::optional<double> at;        // edge: parameter on a line edge; none — its middle (circle and arc — the center)
    bool operator==(const ConstructionPoint&) const = default;
};

// Construction on a sheet view (§2.4г, drawing.md §6.29, FX14-DRW-006): thin lines drawn on the view.
struct SheetConstruction {
    std::string id;
    std::string kind = "line";  // line | circle | arc | pitchCircle | axis
    std::string view;
    std::vector<ConstructionPoint> points;
    std::optional<double> radius;  // circle with one point (the center): model mm
    std::string line;              // GOST 2.303 line type: solid | dashDot | dashed; empty — by kind (drawing.md §6.29)
    bool operator==(const SheetConstruction&) const = default;
};

// Drawing sheet (§2.4г, drawing.md §2).
struct Sheet {
    std::string id;
    std::string name;
    std::string format = "A3";
    std::string orientation = "landscape";
    std::string scale = "1:1";
    bool scaleManual = false;  // the scale was set by the user: three views do not change it (drawing.md §6.2б, FX10-DRW-001)
    std::map<std::string, std::string> titleBlock;
    std::vector<DrawingView> views;
    std::vector<SheetDimension> dimensions;
    std::vector<SheetText> texts;  // FX7-DRW-006
    std::vector<SheetMark> marks;  // FX9-DRW-008
    std::vector<SheetConstruction> constructions;  // FX14-DRW-006
    bool operator==(const Sheet&) const = default;
    int viewIndex(const std::string& viewId) const;
    int dimensionIndex(const std::string& id) const;
    int textIndex(const std::string& id) const;
    int markIndex(const std::string& id) const;
    int constructionIndex(const std::string& id) const;
};

// Assembly mate (assembly.md §7.1).
struct Mate {
    std::string id;
    std::string name;
    std::string type;
    std::array<std::string, 2> refs;
    std::optional<std::string> value;
    bool flip = false;
    std::optional<std::array<double, 7>> relative;  // lock: [qw, qx, qy, qz, tx, ty, tz]
    std::optional<std::array<double, 2>> angles;  // gear: rim angles [α_A, α_B] from the line of centres, rad (assembly.md §7.4)
    bool suppressed = false;
    bool operator==(const Mate&) const = default;
};

// Exploded view of an assembly (assembly.md §7д, FX7-ASM-005): its own top-level placements and mates suppressed in the mode.
struct ExplodedPlacement {
    std::string instance;
    std::array<double, 3> position{0, 0, 0};
    std::array<double, 4> rotation{1, 0, 0, 0};
    bool operator==(const ExplodedPlacement&) const = default;
};
struct Exploded {
    bool active = false;
    std::optional<std::vector<ExplodedPlacement>> placements;  // none — not computed yet (automatic explosion, §7д.3)
    std::vector<std::string> suppressedMates;
    // Mates the saved view has taken into account (§7д.1, FX12-ASM-002); none — all of them (older files).
    std::optional<std::vector<std::string>> knownMates;
    bool operator==(const Exploded&) const = default;
};

// Instance pattern of an assembly (assembly.md §7е, FX7-ASM-007): the record as in the file (schema $defs/instancePattern).
struct InstancePattern {
    std::string id;
    std::string name;
    Json def;  // the record without id and name: source, mode (linear | circular | feature), mode fields, suppressed
    std::string source() const { return def.value("source", std::string()); }
    std::string mode() const { return def.value("mode", std::string()); }
    bool suppressed() const { return def.value("suppressed", false); }
    bool operator==(const InstancePattern&) const = default;
};

// Snapshot of a frozen context reference (assembly.md §7в.5).
struct FrozenInstance {
    std::string instance;
    std::array<double, 3> position{0, 0, 0};
    std::array<double, 4> rotation{1, 0, 0, 0};
    Json element;  // description of the part element without the context field
    bool operator==(const FrozenInstance&) const = default;
};

// Designing a part in the context of an assembly (assembly.md §7в, document.md §2.4).
struct PartContext {
    std::string assembly, instance;
    std::optional<std::vector<FrozenInstance>> frozen;
    bool operator==(const PartContext&) const = default;
};

struct Element {
    std::string id;
    std::string kind = "part";
    std::string name;
    std::string group;  // element group ID (§2.1а, FX10-TREE-001); empty — none
    std::optional<std::string> designation;  // designation (§2.4, S2-PART-001)
    std::optional<Json> material;            // copy of the material properties (materials.md §3, S2-PART-002)
    std::optional<std::string> color;        // #RRGGBB (S2-PART-004)
    std::vector<std::pair<std::string, std::string>> faceColors;  // (stable face name, #RRGGBB), by name bytewise
    Rollback rollback;
    std::vector<FeaturePtr> features;
    std::vector<Section> sections;  // §2.4а
    bool transparent = false;       // the part is transparent in assemblies (S2-VIEW-004)
    // Part kind (§2.4, FX7-SH-002, S5-SHEET-006): true — sheet metal, false — ordinary (converted); none — by the history.
    std::optional<bool> sheetMetal;
    // Sheet metal part (§2.4, FX7-SH-002, S5-SHEET-006): the sheetMetal field, otherwise a «Лист» ("Sheet") feature.
    bool isSheetPart() const;
    std::vector<Instance> instances;  // assembly (§2.4б)
    std::vector<Mate> mates;          // assembly (assembly.md §7)
    std::vector<Json> welds;          // assembly: weld seams (§2.4б, weld.md §2.1), records per schema $defs/weld
    std::vector<InstancePattern> patterns;  // assembly: instance patterns (§2.4б, assembly.md §7е)
    std::optional<Exploded> exploded;       // assembly: exploded view (§2.4б, assembly.md §7д)
    std::optional<PartContext> context;  // part in the context of an assembly (assembly.md §7в)
    std::vector<Sheet> sheets;           // drawing (§2.4г)
    std::optional<std::string> standard, projection;  // drawing: standard, projection method (§2.4г, drawing.md §2.5)
    bool nameManual = false;                         // drawing: the name was set by the user (drawing.md §2.6, FX9-DRW-003)
    std::vector<DismissedMark> dismissedMarks;       // drawing: model marks deleted by the user (drawing.md §6.25)
    std::vector<Json> annotations;       // part technical requirements (§2.4д), records per schema $defs/annotation
    std::optional<Json> restRoughness;   // roughness of the remaining surfaces (§2.4, pmi.md §2.1а): {sign, parameter?, value?}
    // Specification (§2.4е, specification.md): assembly, manual title block fields, overrides and manual rows
    // (records per schema $defs/specRowOverride, $defs/specExtraRow).
    std::string specAssembly;
    std::map<std::string, std::string> specTitleBlock;
    std::vector<Json> specRows, specExtraRows;
    bool isAssembly() const { return kind == "assembly"; }
    bool isSpecification() const { return kind == "specification"; }
    bool isDrawing() const { return kind == "drawing"; }
    int sheetIndex(const std::string& sheetId) const;
    bool isBody() const { return kind == "body"; }  // imported body (§2.4в)

    int indexOf(const std::string& featureId) const;
    int annotationIndex(const std::string& id) const;  // -1 — none
    int weldIndex(const std::string& id) const;        // -1 — none
    int patternIndex(const std::string& id) const;     // -1 — none
    // Mate suppressed in the shown state: in the working position or, with the exploded view on, in its mode (§7д.4).
    bool mateSuppressedShown(const Mate& m) const;
    bool explodedActive() const { return exploded && exploded->active; }
    FeaturePtr find(const std::string& featureId) const;
};

struct Variable {
    std::string id;
    std::string name;
    std::string type = "float";        // float | int | string | list (§2.3, S6-PARAM-001)
    std::string expr;
    std::vector<std::string> values;   // list only
    std::optional<std::string> comment;
};

// Variant table (§2.3а, S6-PARAM-003).
struct VariantRow {
    std::string name;
    std::vector<std::string> values;  // a cell per column
    bool operator==(const VariantRow&) const = default;
};
struct VariantTable {
    std::string id;
    std::string name;
    std::vector<std::string> columns;  // variable IDs
    std::vector<VariantRow> variants;
    std::string active;
    bool operator==(const VariantTable&) const = default;
    const VariantRow* row(const std::string& name) const;
};

struct Counters {
    int64_t element = 1, feature = 1, entity = 1, constraint = 1, variable = 1, instance = 1, mate = 1, sheet = 1, view = 1,
            annotation = 1, dimension = 1, group = 1, table = 1, weld = 1, text = 1, pattern = 1, mark = 1,
            elementGroup = 1;  // element groups of the objects panel (§2.1а, FX10-TREE-001)
    int64_t construction = 1;  // sheet constructions (§2.4г, FX14-DRW-006)
};

enum class IdKind { Element, Feature, Entity, Constraint, Variable, Instance, Mate, Sheet, View, Annotation, Dimension, Group, Table, Weld, Text, Pattern, Mark,
                    ElementGroup,
                    Construction };

// Which program saved the file (document.md §2): written by saveDocument, does not affect meaning.
struct SavedBy {
    std::string app;
    std::string version;
    bool operator==(const SavedBy&) const = default;
};

struct Document;

// Instance reference to a library part copy (§2.4б, §2.13): refFile = LIBRARY_REF_PREFIX + LibraryPart::id; in the
// file — ref.library. The linked document of the session under the key refFile (library.md §6.3).
inline constexpr const char* LIBRARY_REF_PREFIX = "library:";
bool isLibraryRef(const std::string& refFile);
std::string libraryRef(const std::string& libraryPartId);

// Copy of a library package in the document (§2.13, library.md §6.1): the document is self-contained.
struct LibraryPart {
    std::string id;           // "<package>@<fingerprint>"
    std::string package;      // package id
    std::string fingerprint;  // 16 hex digits (library.md §3)
    std::string title;
    std::string standard;     // main standard ("ГОСТ Р ИСО 4017-2013")
    std::string designation;  // designation rule (library.md §7)
    std::string sizesTable;   // ID of the sizes variant table in document
    std::shared_ptr<const Document> document;  // template with the sizes table; immutable, shared between snapshots
};

// Copy of a solution package in the document (§2.14, solutions.md §5.2): definition and the package's own tables.
struct SolutionTable {
    std::string id;
    std::vector<std::string> columns;
    std::vector<VariantRow> rows;
    bool operator==(const SolutionTable&) const = default;
};
struct SolutionCopy {
    std::string id;           // "<package>@<fingerprint>"
    std::string package;
    std::string fingerprint;  // 16 hex digits (solutions.md §5.1)
    std::string title;
    Json definition;          // solution.json as an object
    std::vector<SolutionTable> tables;
    bool operator==(const SolutionCopy&) const = default;
};

// Named group of elements in the objects panel (§2.1а, FX10-TREE-001): one level, members are the elements whose
// `group` is the group ID, contiguous in `elements`.
struct ElementGroup {
    std::string id;  // eg<N>
    std::string name;
    bool operator==(const ElementGroup&) const = default;
};

struct Document {
    std::optional<SavedBy> savedBy;
    std::string lengthUnit = "mm";
    std::string angleUnit = "deg";
    Counters counters;
    std::vector<Variable> variables;
    std::vector<VariantTable> variantTables;  // §2.3а
    std::vector<ElementGroup> elementGroups;  // §2.1а, in order of creation
    std::vector<Element> elements;
    std::vector<LibraryPart> libraryParts;  // §2.13
    std::vector<SolutionCopy> solutions;    // §2.14
    // Active element (core-api.md §2, document.md §6): session state, not written to the file and not
    // part of the canonical form; empty or an unknown ID — elements[0].
    std::string activeElement;

    int activeIndex() const;
    Element& active() { return elements[static_cast<size_t>(activeIndex())]; }
    const Element& active() const { return elements[static_cast<size_t>(activeIndex())]; }
    int elementIndex(const std::string& id) const;  // -1 — none
    const Variable* variable(const std::string& id) const;
    const VariantTable* variantTable(const std::string& id) const;
    const LibraryPart* libraryPart(const std::string& id) const;
    const SolutionCopy* solution(const std::string& id) const;
    const ElementGroup* elementGroup(const std::string& id) const;
    // Table whose column is variable id (§2.3а); nullptr — none.
    const VariantTable* tableOfVariable(const std::string& variableId) const;
    // Expression of a variable for evaluation: the cell of the given row of its table (the active one if row is empty
    // or not found), otherwise expr (§2.3а).
    std::string effectiveExpr(const Variable& v, const std::string& row = {}) const;
};

struct ParseOptions {
    bool allowTestVersion0 = false;  // test migration v0 → v1 (§4.3; tests only)
};

// Element group invariants (§2.1а): a reference to an unknown group is cleared, the members of a group are gathered
// right after its first member (in their order), groups without members are removed.
void normalizeElementGroups(Document& doc);

Document createNew(const std::string& lengthUnit = "mm", const std::string& angleUnit = "deg");
Result<Document> parse(const std::string& text, const ParseOptions& options = {});
std::string serialize(const Document& doc);
std::string serializeFeature(const Feature& feature);
Status validate(const Document& doc);  // stages 5–6 of §5 for an in-memory document

// JSON representation (structure of document.schema.json) — for api commands that accept
// feature definitions as JSON. documentFromJson performs stages 5–6 of §5.
Json documentToJsonValue(const Document& doc);
Json elementToJsonValue(const Element& element);
Element elementFromJsonValue(const Json& j);  // input is schema-validated ($defs/partElement | assemblyElement)
Json featureToJsonValue(const Feature& feature);
// Section ↔ JSON (§2.4а); the input of sectionFromJsonValue is validated against the schema ($defs/section).
Json sectionToJsonValue(const Section& section);
Json mateToJsonValue(const Mate& mate);
Mate mateFromJsonValue(const Json& j);  // input is validated against the schema ($defs/mate)
Json instancePatternToJsonValue(const InstancePattern& p);  // the record as in the file ($defs/instancePattern)
InstancePattern instancePatternFromJsonValue(const Json& j);
Section sectionFromJsonValue(const Json& j);
// Sheet/view ↔ JSON (§2.4г); input of *FromJsonValue is schema-validated ($defs/sheet, $defs/baseView|projectedView).
Json sheetToJsonValue(const Sheet& sheet);
Sheet sheetFromJsonValue(const Json& j);
Json viewToJsonValue(const DrawingView& view);
DrawingView viewFromJsonValue(const Json& j);
// Sheet dimension ↔ JSON (§2.4г); input of dimensionFromJsonValue is schema-validated ($defs/*Dimension).
Json dimensionToJsonValue(const SheetDimension& a);
Json sheetTextToJsonValue(const SheetText& t);
Json sheetMarkToJsonValue(const SheetMark& m);
Json sheetConstructionToJsonValue(const SheetConstruction& c);
SheetDimension dimensionFromJsonValue(const Json& j);
Result<Document> documentFromJson(const Json& j);
// Parsing an entity/constraint from JSON without schema validation (nlohmann::json exception on
// wrong field types); the caller checks the result with validate.
sketch::Entity entityFromJson(const Json& je);
sketch::Constraint constraintFromJson(const Json& jc);
Json entityToJsonValue(const sketch::Entity& e);
Json constraintToJsonValue(const sketch::Constraint& c);
// Linked sketch group ↔ JSON (sketch.md §2.6.1); input of groupFromJson is schema-validated ($defs/sketchGroup).
sketch::Group groupFromJson(const Json& jg);
Json groupToJsonValue(const sketch::Group& g);

// ID allocation: returns the ID and increments the counter (the document is a value owned by the caller).
std::string allocate(Document& doc, IdKind kind);
// Default name (§2.12) for a feature of type t in an element (or for a new element if t == nullopt).
std::string defaultFeatureName(const Element& element, FeatureType t);
// Same, considering the operation (§2.12): extrude with cut/intersect — "Cut<N>" (FX1-FEAT-003); exclude — feature
// whose name is not considered taken (renaming while editing).
std::string defaultFeatureName(const Element& element, FeatureType t, const std::string& operation, const std::string& exclude = {});
// Base of a feature's default name (§2.12) taking the operation into account.
std::string defaultFeatureBase(FeatureType t, const std::string& operation = {});
std::string defaultElementName(const Document& doc, const std::string& kind = "part");
// Bases of the default names in the interface language (§2.12, FX14-UI-004): the client sets a table "key → base"
// (keys element.<kind>, feature.<type>, feature.cut, mate.<type>, section, sheet, view, table, instancePattern, weld,
// elementGroup); a key missing from it — the built-in (Russian) base. Empty table — built-in bases only.
void setNameBases(std::map<std::string, std::string> table);
// The base of a key: the client's or the built-in one; an unknown key — empty.
std::string nameBase(const std::string& key);
// The built-in bases (key → Russian base) — the client's table is checked against them.
const std::map<std::string, std::string>& builtinNameBases();
// base + N with the smallest N ≥ 1 not in taken (§2.12).
std::string firstFreeName(const std::string& base, const std::set<std::string>& taken);
// Default names of a drawing sheet and view: "Sheet N", "View N" (§2.12, §2.4г).
std::string defaultSheetName(const Element& drawing);
// Default name of a variant table: "Table N" (§2.12).
std::string defaultTableName(const Document& doc);
std::string defaultViewName(const Element& drawing);
// Value of the scale "a:b" (drawing.md §2.2); nullopt — invalid string or a, b ≤ 0.
std::optional<double> scaleValue(const std::string& text);
// Material in canonical form (field order of materials.md §3): id?, name, density, youngModulus, poisson,
// yieldStrength, tensileStrength, color.
Json canonicalMaterial(const Json& m);
// restRoughness in canonical field order sign, parameter, value (§2.4).
Json canonicalRestRoughness(const Json& r);

// The sections a main view is cut with (drawing.md §6.26, S7F-DRW-005): the view's list (IDs present in the model, in
// the model's order), none — the model's sections that are not hidden.
std::vector<std::string> effectiveViewSections(const Element& model, const std::optional<std::vector<std::string>>& sections);
// The section list a view of a sheet is shown with (drawing.md §6.26, FX10-DRW-003): its own sections (main and
// projected views), otherwise the list of its parent, up the chain to the main view; nullopt — none in the chain (the
// sections enabled in the model). A missing parent or a cycle ends the chain.
std::optional<std::vector<std::string>> viewSectionsList(const Sheet& sheet, const std::string& viewId);
// Main view at the start of the parent chain of a view (nullptr — missing parent or a cycle).
const DrawingView* chainBaseView(const Sheet& sheet, const std::string& viewId);

}  // namespace construe::doc
