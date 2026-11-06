// Part regeneration (spec/regen.md).
#pragma once

#include "regen/assembly.h"
#include "base/cancel.h"
#include "doc/document.h"
#include "expr/expr.h"
#include "kernel/kernel.h"
#include "regen/naming.h"
#include "sheet/sheet.h"
#include "sketch/sketch.h"
#include "solutions/solutions.h"

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace construe::regen {

inline constexpr int REGEN_ALGO_VERSION = 30;  // includes KERNEL_CONTRACT_VERSION and NAMING_VERSION
inline constexpr double REGEN_WALL_TOL = 1e-4;  // mm (× max(1, t)): inner wall edge opposite the outer faces (§4.4 item 3a)
inline constexpr double REGEN_TOOL_GAP = 1e-4;  // mm: region body bounding boxes are separated — tool without a boolean (§4.2 item 4)
inline constexpr int PATTERN_MAX_INSTANCES = 1000;  // instances along a pattern direction (§4.7)
inline constexpr double PATTERN_INT_TOL = 1e-9;     // "integer" number of instances, "full turn" (§4.7)
inline constexpr double SWEEP_JOIN_TOL = 1e-6;      // mm: path joints, start on the profile plane (§4.11)
inline constexpr double HOLE_POINT_MERGE_TOL = 1e-6;  // mm: coincident hole points (§4.13 item 1)
inline constexpr double SWEEP_ANGLE_TOL = 1e-6;     // |sin|: path tangents (§4.11)
inline constexpr double SWEEP_MIN_PROFILE_SIN = 0.0174524;  // sin 1°: the start tangent leaves the profile plane (§4.11, FX7-PT-002)
inline constexpr size_t CACHE_MAX_ENTRIES = 256;
inline constexpr double REF_PARALLEL_TOL = 1e-9;    // |sin| of the angle: parallelism in reference geometry (§4.6)
inline constexpr double REF_DEGENERATE_TOL = 1e-6;  // mm²: degenerate triangle of three points (§4.6)
inline constexpr double ADD_NO_EFFECT_REL_TOL = 1e-9;  // volume fraction: «Add» did not change the bodies (§4.2 item 5)
inline constexpr double GEAR_DIAMETER_TOL = 1e-3;     // mm: face diameter equals the tip diameter (§4.17 item 3)
inline constexpr double GEAR_TIP_SHIFT = 0.25;        // × module: tool tips past the tip circle when the face carries the tips (§4.17 item 7)
inline constexpr double GEAR_FILL_DEPTH = 0.1;        // × module: fill below the root circle (§4.17 item 8, drawing.md §6.20)
inline constexpr double GEAR_ADD_OVERLAP = 0.1;  // × m: the added rim overlaps the face (§4.17 item 7, FX7-PT-006)

struct Body {
    std::string id;
    kernel::Shape shape;
    std::vector<std::string> bases;  // base names of faces (naming §4.1)
};

// Sketch curve in world coordinates (§2.1, piercing sketch.md §2.5): arc — counterclockwise about axis from a to b.
struct WorldCurve {
    enum class Kind { Line, Arc, Circle };
    std::string id;
    Kind kind = Kind::Line;
    Point3 a = Point3::Zero(), b = Point3::Zero(), center = Point3::Zero();
    Vec3 axis = Vec3::UnitZ();
    double radius = 0;
};

// The piercing point of curves with a sketch plane (sketch.md §2.5): of the crossings — the nearest to the frame
// origin; an end of a segment closer than 1e-9 to the plane lies on it.
std::optional<Point3> piercePoint(const std::vector<WorldCurve>& curves, const Plane& frame);

struct SketchEntry {
    sketch::SketchResult result;
    Plane frame;
    std::map<std::string, sketch::Projection> projections;  // geometry of projected entities (sketch.md §2.5)
    std::vector<WorldCurve> curves;                         // non-construction curves (§2.1)
};

// Reference axis (§2.1, §4.6): point and unit direction.
struct RefAxis {
    Point3 point = Point3::Zero();
    Vec3 dir = Vec3::UnitZ();
};

// Extrude/revolve tool for patterns (§2.1, §4.7).
struct ToolEntry {
    kernel::Shape shape;
    std::vector<std::string> bases;  // tool face bases (by FaceIdx)
    std::string operation;           // add | cut | intersect
};

// Thread (§2.1, §4.14): cosmetic representation and check; does not change the body geometry.
struct ThreadEntry {
    std::string feature, designation;
    bool internal = false;
    Point3 origin = Point3::Zero();
    Vec3 dir = Vec3::UnitZ();
    double radius = 0, d = 0, pitch = 0, length = 0;
    double height = 0;  // > 0: own profile height of a free thread (§4.14 item 2, FX7-PT-003); 0 — ISO 68-1
};

// Gear rim (§2.1, §4.17, gear.md): parameters for the drawing representation and the parameter table.
struct GearEntry {
    std::string feature;
    bool internal = false, sector = false;
    double module = 0, shift = 0;
    int teeth = 0;        // z of the full gear
    int formed = 0;       // teeth formed (sector: n, full gear: z)
    std::string accuracy;  // empty — not given
    Point3 origin = Point3::Zero();  // rim start on the axis
    Vec3 dir = Vec3::UnitZ();        // unit, along the rim width
    Vec3 ref = Vec3::UnitX();        // unit, reference direction of angles (§4.17 item 5)
    double width = 0;
    double start = 0, angle = 0;     // sector: from start, angle n·τ (rad, counterclockwise about dir)
    double d = 0, da = 0, df = 0;
    kernel::Shape fill;  // spaces filled up to the tip circle — conventional representation (drawing.md §6.20)
};

// Sheet metal part (§2.1, §4.16, sheet.md §2.3): the model and its bodies.
struct SheetEntry {
    std::shared_ptr<const sheet::Model> model;
    std::string body;                    // ID of the sheet body; empty — parameters only (no base wall yet)
    kernel::Shape folded;                // sheet body shape produced by the last sheet feature
    kernel::Shape flat;                  // flat pattern body (world, fold F₀)
    std::vector<std::string> flatBases;  // face bases of the flat pattern body
    double overlap = 0;                  // flat pattern overlap volume, mm³ (sheet.md §4.8)
};

// Part of the virtual assembly state (assembly.md §7г.2): an instance's part state in part coordinates and its placement.
struct AssemblyPart {
    std::string path;
    Motion3 world;
    std::shared_ptr<const PartState> local;
    std::string element, file;  // the inserted part: element ID and file (InstanceEntry::file; empty — this document)
};

// A new state field also goes into the cache dump (regen/state_io.cpp, regen.md §5.4), bumping REGEN_DISK_CACHE_FORMAT.
struct PartState {
    std::vector<Body> bodies;  // in body order (§2.4)
    naming::NameTable names;
    std::map<std::string, SketchEntry> sketches;
    std::map<std::string, Plane> planes;    // reference planes (§4.6)
    std::map<std::string, RefAxis> axes;    // reference axes (§4.6)
    std::map<std::string, ToolEntry> tools;  // tools for patterns (§4.7)
    std::vector<ThreadEntry> threads;        // threads (§4.13, §4.14)
    std::vector<GearEntry> gears;            // gear rims (§4.17)
    std::optional<SheetEntry> sheet;         // sheet metal part (§4.16)
    // In-context state of a part within an assembly (assembly.md §7в.2): bodies of other parts in part coordinates;
    // only for resolving in-context references. contextError — a cycle (REGEN_CONTEXT_CYCLE).
    std::shared_ptr<const PartState> context;
    std::optional<Error> contextError;
    // Virtual assembly state (assembly.md §7г.2): parts in instance order; bodies and names are their concatenation.
    std::vector<AssemblyPart> assemblyParts;
    // Connection reports by feature ID (solutions.md §7.7): measurements, chosen rows, cuts, placed parts.
    std::map<std::string, doc::Json> connections;
    // Instances of evaluated patterns by feature ID (§4.7 item 7): label and rigid motion, part coordinates.
    std::map<std::string, std::vector<std::pair<std::string, kernel::Motion>>> patterns;
};

using StatePtr = std::shared_ptr<const PartState>;

enum class FeatureStatus { Ok, Warning, Error, Suppressed, RolledBack };
const char* featureStatusName(FeatureStatus s);

struct Message {
    bool error = true;  // otherwise a warning
    Error e;            // code, params, path
};

struct EvalResult {
    FeatureStatus status = FeatureStatus::Ok;
    std::vector<Message> messages;
    StatePtr stateOut;
    kernel::Shape tool;
};

using EvalPtr = std::shared_ptr<const EvalResult>;

struct FeatureResult {
    FeatureStatus status = FeatureStatus::Ok;
    std::vector<Message> messages;
};

// Drawing view model (§3а): bodies in world placements or an error.
struct PartResult;
// Planar cut face of a body cut by model sections (drawing.md §6.26, FX10-DRW-002): world coordinates.
inline constexpr double DRAWING_CUT_CHORD = 0.02;   // model mm: triangulation of cut faces for hatching (drawing.md §6.26)
inline constexpr double DRAWING_CUT_ANGLE = 0.2;    // rad
struct DrawingCutFace {
    int body = 0;                               // index in DrawingModel::bodies
    Vec3 normal = Vec3::UnitZ();                // outward normal of the cut body at the face
    std::vector<std::vector<Point3>> loops;     // boundary loops, the outer first
};
struct DrawingModel {
    StatePtr state;
    std::vector<kernel::PlacedShape> bodies;
    std::vector<std::string> colors;  // per body: part color "#RRGGBB" for the render view, "" — default (§3а, FX8-DRW-003)
    std::vector<DrawingCutFace> cutFaces;  // a variant with model sections: planar cut faces to hatch (FX10-DRW-002)
    std::optional<Error> error;
    // The model document (active — the model) and its regeneration: for the section variants (S7F-DRW-005).
    std::shared_ptr<const doc::Document> modelDoc;
    std::shared_ptr<const PartResult> modelResult;
};
// View model key: absolute file path (empty — this document) + "#" + element ID.
inline std::string drawingModelKey(const std::string& absFile, const std::string& element) { return absFile + "#" + element; }
// Element part of the model key of a view (drawing.md §6.24): an exploded view of an assembly — "<element>#exploded".
inline std::string drawingModelElement(const std::string& element, bool exploded) { return exploded ? element + "#exploded" : element; }
// Element part of the model key of the bodies of a main view with model sections (drawing.md §6.26, S7F-DRW-005):
// "<drawingModelElement>#sections" — the sections enabled in the model; with the view's list — "…#sections:<id>,<id>"
// (sorted).
std::string drawingSectionsElement(const std::string& modelElement, const std::optional<std::vector<std::string>>& sections);

struct PartResult {
    uint64_t revision = 0;
    std::map<std::string, FeatureResult> featureStatus;
    StatePtr final;
    std::map<std::string, StatePtr> after;
    StatePtr initial;
    uint64_t finalKey = 0;  // key of the final state (§5.1)
    expr::VariableTable variables;
    AssemblyPtr assembly;  // active element is an assembly (assembly.md §2)
    std::map<std::string, DrawingModel> models;  // active element is a drawing: view models (§3а)
    // Active element is a specification of an assembly of this document: the assembly's final state (its connection
    // reports — the fastener rows, specification.md §3.1 item 2а, FX14-SPEC-001).
    StatePtr specAssembly;
};

// Comparison of body IDs by creation key (§2.4).
bool bodyIdLess(const std::string& a, const std::string& b);

namespace detail {
struct Ctx;
}

// Extra checks of a rebuild (§4.16): tabsFrom ≥ 0 — pieces from this index are tabs with extensions, a grown overlap is
// SHEET_TAB_OVERLAP (FX7-SH-003); source — the recognized body (S5-SHEET-006): its ID is kept, the rebuilt body must
// repeat its volume and area, an overlap is an error (sheet.md §4.9 item 12).
struct SheetCheck {
    int tabsFrom = -1;
    const Body* source = nullptr;
};

// Additional bodies of an assembly drawing view model (drawing.md §6.3, FX9-WLD-002): weld bodies need the drawing
// module, which depends on regen, so the api installs this hook once (process-wide). pd — the model document (active —
// the assembly), pr — its regeneration result; appends to m.bodies / m.colors.
using DrawingModelExtras =
    std::function<void(const kernel::Kernel&, const doc::Document& pd, const PartResult& pr, const LinkMap* links, DrawingModel& m)>;
void setDrawingModelExtras(DrawingModelExtras f);
// Cutting the bodies of a view model by model sections (drawing.md §6.26, S7F-DRW-005): the section tools need the api
// module, which installs this hook once (process-wide). pd — the model document (active — the model), pr — its
// regeneration, sections — the IDs to cut with; replaces m.bodies / m.colors by the cut bodies.
using DrawingModelSections = std::function<void(const kernel::Kernel&, const doc::Document& pd, const PartResult& pr,
                                                const std::vector<std::string>& sections, DrawingModel& m)>;
void setDrawingModelSections(DrawingModelSections f);

class Regenerator {
public:
    explicit Regenerator(const kernel::Kernel& k);

    // nullopt — cancelled.
    // countStats = false — service regeneration (preview input, export of another revision):
    // the §5.3 counters do not change.
    // links, selfPath — linked documents and the document's own path (assemblies with objects from other files, assembly.md §5а).
    // vars — the variable table to use instead of documentVariables (instance variables, assembly.md §2а).
    std::optional<PartResult> regenerate(const doc::Document& d, uint64_t revision, const Cancel* cancel = nullptr,
                                         bool countStats = true, const LinkMap* links = nullptr, const std::string& selfPath = {},
                                         const expr::VariableTable* vars = nullptr);

    // Evaluator of a single feature (for preview); does not change stats.
    std::optional<EvalPtr> evaluateFeature(const doc::Document& d, const doc::Feature& f, int featureIndex,
                                           const StatePtr& input, uint64_t inputKey,
                                           const expr::VariableTable& vars, const Cancel* cancel = nullptr);

    const std::map<std::string, int>& stats() const { return stats_; }
    // Time of the last feature evaluation, ms (§5.3): same events as stats.
    const std::map<std::string, double>& timings() const { return timings_; }
    // Clear the evaluator cache (regen.md §5): the next regeneration evaluates all features anew.
    void clearCache() {
        lru_.clear();
        index_.clear();
    }
    // Preloading entries (§5.2, disk cache): like cache.put in list order; stats do not change.
    void preload(const std::vector<std::pair<uint64_t, EvalPtr>>& entries);
    // Tracking of used entries (§5.2): keys read from the cache or put into it, in order of first access.
    void startRecording();
    std::vector<std::pair<uint64_t, EvalPtr>> stopRecording();
    // Total evaluator calls in regenerations since the session start (§5.2), including service ones.
    long long evaluations() const { return evaluations_; }
    static uint64_t initialKey();

    // Evaluator key (§5.1).
    static uint64_t evaluatorKey(const doc::Document& d, const doc::Feature& f, uint64_t inputKey,
                                 const expr::VariableTable& vars);

private:
    EvalPtr evaluate(const doc::Document& d, const doc::Feature& f, int featureIndex, const StatePtr& input,
                     const expr::VariableTable& vars, const Cancel* cancel, bool& cancelledOut);
    // Evaluators of feature groups (eval_*.cpp).
    EvalPtr evalShellDraft(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    EvalPtr evalSweep(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    EvalPtr evalImport(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    EvalPtr evalThread(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    EvalPtr evalHole(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    EvalPtr evalGear(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    EvalPtr evalCoil(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    EvalPtr evalConnection(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    EvalPtr evalConnectionCut(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    EvalPtr evalSheet(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    // Extrusion in a sheet part (§4.16 item 5, FX7-SH-001, FX7-SH-002): a cut through walls → cuts of the model, an
    // addition from a sketch on a wall end face → a tab; handled = false — the extrusion is evaluated as usual.
    EvalPtr evalSheetExtrude(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut,
                             const doc::ExtrudeData* x, const SketchEntry& se, const std::vector<const sketch::Region*>& regions,
                             bool& handled);
    EvalPtr evalSheetHole(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut,
                          const std::vector<std::pair<std::string, Point3>>& points, const Vec3& dir, double diameter);
    // Fillet and chamfer of corners through the thickness in a sheet part (§4.16 item 5б, FX8-SH-004): edges — resolved
    // logical edges of the sheet body (input body index, logical edge, stable name, input path).
    struct SheetEdgeRef {
        int body = 0, logical = 0;
        std::string name, path;
    };
    EvalPtr evalSheetEdge(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut,
                          const std::vector<SheetEdgeRef>& edges, double size, double size2, double angle, bool flip);
    // Pattern and mirror in a sheet part (§4.16 item 5в, FX8-SH-005).
    EvalPtr evalSheetPattern(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    // The input has a sheet body unchanged by other features (§4.16 item 2).
    static bool sheetIntact(const PartState& in);
    EvalPtr sheetRebuild(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut,
                         sheet::Model model, const SheetCheck& check = SheetCheck());
    EvalPtr evalLoft(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut);
    EvalPtr evalExtrude(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut, const doc::ExtrudeData* x);
    EvalPtr evalRevolve(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut, const doc::RevolveData* rv);
    EvalPtr evalEdgeFeature(detail::Ctx& c, const StatePtr& input, const std::shared_ptr<PartState>& st, bool& cancelledOut, const doc::EdgeFeatureData* ef);
    bool drawingModels(const doc::Document& d, PartResult& res, uint64_t revision, const Cancel* cancel, const LinkMap* links,
                       const std::string& selfPath);  // false — cancelled
    EvalPtr cacheGet(uint64_t key);
    void cachePut(uint64_t key, EvalPtr r);
    // A nested assembly evaluated as when opened by itself (assembly.md §2, FX9-ASM-008, FX9-ASM-012): its instances with
    // solved mates, assembly features, connections and instance patterns, in its own frame; form — placements of its
    // nested instances given by the instance (§2б, FX9-ASM-010). nullopt — cancelled.
    std::optional<AssemblyPtr> nestedAssembly(const doc::Document& D, const doc::Element& asmEl, const std::string& file,
                                              uint64_t revision, const Cancel* cancel, const LinkMap* links,
                                              const expr::VariableTable& vars, const std::vector<std::string>& chain,
                                              const std::vector<doc::Instance::FormPlacement>& form);

    const kernel::Kernel& k_;
    // Nested assembly evaluation (nestedAssembly): the enclosing assemblies ("<file>#<id>", outermost first) — cycle
    // detection and owners of connection cuts; the next regenerate is nested (no exploded view).
    std::vector<std::string> asmChain_;
    bool nestedCall_ = false;
    std::map<std::string, AssemblyPtr>* nestedMemo_ = nullptr;  // within one top-level regeneration
    std::map<std::string, int> stats_;
    // Parts whose in-context state is being built (assembly.md §7в.2 item 4, S7F-ASM-005): a part shown in the context
    // of another one does not show that one back (two parts in the context of one assembly recursed without end).
    std::vector<std::string> contextBuilding_;
    std::map<std::string, double> timings_;
    std::list<std::pair<uint64_t, EvalPtr>> lru_;
    std::map<uint64_t, std::list<std::pair<uint64_t, EvalPtr>>::iterator> index_;
    long long evaluations_ = 0;
    bool recording_ = false;
    std::vector<std::pair<uint64_t, EvalPtr>> recorded_;
    std::map<uint64_t, size_t> recordedIndex_;
    void record(uint64_t key, const EvalPtr& r);
};

// Document units and variables (spec/expr.md §5).
expr::DocumentUnits documentUnits(const doc::Document& d);
// Variables of the document with instance variables of its assemblies (assembly.md §2а); links, selfPath — for objects of
// other files (without links their instances give no variables).
expr::VariableTable documentVariables(const doc::Document& d, const LinkMap* links = nullptr, const std::string& selfPath = {});
// Instance qualifiers of all assemblies of the document (assembly.md §2а): instance ID → qualifier.
std::map<std::string, std::string> instanceQualifiers(const doc::Document& d);
// Document of the object inserted by an instance (this one or a linked file, assembly.md §5а); nullptr — not found.
// file — absolute path of that document (selfPath for this one).
const doc::Document* instanceTarget(const doc::Document& d, const doc::Instance& in, const LinkMap* links, const std::string& selfPath,
                                    std::string* file = nullptr);
// Unresolved instance variables (assembly.md §2а): ASM_VARIANT_NOT_FOUND / ASM_VARIABLE_NOT_FOUND.
std::optional<Error> instanceVariablesError(const doc::Instance& in, const doc::Document& target);
// Variable table of an instance for regeneration of its part (assembly.md §2а): docTable — documentVariables of the
// document holding the assembly; sameFile — the object is in that document.
expr::VariableTable instanceVariableTable(const expr::VariableTable& docTable, const std::string& qualifier, const doc::Document& target,
                                          bool sameFile);
// Content digest of a variable table (names, types, values) — key of "part evaluated in this revision" (assembly.md §2а).
uint64_t variableTableDigest(const expr::VariableTable& t);

// Flat pattern state of a sheet part (§4.16 item 4, core-api.md StateRef "flat"): the sheet body replaced by its flat
// pattern body with its own names; nullptr — not a sheet part or no base wall.
StatePtr sheetFlatState(const kernel::Kernel& k, const PartState& s);

// Values of the sketch's driving dimensions (spec/regen.md §4.1); evaluation errors get
// path = featurePath + "/constraints/<i>/value".
sketch::Values dimensionValues(const sketch::SketchData& s, const expr::VariableTable& vars,
                               const expr::DocumentUnits& units, const std::string& featurePath);

// Sketch projection geometry in state state (spec/regen.md §4.1 item 1а): resolving source and projecting with the kernel.
// messages — errors (REGEN_REF_LOST/AMBIGUOUS, KERNEL_NOT_SUPPORTED) and warnings (NAMING_REF_*).
struct ProjectionResult {
    std::map<std::string, sketch::Projection> projections;
    std::vector<Message> messages;
};
ProjectionResult resolveProjections(const sketch::SketchData& s, const Plane& frame, const PartState& state,
                                    const kernel::Kernel& k);

// Implicit body targets of a sketch (sketch.md §2.5): first projections of edges and vertices of the state that do not
// lie in the sketch plane (FX8-SK-010; source — the edge/vertex name, by depth), then silhouette lines of cylinders and
// cones and extreme points of meridians of tori and other surfaces of revolution whose axis is parallel to the sketch
// plane (FX8-SK-002; source — "silhouette+:<face>", …; faces of the name table in order, then the kinds in the order of
// sketch::PROJECTION_SOURCE_PREFIXES). Geometry — in sketch coordinates.
struct BodyTarget {
    std::string source;
    sketch::Projection geometry;
    bool revolution = false;  // a silhouette or an extreme (FX8-SK-002), not an edge/vertex projection
};
std::vector<BodyTarget> bodyTargets(const Plane& frame, const PartState& state, const kernel::Kernel& k);
// Geometry of one implicit target source on face of body shape; nothing — not computable (axis not parallel to the
// plane, a surface without an axis, the half-plane misses the face).
std::optional<sketch::Projection> bodyTargetGeometry(const std::string& kind, const kernel::Shape& shape, int face, const Plane& frame,
                                                     const kernel::Kernel& k);
inline constexpr double SILHOUETTE_PARALLEL_TOL = 1e-7;  // |axis · sketch normal| (sketch.md §7)
inline constexpr double BODY_TARGET_PLANE_TOL = 1e-4;   // mm: an edge/vertex lies in the sketch plane (sketch.md §7)

// Warnings of a successful resolution of reference name (naming.md §5.4, "General rules"): NAMING_REF_MERGED, NAMING_REF_ORDINAL.
void referenceWarnings(const naming::ResolveResult& rr, const std::string& name, std::vector<Message>& out);

// Sketch plane on face face in state state (regen.md §4.1 item 1, naming.md §5.4): error — REGEN_REF_LOST,
// REGEN_REF_AMBIGUOUS (incl. SPLIT into non-coplanar parts), REGEN_FACE_NOT_PLANAR; warnings — into warnings.
Result<Plane> sketchPlaneOnFace(const std::string& face, const PartState& state, const kernel::Kernel& k,
                                std::vector<Message>& warnings);

// Reference geometry of a definition over state in without a document (selection.md §7.1, FX7-SEL-001): candidate preview
// by the same evaluator that regeneration uses. def — a plane/axis feature definition ("type": "plane" | "axis", §4.6) or
// {"type": "direction", "ref": DirRef} (§4.7 item 2). Result — plane or axis; messages — evaluation errors.
struct RefPreview {
    std::optional<Plane> plane;
    std::optional<RefAxis> axis;
    std::vector<Message> messages;
};
RefPreview previewReference(const kernel::Kernel& k, const doc::Document& d, const PartState& in, const doc::Json& def,
                            const expr::VariableTable& vars);

// Sketches of an element shown by default (core-api visibleSketches, app-ux §3.4): not suppressed and not used by an
// extrude, revolve, coil, sweep or loft; history order.
std::vector<std::string> visibleSketchIds(const doc::Element& el);
// Reference planes, axes and visible sketches of assembly element el in its final state, own (node empty), for showing
// where the assembly is nested (assembly.md §2 «Assembly objects», FX9-ASM-008).
std::vector<AssemblyDatum> ownAssemblyDatums(const doc::Element& el, const PartState& final);
// World datums of the nested assemblies of st (node not empty): the carrying entry shown (not hidden, suppressed or in
// error), geometry moved by its world placement — the form of an underdetermined subassembly included.
std::vector<AssemblyDatum> nestedDatumsWorld(const AssemblyState& st);

// Kernel profile from a sketch region (spec/sketch.md §3.4).
kernel::Profile toKernelProfile(const sketch::Region& r, const Plane& frame);
// Explicit sketch plane frame (document.md §2.7): orthonormalization; degenerate — nullopt.
std::optional<Plane> planeFromFrame(const std::array<double, 9>& f);

// In-context design (assembly.md §7в): the part's in-context references (sketch planes, projection sources; history
// order), used instances (by compareIds), dependency along the chain of used instances.
std::vector<std::string> contextRefs(const doc::Element& el);
std::vector<std::string> usedContextPaths(const doc::Element& el);
bool contextDependsOn(const doc::Document& d, const std::string& from, const std::string& target);
// Bodies and names of part part under path path, transformed by motion M, — into state v (assembly.md §7б, §7в.2).
// reuse — already transformed bodies by bodyId in v (the transform is skipped for them).
void appendInstanceState(const kernel::Kernel& k, PartState& v, const std::string& path, const PartState& part, const Motion3& M,
                         const std::map<std::string, kernel::Shape>* reuse = nullptr);
// Assembly features (assembly.md §7г): bodies and names of the virtual state v rebuilt from its parts (§7г.2);
// world — the previous world bodies by bodyId, reused for unchanged parts (no transform).
void rebuildAssemblyState(const kernel::Kernel& k, PartState& v, const std::map<std::string, kernel::Shape>& world = {});
// Assembly instance states after assembly features (§7г.4): each part instance's state — its part in state final.
AssemblyPtr assemblyWithFeatures(const AssemblyPtr& a, const PartState& final, uint64_t finalKey);

// Owned connection cuts (solutions.md §7.10, assembly.md §2): whether feature f belongs to a connection of assembly
// assemblyId of the document at assemblyFile (absolute; empty — unsaved), f being in a part of the document at partFile
// (sameDocument — the part is in the assembly's document); and the element without such features (the rollback marker
// on a removed one moves to the previous remaining).
bool ownedByAssembly(const doc::Feature& f, const std::string& assemblyId, bool sameDocument, const std::string& partFile,
                     const std::string& assemblyFile);
void withoutOwnedCuts(doc::Element& el, const std::string& assemblyId, bool sameDocument, const std::string& partFile,
                      const std::string& assemblyFile);

// Connections (solutions.md §7): the frame, the pack and the evaluated plan of a connection over virtual state v.
struct ConnectionFrame {
    Point3 O = Point3::Zero();
    Vec3 X = Vec3::UnitX(), Y = Vec3::UnitY(), Z = Vec3::UnitZ();
    Point3 at(double x, double y, double z) const { return O + x * X + y * Y + z * Z; }
};
struct ConnectionPlan {
    solutions::Plan plan;
    ConnectionFrame frame;
    std::vector<std::string> pack;                  // part paths (§7.4)
    std::map<std::string, std::string> inputParts;  // face/cylinder input → part path
};
// Measurements of one state for repeated plans (solutions.md §8, FX8-BLT-001): packs by (frame, probe radius, pack
// end) and part depths by (part, frame). Valid for the state it was filled on — the caller drops it when the state
// changes. measure = false — a pack not in the cache is not measured: the plan fails with SOL_PACK_UNMEASURED.
struct PackCache {
    struct Pack {
        std::optional<Error> error;
        std::vector<solutions::Value> values;
        std::vector<std::string> parts;
    };
    std::map<std::string, Pack> packs;
    std::map<std::string, double> depths;
    bool measure = true;
    int probes = 0;  // packs measured (probe booleans run) — for tests
};
ConnectionPlan planConnection(const kernel::Kernel& k, const solutions::Definition& def, const solutions::TableProvider& tables,
                              const doc::Json& inputs, const doc::Json& choices, const PartState& v, const Cancel* cancel = nullptr,
                              PackCache* cache = nullptr);
// Points of the frame point input given as a sketch {"sketch": id} (solutions.md §7.12, FX8-BLT-002): the hole points of
// the assembly sketch (regen.md §4.13 item 1) in world coordinates, in sketch order. nullopt — the input is not a sketch;
// errors: REGEN_INPUT_ERROR (no sketch state), SOL_NO_POINTS (no points).
Result<std::optional<std::vector<Point3>>> connectionSketchPoints(const doc::Element& el, const PartState& v,
                                                                  const solutions::Definition& def, const doc::Json& inputs);
// Inputs of the k-th connection of a sketch input: the point input replaced by the point [x, y, z].
doc::Json inputsAtPoint(const solutions::Definition& def, const doc::Json& inputs, const Point3& p);
// Output name of the k-th point (0-based) of a connection over sketch points: the first — as is, others "<id>_p<k+1>".
std::string pointOutput(const std::string& output, size_t k);
// The report of several points (§7.12): the first point's report with the cuts and parts of the others appended under
// their point output names; countKey ("points"; "axes" — several axes, §7.11) — the count.
doc::Json mergePointReports(const std::vector<doc::Json>& reports, const char* countKey = "points");
// The frame axis input given as a list of axes ["<face>", …] (solutions.md §7.11, FX10-WZ-005): ≥ 1 non-empty strings.
bool isAxisList(const doc::Json& v);
// Inputs per position of a connection (solutions.md §7.11, §7.12): an axis list — one per axis (the frame axis input is
// that axis); a points sketch — one per point (connectionSketchPoints); otherwise the inputs as is. byAxes — set when
// the positions are the axes of a list (the report counts them as "axes").
Result<std::vector<doc::Json>> connectionPositions(const doc::Element& el, const PartState& v, const solutions::Definition& def,
                                                   const doc::Json& inputs, bool* byAxes = nullptr);
// Table of a library package copy (solutions.md §3.3): size rows, cell values with the row active, designations.
solutions::TableData libraryCopyTable(const doc::LibraryPart& lp);
// Tables of a connection from the document (§5.2): own tables of the copy, built-in, library copies by tableMap.
solutions::TableProvider connectionTables(const doc::Document& d, const doc::SolutionCopy& copy, const doc::Json& tableMap);
// Report of a plan (§7.7); libraries — table id → library copy id; changed — per cut, changed part paths (empty — preview);
// partsOf — the virtual state of the plan for the "parts" mode (§7.10 item 3.3: "cut" and the owned entries); nullptr —
// the "assembly" mode.
doc::Json connectionReport(const ConnectionPlan& p, const std::map<std::string, std::string>& libraries,
                           const std::vector<std::vector<std::string>>& changed = {}, const PartState* partsOf = nullptr);
// Frame of an owned cut as JSON {origin, x, y, z} and back (document.md §2.10о).
doc::Json frameJson(const ConnectionFrame& f);
ConnectionFrame frameFromJson(const doc::Json& j);
// World motion of a placed part (§7.6).
Motion3 connectionPartMotion(const ConnectionFrame& f, const solutions::PartPlan& p);
// Part element of a library copy (§7.6): the first part element of the copy document; empty — none.
std::string libraryCopyElement(const doc::LibraryPart& lp);

}  // namespace construe::regen
